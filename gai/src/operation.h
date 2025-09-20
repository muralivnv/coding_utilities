#ifndef OPERATION_H_
#define OPERATION_H_

#include "regex.h"
#include <optional>
#include <vector>
#include <variant>

namespace gai {

using RangeValue = std::variant<std::monostate, size_t, Pcre2Regex>;
struct Range {
  RangeValue start;
  RangeValue end;

  bool IsStartReached(std::string_view content, size_t linenum);
  bool IsEndReached(std::string_view content, size_t linenum);
  void Reset();

 private:
  bool is_start_reached_{false};
  bool is_end_reached_{false};
};

std::vector<Pcre2Regex> ParseFilters(const std::vector<std::string_view>& filters, bool jit, bool utf);
std::vector<Pcre2Substitution> ParseSubstitutions(const std::vector<std::string_view>& substitutions, bool jit, bool utf);
std::optional<Range> ParseRange(std::string_view expr, bool jit, bool utf);

std::string_view Trim(std::string_view v);
std::vector<std::string_view> Split(std::string_view expr);
std::optional<Pcre2Substitution> ParseSub(std::string_view expr, bool jit, bool utf);

} // namespace gai

#endif // OPERATION_H_
