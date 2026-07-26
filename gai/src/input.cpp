#include "input.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace gai {

std::optional<std::string_view> InputStream::GetLine() {
  const char delim = read0_ ? '\0' : '\n';
  bool straddled = false;
  carry_.clear();

  for (;;) {
    if (pos_ >= len_) {
      if (eof_)
        break;
      if (!buf_)
        buf_ = std::make_unique_for_overwrite<char[]>(kReadBufSize);
      ssize_t n = ::read(STDIN_FILENO, buf_.get(), kReadBufSize);
      while (n < 0 && errno == EINTR)
        n = ::read(STDIN_FILENO, buf_.get(), kReadBufSize);
      if (n <= 0) {
        eof_ = true;
        break;
      }
      pos_ = 0;
      len_ = static_cast<size_t>(n);
    }

    const char* base = &buf_[pos_];
    const size_t avail = len_ - pos_;
    const char* delim_ptr = static_cast<const char*>(std::memchr(base, delim, avail));
    if (delim_ptr) {
      const size_t length = static_cast<size_t>(delim_ptr - base);
      pos_ += length + 1;  // advance past the delimiter
      if (!straddled)
        return std::string_view{base, length};
      carry_.append(base, length);
      return std::string_view{carry_};
    }

    // No delimiter in what is left of the window: stash it and refill.
    carry_.append(base, avail);
    straddled = true;
    pos_ = len_;
  }

  // handle last line without a delimiter
  if (!carry_.empty())
    return std::string_view{carry_};
  return std::nullopt;
}

InputMemMappedFile::InputMemMappedFile(const char* begin, const char* end, bool read0)
    : ptr_{begin}, end_{end}, read0_{read0} {}

std::optional<std::string_view> InputMemMappedFile::GetLine() {
  if (ptr_ >= end_)
    return std::nullopt;

  const char* newline_ptr = static_cast<const char*>(std::memchr(ptr_, read0_ ? '\0' : '\n', end_ - ptr_));

  if (newline_ptr) {
    std::string_view line(ptr_, newline_ptr - ptr_);
    ptr_ = newline_ptr + 1;  // advance past newline
    return line;
  }

  // handle last line without newline
  if (ptr_ != end_) {
    std::string_view line(ptr_, end_ - ptr_);
    ptr_ = end_;
    return line;
  }
  return std::nullopt;
}

}  // namespace gai
