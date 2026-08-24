#ifndef _MMAP_STREAM_H_
#define _MMAP_STREAM_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace common {

// Ceiling on how much one ReadAvailable call takes when the caller names no
// other, so an interactive one gets a turn back even when the producer is
// faster than we can consume.
inline constexpr size_t kMaxBytesPerCall{8u << 20};

// Outcome of one incremental read.
// `delta` is how far the mapping moved while growing: any pointer taken into the
// buffer before the call is off by exactly this much and must have it added back.
struct ReadProgress {
  std::ptrdiff_t delta{0};
  size_t new_bytes{0};
  bool eof{false};
  bool error{false};
};

struct MmapStream {
  char* buffer{nullptr};
  size_t size{0};
  size_t capacity{0};

  MmapStream() = default;
  MmapStream(const MmapStream&) = delete;
  MmapStream& operator=(const MmapStream&) = delete;
  MmapStream(MmapStream&& other) noexcept;
  MmapStream& operator=(MmapStream&& other) noexcept;
  ~MmapStream();

  // Appends what `fd` can supply, growing the mapping as needed. A blocking fd is
  // drained until EOF, a non-blocking one until it would block. Returns after
  // `max_bytes` so a fast producer cannot starve an interactive caller; a caller
  // that does work per byte read sets its own budget, one it can finish inside a
  // wake. The mapping may be relocated -- see ReadProgress::delta.
  ReadProgress ReadAvailable(int fd, size_t max_bytes = kMaxBytesPerCall);
};

// Wraps a copy of `bytes` in a stream, for callers that already hold their data
// in memory and need to hand it to a child as stdin. Same mapping discipline as
// the read path, so the destructor frees it the same way.
std::optional<MmapStream> MmapStreamFromBytes(const char* data, size_t size);

// Reads every fd to EOF. Convenience wrapper over ReadAvailable for callers that
// have nothing else to do while waiting.
std::optional<MmapStream> ReadFdsToMmap(const std::vector<int>& fds);

}  // namespace common

#endif  // _MMAP_STREAM_H_
