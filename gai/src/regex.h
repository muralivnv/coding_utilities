#ifndef REGEX_H_
#define REGEX_H_

#include <pcre2.h>

#include <string>
#include <string_view>

namespace gai {

struct Pcre2Compiled {
  pcre2_code* p{nullptr};
  bool jitted{false};

  Pcre2Compiled() = delete;
  Pcre2Compiled(pcre2_code* p_, bool jitted_);
  Pcre2Compiled(Pcre2Compiled&& other) noexcept : p(other.p), jitted(other.jitted) {
    other.p = nullptr;
    other.jitted = false;
  }

  Pcre2Compiled& operator=(Pcre2Compiled&& other) noexcept {
    if (this != &other) {
      if (p) pcre2_code_free(p);
      p = other.p;
      jitted = other.jitted;
      other.p = nullptr;
      other.jitted = false;
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

  Pcre2Regex(Pcre2Regex&& other) noexcept
      : re(std::move(other.re)),
        match_data(other.match_data) {
    other.match_data = nullptr;
  }

  Pcre2Regex& operator=(Pcre2Regex&& other) noexcept {
    if (this != &other) {
      re = std::move(other.re);
      if (match_data) pcre2_match_data_free(match_data);
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

  Pcre2Substitution() = delete;
  Pcre2Substitution(Pcre2Substitution&&) = default;
  Pcre2Substitution& operator=(Pcre2Substitution&&) = default;
  Pcre2Substitution(const Pcre2Substitution&) = delete;
  Pcre2Substitution& operator=(const Pcre2Substitution&) = delete;
  Pcre2Substitution(Pcre2Compiled&& re_, std::string_view sub_);
  ~Pcre2Substitution() = default;
};

Pcre2Compiled Compile(std::string_view pattern, bool jit_compile, bool enable_utf);
Pcre2Regex Regex(Pcre2Compiled&& pattern);

bool Find(const Pcre2Regex& search_pattern, std::string_view content);
std::string_view Substitute(const Pcre2Substitution& substitution, std::string_view content,
                            std::string& scratch_buffer);
}  // namespace gai

#endif  // REGEX_H_
