#include "../include/net/accepter.h"
#include "../include/net/socket.h"

#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

Acceptor::Acceptor(uint64_t port)
{
     listen_fd_ = create_tcp_socket();
     set_reuseaddr(listen_fd_);
     set_nonblocking(listen_fd_);

     sockaddr_in addr{};
     addr.sin_family = AF_INET;
     addr.sin_addr.s_addr = htonl(INADDR_ANY);
     addr.sin_port = htons(port);

     if (::bind(listen_fd_, (sockaddr *)&addr, sizeof(addr)) < 0)
     {
          throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));
     }

     if (::listen(listen_fd_, SOMAXCONN) < 0)
          throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));
}
