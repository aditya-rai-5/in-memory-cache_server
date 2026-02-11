#pragma once
#include "buffer.h"
#include "protocol.h"

class Connection
{
public:
     explicit Connection(int fd);
     ~Connection();

     int fd() const noexcept;

     bool handle_read();
     bool handle_write();

     bool want_write() const noexcept;

private:
     void process_frames();

     int fd_;
     Buffer read_buf_;
     Buffer write_buf_;
     Protocol protocol_;
};
