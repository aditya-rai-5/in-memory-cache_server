#include "../include/net/protocol.h"

#include <arpa/inet.h>
#include <cstring>
#include <string>

ParseResult Protocol::try_parse(Buffer &buf,
                                std::string &out)
{
     if (buf.readable() < sizeof(uint32_t))
          return ParseResult::NeedMoreData;

     uint32_t len;
     std::memcpy(&len, buf.read_ptr(), sizeof(uint32_t));
     len = ntohl(len);

     if (len > kMaxFrameSize)
          return ParseResult::ProtocolError;

     if (buf.readable() < sizeof(uint32_t) + len)
          return ParseResult::NeedMoreData;

     buf.has_read(sizeof(uint32_t));

     out.assign(buf.read_ptr(), len);
     buf.has_read(len);

     return ParseResult::FrameReady;
}
