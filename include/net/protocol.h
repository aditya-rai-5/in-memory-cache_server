#pragma once

#include <cstdint>
#include "buffer.h"
#include <string>

enum class ParseResult
{
     NeedMoreData,
     FrameReady,
     ProtocolError
};

class Protocol
{
public:
     ParseResult try_parse(Buffer &buf,
                           std::string &out_played);

private:
     static constexpr uint32_t kMaxFrameSize = 1024 * 1024;
};