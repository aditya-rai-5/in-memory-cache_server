#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace cache {

class CacheStore {
public:
  static CacheStore &instance();

  void set(const std::string &key, const std::string &value);

  void set_ex(const std::string &key, const std::string &value, std::chrono::seconds ttl);

  std::optional<std::string> get(const std::string &key);

  bool exists(const std::string &key);

  int64_t ttl(const std::string &key);

  int del(const std::string &key);

  void purge_expired();

  std::size_t size();

private:
  CacheStore() = default;
  ~CacheStore() = default;
  CacheStore(const CacheStore &) = delete;
  CacheStore &operator=(const CacheStore &) = delete;

  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;

  struct Entry {
    std::string value;
    std::optional<TimePoint> expires_at;
  };

  static bool is_expired(const Entry &e) noexcept;

  std::unordered_map<std::string, Entry> store_;
  mutable std::mutex mutex_;
};

}
