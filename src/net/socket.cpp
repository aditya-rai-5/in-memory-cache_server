#include "../include/net/socket.h"

#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

int create_tcp_socket()
{
     int fd = ::socket(AF_INET, SOCK_STREAM, 0);
     if (fd < 0)
     {
          throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));
     }
     return fd;
}

void set_reuseaddr(int fd)
{
     int opt = 1;
     if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
     {
          throw std::runtime_error(
              std::string("setsockopt(SO_REUSEADDR) failed: ") + std::strerror(errno));
     }
}

void set_nonblocking(int fd)
{
     int flags = ::fcntl(fd, F_GETFL, 0);
     if (flags < 0)
          throw std::runtime_error("fcntl(F_GETFL) failed");

     if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
          throw std::runtime_error("fcntl(O_NONBLOCK) failed");
}
