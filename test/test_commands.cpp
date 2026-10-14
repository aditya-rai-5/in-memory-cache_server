#include "cache/cache_store.h"
#include "protocol/command.h"
#include "protocol/executor.h"

#include <chrono>
#include <iostream>
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

static std::string run(const std::string &payload) {
  return execute_command(parse_command(payload));
}

int main() {
  auto &store = cache::CacheStore::instance();
  const std::string integer_error = "ERR value is not an integer or out of range";
  const std::string expiry_error = "ERR invalid expire time in 'SET' command";

  check("PING", run("PING") == "PONG");
  check("case-insensitive PING with whitespace", run(" \tpiNg\n") == "PONG");
  check("PING rejects arguments",
        run("PING ignored extra") == "ERR wrong number of arguments for 'PING' command");

  check("plain SET", run("SET syntax original") == "OK");
  check("case-insensitive EX", run("sEt syntax original eX 600") == "OK");
  check("valid GET", run("GET syntax") == "original");
  check("valid EXISTS", run("EXISTS syntax") == "1");
  const auto original_ttl = store.ttl("syntax");
  const auto validation_started = std::chrono::steady_clock::now();
  check("valid TTL", original_ttl >= 599 && original_ttl <= 600);

  struct InvalidCommand {
    std::string payload;
    std::string expected;
  };
  const std::string set_arity = "ERR wrong number of arguments for 'SET' command";
  const std::vector<InvalidCommand> invalid_commands = {
      {"SET", set_arity},
      {"SET syntax", set_arity},
      {"SET syntax changed EX", set_arity},
      {"SET syntax changed unexpected", set_arity},
      {"SET syntax changed EX 5 extra", set_arity},
      {"SET syntax changed EX 5 EX 6", set_arity},
      {"SET syntax changed PX 5", "ERR syntax error"},
      {"SET syntax changed EX 5abc", integer_error},
      {"SET syntax changed EX 5.0", integer_error},
      {"SET syntax changed EX 0x5", integer_error},
      {"SET syntax changed EX +5", integer_error},
      {"SET syntax changed EX notanumber", integer_error},
      {"SET syntax changed EX 9223372036854775808", integer_error},
      {"SET syntax changed EX -9223372036854775809", integer_error},
      {"SET syntax changed EX 9223372036854775807", integer_error},
      {"SET syntax changed EX 0", expiry_error},
      {"SET syntax changed EX -5", expiry_error},
      {"SET syntax changed EX -9223372036854775808", expiry_error},
      {"GET", "ERR wrong number of arguments for 'GET' command"},
      {"GET syntax extra", "ERR wrong number of arguments for 'GET' command"},
      {"DEL", "ERR wrong number of arguments for 'DEL' command"},
      {"DEL syntax extra", "ERR wrong number of arguments for 'DEL' command"},
      {"EXISTS", "ERR wrong number of arguments for 'EXISTS' command"},
      {"EXISTS syntax extra", "ERR wrong number of arguments for 'EXISTS' command"},
      {"TTL", "ERR wrong number of arguments for 'TTL' command"},
      {"TTL syntax extra", "ERR wrong number of arguments for 'TTL' command"},
      {"", "ERR unknown command"},
      {" \t\n", "ERR unknown command"},
      {"BLAH syntax", "ERR unknown command"},
  };
  for (const auto &test : invalid_commands) {
    check(test.payload + " rejected", run(test.payload) == test.expected);
    check(test.payload + " preserves value", store.get("syntax") == "original");
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - validation_started).count();
    const auto ttl = store.ttl("syntax");
    check(test.payload + " preserves expiry",
          ttl >= original_ttl - elapsed - 1 && ttl <= original_ttl);
  }

  check("rejected SET does not create a key",
        run("SET absent value EX 5abc") == integer_error && !store.exists("absent"));
  check("plain SET still removes expiry", run("SET syntax permanent") == "OK" &&
        store.get("syntax") == "permanent" && store.ttl("syntax") == -1);
  check("minimum positive expiry", run("SET minimum value EX 1") == "OK" &&
        store.get("minimum") == "value");

  check("leading zeros accepted", run("SET zeros value EX 0005") == "OK" &&
        store.get("zeros") == "value");

  // Exercise both overflow paths without waiting for any key to expire.
  using Clock = std::chrono::steady_clock;
  const auto max_duration_seconds =
      std::chrono::duration_cast<std::chrono::seconds>(Clock::duration::max()).count();
  check("clock duration overflow rejected",
        run("SET syntax changed EX " + std::to_string(max_duration_seconds + 1)) ==
            integer_error);
  check("clock timestamp overflow rejected",
        run("SET syntax changed EX " + std::to_string(max_duration_seconds)) ==
            integer_error);
  const auto safe_seconds = std::chrono::duration_cast<std::chrono::seconds>(
      Clock::time_point::max() - Clock::now()).count() - 60;
  check("large representable expiry accepted",
        run("SET large value EX " + std::to_string(safe_seconds)) == "OK" &&
        store.get("large") == "value" && store.ttl("large") > 0);

  for (auto ttl : {std::chrono::seconds(0), std::chrono::seconds(-1),
                   std::chrono::seconds::max(),
                   std::chrono::seconds(max_duration_seconds)}) {
    bool rejected = false;
    try {
      store.set_ex("syntax", "changed", ttl);
    } catch (const std::invalid_argument &) {
      rejected = ttl <= std::chrono::seconds::zero();
    } catch (const std::out_of_range &) {
      rejected = ttl > std::chrono::seconds::zero();
    }
    check("cache rejects unsafe expiry " + std::to_string(ttl.count()), rejected);
    check("unsafe expiry leaves cache unchanged",
          store.get("syntax") == "permanent" && store.ttl("syntax") == -1);
  }
  check("valid DEL", run("DEL syntax") == "1" && run("DEL syntax") == "0");

  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
  return failed == 0 ? 0 : 1;
}
