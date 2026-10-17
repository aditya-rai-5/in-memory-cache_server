// Standalone smoke-test for CacheStore — compiled separately, not part of the
// server binary. Build: g++ -std=c++17 -I./include test_cache.cpp
// src/cache/cache_store.cpp -o test_cache && ./test_cache

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <limits>
#include <stdexcept>
#include <vector>

#include "cache/cache_store.h"

using namespace cache;

static int passed = 0;
static int failed = 0;

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (expr) {                                                                \
      std::cout << "  PASS  " << #expr << "\n";                                \
      ++passed;                                                                \
    } else {                                                                   \
      std::cerr << "  FAIL  " << #expr << "  (" << __FILE__ << ":" << __LINE__ \
                << ")\n";                                                      \
      ++failed;                                                                \
    }                                                                          \
  } while (0)

static void reset_lru_cache(std::size_t limit = 3) {
  auto &cs = CacheStore::instance();
  cs.set_max_keys(1);
  cs.set("reset", "value");
  cs.del("reset");
  cs.set_max_keys(limit);
}

static void test_lru_access_and_overwrite() {
  auto &cs = CacheStore::instance();
  for (int access = 0; access < 5; ++access) {
    reset_lru_cache();
    cs.set("a", "one");
    cs.set("b", "two");
    cs.set("c", "three");
    if (access == 0) CHECK(cs.get("a") == "one");
    if (access == 1) CHECK(cs.exists("a"));
    if (access == 2) CHECK(cs.ttl("a") == -1);
    if (access == 3) cs.set("a", "updated");
    if (access == 4) cs.set_ex("a", "updated", std::chrono::seconds(60));
    CHECK(cs.size() == 3);
    cs.set("d", "four");
    CHECK(cs.size() == 3);
    CHECK(!cs.get("b"));
    CHECK(cs.get("a").has_value());
    CHECK(cs.get("c") == "three");
    CHECK(cs.get("d") == "four");
  }
  reset_lru_cache(1);
  cs.set("a", "one");
  cs.set("a", "two");
  CHECK(cs.get("a") == "two");
  cs.set_ex("b", "three", std::chrono::seconds(60));
  CHECK(!cs.get("a"));
  CHECK(cs.get("b") == "three");
  CHECK(cs.size() == 1);
}

static void test_lru_rejected_writes_and_resize() {
  auto &cs = CacheStore::instance();
  reset_lru_cache(2);
  cs.set("a", "original");
  cs.set("b", "two");
  for (const auto ttl : {std::chrono::seconds(0), std::chrono::seconds(-1),
                         std::chrono::seconds::max()}) {
    for (const auto &key : {"a", "new"}) {
      bool rejected = false;
      try { cs.set_ex(key, "changed", ttl); }
      catch (const std::invalid_argument &) { rejected = true; }
      catch (const std::out_of_range &) { rejected = true; }
      CHECK(rejected);
      CHECK(cs.size() == 2);
    }
  }
  // Invalid writes must not make a recent or evict b.
  cs.set("c", "three");
  CHECK(!cs.get("a"));
  CHECK(cs.get("b") == "two");
  CHECK(cs.get("c") == "three");
  bool rejected = false;
  try { cs.set_max_keys(0); }
  catch (const std::invalid_argument &) { rejected = true; }
  CHECK(rejected);
  CHECK(cs.size() == 2);
  cs.set_max_keys(1);
  CHECK(!cs.get("b"));
  CHECK(cs.get("c") == "three");
  cs.set_max_keys(3);
  cs.set("d", "four");
  cs.set("e", "five");
  CHECK(cs.size() == 3);
}

static void test_lru_expiry_and_deletion() {
  auto &cs = CacheStore::instance();
  reset_lru_cache(6);
  for (const auto &key : {"get", "exists", "ttl", "del", "purge"})
    cs.set_ex(key, "expired", std::chrono::seconds(1));
  cs.set("live", "value");
  std::this_thread::sleep_for(std::chrono::seconds(2));
  CHECK(!cs.get("get"));
  CHECK(!cs.exists("exists"));
  CHECK(cs.ttl("ttl") == -2);
  CHECK(cs.del("del") == 0);
  cs.purge_expired();
  CHECK(cs.size() == 1);
  cs.set_max_keys(2);
  cs.set("new", "value");
  CHECK(cs.del("live") == 1);
  cs.set("live", "reborn");
  cs.set("last", "value");
  CHECK(!cs.get("new"));
  CHECK(cs.get("live") == "reborn");
  CHECK(cs.size() == 2);
}

static void test_lru_concurrent_access() {
  auto &cs = CacheStore::instance();
  reset_lru_cache(3);
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([&cs, worker]() {
      for (int i = 0; i < 1000; ++i) {
        const auto key = std::to_string(worker) + ":" + std::to_string(i % 10);
        cs.set(key, "value");
        cs.get(key);
        cs.exists(key);
        cs.ttl(key);
        if (i % 3 == 0) cs.del(key);
        if (i % 7 == 0) cs.purge_expired();
      }
    });
  }
  for (auto &worker : workers) worker.join();
  CHECK(cs.size() <= 3);
  reset_lru_cache(10000);
}

int main() {
  auto &cs = CacheStore::instance();

  std::cout << "\n=== SET / GET ===\n";
  cs.set("name", "aditya");
  CHECK(cs.get("name").value_or("") == "aditya");
  CHECK(cs.get("missing") == std::nullopt);

  std::cout << "\n=== EXISTS ===\n";
  CHECK(cs.exists("name") == true);
  CHECK(cs.exists("ghost") == false);

  std::cout << "\n=== DEL ===\n";
  CHECK(cs.del("name") == 1);
  CHECK(cs.del("name") == 0);
  CHECK(cs.get("name") == std::nullopt);

  std::cout << "\n=== TTL (no expiry) ===\n";
  cs.set("persistent", "yes");
  CHECK(cs.ttl("persistent") == -1);
  CHECK(cs.ttl("nokey") == -2);

  std::cout << "\n=== SET_EX / TTL ===\n";
  cs.set_ex("tmp", "value", std::chrono::seconds(2));
  CHECK(cs.get("tmp").value_or("") == "value");
  auto remaining = cs.ttl("tmp");
  CHECK(remaining >= 1 && remaining <= 2);

  std::cout << "\n=== Expiry (sleeping 3s...) ===\n";
  std::this_thread::sleep_for(std::chrono::seconds(3));
  CHECK(cs.get("tmp") == std::nullopt);
  CHECK(cs.exists("tmp") == false);
  CHECK(cs.ttl("tmp") == -2);

  std::cout << "\n=== OVERWRITE ===\n";
  cs.set("key", "v1");
  cs.set("key", "v2");
  CHECK(cs.get("key").value_or("") == "v2");

  std::cout << "\n=== SET_EX overwrite removes TTL ===\n";
  cs.set_ex("temp2", "x", std::chrono::seconds(5));
  cs.set("temp2", "x_no_ttl");
  CHECK(cs.ttl("temp2") == -1);

  std::cout << "\n=== SIZE ===\n";
  CHECK(cs.size() >= 3);

  std::cout << "\n=== PURGE_EXPIRED ===\n";
  cs.set_ex("dead1", "x", std::chrono::seconds(1));
  cs.set_ex("dead2", "y", std::chrono::seconds(1));
  std::this_thread::sleep_for(std::chrono::seconds(2));
  cs.purge_expired();
  CHECK(cs.get("dead1") == std::nullopt);
  CHECK(cs.get("dead2") == std::nullopt);

  std::cout << "\n=== LRU eviction ===\n";
  test_lru_access_and_overwrite();
  test_lru_rejected_writes_and_resize();
  test_lru_expiry_and_deletion();
  test_lru_concurrent_access();

  std::cout << "\n────────────────────────────────\n";
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
