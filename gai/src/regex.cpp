#include "regex.h"

#include <algorithm>
#include <stdexcept>

#include "format.h"

namespace gai {
using namespace std::string_literals;

// Manages JIT resources. An instance of this will be created per thread.
struct JITContext {
  pcre2_match_context* match_context{nullptr};
  pcre2_jit_stack* jit_stack{nullptr};

  JITContext() {
    match_context = pcre2_match_context_create(nullptr);
    jit_stack = pcre2_jit_stack_create(32 * 1024, 512 * 1024, nullptr);
    pcre2_jit_stack_assign(match_context, nullptr, jit_stack);
  }

  ~JITContext() {
    if (match_context)
      pcre2_match_context_free(match_context);
    if (jit_stack)
      pcre2_jit_stack_free(jit_stack);
  }

  JITContext(const JITContext&) = delete;
  JITContext& operator=(const JITContext&) = delete;
};
thread_local JITContext thread_local_jit_context;

Pcre2Compiled::Pcre2Compiled(pcre2_code* p_, bool jitted_, bool utf_) : p{p_}, jitted{jitted_}, utf{utf_} {}

Pcre2Compiled::~Pcre2Compiled() {
  if (p)
    pcre2_code_free(p);
}

Pcre2Regex::Pcre2Regex(Pcre2Compiled&& re_, pcre2_match_data* m) : re{std::move(re_)}, match_data{m} {}

Pcre2Regex::~Pcre2Regex() {
  if (match_data)
    pcre2_match_data_free(match_data);
}

Pcre2Substitution::Pcre2Substitution(Pcre2Compiled&& re_, std::string_view sub_, bool global_)
    : re{std::move(re_)}, substitute_pattern{sub_}, global{global_} {}

Pcre2Compiled Compile(std::string_view pattern, bool jit_compile, bool enable_utf) {
  int errornumber{0};
  PCRE2_SIZE erroroffset{0};

  uint32_t compile_options = 0;
  if (enable_utf)
    compile_options = PCRE2_UTF | PCRE2_UCP;  // enable UTF-8 and Unicode property support

  Pcre2Compiled compiled{pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), compile_options,
                                       &errornumber, &erroroffset, nullptr),
                         false /* jitted */, enable_utf};
  if (compiled.p && jit_compile) {
    int jit_errorcode = pcre2_jit_compile(compiled.p, PCRE2_JIT_COMPLETE);
    if (jit_errorcode != 0) {
      std::string error;
      switch (jit_errorcode) {
        case PCRE2_ERROR_JIT_BADOPTION:
          error = "PCRE2 JIT compilation failed -- 'BADOPTION'\n"s;
          break;
        case PCRE2_ERROR_NOMEMORY:
          error = "PCRE2 JIT compilation failed -- cannot allocate memory\n"s;
          break;
        case PCRE2_ERROR_JIT_UNSUPPORTED:
          error = "PCRE2 JIT no supported on pattern\n"s;
          break;
        default:
          break;
      }
      throw std::runtime_error(error);
    }
    compiled.jitted = true;
  }
  if (!compiled.p) {
    std::string msg(256, '.');
    pcre2_get_error_message(errornumber, reinterpret_cast<PCRE2_UCHAR*>(&msg[1]), msg.size() - 1);
    const char* error_msg =
        common::FormatIntoCString<"PCRE2 compilation failed on pattern.\nPattern: %s\nError Offset: %d\nError: %s\n">(
            pattern, erroroffset, msg);
    throw std::runtime_error(error_msg);
  }
  return compiled;
}

Pcre2Regex Regex(Pcre2Compiled&& pattern) {
  Pcre2Regex out(std::move(pattern), nullptr);
  out.match_data = pcre2_match_data_create_from_pattern(out.re.p, nullptr);
  return out;
}

bool Find(const Pcre2Regex& search_pattern, std::string_view content) {
  if (!search_pattern.re.p)
    return false;

  int retcode{0};
  if (!search_pattern.re.jitted) {
    retcode = pcre2_match(search_pattern.re.p, reinterpret_cast<PCRE2_SPTR>(content.data()), content.size(), 0, 0,
                          search_pattern.match_data, nullptr);
  } else {
    retcode = pcre2_jit_match(search_pattern.re.p, reinterpret_cast<PCRE2_SPTR>(content.data()), content.size(), 0, 0,
                              search_pattern.match_data, thread_local_jit_context.match_context);
  }
  return retcode >= 0;
}

size_t FindSpans(const Pcre2Regex& search_pattern, std::string_view content, std::vector<MatchSpan>& spans) {
  if (!search_pattern.re.p)
    return 0;

  const size_t initial = spans.size();
  const PCRE2_SPTR subject = reinterpret_cast<PCRE2_SPTR>(content.data());
  PCRE2_SIZE offset = 0;
  uint32_t options = 0;

  for (;;) {
    const int retcode =
        search_pattern.re.jitted
            ? pcre2_jit_match(search_pattern.re.p, subject, content.size(), offset, options,
                              search_pattern.match_data, thread_local_jit_context.match_context)
            : pcre2_match(search_pattern.re.p, subject, content.size(), offset, options,
                          search_pattern.match_data, nullptr);

    if (retcode < 0) {
      // With no options set this is a genuine end of matches. Otherwise it is the
      // retry after an empty match failing, which only means nothing else starts
      // here -- step over one character and carry on.
      if (options == 0 || offset >= content.size())
        break;
      ++offset;
      if (search_pattern.re.utf) {
        while (offset < content.size() && (static_cast<unsigned char>(content[offset]) & 0xC0) == 0x80)
          ++offset;
      }
      options = 0;
      continue;
    }

    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(search_pattern.match_data);
    spans.push_back({static_cast<uint32_t>(ovector[0]), static_cast<uint32_t>(ovector[1])});
    offset = ovector[1];

    // A zero-length match would be found again forever at the same offset, so the
    // next attempt is anchored there and forbidden from matching empty.
    options = (ovector[0] == ovector[1]) ? (PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED) : 0;
    if (offset > content.size())
      break;
  }
  return spans.size() - initial;
}

void MergeSpans(std::vector<MatchSpan>& spans) {
  if (spans.size() < 2)
    return;

  std::sort(spans.begin(), spans.end(), [](const MatchSpan& a, const MatchSpan& b) {
    return (a.start != b.start) ? (a.start < b.start) : (a.end < b.end);
  });

  // `<=` rather than `<` so that adjacent spans coalesce too: highlighting
  // "aeiou" as one run beats five back-to-back escape pairs.
  size_t write = 0;
  for (size_t read = 1; read < spans.size(); ++read) {
    if (spans[read].start <= spans[write].end) {
      spans[write].end = std::max(spans[write].end, spans[read].end);
    } else {
      spans[++write] = spans[read];
    }
  }
  spans.resize(write + 1);
}

std::string_view Substitute(const Pcre2Substitution& substitution, std::string_view content,
                            std::string& scratch_buffer) {
  if (!substitution.re.p) {
    return content;
  }

  // OVERFLOW_LENGTH is what makes out_length meaningful when the buffer is too
  // small; without it pcre2 leaves the value unset and there is nothing to resize
  // to. The required length it reports includes room for the terminating zero.
  uint32_t options = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
  if (substitution.global)
    options |= PCRE2_SUBSTITUTE_GLOBAL;

  // Two attempts at most: the first sizes the buffer, the second cannot overflow.
  for (int attempt = 0; attempt < 2; ++attempt) {
    PCRE2_SIZE out_length = scratch_buffer.size();
    int rc = pcre2_substitute(
        substitution.re.p, reinterpret_cast<PCRE2_SPTR>(content.data()), content.size(), 0, options, nullptr, nullptr,
        reinterpret_cast<PCRE2_SPTR>(substitution.substitute_pattern.data()), substitution.substitute_pattern.size(),
        reinterpret_cast<PCRE2_UCHAR*>(scratch_buffer.data()), &out_length);
    // no substitution performed
    if (rc == 0)
      return content;
    if (rc > 0)
      return {scratch_buffer.data(), out_length};

    if (rc == PCRE2_ERROR_NOMEMORY && attempt == 0) {
      scratch_buffer.resize(out_length);
      continue;
    }

    std::string msg(256, '\0');
    pcre2_get_error_message(rc, reinterpret_cast<PCRE2_UCHAR*>(msg.data()), msg.size());
    const char* error_msg = common::FormatIntoCString<"Substitution failed.\nError: %s\n">(msg.c_str());
    throw std::runtime_error(error_msg);
  }
  return content;
}

}  // namespace gai
