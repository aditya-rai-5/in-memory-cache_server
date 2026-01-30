#pragma once

namespace logging
{
     enum class LogLevel
     {
          TRACE = 0,
          DEBUG,
          INFO,
          WARN,
          ERROR,
          FATAL
     };

     inline const char *to_string(LogLevel level)
     {
          switch (level)
          {
          case LogLevel::TRACE:
               return "TRACE";
          case LogLevel::DEBUG:
               return "DEBUG";
          case LogLevel::INFO:
               return "INFO";
          case LogLevel::WARN:
               return "WARN";
          case LogLevel::ERROR:
               return "ERROR";
          case LogLevel::FATAL:
               return "FATAL";
          default:
               return "UNKNOWN";
          }
     }
}
