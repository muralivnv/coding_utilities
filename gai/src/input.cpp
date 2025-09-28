#include "input.h"
#include <iostream>
#include <immintrin.h>  // AVX2

namespace gai {

std::optional<std::string_view> InputStream::GetLine() {
  if (std::getline(std::cin, line_)) return std::string_view{line_};
  return std::nullopt;
}

InputMemMappedFile::InputMemMappedFile(mio::mmap_source *mmap_file)
    : file_{mmap_file}, ptr_{mmap_file->data()}, end_{mmap_file->data() + mmap_file->size()} {}

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

void InputMemMappedFile::Reset(mio::mmap_source *mmap_file) {
  file_ = mmap_file;
  ptr_ = file_->data();
  end_ = ptr_ + file_->size();
}

} // namespace gai
