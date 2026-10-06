#pragma once
#include "acceptor.h"
#include "connection.h"

#include <unordered_map>

class EventLoop
{
public:
     explicit EventLoop(Acceptor &acceptor);
     ~EventLoop();
     void run();

private:
     void accept_new();
     void update_events(int fd, int events, int op);

     Acceptor &acceptor_;
     std::unordered_map<int, Connection *> conns_;
     int epoll_fd_;
};
