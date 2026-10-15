#pragma once

#include <netinet/in.h>

class Acceptor
{
public:
     Acceptor(uint64_t port);
     ~Acceptor();
     Acceptor(const Acceptor &) = delete;
     Acceptor &operator=(const Acceptor &) = delete;
     int fd() const noexcept
     {
          return listen_fd_;
     }

private:
     int listen_fd_;
};
