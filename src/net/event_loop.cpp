#include "../include/net/event_loop.h"
#include "../include/common/logger.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/epoll.h>
#include <system_error>
#include <unistd.h>
#include <vector>

EventLoop::EventLoop(Acceptor &a) : acceptor_(a) {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0)
    throw std::system_error(errno, std::generic_category(), "epoll_create1() failed");

  if (!update_events(acceptor_.fd(), EPOLLIN, EPOLL_CTL_ADD)) {
    ::close(epoll_fd_);
    epoll_fd_ = -1;
    throw std::runtime_error("Failed to register listener with epoll");
  }
}

EventLoop::~EventLoop() { ::close(epoll_fd_); }

bool EventLoop::update_events(int fd, int events, int op) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_fd_, op, fd, &ev) < 0) {
    const int error = errno;
    LOG_ERROR("epoll_ctl failed on fd " + std::to_string(fd) +
              " op " + std::to_string(op) + ": " + std::strerror(error));
    return false;
  }
  return true;
}

void EventLoop::remove_connection(int fd) {
  LOG_INFO("Connection closed: fd " + std::to_string(fd));
  // Closing the owned socket also removes it from epoll if DEL fails.
  update_events(fd, 0, EPOLL_CTL_DEL);
  conns_.erase(fd);
}

void EventLoop::accept_new() {
  while (true) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int fd = ::accept4(acceptor_.fd(), (sockaddr *)&addr, &len,
                       SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return;
      LOG_ERROR("accept4 error: " + std::to_string(errno));
      return;
    }

    auto connection = std::make_unique<Connection>(fd);
    if (!update_events(fd, EPOLLIN | EPOLLRDHUP, EPOLL_CTL_ADD))
      continue;
    conns_.emplace(fd, std::move(connection));
    LOG_INFO("New connection accepted: fd " + std::to_string(fd));
  }
}

void EventLoop::run() {
  LOG_INFO("EventLoop started running with epoll");
  std::vector<epoll_event> events(1024);

  while (true) {
    int n = ::epoll_wait(epoll_fd_, events.data(), events.size(), -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      throw std::system_error(errno, std::generic_category(), "epoll_wait() failed");
    }

    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      uint32_t revents = events[i].events;

      if (fd == acceptor_.fd()) {
        if (revents & (EPOLLERR | EPOLLHUP))
          throw std::runtime_error("Listener received an epoll error or hangup");
        if (revents & EPOLLIN)
          accept_new();
        continue;
      }

      auto it = conns_.find(fd);
      if (it == conns_.end())
        continue;
      Connection &connection = *it->second;

      if (revents & (EPOLLERR | EPOLLHUP)) {
        LOG_ERROR("Socket error or hangup on fd " + std::to_string(fd));
        remove_connection(fd);
        continue;
      }

      bool ok = true;
      if (revents & (EPOLLIN | EPOLLRDHUP))
        ok = connection.handle_read();
      if (ok && (revents & EPOLLOUT))
        ok = connection.handle_write();

      if (!ok || (connection.read_closed() && !connection.want_write())) {
        remove_connection(fd);
        continue;
      }

      // After EOF, only wait for pending replies to drain. Keeping RDHUP
      // subscribed would repeatedly wake the loop while a slow reader blocks.
      int interest = connection.read_closed() ? 0 : EPOLLIN | EPOLLRDHUP;
      if (connection.want_write())
        interest |= EPOLLOUT;
      if (!update_events(fd, interest, EPOLL_CTL_MOD))
        remove_connection(fd);
    }
  }
}
