#ifndef GAI_INPUT_H_
#define GAI_INPUT_H_

#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace gai {

class InputBase {
 public:
  virtual ~InputBase() = default;
  virtual std::optional<std::string_view> GetLine() = 0;
};

class InputStream : public InputBase {
 public:
  InputStream(bool read0 = false) : read0_{read0} {}
  ~InputStream() = default;

  std::optional<std::string_view> GetLine() override;

 private:
  static constexpr size_t kReadBufSize = 64 * 1024;

  // Allocated on first read and deliberately left uninitialised. Sizing and
  // zeroing this up front is measurable at startup.
  std::unique_ptr<char[]> buf_;
  std::string carry_;  // holds a line that straddles two reads
  size_t pos_{0};
  size_t len_{0};
  bool eof_{false};
  bool read0_{false};
};

class InputMemMappedFile : public InputBase {
 public:
  InputMemMappedFile() = delete;
  InputMemMappedFile(const char* begin, const char* end, bool read0 = false);
  ~InputMemMappedFile() override = default;

  std::optional<std::string_view> GetLine() override;

 private:
  const char* ptr_{nullptr};
  const char* end_{nullptr};
  bool read0_{false};
};

}  // namespace gai

#endif  // GAI_INPUT_H_
