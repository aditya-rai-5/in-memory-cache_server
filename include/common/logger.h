#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>

#include "log_level.h"

namespace logging
{
     enum class QueuePolicy
     {
          DROP,
          BLOCK
     };

     struct logMessage
     {
          /* data */
          LogLevel level;
          std::string json;
     };

     class Logger
     {
     public:
          static Logger &instance();

          void init(const std::string &file_path,
                    LogLevel level = LogLevel::INFO,
                    std::size_t file_size = 5 * 1024 * 1024,
                    int max_files = 5,
                    std::size_t max_queue_size = 8192,
                    QueuePolicy policy = QueuePolicy::DROP);

          void log(LogLevel level,
                   const std::string &message,
                   const char *file,
                   int line);

          // void flush();

          void shutdown();
          LogLevel level() const;

     private:
          int fd_{-1};

     private:
          Logger() = default;
          ~Logger();

          Logger(const Logger &) = delete;
          Logger &operator=(const Logger &) = delete;

     private:
          void worker_loop();
          void rotate_if_needed();
          void crash_safe_abort();
          std::string timestamp();
          std::string escape_json(const std::string &s);

     private:
          std::ofstream file_;
          std::string base_path_;

          LogLevel level_{LogLevel::INFO};
          std::size_t max_file_size_{0};
          int max_files_{0};

          std::size_t max_queue_size_{0};
          QueuePolicy queue_policy_{QueuePolicy::DROP};

          std::thread worker_;
          std::mutex mutex_;
          std::condition_variable cv_;

          std::queue<logMessage> queue_;
          std::atomic<bool> running_{false};
          bool initialized_{false};
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
