#include "common/config.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>


namespace config {
namespace {

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

int parse_number(const std::string &value, int maximum,
                 const std::string &context) {
  int result = 0;
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
      result < 1 || result > maximum) {
    throw std::runtime_error(context + "invalid value '" + value +
                             "'; expected a decimal integer in 1.." +
                             std::to_string(maximum));
  }
  return result;
}

logging::LogLevel parse_level(const std::string &value,
                              const std::string &context) {
  for (auto level : {logging::LogLevel::TRACE, logging::LogLevel::DEBUG,
                     logging::LogLevel::INFO, logging::LogLevel::WARN,
                     logging::LogLevel::ERROR, logging::LogLevel::FATAL}) {
    if (value == logging::to_string(level))
      return level;
  }
  throw std::runtime_error(
      context + "invalid value '" + value +
      "'; expected TRACE, DEBUG, INFO, WARN, ERROR, or FATAL");
}

} // namespace

Config &Config::instance() {
  static Config cfg;
  return cfg;
}

void Config::load(const std::string &path) {
  std::ifstream file(path);
  if (!file.is_open())
    throw std::runtime_error(path + ": cannot open configuration file");

  Settings candidate;
  std::unordered_set<std::string> seen;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    const auto location = path + ":" + std::to_string(line_number) + ": ";
    if (line.find('\0') != std::string::npos)
      throw std::runtime_error(location + "NUL byte in configuration");
    line = trim(std::move(line));
    if (line.empty() || line.front() == '#')
      continue;

    const auto separator = line.find('=');
    if (separator == std::string::npos)
      throw std::runtime_error(location + "expected key = value");
    const auto key = trim(line.substr(0, separator));
    const auto value = trim(line.substr(separator + 1));
    if (key.empty())
      throw std::runtime_error(location + "setting name must not be empty");
    const auto context = location + key + ": ";
    if (key == "server.workers")
      throw std::runtime_error(context +
                               "unsupported setting; remove it because "
                               "the server uses a single event-loop thread");
    if (key != "server.port" && key != "log.level" && key != "log.file" &&
        key != "log.max_size" && key != "log.max_files")
      throw std::runtime_error(context + "unknown setting");
    if (!seen.insert(key).second)
      throw std::runtime_error(context + "duplicate setting");
    if (value.empty())
      throw std::runtime_error(context + "value must not be empty");

    if (key == "server.port")
      candidate.port = parse_number(value, 65535, context);
    else if (key == "log.max_size")
      candidate.log_max_size =
          parse_number(value, std::numeric_limits<int>::max(), context);
    else if (key == "log.max_files")
      // Rotation checks each numbered backup; bound the work per rotation.
      candidate.log_max_files = parse_number(value, 1000, context);
    else if (key == "log.level")
      candidate.log_level = parse_level(value, context);
    else
      candidate.log_file = value;
  }
  if (!file.eof() || file.bad())
    throw std::runtime_error(path + ": error reading configuration file");

  settings_ = std::move(candidate);
}

}
