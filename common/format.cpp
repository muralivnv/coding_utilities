#include "format.h"
#include <string>
#include <string_view>
#include <stdexcept>
#include <cstdarg>
#include <cstdio>

namespace common {
std::string_view FormatIntoStringView(const char* fmt, ...) {
  thread_local std::string buffer(512, ' ');

  while (true) {
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(const_cast<char*>(buffer.data()),
                           buffer.size(), fmt, args);
    va_end(args);
    if (n < 0) throw std::runtime_error("String formatting error");
    if (static_cast<size_t>(n) < buffer.size()) {
      return std::string_view(buffer.data(), n);
    }
    buffer.resize(n + 1);
  }
}

} // namespace common
