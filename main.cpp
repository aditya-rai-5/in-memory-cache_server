#include "include/common/config.h"
#include "include/common/logger.h"
#include "include/net/acceptor.h"
#include "include/net/event_loop.h"
#include "include/cache/cache_store.h"

#include <filesystem>
#include <thread>
#include <chrono>

int main() {
  auto &cfg = config::Config::instance();
  cfg.load("server.conf");

  int port = cfg.get_int("server.port", 6379);
  std::string log_file = cfg.get_string("log.file", "logs/server.log");
  int max_size = cfg.get_int("log.max_size", 10 * 1024 * 1024);
  int max_files = cfg.get_int("log.max_files", 3);

  std::filesystem::create_directories(
      std::filesystem::path(log_file).parent_path());

  auto &logger = logging::Logger::instance();
  logger.init(log_file, logging::LogLevel::DEBUG,
              static_cast<std::size_t>(max_size), max_files);

    LOG_INFO("Server starting on port " + std::to_string(port));

  // ── Start Active Expiry (Background TTL Sweep) ──────────────────────
  std::thread evictor_thread([]() {
      LOG_INFO("Active expiry background thread started");
      auto &cs = cache::CacheStore::instance();
      while (true) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
          cs.purge_expired();
      }
  });
  evictor_thread.detach(); // Let it run independently

  Acceptor acceptor(port);
  EventLoop loop(acceptor);
  loop.run();
}
