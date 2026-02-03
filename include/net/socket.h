#pragma once

#include <netinet/in.h>

int create_tcp_socket();
void set_reuseaddr(int fd);
void set_nonblocking(int fd);