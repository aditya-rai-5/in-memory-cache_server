#pragma once
#include <vector>
#include <cstring>

class Buffer {
public:
    size_t readable() const noexcept;
    const char* read_ptr() const;

    void append(const char* data, size_t len);
    void has_read(size_t n);

private:
    std::vector<char> buf_;
    size_t read_idx_ = 0;
};
