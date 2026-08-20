#include "regex.h"

#include <algorithm>
#include <stdexcept>

#include "format.h"

namespace gai {
using namespace std::string_literals;

// pcre2's defaults are 10 million match steps and 10 million frames of depth,
// which is not a budget anyone waits out: a catastrophic pattern like
// `(x+x+)+$` spends the whole 10 million on a single 30-character subject, and
// that costs ~35 ms -- as a constant, not as a failure. koi runs the pattern
// once per document line and re-runs the whole document on every prompt
// keystroke, so 10M/subject is minutes of frozen, unkillable editor.
//
// 100k steps caps one subject at well under a millisecond and is still far
// more than an honest search over a line of text ever asks for: a literal or a
// `\w+` walks the subject roughly once. Hitting the cap is a match-time error,
// which since the errors were classified travels out to the status line -- a
// runaway pattern now fails fast and says so instead of hanging.
constexpr uint32_t kMatchLimit = 100000;

// The depth limit caps how deeply the interpreter nests its backtracking
// frames, which is what bounds the frame memory one match can ask for. It is
// deliberately the same number as the match limit rather than something
// tighter: nesting depth is never more than the number of backtracks, so at
// this value the match limit is always the one that fires, and the two engines
// keep agreeing. A tighter depth is not free -- the JIT ignores the depth
// limit entirely, so a depth low enough to bite is a pattern that matches on a
// JIT box and reports an error on a no-JIT one. At 2000, `(word ?)+` over a
// three-thousand-word line and `(ab)+` over a twenty-thousand-token line -- an
// ordinary minified file -- did exactly that.
constexpr uint32_t kDepthLimit = kMatchLimit;

// Manages the per-thread match resources: the JIT stack, and the limits every
// match runs under. Both engines use it -- the interpreter is the leg that
// runs under TSan and wherever the JIT will not compile, and an unbounded
// interpreter is the same hang with a different stack.
struct MatchContext {
  pcre2_match_context* match_context{nullptr};
  pcre2_jit_stack* jit_stack{nullptr};

  MatchContext() {
    match_context = pcre2_match_context_create(nullptr);
    jit_stack = pcre2_jit_stack_create(32 * 1024, 512 * 1024, nullptr);
    if (match_context) {
      pcre2_jit_stack_assign(match_context, nullptr, jit_stack);
      // A pattern may still say (*LIMIT_MATCH=n) for itself, but pcre2 honours
      // it only when it is lower than the context's.
      pcre2_set_match_limit(match_context, kMatchLimit);
      pcre2_set_depth_limit(match_context, kDepthLimit);
    }
  }

  ~MatchContext() {
    if (match_context)
      pcre2_match_context_free(match_context);
    if (jit_stack)
      pcre2_jit_stack_free(jit_stack);
  }

  MatchContext(const MatchContext&) = delete;
  MatchContext& operator=(const MatchContext&) = delete;
};
thread_local MatchContext thread_local_match_context;

Pcre2Compiled::Pcre2Compiled(pcre2_code* p_, bool jitted_, bool utf_, std::string_view pattern_)
    : p{p_}, jitted{jitted_}, utf{utf_}, pattern{pattern_} {}

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
  if (enable_utf) {
    // PCRE2_MATCH_INVALID_UTF, not plain PCRE2_UTF: without it pcre2_match
    // validates the whole subject up front and refuses the line on the first
    // bad byte, while pcre2_jit_match does no such check and matches anyway.
    // Which of the two runs is decided by whether pcre2_jit_compile happened to
    // succeed, so the same pattern over the same bytes found different things
    // on different machines. The two paths have to agree, and agreeing on "scan
    // what is there" is the only answer that finds the ASCII word sitting next
    // to a latin-1 byte. Ill-formed stretches simply never match; the rest of
    // the subject is searched normally.
    compile_options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF;
  }

  Pcre2Compiled compiled{pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), compile_options,
                                       &errornumber, &erroroffset, nullptr),
                         false /* jitted */, enable_utf, pattern};
#if defined(__SANITIZE_THREAD__)
  // JIT-emitted match code is invisible to ThreadSanitizer's shadow memory
  // and returns wrong results under it; the interpreter is correct and only
  // slower, which a sanitizer run is anyway.
  jit_compile = false;
#endif
  if (compiled.p && jit_compile) {
    // A JIT that will not compile is not an error worth failing the search
    // over. Find and FindSpans both branch on `jitted` and call pcre2_match
    // when it is false, so the interpreter is a supported path, not a
    // fallback bolted on here -- it is only slower.
    if (pcre2_jit_compile(compiled.p, PCRE2_JIT_COMPLETE) == 0) {
      compiled.jitted = true;
    }
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

namespace {

// pcre2 says three different things through one int, and only one of them means
// "not here". PCRE2_ERROR_NOMATCH is -1 and every real failure -- the UTF
// validation codes, the match/depth/JIT-stack limits, out of memory -- is more
// negative still, so `retcode >= 0` as the whole answer silently turned each of
// them into an empty result.
enum class MatchOutcome : std::uint8_t { kMatch, kNoMatch, kError };

MatchOutcome MatchOnce(const Pcre2Regex& search_pattern, std::string_view content, PCRE2_SIZE offset,
                       uint32_t options, int& retcode) {
  const PCRE2_SPTR subject = reinterpret_cast<PCRE2_SPTR>(content.data());
  retcode = search_pattern.re.jitted
                ? pcre2_jit_match(search_pattern.re.p, subject, content.size(), offset, options,
                                  search_pattern.match_data, thread_local_match_context.match_context)
                : pcre2_match(search_pattern.re.p, subject, content.size(), offset, options,
                              search_pattern.match_data, thread_local_match_context.match_context);

  // Zero is a match too: it only means the ovector was too small for every
  // capture, and the whole-match pair at [0] and [1] is always there.
  if (retcode >= 0)
    return MatchOutcome::kMatch;
  // PCRE2_ERROR_PARTIAL cannot arrive without PCRE2_PARTIAL_*, but it is a
  // report about the subject rather than a failure, so it belongs with no-match.
  if ((retcode == PCRE2_ERROR_NOMATCH) || (retcode == PCRE2_ERROR_PARTIAL))
    return MatchOutcome::kNoMatch;
  return MatchOutcome::kError;
}

void DescribeError(int retcode, std::string* error) {
  if (!error)
    return;
  std::string msg(256, '\0');
  const int len = pcre2_get_error_message(retcode, reinterpret_cast<PCRE2_UCHAR*>(msg.data()), msg.size());
  if (len > 0) {
    msg.resize(static_cast<size_t>(len));
    *error = std::move(msg);
  } else {
    *error = "PCRE2 match failed with code " + std::to_string(retcode);
  }
}

// Which of pcre2_substitute's failures belong to the subject rather than to the
// replacement text. A limit was reached by this line and the next line may well
// be fine, so it is a match-time error like any other -- reported, and the line
// left alone. A replacement pcre2 will not parse, or a capture that does not
// exist, is wrong for every line the scan will ever see, so it stays fatal.
bool IsMatchTimeError(int retcode) {
  switch (retcode) {
    case PCRE2_ERROR_MATCHLIMIT:
    case PCRE2_ERROR_DEPTHLIMIT:
    case PCRE2_ERROR_HEAPLIMIT:
    case PCRE2_ERROR_JIT_STACKLIMIT:
    case PCRE2_ERROR_RECURSELOOP:
      return true;
    default:
      return false;
  }
}

}  // namespace

bool Find(const Pcre2Regex& search_pattern, std::string_view content, std::string* error) {
  if (!search_pattern.re.p)
    return false;

  int retcode{0};
  const MatchOutcome outcome = MatchOnce(search_pattern, content, 0, 0, retcode);
  if (outcome == MatchOutcome::kError)
    DescribeError(retcode, error);
  return outcome == MatchOutcome::kMatch;
}

size_t FindSpans(const Pcre2Regex& search_pattern, std::string_view content, std::vector<MatchSpan>& spans,
                 std::string* error) {
  if (!search_pattern.re.p)
    return 0;

  const size_t initial = spans.size();
  PCRE2_SIZE offset = 0;
  uint32_t options = 0;

  for (;;) {
    int retcode{0};
    const MatchOutcome outcome = MatchOnce(search_pattern, content, offset, options, retcode);

    if (outcome == MatchOutcome::kError) {
      // Whatever was found before the failure stays in `spans` and is counted;
      // `error` is how the caller learns the answer is a prefix, not the whole.
      DescribeError(retcode, error);
      break;
    }

    if (outcome == MatchOutcome::kNoMatch) {
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
                            std::string& scratch_buffer, std::string* error) {
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
    // The match context is the same one every Find runs under, and it is the
    // load-bearing argument here: a substitution matches per line exactly as a
    // filter does, and without a context pcre2 falls back to its ten-million
    // step default -- the per-line grind the Find side was already capped for.
    // The match data stays null on purpose: pcre2_substitute creates one sized
    // from the pattern when it is not given one, and a Pcre2Substitution has
    // nowhere to keep it.
    int rc = pcre2_substitute(
        substitution.re.p, reinterpret_cast<PCRE2_SPTR>(content.data()), content.size(), 0, options, nullptr,
        thread_local_match_context.match_context,
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

    if (IsMatchTimeError(rc)) {
      // pcre2 abandons the whole call on a match-time failure, so there is no
      // half-substituted line to hand back and no way to know which of a global
      // substitution's matches were reached. The line goes out as it arrived --
      // the same fail-open a filter that cannot finish takes -- and `error` is
      // the only thing that says it was never really substituted.
      DescribeError(rc, error);
      return content;
    }

    std::string msg(256, '\0');
    pcre2_get_error_message(rc, reinterpret_cast<PCRE2_UCHAR*>(msg.data()), msg.size());
    const char* error_msg = common::FormatIntoCString<"Substitution failed.\nError: %s\n">(msg.c_str());
    throw std::runtime_error(error_msg);
  }
  return content;
}

}  // namespace gai
