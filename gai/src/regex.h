#ifndef REGEX_H_
#define REGEX_H_

#include <pcre2.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace gai {

struct Pcre2Compiled {
  pcre2_code* p{nullptr};
  bool jitted{false};
  bool utf{false};  // FindSpans needs it to step over continuation bytes

  Pcre2Compiled() = delete;
  Pcre2Compiled(pcre2_code* p_, bool jitted_, bool utf_ = false);
  Pcre2Compiled(Pcre2Compiled&& other) noexcept : p(other.p), jitted(other.jitted), utf(other.utf) {
    other.p = nullptr;
    other.jitted = false;
    other.utf = false;
  }

  Pcre2Compiled& operator=(Pcre2Compiled&& other) noexcept {
    if (this != &other) {
      if (p)
        pcre2_code_free(p);
      p = other.p;
      jitted = other.jitted;
      utf = other.utf;
      other.p = nullptr;
      other.jitted = false;
      other.utf = false;
    }
    return *this;
  }
  Pcre2Compiled(const Pcre2Compiled&) = delete;
  Pcre2Compiled& operator=(const Pcre2Compiled&) = delete;
  ~Pcre2Compiled();
};

struct Pcre2Regex {
  Pcre2Compiled re;
  pcre2_match_data* match_data{nullptr};

  Pcre2Regex() = delete;
  Pcre2Regex(Pcre2Compiled&& re_, pcre2_match_data* m);

  Pcre2Regex(Pcre2Regex&& other) noexcept : re(std::move(other.re)), match_data(other.match_data) {
    other.match_data = nullptr;
  }

  Pcre2Regex& operator=(Pcre2Regex&& other) noexcept {
    if (this != &other) {
      re = std::move(other.re);
      if (match_data)
        pcre2_match_data_free(match_data);
      match_data = other.match_data;
      other.match_data = nullptr;
    }
    return *this;
  }
  Pcre2Regex(const Pcre2Regex&) = delete;
  Pcre2Regex& operator=(const Pcre2Regex&) = delete;
  ~Pcre2Regex();
};

struct Pcre2Substitution {
  Pcre2Compiled re;
  std::string substitute_pattern;
  bool global{false};  // the 'g' flag: replace every match, not just the first

  Pcre2Substitution() = delete;
  Pcre2Substitution(Pcre2Substitution&&) = default;
  Pcre2Substitution& operator=(Pcre2Substitution&&) = default;
  Pcre2Substitution(const Pcre2Substitution&) = delete;
  Pcre2Substitution& operator=(const Pcre2Substitution&) = delete;
  Pcre2Substitution(Pcre2Compiled&& re_, std::string_view sub_, bool global_ = false);
  ~Pcre2Substitution() = default;
};

Pcre2Compiled Compile(std::string_view pattern, bool jit_compile, bool enable_utf);
Pcre2Regex Regex(Pcre2Compiled&& pattern);

bool Find(const Pcre2Regex& search_pattern, std::string_view content);

// Half-open byte range [start, end) of one match within a line.
struct MatchSpan {
  uint32_t start;
  uint32_t end;
};

// Appends every match of search_pattern in content to spans and returns how many
// were added. Spans come out ordered and non-overlapping. Existing entries are
// kept, so several patterns can accumulate into one vector; call MergeSpans
// afterwards when more than one has contributed.
size_t FindSpans(const Pcre2Regex& search_pattern, std::string_view content,
                 std::vector<MatchSpan>& spans);

// Sorts spans and coalesces overlapping or touching ones, so that highlighting
// them emits no nested or redundant escape sequences.
void MergeSpans(std::vector<MatchSpan>& spans);

// Substitutes into scratch_buffer, growing it if the result does not fit, and
// returns a view of the result. The view points into scratch_buffer unless no
// match was found, in which case it is content itself.
std::string_view Substitute(const Pcre2Substitution& substitution, std::string_view content,
                            std::string& scratch_buffer);
}  // namespace gai

#endif  // REGEX_H_
