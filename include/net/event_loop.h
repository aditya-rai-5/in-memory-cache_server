#pragma once
#include "acceptor.h"
#include "connection.h"

#include <unordered_map>

class EventLoop
{
public:
     explicit EventLoop(Acceptor &acceptor);
     void run();

private:
     void accept_new();

     Acceptor &acceptor_;
     std::unordered_map<int, Connection *> conns_;
};
