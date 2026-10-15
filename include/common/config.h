#pragma once

#include "log_level.h"

#include <cstddef>
#include <string>

namespace config {

struct Settings {
  int port = 6379;
  logging::LogLevel log_level = logging::LogLevel::DEBUG;
  std::string log_file = "logs/server.log";
  std::size_t log_max_size = 10 * 1024 * 1024;
  int log_max_files = 3;
};

class Config {
public:
  static Config &instance();

  void load(const std::string &path);
  const Settings &settings() const noexcept { return settings_; }

private:
  Config() = default;
  Config(const Config &) = delete;
  Config &operator=(const Config &) = delete;

  Settings settings_;
};

}
