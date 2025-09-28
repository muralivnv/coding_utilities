#ifndef GAI_INPUT_H_
#define GAI_INPUT_H_

#include <string_view>
#include <string>
#include <optional>
#include <mio/mmap.hpp>

namespace gai {

class InputBase {
 public:
  virtual ~InputBase() = default;
  virtual std::optional<std::string_view> GetLine() = 0;
};

class InputStream : public InputBase {
 public:
  InputStream() = default;
  ~InputStream() = default;

  std::optional<std::string_view> GetLine() override;
 private:
   std::string line_;
};

class InputMemMappedFile : public InputBase {
 public:
  InputMemMappedFile() = default;
  InputMemMappedFile(mio::mmap_source *mmap_file);
  ~InputMemMappedFile() override = default;

  std::optional<std::string_view> GetLine() override;
  void Reset(mio::mmap_source *mmap_file);
 private:
  mio::mmap_source* file_{nullptr};
  const char* ptr_{nullptr};
  const char* end_{nullptr};
};

} // namespace gai

#endif // GAI_INPUT_H_
