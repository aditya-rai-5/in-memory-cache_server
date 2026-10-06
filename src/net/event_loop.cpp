#include "../include/net/event_loop.h"
#include "../include/common/logger.h"

#include <cerrno>
#include <sys/epoll.h>
#include <string>
#include <unistd.h>
#include <vector>

EventLoop::EventLoop(Acceptor &a) : acceptor_(a) {
  epoll_fd_ = ::epoll_create1(0);
  if (epoll_fd_ < 0) {
    LOG_ERROR("epoll_create1 error: " + std::to_string(errno));
  }
  update_events(acceptor_.fd(), EPOLLIN, EPOLL_CTL_ADD);
}

EventLoop::~EventLoop() {
  ::close(epoll_fd_);
  for (auto &[fd, c] : conns_) {
    delete c;
  }
}

void EventLoop::update_events(int fd, int events, int op) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  ::epoll_ctl(epoll_fd_, op, fd, &ev);
}

void EventLoop::accept_new() {
  while (true) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);

    int fd = ::accept4(acceptor_.fd(), (sockaddr *)&addr, &len, SOCK_NONBLOCK);

    if (fd < 0) {
      if (errno == EAGAIN || errno == EINTR)
        break;
      LOG_ERROR("accept4 error: " + std::to_string(errno));
      return;
    }
    conns_[fd] = new Connection(fd);
    update_events(fd, EPOLLIN, EPOLL_CTL_ADD);
    LOG_INFO("New connection accepted: fd " + std::to_string(fd));
  }
}

void EventLoop::run() {
  LOG_INFO("EventLoop started running with epoll");
  std::vector<epoll_event> events(1024);

  while (true) {
    int n = ::epoll_wait(epoll_fd_, events.data(), events.size(), -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      LOG_ERROR("epoll_wait error: " + std::to_string(errno));
      break;
    }

    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      uint32_t revents = events[i].events;

      if (fd == acceptor_.fd()) {
        if (revents & EPOLLIN) {
          accept_new();
        }
      } else {
        auto it = conns_.find(fd);
        if (it == conns_.end())
          continue;

        Connection *c = it->second;
        bool ok = true;

        if (revents & EPOLLIN)
          ok = c->handle_read();
        
        if (ok && (revents & EPOLLOUT))
          ok = c->handle_write();

        if (ok) {
          int ev = EPOLLIN;
          if (c->want_write())
            ev |= EPOLLOUT;
          update_events(fd, ev, EPOLL_CTL_MOD);
        } else {
          LOG_INFO("Connection closed: fd " + std::to_string(c->fd()));
          update_events(fd, 0, EPOLL_CTL_DEL);
          delete c;
          conns_.erase(it);
        }
      }
    }
  }
}
