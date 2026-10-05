#pragma once

#include <string>
#include <vector>

// ── Supported commands ────────────────────────────────────────────────────────
enum class CommandType
{
    PING,
    GET,
    SET,    // SET key value  |  SET key value EX seconds
    DEL,
    EXISTS,
    TTL,
    UNKNOWN
};

// ── Parsed command ────────────────────────────────────────────────────────────
struct Command
{
    CommandType          type = CommandType::UNKNOWN;
    std::vector<std::string> args; // tokens AFTER the command name
                                   // e.g. SET foo bar EX 60  →  ["foo","bar","EX","60"]
};

// Parse a raw payload string into a Command.
// Never throws — returns UNKNOWN on any parse error.
Command parse_command(const std::string &payload);
