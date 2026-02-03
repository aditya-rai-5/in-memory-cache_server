#include "../../include/common/logger.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <cstdlib>

namespace fs = std::filesystem;

namespace logging
{

     Logger &Logger::instance()
     {
          static Logger instance;
          return instance;
     }

     // Logger::Logger() = default;

     Logger::~Logger()
     {
          shutdown();
     }

     void Logger::init(const std::string &file_path,
                       LogLevel level,
                       std::size_t max_file_size,
                       int max_files,
                       std::size_t max_queue_size,
                       QueuePolicy policy)
     {
          std::lock_guard<std::mutex> lock(mutex_);
          if (initialized_)
               return;

          base_path_ = file_path;
          level_ = level;
          max_file_size_ = max_file_size;
          max_files_ = max_files;
          max_queue_size_ = max_queue_size;
          queue_policy_ = policy;

          fd_ = ::open(base_path_.c_str(),
                       O_CREAT | O_WRONLY | O_APPEND,
                       0644);

          if (fd_ == -1)
          {
               throw std::runtime_error("Failed to open log fd");
          }

          file_.open(base_path_, std::ios::app);
          if (!file_)
          {
               throw std::runtime_error("Failed to open log file");
          }

          running_ = true;
          worker_ = std::thread(&Logger::worker_loop, this);
          initialized_ = true;
     }

     void Logger::shutdown()
     {
          {
               std::lock_guard<std::mutex> lock(mutex_);
               if (!initialized_)
               {
                    return;
               }
               running_ = false;
          }
          if (fd_ != -1)
          {
               ::close(fd_);
               fd_ = -1;
          }

          cv_.notify_all();
          if (worker_.joinable())
          {
               worker_.join();
          }

          if (file_)
          {
               file_.close();
          }

          initialized_ = false;
     }

     LogLevel Logger::level() const
     {
          return level_;
     }

     // void Logger::flush()
     // {
     //      std::lock_guard<std::mutex> lock(mutex_);
     //      if (file_)
     //           file_.flush();
     // }

     void Logger::log(LogLevel level,
                      const std::string &message,
                      const char *file,
                      int line)
     {
          if (!initialized_ || level < level_)
               return;

          std::ostringstream json;
          json << "{"
               << "\"ts\":\"" << timestamp() << "\","
               << "\"level\":\"" << to_string(level) << "\","
               << "\"file\":\"" << file << "\","
               << "\"line\":" << line << ","
               << "\"thread\":" << std::this_thread::get_id() << ","
               << "\"msg\":\"" << escape_json(message) << "\""
               << "}";

          std::unique_lock<std::mutex> lock(mutex_);

          if (queue_.size() >= max_queue_size_)
          {
               if (queue_policy_ == QueuePolicy::DROP)
               {
                    return;
               }
               cv_.wait(lock, [&]
                        { return queue_.size() < max_queue_size_; });
          }

          queue_.push({level, json.str()});
          cv_.notify_one();

          if (level == LogLevel::FATAL)
          {
               crash_safe_abort();
          }
     }

     void Logger::worker_loop()
     {
          while (true)
          {
               std::queue<logMessage> local;
               {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [&]
                             { return !queue_.empty() || !running_; });

                    if (!running_ && queue_.empty())
                    {
                         break;
                    }
                    local.swap(queue_);
               }

               while (!local.empty())
               {
                    auto msg = std::move(local.front());
                    local.pop();

                    rotate_if_needed();
                    file_ << msg.json << '\n';

                    if (msg.level >= LogLevel::WARN)
                    {
                         std::cerr << msg.json;
                    }
                    else
                    {
                         std::cout << msg.json;
                    }
               }
          }
     }

     void Logger::crash_safe_abort()
     {
          while (!queue_.empty())
          {
               auto msg = std::move(queue_.front());
               queue_.pop();
               file_ << msg.json << '\n';
          }

          file_.flush();

          if (fd_ != -1)
          {
               ::fsync(fd_);
          }

          std::abort();
     }

     void Logger::rotate_if_needed()
     {
          if (!fs::exists(base_path_))
               return;
          if (fs::file_size(base_path_) < max_file_size_)
               return;

          file_.close();

          for (int i = max_files_ - 1; i >= 1; --i)
          {
               fs::path old = base_path_ + "." + std::to_string(i);
               fs::path next = base_path_ + "." + std::to_string(i + 1);
               if (fs::exists(old))
                    fs::rename(old, next);
          }

          fs::rename(base_path_, base_path_ + ".1");
          file_.open(base_path_, std::ios::trunc);
     }

     std::string Logger::timestamp()
     {
          using clock = std::chrono::system_clock;
          auto now = clock::now();
          auto t = clock::to_time_t(now);

          std::tm tm{};
          // #if defined(_WIN32)
          // localtime_s(&tm, &t);
          // #else
          localtime_r(&t, &tm);
          // #endif

          std::ostringstream oss;
          oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
          return oss.str();
     }

     std::string Logger::escape_json(const std::string &s)
     {
          std::string out;
          for (char c : s)
          {
               if (c == '"' || c == '\\')
                    out += '\\';
               out += c;
          }
          return out;
     }

}
