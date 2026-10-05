#include "../../include/protocol/executor.h"
#include "../../include/cache/cache_store.h"

#include <chrono>
#include <stdexcept>
#include <string>

static std::string err(const std::string &msg) { return "ERR " + msg; }

std::string execute_command(const Command &cmd) {
  auto &cs = cache::CacheStore::instance();

  switch (cmd.type) {
  case CommandType::PING:
    return "PONG";

  case CommandType::GET: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'GET' command");

    auto val = cs.get(cmd.args[0]);
    return val.has_value() ? *val : "(nil)";
  }

  case CommandType::SET: {
    if (cmd.args.size() < 2)
      return err("wrong number of arguments for 'SET' command");

    const std::string &key = cmd.args[0];
    const std::string &value = cmd.args[1];

    if (cmd.args.size() >= 4) {
      std::string opt = cmd.args[2];
      for (auto &c : opt)
        c = (char)std::toupper((unsigned char)c);

      if (opt == "EX") {
        long seconds = 0;
        try {
          seconds = std::stol(cmd.args[3]);
        } catch (...) {
          return err("value is not an integer or out of range");
        }

        if (seconds <= 0)
          return err("invalid expire time in 'SET' command");

        cs.set_ex(key, value, std::chrono::seconds(seconds));
        return "OK";
      }
      return err("syntax error");
    }

    cs.set(key, value);
    return "OK";
  }

  case CommandType::DEL: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'DEL' command");

    int deleted = cs.del(cmd.args[0]);
    return std::to_string(deleted); // "1" or "0"
  }

  case CommandType::EXISTS: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'EXISTS' command");

    return cs.exists(cmd.args[0]) ? "1" : "0";
  }

  case CommandType::TTL: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'TTL' command");

    return std::to_string(cs.ttl(cmd.args[0]));
  }

  default:
    return err("unknown command");
  }
}
