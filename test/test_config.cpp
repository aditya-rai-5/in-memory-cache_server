#include "common/config.h"
#include "common/logger.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

static int passed = 0;
static int failed = 0;

static void check(const std::string &label, bool condition) {
  if (condition) {
    ++passed;
    std::cout << "  PASS  " << label << '\n';
  } else {
    ++failed;
    std::cerr << "  FAIL  " << label << '\n';
  }
}

struct Workspace {
  std::filesystem::path directory;
  Workspace() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "cache-config-XXXXXX").string();
    auto *created = ::mkdtemp(pattern.data());
    if (!created)
      throw std::runtime_error("mkdtemp failed");
    directory = created;
  }
  ~Workspace() {
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::string path() const { return (directory / "server.conf").string(); }
  void write(const std::string &contents) const {
    std::ofstream file(path(), std::ios::binary);
    file << contents;
    if (!file)
      throw std::runtime_error("cannot write test configuration");
  }
};

static bool same_settings(const config::Settings &a, const config::Settings &b) {
  return a.port == b.port && a.cache_max_keys == b.cache_max_keys && a.log_level == b.log_level &&
         a.log_file == b.log_file && a.log_max_size == b.log_max_size &&
         a.log_max_files == b.log_max_files;
}

static void expect_error(const std::string &label, const std::string &path,
                         const std::vector<std::string> &fragments) {
  auto &cfg = config::Config::instance();
  const auto previous = cfg.settings();
  bool rejected = false;
  try {
    cfg.load(path);
  } catch (const std::runtime_error &error) {
    const std::string message = error.what();
    rejected = message.find(path) != std::string::npos;
    for (const auto &fragment : fragments)
      rejected = rejected && message.find(fragment) != std::string::npos;
    if (!rejected)
      std::cerr << "Unexpected diagnostic: " << message << '\n';
  }
  check(label + " reports the file and cause", rejected);
  check(label + " preserves all previous settings",
        same_settings(previous, cfg.settings()));
}

static void test_defaults_and_reload(const Workspace &workspace) {
  auto &cfg = config::Config::instance();
  workspace.write("");
  cfg.load(workspace.path());
  const auto &defaults = cfg.settings();
  check("empty file uses documented defaults",
        defaults.port == 6379 && defaults.cache_max_keys == 20000 && defaults.log_level == logging::LogLevel::DEBUG &&
        defaults.log_file == "logs/server.log" &&
        defaults.log_max_size == 10485760 && defaults.log_max_files == 3);

  workspace.write(" # comment\r\n\t\r\n server.port = 12345\r\n"
                  "log.level = WARN\r\nlog.file = custom logs/server=one.log\r\n"
                  "log.max_size = 2048\r\ncache.max_keys = 17\r\n log.max_files = 7");
  cfg.load(workspace.path());
  const auto &settings = cfg.settings();
  check("whitespace, CRLF, and final line without newline", settings.port == 12345);
  check("configured cache limit", settings.cache_max_keys == 17);
  check("configured log level", settings.log_level == logging::LogLevel::WARN);
  check("log filename may contain spaces and equals",
        settings.log_file == "custom logs/server=one.log");
  check("configured rotation settings",
        settings.log_max_size == 2048 && settings.log_max_files == 7);

  workspace.write("server.port=6380\n");
  cfg.load(workspace.path());
  auto expected = config::Settings{};
  expected.port = 6380;
  check("successful reload drops stale settings", same_settings(cfg.settings(), expected));
}

static void test_numeric_settings(const Workspace &workspace) {
  auto &cfg = config::Config::instance();
  struct NumericSetting {
    std::string key;
    std::size_t maximum;
  };
  for (const auto &setting : std::vector<NumericSetting>{
           {"server.port", 65535},
           {"log.max_size", std::numeric_limits<int>::max()},
           {"log.max_files", 1000},
           {"cache.max_keys", std::numeric_limits<std::size_t>::max()}}) {
    for (const auto &value : {std::string("1"), std::string("00042"),
                              std::to_string(setting.maximum)}) {
      workspace.write(setting.key + "=" + value + "\n");
      cfg.load(workspace.path());
      const auto &s = cfg.settings();
      const auto actual = setting.key == "server.port" ? s.port :
          setting.key == "log.max_files" ? s.log_max_files :
          setting.key == "cache.max_keys" ? s.cache_max_keys : s.log_max_size;
      check(setting.key + " accepts " + value,
            actual == static_cast<std::size_t>(std::stoull(value)));
    }
    const auto above_max = (setting.maximum == std::numeric_limits<std::size_t>::max()
        ? std::to_string(setting.maximum) + "0" : std::to_string(setting.maximum + 1));
    const std::string invalid_values[] = {
        "0", "-1", "+1", "-0", "1junk", "1.5", "0x10", "1e3", "1 2",
        "true", "999999999999999999999999999999", "-999999999999999999999999999999",
        above_max};
    for (const auto &value : invalid_values) {
      // Change a setting before the invalid line to exercise atomic loading.
      workspace.write("log.level=ERROR\n# test line numbers\n" +
                      setting.key + "=" + value + "\n");
      expect_error(setting.key + " rejects " + value, workspace.path(),
                   {":3: " + setting.key + ":", "'" + value + "'", "1.." +
                    std::to_string(setting.maximum)});
    }
    workspace.write(setting.key + "=\n");
    expect_error(setting.key + " rejects empty value", workspace.path(),
                 {":1: " + setting.key + ":", "value must not be empty"});
  }
}

static void test_syntax_and_io(const Workspace &workspace) {
  struct InvalidSetting {
    std::string contents;
    std::string reason;
  };
  for (const auto &test : std::vector<InvalidSetting>{
           {"server.port 6379", "expected key = value"},
           {"=6379", "setting name must not be empty"},
           {"log.file=  ", "log.file: value must not be empty"},
           {"server.potr=6379", "server.potr: unknown setting"},
           {"server.workers=4", "single event-loop thread"},
           {"log.level=verbose", "expected TRACE, DEBUG, INFO, WARN, ERROR, or FATAL"},
           {"log.level=info", "log.level: invalid value 'info'"},
           {"log.level=", "log.level: value must not be empty"},
           {std::string("log.file=bad\0path", 17), "NUL byte"},
           {"server.port=6379 # inline comment", "invalid value"}}) {
    workspace.write(test.contents);
    expect_error(test.reason, workspace.path(), {":1:", test.reason});
  }
  for (const auto &key : {"server.port", "log.level", "log.file",
                           "log.max_size", "log.max_files", "cache.max_keys"}) {
    workspace.write(std::string(key) + "=1\n" + key + "=2\n");
    if (std::string(key) == "log.level")
      workspace.write("log.level=INFO\nlog.level=ERROR\n");
    expect_error(std::string(key) + " duplicate", workspace.path(),
                 {":2: " + std::string(key) + ": duplicate setting"});
  }
  expect_error("missing file", (workspace.directory / "missing.conf").string(),
               {"cannot open configuration file"});
  expect_error("directory read failure", workspace.directory.string(),
               {"reading configuration file"});
}

static void test_log_levels(const Workspace &workspace) {
  auto &cfg = config::Config::instance();
  auto &logger = logging::Logger::instance();
  for (auto level : {logging::LogLevel::TRACE, logging::LogLevel::DEBUG,
                     logging::LogLevel::INFO, logging::LogLevel::WARN,
                     logging::LogLevel::ERROR, logging::LogLevel::FATAL}) {
    const std::string name = logging::to_string(level);
    workspace.write("log.level=" + name + "\n");
    cfg.load(workspace.path());
    const auto path = (workspace.directory / (name + ".log")).string();
    logger.init(path, cfg.settings().log_level);
    check("logger accepts configured " + name, logger.level() == level);
    // FATAL deliberately aborts, so test its threshold with lower priorities.
    for (auto message_level : {logging::LogLevel::TRACE, logging::LogLevel::DEBUG,
                               logging::LogLevel::INFO, logging::LogLevel::WARN,
                               logging::LogLevel::ERROR}) {
      logger.log(message_level, std::string("marker_") + logging::to_string(message_level),
                 __FILE__, __LINE__);
    }
    logger.shutdown();
    std::ifstream file(path);
    check(name + " creates the configured log file", file.is_open());
    const std::string contents{std::istreambuf_iterator<char>(file), {}};
    for (auto message_level : {logging::LogLevel::TRACE, logging::LogLevel::DEBUG,
                               logging::LogLevel::INFO, logging::LogLevel::WARN,
                               logging::LogLevel::ERROR}) {
      const std::string marker = std::string("marker_") + logging::to_string(message_level);
      check(name + " filters " + marker,
            (contents.find(marker) != std::string::npos) == (message_level >= level));
    }
  }
}

int main() {
  try {
    Workspace workspace;
    test_defaults_and_reload(workspace);
    test_numeric_settings(workspace);
    test_syntax_and_io(workspace);
    test_log_levels(workspace);
  } catch (const std::exception &error) {
    std::cerr << "Unexpected test error: " << error.what() << '\n';
    return 1;
  }
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
