#include "common/logger.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace logging {
namespace {

constexpr std::size_t buffer_limit = 8192;

void write_all(int fd, const std::string &data, const std::string &context) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto count = std::min(data.size() - offset,
                                static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const auto written = ::write(fd, data.data() + offset, count);
    if (written < 0 && errno == EINTR)
      continue;
    if (written < 0)
      throw std::system_error(errno, std::generic_category(), context);
    if (written == 0)
      throw std::runtime_error(context + ": write returned zero");
    offset += static_cast<std::size_t>(written);
  }
}

// Avoid recursive logging and throwing from worker/destructor error paths.
void emergency_write(const std::string &message) noexcept {
  try {
    write_all(STDERR_FILENO, message + "\n", "stderr");
  } catch (...) {
  }
}

} // namespace

Logger &Logger::instance() {
  static Logger instance;
  return instance;
}

Logger::~Logger() { shutdown(); }

void Logger::init(const std::string &file_path, LogLevel level,
                  std::size_t max_file_size, int max_files,
                  std::size_t max_queue_size, QueuePolicy policy) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_)
    return;
  if (max_file_size == 0 || max_files < 1 || max_files > 1000 || max_queue_size == 0)
    throw std::invalid_argument("Logger requires positive file/queue sizes and 1..1000 backups");
  if (level < LogLevel::TRACE || level > LogLevel::FATAL ||
      (policy != QueuePolicy::DROP && policy != QueuePolicy::BLOCK))
    throw std::invalid_argument("Invalid logger level or queue policy");

  base_path_ = file_path;
  level_ = level;
  max_file_size_ = max_file_size;
  max_files_ = max_files;
  max_queue_size_ = max_queue_size;
  queue_policy_ = policy;
  buffer_.clear();
  worker_failed_ = false;
  sync_on_exit_ = false;
  open_file();
  try {
    accepting_ = true;
    worker_ = std::thread(&Logger::worker_loop, this);
  } catch (...) {
    accepting_ = false;
    close_file();
    throw;
  }
  ++generation_;
  initialized_ = true;
}

void Logger::open_file() {
  // O_NONBLOCK prevents opening a FIFO from hanging before the type check.
  fd_ = ::open(base_path_.c_str(), O_CREAT | O_WRONLY | O_APPEND | O_CLOEXEC | O_NONBLOCK, 0644);
  if (fd_ < 0)
    throw std::system_error(errno, std::generic_category(),
                            "Cannot open log.file '" + base_path_ + "'");
  struct stat status{};
  if (::fstat(fd_, &status) < 0) {
    const int error = errno;
    close_file();
    throw std::system_error(error, std::generic_category(),
                            "Cannot stat log.file '" + base_path_ + "'");
  }
  if (!S_ISREG(status.st_mode)) {
    close_file();
    throw std::invalid_argument("log.file '" + base_path_ + "' must be a regular file");
  }
  current_size_ = status.st_size > 0 ? static_cast<std::uintmax_t>(status.st_size) : 0;
}

void Logger::close_file() noexcept {
  const int old_fd = fd_;
  fd_ = -1;
  // Linux releases the descriptor even on EINTR; retrying could close a reused fd.
  if (old_fd >= 0 && ::close(old_fd) < 0)
    emergency_write("Logging error: cannot close log.file '" + base_path_ + "'");
}

void Logger::stop_worker() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_)
      return;
    accepting_ = false;
  }
  work_cv_.notify_all();
  space_cv_.notify_all();
  if (worker_.joinable())
    worker_.join();
  close_file();
  std::lock_guard<std::mutex> lock(mutex_);
  initialized_ = false;
}

void Logger::shutdown() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  stop_worker();
}

LogLevel Logger::level() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return level_;
}

void Logger::log(LogLevel level, const std::string &message, const char *file, int line) {
  if (level == LogLevel::FATAL)
    fatal_abort(make_record(level, message, file, line));

  std::unique_lock<std::mutex> lock(mutex_);
  if (!accepting_ || level < level_)
    return;
  const auto generation = generation_;
  if (queue_policy_ == QueuePolicy::BLOCK) {
    space_cv_.wait(lock, [&] {
      return !accepting_ || generation_ != generation || queue_.size() < max_queue_size_;
    });
  }
  if (!accepting_ || generation_ != generation || queue_.size() >= max_queue_size_)
    return;
  queue_.push({level, make_record(level, message, file, line)});
  work_cv_.notify_one();
}

[[noreturn]] void Logger::fatal_abort(const std::string &json) {
  // Exclude shutdown and reinitialization while the worker drains and syncs.
  std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
  bool queued = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_ && accepting_) {
      queue_.push({LogLevel::FATAL, json}); // FATAL is never dropped or capacity-blocked.
      accepting_ = false;
      sync_on_exit_ = true;
      queued = true;
    }
  }
  stop_worker(); // Never join while holding the queue mutex.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!queued || worker_failed_)
      emergency_write(json);
  }
  lifecycle_lock.unlock();
  std::abort();
}

void Logger::worker_loop() noexcept {
  try {
    while (true) {
      std::queue<logMessage> local;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_cv_.wait(lock, [&] { return !queue_.empty() || !accepting_; });
        if (queue_.empty() && !accepting_)
          break;
        local.swap(queue_);
      }
      space_cv_.notify_all();
      while (!local.empty()) {
        write_record(local.front());
        local.pop();
      }
      flush_buffer();
    }
    flush_buffer();
    bool sync;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      sync = sync_on_exit_;
    }
    if (sync) {
      int result;
      do {
        result = ::fsync(fd_);
      } while (result < 0 && errno == EINTR);
      if (result < 0)
        throw std::system_error(errno, std::generic_category(), "Cannot sync log.file '" + base_path_ + "'");
    }
  } catch (const std::exception &error) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_ = false;
      worker_failed_ = true;
      // A failed sink cannot drain more records. Unblock all waiting producers.
      std::queue<logMessage> empty;
      queue_.swap(empty);
    }
    space_cv_.notify_all();
    emergency_write(std::string("Logging error: ") + error.what());
  }
}

void Logger::write_record(const logMessage &message) {
  rotate_if_needed();
  const auto bytes = message.json.size() + 1;
  if (current_size_ > std::numeric_limits<std::uintmax_t>::max() - bytes)
    throw std::overflow_error("Log file byte count overflow");
  if (bytes > buffer_limit) {
    flush_buffer();
    write_all(fd_, message.json + "\n", "Cannot write log.file '" + base_path_ + "'");
  } else {
    if (buffer_.size() + bytes > buffer_limit)
      flush_buffer();
    buffer_ += message.json;
    buffer_ += '\n';
  }
  current_size_ += bytes;
  auto &console = message.level >= LogLevel::WARN ? std::cerr : std::cout;
  console << message.json << '\n';
}

void Logger::flush_buffer() {
  if (buffer_.empty())
    return;
  write_all(fd_, buffer_, "Cannot write log.file '" + base_path_ + "'");
  buffer_.clear();
}

void Logger::rotate_if_needed() {
  if (current_size_ < max_file_size_)
    return;
  flush_buffer();
  close_file();
  for (int i = max_files_ - 1; i >= 1; --i) {
    const fs::path old = base_path_ + "." + std::to_string(i);
    if (fs::exists(old))
      fs::rename(old, base_path_ + "." + std::to_string(i + 1));
  }
  fs::rename(base_path_, base_path_ + ".1");
  open_file(); // Own and eventually fsync the new inode, never the rotated file.
}

std::string Logger::make_record(LogLevel level, const std::string &message,
                                 const char *file, int line) {
  std::ostringstream thread_id;
  thread_id.imbue(std::locale::classic());
  thread_id << std::this_thread::get_id();
  std::ostringstream json;
  json.imbue(std::locale::classic());
  json << "{\"ts\":\"" << timestamp() << "\",\"level\":\"" << to_string(level)
       << "\",\"file\":\"" << escape_json(file ? file : "") << "\",\"line\":" << line
       << ",\"thread\":\"" << escape_json(thread_id.str())
       << "\",\"msg\":\"" << escape_json(message) << "\"}";
  return json.str();
}

std::string Logger::timestamp() {
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
  localtime_r(&now, &tm);
  std::ostringstream text;
  text.imbue(std::locale::classic());
  text << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return text.str();
}

std::string Logger::escape_json(const std::string &s) {
  static constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    switch (c) {
    case '"': out += "\\\""; continue;
    case '\\': out += "\\\\"; continue;
    case '\b': out += "\\b"; continue;
    case '\f': out += "\\f"; continue;
    case '\n': out += "\\n"; continue;
    case '\r': out += "\\r"; continue;
    case '\t': out += "\\t"; continue;
    default: break;
    }
    if (c >= 0x20 && c < 0x80) {
      out += static_cast<char>(c);
      continue;
    }
    // Preserve valid UTF-8; escape invalid bytes so arbitrary client input still
    // yields valid JSON text rather than an invalid UTF-8 log file.
    std::size_t length = c >= 0xc2 && c <= 0xdf ? 2 :
        c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
    bool valid = length != 0 && length <= s.size() - i;
    for (std::size_t j = 1; valid && j < length; ++j)
      valid = (static_cast<unsigned char>(s[i + j]) & 0xc0) == 0x80;
    if (valid && length >= 3) {
      const auto second = static_cast<unsigned char>(s[i + 1]);
      valid = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) &&
              !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
    }
    if (valid) {
      out.append(s, i, length);
      i += length - 1;
    } else {
      out += "\\u00";
      out += hex[c >> 4];
      out += hex[c & 0x0f];
    }
  }
  return out;
}

} // namespace logging
