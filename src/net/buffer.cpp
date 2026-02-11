#include "../include/net/buffer.h"

size_t Buffer::readable() const noexcept
{
     return buf_.size() - read_idx_;
}

const char *Buffer::read_ptr() const
{
     return buf_.data() + read_idx_;
}

void Buffer::append(const char *data, size_t len)
{
     buf_.insert(buf_.end(), data, data + len);
}

void Buffer::has_read(size_t n)
{
     read_idx_ += n;
     if (read_idx_ >= buf_.size())
     {
          buf_.clear();
          read_idx_ = 0;
     }
}
