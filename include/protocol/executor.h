#pragma once

#include "command.h"
#include <string>

// Execute a parsed Command against the CacheStore and return a response string.
// The response is always a plain UTF-8 string — the caller wraps it in the
// 4-byte length-prefix frame before sending.
std::string execute_command(const Command &cmd);
