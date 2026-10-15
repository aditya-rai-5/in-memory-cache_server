#pragma once
#include "acceptor.h"
#include "connection.h"

#include <memory>
#include <unordered_map>

class EventLoop
{
public:
     explicit EventLoop(Acceptor &acceptor);
     ~EventLoop();
     EventLoop(const EventLoop &) = delete;
     EventLoop &operator=(const EventLoop &) = delete;
     void run();

private:
     void accept_new();
     bool update_events(int fd, int events, int op);
     void remove_connection(int fd);

     Acceptor &acceptor_;
     std::unordered_map<int, std::unique_ptr<Connection>> conns_;
     int epoll_fd_ = -1;
};
