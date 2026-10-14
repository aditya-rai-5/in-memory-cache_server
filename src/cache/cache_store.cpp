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

void CacheStore::set(const std::string &key, const std::string &value) {
  std::lock_guard<std::mutex> lock(mutex_);
  store_[key] = Entry{value, std::nullopt};
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

  store_[key] = Entry{value, now + duration};
}

std::optional<std::string> CacheStore::get(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return std::nullopt;

  if (is_expired(it->second)) {
    store_.erase(it);
    return std::nullopt;
  }

  return it->second.value;
}

bool CacheStore::exists(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return false;

  if (is_expired(it->second)) {
    store_.erase(it);
    return false;
  }

  return true;
}

int64_t CacheStore::ttl(const std::string &key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = store_.find(key);
  if (it == store_.end())
    return -2;

  if (is_expired(it->second)) {
    store_.erase(it);
    return -2;
  }

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
    store_.erase(it);
    return 0;
  }

  store_.erase(it);
  return 1;
}

void CacheStore::purge_expired() {
  std::lock_guard<std::mutex> lock(mutex_);

  for (auto it = store_.begin(); it != store_.end();) {
    if (is_expired(it->second))
      it = store_.erase(it);
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