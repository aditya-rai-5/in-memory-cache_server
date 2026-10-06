#include "../../include/protocol/command.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

static std::vector<std::string> tokenize(const std::string &s) {
  std::vector<std::string> tokens;
  std::istringstream ss(s);
  std::string tok;
  while (ss >> tok)
    tokens.push_back(std::move(tok));
  return tokens;
}

static bool iequal(const std::string &a, const std::string &b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
      return false;
  return true;
}

Command parse_command(const std::string &payload) {
  Command cmd;

  auto tokens = tokenize(payload);
  if (tokens.empty())
    return cmd;

  const std::string &name = tokens[0];

  cmd.args.assign(tokens.begin() + 1, tokens.end());

  if (iequal(name, "PING"))
    cmd.type = CommandType::PING;
  else if (iequal(name, "GET"))
    cmd.type = CommandType::GET;
  else if (iequal(name, "SET"))
    cmd.type = CommandType::SET;
  else if (iequal(name, "DEL"))
    cmd.type = CommandType::DEL;
  else if (iequal(name, "EXISTS"))
    cmd.type = CommandType::EXISTS;
  else if (iequal(name, "TTL"))
    cmd.type = CommandType::TTL;
  else
    cmd.type = CommandType::UNKNOWN;

  return cmd;
}
