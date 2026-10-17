#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace cache {

class CacheStore {
public:
  static CacheStore &instance();

  // A positive limit; lowering it immediately evicts least-recently-used keys.
  void set_max_keys(std::size_t max_keys);

  void set(const std::string &key, const std::string &value);

  void set_ex(const std::string &key, const std::string &value,
      std::chrono::seconds ttl);

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
    std::list<std::string>::iterator lru_position;
  };

  static bool is_expired(const Entry &e) noexcept;

  using Store = std::unordered_map<std::string, Entry>;

  // Helpers require mutex_. Front is most recently used; back is the victim.
  void write(const std::string &key, const std::string &value,
             std::optional<TimePoint> expires_at);
  void touch(Entry &entry);
  Store::iterator erase(Store::iterator it);

  std::size_t max_keys_ = 20000;
  std::list<std::string> lru_;
  Store store_;
  mutable std::mutex mutex_;
};

} // namespace cache
