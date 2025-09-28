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
  InputMemMappedFile() = delete;
  InputMemMappedFile(const char* begin, const char* end);
  ~InputMemMappedFile() override = default;

  std::optional<std::string_view> GetLine() override;
 private:
  const char* ptr_{nullptr};
  const char* end_{nullptr};
};

} // namespace gai

#endif // GAI_INPUT_H_
