#ifndef COMMON_FORMAT_H_
#define COMMON_FORMAT_H_

#include <string>

namespace common {
  
std::string_view FormatIntoStringView(const char* fmt, ...);

} // namespace common

#endif // COMMON_FORMAT_H_
