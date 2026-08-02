#include "mmap_file.h"

#include <utility>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace common {

MmapFileReadOnly::~MmapFileReadOnly() {
  if (mapped_ptr_ && (size_ > 0u) ) {
    munmap(mapped_ptr_, size_);
  }
}

MmapFileReadOnly::MmapFileReadOnly(MmapFileReadOnly&& other) noexcept
    : mapped_ptr_{std::exchange(other.mapped_ptr_, nullptr)},
      size_{std::exchange(other.size_, 0)} {}

MmapFileReadOnly& MmapFileReadOnly::operator=(MmapFileReadOnly&& other) noexcept {
  if (this != &other) {
    if (mapped_ptr_ && (size_ > 0)) {
      munmap(mapped_ptr_, size_);
    }
    mapped_ptr_ = std::exchange(other.mapped_ptr_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

std::optional<MmapFileReadOnly> MmapFileReadOnly::Open(const std::string& filepath) {
  int fd = open(filepath.c_str(), O_RDONLY);
  if (fd < 0) {
    return std::nullopt;
  }
  struct stat filestat;
  const int err = fstat(fd, &filestat);
  if (err < 0) {
    close(fd);
    return std::nullopt;
  }
  if (filestat.st_size == 0) {
    close(fd);
    return MmapFileReadOnly(nullptr, 0); 
  }
  char* ptr = static_cast<char*>(mmap(nullptr, filestat.st_size, PROT_READ, MAP_PRIVATE, fd, 0));
  if (ptr == MAP_FAILED) {
    close(fd);
    return std::nullopt;
  }
  close(fd);
  return MmapFileReadOnly(ptr, static_cast<size_t>(filestat.st_size));
}

const char* MmapFileReadOnly::begin() const {
  return mapped_ptr_;
}

const char* MmapFileReadOnly::end() const {
  return mapped_ptr_ + size_;
}

const char* MmapFileReadOnly::data() const {
  return mapped_ptr_;
}

size_t MmapFileReadOnly::size() const {
  return size_;
}

}  // namespace common
