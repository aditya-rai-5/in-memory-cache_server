#include "../include/net/connection.h"

#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>

Connection::Connection(int fd) : fd_(fd) {}

Connection::~Connection()
{
     ::close(fd_);
}

int Connection::fd() const noexcept
{
     return fd_;
}

bool Connection::handle_read()
{
     while (true)
     {
          char tmp[4096];
          ssize_t n = ::read(fd_, tmp, sizeof(tmp));

          if (n > 0)
          {
               read_buf_.append(tmp, n);
               process_frames();
          }
          else if (n == 0)
          {
               return false;
          }
          else
          {
               if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
               if (errno == EINTR)
                    continue;
               return false;
          }
     }
     return true;
}

void Connection::process_frames()
{
     while (true)
     {
          std::string payload;
          auto res = protocol_.try_parse(read_buf_, payload);

          if (res == ParseResult::NeedMoreData)
               break;

          if (res == ParseResult::ProtocolError)
          {
               ::close(fd_);
               fd_ = -1;
               break;
          }

          uint32_t len = htonl(payload.size());
          write_buf_.append(reinterpret_cast<char *>(&len), 4);
          write_buf_.append(payload.data(), payload.size());
     }
}

bool Connection::handle_write()
{
     while (write_buf_.readable() > 0)
     {
          ssize_t n = ::write(fd_, write_buf_.read_ptr(),
                              write_buf_.readable());
          if (n > 0)
          {
               write_buf_.has_read(n);
          }
          else
          {
               if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
               if (errno == EINTR)
                    continue;
               return false;
          }
     }
     return true;
}

bool Connection::want_write() const noexcept
{
     return write_buf_.readable() > 0;
}
