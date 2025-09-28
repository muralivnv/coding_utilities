#include <iostream>
#include "input.h"

namespace gai {

std::optional<std::string_view> InputStream::GetLine() {
  if (std::getline(std::cin, line_)) return std::string_view{line_};
  return std::nullopt;
}

InputMemMappedFile::InputMemMappedFile(const char* begin, const char* end) : ptr_{begin}, end_{end} {}

std::optional<std::string_view> InputMemMappedFile::GetLine() {
  if (ptr_ >= end_) return std::nullopt;

  const char* newline_ptr = static_cast<const char*>(
    std::memchr(ptr_, '\n', end_ - ptr_)
  );

  if (newline_ptr) {
    std::string_view line(ptr_, newline_ptr - ptr_);
    ptr_ = newline_ptr + 1; // advance past newline
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

} // namespace gai
