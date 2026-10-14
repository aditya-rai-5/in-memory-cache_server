#include "../../include/protocol/executor.h"
#include "../../include/cache/cache_store.h"
#include "../../include/common/logger.h"

#include <charconv>
#include <cctype>
#include <chrono>
#include <stdexcept>
#include <string>

static std::string err(const std::string &msg) { 
  LOG_DEBUG("Command error: " + msg);
  return "ERR " + msg; 
}

std::string execute_command(const Command &cmd) {
  auto &cs = cache::CacheStore::instance();

  switch (cmd.type) {
  case CommandType::PING:
    if (!cmd.args.empty())
      return err("wrong number of arguments for 'PING' command");
    LOG_DEBUG("Executed PING");
    return "PONG";

  case CommandType::GET: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'GET' command");

    LOG_DEBUG("Executed GET " + cmd.args[0]);
    auto val = cs.get(cmd.args[0]);
    return val.has_value() ? *val : "(nil)";
  }

  case CommandType::SET: {
    if (cmd.args.size() != 2 && cmd.args.size() != 4)
      return err("wrong number of arguments for 'SET' command");

    const std::string &key = cmd.args[0];
    const std::string &value = cmd.args[1];

    LOG_DEBUG("Executed SET " + key);

    if (cmd.args.size() == 4) {
      std::string opt = cmd.args[2];
      for (auto &c : opt)
        c = (char)std::toupper((unsigned char)c);

      if (opt == "EX") {
        std::chrono::seconds::rep seconds = 0;
        const std::string &input = cmd.args[3];
        const char *end = input.data() + input.size();
        auto result = std::from_chars(input.data(), end, seconds);
        if (result.ec != std::errc{} || result.ptr != end)
          return err("value is not an integer or out of range");

        if (seconds <= 0)
          return err("invalid expire time in 'SET' command");

        try {
          cs.set_ex(key, value, std::chrono::seconds(seconds));
        } catch (const std::out_of_range &) {
          return err("value is not an integer or out of range");
        }
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

    LOG_DEBUG("Executed DEL " + cmd.args[0]);
    int deleted = cs.del(cmd.args[0]);
    return std::to_string(deleted); // "1" or "0"
  }

  case CommandType::EXISTS: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'EXISTS' command");

    LOG_DEBUG("Executed EXISTS " + cmd.args[0]);
    return cs.exists(cmd.args[0]) ? "1" : "0";
  }

  case CommandType::TTL: {
    if (cmd.args.size() != 1)
      return err("wrong number of arguments for 'TTL' command");

    LOG_DEBUG("Executed TTL " + cmd.args[0]);
    return std::to_string(cs.ttl(cmd.args[0]));
  }

  default:
    return err("unknown command");
  }
}
