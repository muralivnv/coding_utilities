#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif  // _GNU_SOURCE

#include "mmap_stream.h"

#include "printx.hpp"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace common {

namespace {

// Ceiling on how much one ReadAvailable call will take, so an interactive caller
// gets a turn back even when the producer is faster than we can consume.
constexpr size_t kMaxBytesPerCall{8u << 20};

static size_t PageSize() {
  const long queried = sysconf(_SC_PAGESIZE);
  return (queried > 0) ? static_cast<size_t>(queried) : 4096;
}

}  // namespace

MmapStream::MmapStream(MmapStream&& other) noexcept : buffer(other.buffer), size(other.size), capacity(other.capacity) {
  other.buffer = nullptr;
  other.capacity = 0;
  other.size = 0;
}

MmapStream& MmapStream::operator=(MmapStream&& other) noexcept {
  if (this != &other) {
    if ((buffer != nullptr) && (capacity > 0)) {
      munmap(buffer, capacity);
    }
    buffer = other.buffer;
    size = other.size;
    capacity = other.capacity;
    other.buffer = nullptr;
    other.size = 0;
    other.capacity = 0;
  }
  return *this;
}

MmapStream::~MmapStream() {
  if (buffer && (capacity > 0)) {
    munmap(buffer, capacity);
  }
}

ReadProgress MmapStream::ReadAvailable(int fd) {
  ReadProgress progress;
  const size_t page_size = PageSize();

  if (buffer == nullptr) {
    void* mapped = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
      progress.error = true;
      return progress;
    }
    buffer = static_cast<char*>(mapped);
    capacity = page_size;
    size = 0;
  }

  while (progress.new_bytes < kMaxBytesPerCall) {
    size_t available_space = capacity - size;

    // Grow when full. One byte of headroom is always left unwritten so the data is
    // followed by a NUL -- see the shrink-to-fit note in ReadFdsToMmap.
    if (available_space <= 1u) {
      const size_t new_capacity = 2 * capacity;
      char* grown = static_cast<char*>(mremap(buffer, capacity, new_capacity, MREMAP_MAYMOVE));
      if (grown == MAP_FAILED) {
        // Keep what we already have; the caller decides whether a short read is fatal.
        progress.error = true;
        return progress;
      }

      // mremap preserves offsets, so relocating shifts every pointer into the
      // buffer by one constant. Report it rather than copying anything.
      progress.delta += grown - buffer;
      buffer = grown;
      capacity = new_capacity;
      available_space = capacity - size;
    }

    const size_t want = std::min(available_space - 1, kMaxBytesPerCall - progress.new_bytes);
    const ssize_t bytes_read = read(fd, buffer + size, want);

    if (bytes_read > 0) {
      size += static_cast<size_t>(bytes_read);
      progress.new_bytes += static_cast<size_t>(bytes_read);
    } else if (bytes_read == 0) {
      progress.eof = true;
      return progress;
    } else if (errno == EINTR) {
      continue;
    } else if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
      return progress;  // nothing more for now, but not the end
    } else {
      progress.error = true;
      return progress;
    }
  }

  return progress;
}

std::optional<MmapStream> ReadFdsToMmap(const std::vector<int>& fds) {
  MmapStream stream;

  for (int fd : fds) {
    while (true) {
      const ReadProgress progress = stream.ReadAvailable(fd);
      if (progress.error)
        return std::nullopt;
      if (progress.eof)
        break;
      if (progress.new_bytes == 0)
        break;  // non-blocking fd with nothing left to give
    }
  }

  // Shrink to fit, but always keep at least one unwritten byte past the data. The
  // tail of the mapping is never written, so it reads as NUL -- that is what stops
  // scanners which peek past the last byte (a truncated UTF-8 lead byte, say) from
  // walking off the end of the mapping. Note the 0 flags: this cannot relocate the
  // mapping, so it never invalidates pointers into it.
  if (stream.buffer != nullptr) {
    const size_t page_size = PageSize();
    const size_t aligned_size = ((stream.size + 1) + page_size - 1) & ~(page_size - 1);
    if (aligned_size < stream.capacity) {
      char* final_buffer = static_cast<char*>(mremap(stream.buffer, stream.capacity, aligned_size, 0));
      if (final_buffer != MAP_FAILED) {
        stream.buffer = final_buffer;
        stream.capacity = aligned_size;
      }
    }
  }

  return stream;
}

}  // namespace common
