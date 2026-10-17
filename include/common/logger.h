#pragma once

#include "log_level.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace logging {

enum class QueuePolicy { DROP, BLOCK };

struct logMessage {
  LogLevel level;
  std::string json;
};

class Logger {
public:
  static Logger &instance();

  void init(const std::string &file_path,
            LogLevel level = LogLevel::INFO,
            std::size_t file_size = 5 * 1024 * 1024,
            int max_files = 5,
            std::size_t max_queue_size = 8192,
            QueuePolicy policy = QueuePolicy::DROP);

  // Normal messages are rejected after shutdown starts. FATAL bypasses queue
  // capacity, drains accepted messages, syncs the current log file, then aborts.
  // This API is thread-safe, but is not async-signal-safe.
  void log(LogLevel level, const std::string &message, const char *file, int line);
  void shutdown();
  LogLevel level() const;

private:
  Logger() = default;
  ~Logger();
  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  void worker_loop() noexcept;
  void write_record(const logMessage &message);
  void flush_buffer();
  void rotate_if_needed();
  void open_file();
  void close_file() noexcept;
  // lifecycle_mutex_ must be held; joins without holding mutex_.
  void stop_worker();
  [[noreturn]] void fatal_abort(const std::string &json);
  static std::string make_record(LogLevel level, const std::string &message,
                                 const char *file, int line);
  static std::string timestamp();
  static std::string escape_json(const std::string &s);

  // Serializes init/shutdown/fatal handling, including join and descriptor close.
  std::mutex lifecycle_mutex_;
  mutable std::mutex mutex_;
  std::condition_variable work_cv_;
  std::condition_variable space_cv_;
  std::queue<logMessage> queue_;
  std::thread worker_;
  LogLevel level_{LogLevel::INFO};
  std::size_t max_queue_size_{0};
  QueuePolicy queue_policy_{QueuePolicy::DROP};
  std::uint64_t generation_{0};
  bool initialized_{false};
  bool accepting_{false};
  bool sync_on_exit_{false};
  bool worker_failed_{false};

  // Only the worker accesses these while it is running. Initialization happens
  // before thread creation; shutdown closes the descriptor after joining.
  int fd_{-1};
  std::string base_path_;
  std::string buffer_;
  std::uintmax_t current_size_{0}; // Includes bytes still buffered in memory.
  std::size_t max_file_size_{0};
  int max_files_{0};
};

} // namespace logging

#define LOG_TRACE(msg) \
  logging::Logger::instance().log(logging::LogLevel::TRACE, msg, __FILE__, __LINE__)
#define LOG_DEBUG(msg) \
  logging::Logger::instance().log(logging::LogLevel::DEBUG, msg, __FILE__, __LINE__)
#define LOG_INFO(msg) \
  logging::Logger::instance().log(logging::LogLevel::INFO, msg, __FILE__, __LINE__)
#define LOG_WARN(msg) \
  logging::Logger::instance().log(logging::LogLevel::WARN, msg, __FILE__, __LINE__)
#define LOG_ERROR(msg) \
  logging::Logger::instance().log(logging::LogLevel::ERROR, msg, __FILE__, __LINE__)
#define LOG_FATAL(msg) \
  logging::Logger::instance().log(logging::LogLevel::FATAL, msg, __FILE__, __LINE__)
