#include <string>
#include <algorithm>
#include <stdexcept>

#include "operation.h"
#include "format.h"

namespace gai {

template<class... Ts>
struct Overloaded : Ts... { using Ts::operator()...; };

template<class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

std::string_view Trim(std::string_view v) {
  // remove leading whitespace
  size_t start = 0;
  while (start < v.size() && std::isspace(v[start])) ++start;
  v.remove_prefix(start);

  // remove trailing whitespace
  size_t end = v.size();
  while (end > 0 && std::isspace(v[end - 1])) --end;
  v.remove_suffix(v.size() - end);
  return v;
}

std::vector<std::string_view> Split(std::string_view expr) {
  std::vector<std::string_view> out{};
  expr = Trim(expr);
  if (expr.empty()) return out;

  const char delim = expr.front();
  size_t start = 1; // skip delimiter

  while (start < expr.size()) {
    const size_t end = expr.find(delim, start);
    if (end == std::string_view::npos) break;

    out.push_back(expr.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

bool Range::IsStartReached(std::string_view content, size_t linenum) {
  if (!is_start_reached_) {
    is_start_reached_ = std::visit(Overloaded{
                                   [](const std::monostate&) { return true; },
                                   [&linenum](size_t start_line) { return linenum == start_line; },
                                   [&content](const Pcre2Regex& regex) { return Find(regex, content); }
                                  }, start);
    if (is_start_reached_) {
     // do not reset 'end' if end represents line-numbers
      if (!std::holds_alternative<size_t>(end)) {
        is_end_reached_ = false;
      }
    }
  }
  return is_start_reached_;
}

bool Range::IsEndReached(std::string_view content, size_t linenum) {
  if (!is_end_reached_) {
    is_end_reached_ = std::visit(Overloaded{
                                   [](const std::monostate&) { return false; },
                                   [&linenum](size_t end_line) { return linenum == end_line; },
                                   [&content](const Pcre2Regex& regex) { return Find(regex, content); }
                                  }, end);
    if (is_end_reached_) {
      // only reset if we are in regex mode for 'start'
      if (std::holds_alternative<Pcre2Regex>(start)) {
        is_start_reached_ = false;
      }
    }
  }
  return is_end_reached_;
}

void Range::Reset() {
  is_start_reached_ = false;
  is_end_reached_ = false;
}

std::optional<Pcre2Substitution> ParseSub(std::string_view expr, bool jit, bool utf) {
  std::vector<std::string_view> parts = Split(expr);
  std::optional<Pcre2Substitution> out;
  if (parts.size() == 2) {
    out.emplace(Compile(parts[0], jit, utf), parts[1]);
  } else {
    std::string_view error_msg = common::FormatIntoStringView("Invalid substitute expression passed.\nExpression: %.*s\n",
                                                              static_cast<int>(expr.size()), expr.data());
    throw std::runtime_error(std::string(error_msg));
  }
  return out;
}

std::vector<Pcre2Regex> ParseFilters(const std::vector<std::string_view>& filters, bool jit, bool utf) {
  std::vector<Pcre2Regex> out{};
  for (const std::string_view& f : filters) {
    out.emplace_back(Regex(Compile(f, jit, utf)));
  }
  return out;
}

std::vector<Pcre2Substitution> ParseSubstitutions(const std::vector<std::string_view>& substitutions, bool jit, bool utf) {
  std::vector<Pcre2Substitution> out{};
  for (const std::string_view& sub : substitutions) {
    auto p = ParseSub(sub, jit, utf);
    if (p) {
      out.emplace_back(std::move(*p));
    }
  }
  return out;
}

std::optional<Range> ParseRange(std::string_view expr, bool jit, bool utf) {
  std::optional<Range> out{std::nullopt};
  std::vector<std::string_view> parts = Split(expr);
  if (parts.empty()) return out;

  auto parse_value = [jit, utf](const std::string_view s) -> RangeValue {
    if (s.empty()) return std::monostate{};
    if (std::all_of(s.begin(), s.end(), ::isdigit)) {
      return static_cast<size_t>(std::stoul(std::string{s}));
    }
    return Regex(Compile(s, jit, utf));
  };

  if (parts.size() == 2) {
    Range r;
    r.start = parse_value(parts[0]);
    r.end = parse_value(parts[1]);
    out = std::move(r);
  } else {
    std::string_view error_msg = common::FormatIntoStringView("Invalid range expression passed.\nExpression: %.*s\n",
                                                              static_cast<int>(expr.size()), expr.data());
    throw std::runtime_error(std::string(error_msg));
  }
  return out;
}

} // namespace gai
