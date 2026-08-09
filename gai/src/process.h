#ifndef GAI_PROCESS_H_
#define GAI_PROCESS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "input.h"
#include "operation.h"
#include "regex.h"

namespace gai {

using OutputFunc = std::function<void(std::string_view, size_t)>;

// Which of the scan's regexes failed. The stage decides what the failure cost
// the user, and the wording says so: a filter that fails drops a line that
// might have matched, an exclude that fails keeps a line that might have been
// excluded, a range bound that fails does not latch, and a highlight that fails
// prints the line with some of its matches unpainted.
enum class MatchStage : std::uint8_t { kFilter, kExclude, kRangeStart, kRangeEnd, kHighlight };

// Collects the match-time failures a scan runs into -- the match limit, the
// depth limit, a JIT stack overflow -- and reports each one once.
//
// Once, because a match-time failure is a property of the pattern and the shape
// of the input, not of one line: a pattern that exhausts the match limit on
// line 3 exhausts it on every line that looks like line 3, and a per-line
// complaint would bury the scan's own output under thousands of identical
// copies. The first occurrence carries the file and line, which is the one a
// user can go and look at; the rest are counted.
class MatchDiagnostics {
 public:
  // Receives one fully formatted, newline-free diagnostic line. Defaulted to
  // writing to stderr -- grep's channel for these, so that `gai ... > out`
  // still shows them and the redirect still holds only matches.
  using Sink = std::function<void(std::string_view)>;

  MatchDiagnostics() = default;
  explicit MatchDiagnostics(Sink sink) : sink_(std::move(sink)) {}

  // `pattern` is the source text of the regex that failed, `message` the pcre2
  // wording. An empty `file` names standard input.
  void Report(MatchStage stage, std::string_view pattern, std::string_view file, size_t linenum,
              std::string_view message);

  // True when the scan hit at least one match-time failure, which is what the
  // process exit status is built from.
  bool Any() const { return occurrences_ > 0; }
  // How many distinct failures were printed, and how many lines ran into one.
  size_t reported() const { return reported_.size(); }
  size_t occurrences() const { return occurrences_; }

 private:
  Sink sink_{};
  std::vector<std::string> reported_;  // the (stage, pattern, message) triples already printed
  size_t occurrences_{0};
};

// What a scan needs that does not change from line to line. The regex vectors
// are borrowed and must outlive the Process call.
struct ScanConfig {
  const std::vector<Pcre2Regex>& filters;
  const std::vector<Pcre2Regex>& excludes;
  const std::vector<Pcre2Substitution>& replacements;
  bool color{false};
  std::string_view filename{};  // diagnostics only; empty means standard input
};

// Reads every line of `input`, applies the range, the filters, the excludes and
// the replacements, and hands what survives to `out_fn` with its line number.
// Match-time failures go to `diagnostics` and do not stop the scan: grep keeps
// reading the rest of the file after an error too, and a truncated result the
// user is told about beats a silent one.
void Process(const ScanConfig& config, const OutputFunc& out_fn, std::optional<Range>& range, InputBase* input,
             MatchDiagnostics& diagnostics);

// Builds the writer for one input: bare lines, or `line:` / `file:line:`
// prefixed ones when verbose, NUL-terminated with print0, SGR-coloured when
// asked.
OutputFunc MakeOutputFunc(bool verbose, std::string_view delimiter, bool print0 = false,
                          std::string_view filename = {}, bool color = false);

}  // namespace gai

#endif  // GAI_PROCESS_H_
