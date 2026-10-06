#include "../include/net/event_loop.h"
#include "../include/common/logger.h"

#include <cerrno>
#include <poll.h>
#include <string>
#include <unistd.h>
#include <vector>


EventLoop::EventLoop(Acceptor &a) : acceptor_(a) {}

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
    LOG_INFO("New connection accepted: fd " + std::to_string(fd));
  }
}

void EventLoop::run() {
  LOG_INFO("EventLoop started running");
  while (true) {
    std::vector<pollfd> fds;
    fds.push_back({acceptor_.fd(), POLLIN, 0});

    for (auto &[fd, c] : conns_) {
      short ev = POLLIN;
      if (c->want_write())
        ev |= POLLOUT;
      fds.push_back({fd, ev, 0});
    }

    int n = ::poll(fds.data(), fds.size(), -1);
    if (n <= 0)
      continue;

    for (auto &p : fds) {
      if (p.fd == acceptor_.fd() && (p.revents & POLLIN))
        accept_new();
      else if (p.revents) {
        auto it = conns_.find(p.fd);
        if (it == conns_.end())
          continue;

        Connection *c = it->second;
        bool ok = true;

        if (p.revents & POLLIN)
          ok = c->handle_read();
        if (ok && (p.revents & POLLOUT))
          ok = c->handle_write();

        if (!ok) {
          LOG_INFO("Connection closed: fd " + std::to_string(c->fd()));
          delete c;
          conns_.erase(it);
        }
      }
    }
  }
}
