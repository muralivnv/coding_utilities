#ifndef COMMON_FORMAT_H_
#define COMMON_FORMAT_H_

#include <string>
#include "printx.hpp"

namespace common {

namespace internal {
template <rostd::printx::literal Fmt, typename... Args>
std::tuple<std::string&, size_t> FormatToString(const Args&... args) {
  thread_local std::string buffer(512, ' ');
  constexpr int kMaxIteration = 3;
  int i = 0;

  while (i < kMaxIteration) {
    int n = rostd::sprintf<Fmt>(buffer, args...);
    if (n < 0) throw std::runtime_error("String formatting error");     
    if (static_cast<size_t>(n) < buffer.size()) {
      // add null termination at n
      buffer[n] = '\0';
      return {buffer, static_cast<size_t>(n)};
    }
    buffer.resize(n + 1);
    ++i;
  }
  throw std::runtime_error("String formatting error");  
}
} // namespace internal

template <rostd::printx::literal Fmt, typename... Args>
std::string_view FormatIntoStringView(const Args&... args) {
  auto [buffer, len] = internal::FormatToString<Fmt>(args...);
  return std::string_view(buffer.data(), len);
}

template <rostd::printx::literal Fmt, typename... Args>
const char* FormatIntoCString(const Args&... args) {
  auto [buffer, len] = internal::FormatToString<Fmt>(args...);
  return buffer.data();
}

} // namespace common

#endif // COMMON_FORMAT_H_
