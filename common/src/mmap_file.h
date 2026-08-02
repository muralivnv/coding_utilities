#ifndef _MMAP_FILE_H_
#define _MMAP_FILE_H_

#include <string>
#include <optional>

namespace common {

class MmapFileReadOnly {
 public:
  MmapFileReadOnly(const MmapFileReadOnly&) = delete;
  MmapFileReadOnly& operator=(const MmapFileReadOnly&) = delete;
  MmapFileReadOnly(MmapFileReadOnly&& other) noexcept;
  MmapFileReadOnly& operator=(MmapFileReadOnly&& other) noexcept;
  ~MmapFileReadOnly();

  static std::optional<MmapFileReadOnly> Open(const std::string& filepath);
  const char* begin() const;
  const char* end() const;
  const char* data() const;
  size_t size() const;

 private:
  MmapFileReadOnly(char* ptr, size_t size) : mapped_ptr_{ptr}, size_{size} {}
  char* mapped_ptr_{nullptr};
  size_t size_{0};
};

}  // namespace common

#endif  // _MMAP_FILE_H_
