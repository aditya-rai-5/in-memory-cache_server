#include "include/common/config.h"
#include "include/common/logger.h"
#include "include/net/acceptor.h"
#include "include/net/event_loop.h"
#include "include/cache/cache_store.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <thread>

int main() {
  auto &cfg = config::Config::instance();
  try {
    cfg.load("server.conf");
  } catch (const std::exception &error) {
    std::cerr << "Configuration error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  try {
    const auto &settings = cfg.settings();
    const auto log_directory = std::filesystem::path(settings.log_file).parent_path();
    if (!log_directory.empty())
      std::filesystem::create_directories(log_directory);

    auto &logger = logging::Logger::instance();
    logger.init(settings.log_file, settings.log_level,
                settings.log_max_size, settings.log_max_files);

    Acceptor acceptor(settings.port);
    EventLoop loop(acceptor);
    LOG_INFO("Server starting on port " + std::to_string(settings.port));

    // Start active expiry only after all startup resources are ready.
    std::atomic<bool> stopping{false};
    std::thread evictor_thread([&stopping]() {
      LOG_INFO("Active expiry background thread started");
      auto &cs = cache::CacheStore::instance();
      while (!stopping) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!stopping)
          cs.purge_expired();
      }
    });
    try {
      loop.run();
    } catch (...) {
      // Join before static cache/logger destruction on a runtime failure.
      stopping = true;
      evictor_thread.join();
      throw;
    }
    stopping = true;
    evictor_thread.join();
  } catch (const std::exception &error) {
    std::cerr << "Server error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
