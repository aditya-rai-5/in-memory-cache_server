#include "../../include/cache/cache_store.h"

#include <stdexcept>

namespace cache {

CacheStore &CacheStore::instance() {
  static CacheStore inst;
  return inst;
}

bool CacheStore::is_expired(const Entry &e) noexcept {
  if (!e.expires_at.has_value())
    return false;
  return Clock::now() >= *e.expires_at;
}

void CacheStore::set_max_keys(std::size_t max_keys) {
  if (max_keys == 0)
    throw std::invalid_argument("max_keys must be positive");
  std::lock_guard<std::mutex> lock(mutex_);
  max_keys_ = max_keys;
  while (store_.size() > max_keys_)
    erase(store_.find(lru_.back()));
}

void CacheStore::touch(Entry &entry) {
  lru_.splice(lru_.begin(), lru_, entry.lru_position);
}

CacheStore::Store::iterator CacheStore::erase(Store::iterator it) {
  lru_.erase(it->second.lru_position);
  return store_.erase(it);
}

void CacheStore::write(const std::string &key, const std::string &value,
                       std::optional<TimePoint> expires_at) {
  auto it = store_.find(key);
  if (it != store_.end()) {
    it->second.value = value;
    it->second.expires_at = expires_at;
    touch(it->second);
    return;
  }

  lru_.push_front(key);
  try {
    store_.emplace(key, Entry{value, expires_at, lru_.begin()});
  } catch (...) {
    lru_.pop_front();
    throw;
  }
  // Commit the new entry before eviction so allocation failure preserves data.
  if (store_.size() > max_keys_)
    erase(store_.find(lru_.back()));
}

void CacheStore::set(const std::string &key, const std::string &value) {
  std::lock_guard<std::mutex> lock(mutex_);
  write(key, value, std::nullopt);
}

void CacheStore::set_ex(const std::string &key, const std::string &value,
                        std::chrono::seconds ttl) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (ttl <= std::chrono::seconds::zero())
    throw std::invalid_argument("expiry must be positive");

  const auto max_ttl =
      std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max());
  if (ttl > max_ttl)
    throw std::out_of_range("expiry duration is out of range");

  const auto duration = std::chrono::duration_cast<Clock::duration>(ttl);
  const auto now = Clock::now();
  // Subtract a validated duration from max instead of overflowing now + duration.
  if (now > TimePoint::max() - duration)
    throw std::out_of_range("expiry timestamp is out of range");

  write(key, value, now + duration);
}

std::optional<std::string> CacheStore::get(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return std::nullopt;

  if (is_expired(it->second)) {
    erase(it);
    return std::nullopt;
  }

  touch(it->second);
  return it->second.value;
}

bool CacheStore::exists(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return false;

  if (is_expired(it->second)) {
    erase(it);
    return false;
  }

  touch(it->second);
  return true;
}

int64_t CacheStore::ttl(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return -2;

  if (is_expired(it->second)) {
    erase(it);
    return -2;
  }

  touch(it->second);
  if (!it->second.expires_at.has_value())
    return -1;

  auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
      *it->second.expires_at - Clock::now());

  return remaining.count() > 0 ? remaining.count() : 0;
}

int CacheStore::del(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return 0;

  if (is_expired(it->second)) {
    erase(it);
    return 0;
  }

  erase(it);
  return 1;
}

void CacheStore::purge_expired() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto it = store_.begin(); it != store_.end();) {
    if (is_expired(it->second))
      it = erase(it);
    else
      ++it;
  }
}

std::size_t CacheStore::size() {
  std::lock_guard<std::mutex> lock(mutex_);

  std::size_t count = 0;
  for (auto &[k, v] : store_)
    if (!is_expired(v))
      ++count;

  return count;
}
}