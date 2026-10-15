#include "../include/net/connection.h"
#include "../include/protocol/command.h"
#include "../include/protocol/executor.h"
#include "../include/common/logger.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

Connection::Connection(int fd) : fd_(fd) {}

Connection::~Connection() { ::close(fd_); }

int Connection::fd() const noexcept { return fd_; }

bool Connection::handle_read() {
  if (read_closed_)
    return true;
  while (true) {
    char tmp[4096];
    ssize_t n = ::read(fd_, tmp, sizeof(tmp));

    if (n > 0) {
      read_buf_.append(tmp, n);
      if (!process_frames())
        return false;
    } else if (n == 0) {
      read_closed_ = true;
      break;
    } else {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (errno == EINTR)
        continue;
      return false;
    }
  }
  return true;
}

bool Connection::process_frames() {
  while (true) {
    std::string payload;
    auto res = protocol_.try_parse(read_buf_, payload);

    if (res == ParseResult::NeedMoreData)
      break;

    if (res == ParseResult::ProtocolError) {
      LOG_ERROR("Protocol error on fd " + std::to_string(fd_));
      return false;
    }
    Command cmd = parse_command(payload);
    std::string response = execute_command(cmd);

    uint32_t len = htonl(static_cast<uint32_t>(response.size()));
    write_buf_.append(reinterpret_cast<char *>(&len), sizeof(len));
    write_buf_.append(response.data(), response.size());
  }
  return true;
}

bool Connection::handle_write() {
  while (write_buf_.readable() > 0) {
    ssize_t n = ::send(fd_, write_buf_.read_ptr(), write_buf_.readable(), MSG_NOSIGNAL);
    if (n > 0) {
      write_buf_.has_read(n);
    } else if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      if (errno == EINTR)
        continue;
      return false;
    } else {
      return false;
    }
  }
  return true;
}

bool Connection::read_closed() const noexcept { return read_closed_; }

bool Connection::want_write() const noexcept {
  return write_buf_.readable() > 0;
}
