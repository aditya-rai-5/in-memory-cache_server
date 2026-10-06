// Standalone smoke-test for CacheStore — compiled separately, not part of the
// server binary. Build: g++ -std=c++17 -I./include test_cache.cpp
// src/cache/cache_store.cpp -o test_cache && ./test_cache

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

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

  std::cout << "\n────────────────────────────────\n";
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
