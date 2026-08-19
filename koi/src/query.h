#ifndef KOI_QUERY_H_
#define KOI_QUERY_H_

#include <tree_sitter/api.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "syntax.h"

namespace koi {

const TSLanguage* LoadGrammar(std::string_view language, std::string& error);

struct CompiledQuery;

std::shared_ptr<CompiledQuery> CompileQuery(std::string_view language,
                                            std::span<const std::string_view> files,
                                            std::string& error);

const TSLanguage* LanguageOf(const CompiledQuery& compiled);
TSQuery* QueryOf(const CompiledQuery& compiled);
std::span<const std::string> CaptureNamesOf(const CompiledQuery& compiled);

// One `#set!` on a pattern: `(#set! "scope" "header")`, or the capture-scoped
// `(#set! @indent "scope" "header")`. A key with no value at all is legal in the
// dialect and arrives with `value` empty.
//
// The capture the second shape names is recognised -- it has to be, or the
// capture step would be counted as the key -- and then dropped: every property
// koi stores applies to the whole pattern, and every consumer reads it that way
// (ScopeIsHeader, and syntax.cpp's LiteralLanguageOf). So a capture-scoped
// `#set!` is treated as if it had been written pattern-wide, which is wider
// than the dialect means it and is what the code has always done. Nothing koi
// ships uses the shape; giving it its own field and no reader was a promise the
// engine did not keep. A consumer that genuinely needs per-capture scope has to
// carry the id here *and* narrow the readers to match.
//
// Kept as an untyped key and value rather than as a scope enum: the vendored
// corpus sets nothing but `scope` and `injection.language` today, and what a key
// means belongs to the algorithm that reads it, not to the parser that finds it.
//
// Not a predicate, and deliberately kept out of the predicate list: a property
// says something *about* a pattern that matched, it does not decide whether it
// matched, so a pattern carrying nothing but a `#set!` must still match
// everything it names. tree-sitter's C API has no property call of its own --
// ts_query_property_settings_for_pattern is on the Rust side, and the header
// koi links against offers only ts_query_predicates_for_pattern -- so these
// come back as ordinary predicate steps and are picked out of them by name.
struct QueryProperty {
  std::string key;
  std::string value;
};

// The properties set on one pattern, in the order the query file wrote them,
// or empty for a pattern that set none and for an index no pattern has.
std::span<const QueryProperty> PropertiesFor(const CompiledQuery& compiled,
                                             std::uint32_t pattern_index);

// Whether pattern `pattern_index` carries `(#set! "scope" "header")`: the one
// property the shipped indent corpus sets, decided once per pattern at compile
// time rather than re-derived from PropertiesFor at every use.
//
// About the pattern and never about one capture on it, including for the
// capture-scoped spelling -- see QueryProperty. A pattern with two `@indent`
// captures and a `#set!` naming one of them makes both of them headers.
//
// Precomputed because both readers ask it on the indent hot path -- once per
// capture in syntax.cpp's Captures, to decide whether that capture is worth an
// O(depth) walk to its parent, and once per match in indent.cpp's fold -- and
// because two hand-rolled scans for the same two strings is exactly the drift
// that a single accessor rules out. The name of the property stays generic
// data in `properties`; only this one question, asked often enough to matter,
// gets an answer cached beside it.
bool ScopeIsHeader(const CompiledQuery& compiled, std::uint32_t pattern_index);

using NodeText = std::string_view (*)(const void* ctx, TSNode node, std::string& scratch);

bool PredicatesHold(const CompiledQuery& compiled, const TSQueryMatch& match, NodeText text,
                    const void* ctx);

// The whole file, or nothing and a reason. There is deliberately no overload
// that drops the error: every failure -- open, fstat, a non-regular file, a
// read that dies part-way -- returns an empty string, so a caller that does not
// look at `error` cannot tell a failure from an empty file. The call sites that
// genuinely want "not there reads as empty" say so themselves (editor.cpp's
// open checks fs::exists first; symbols.cpp skips ENOENT by name).
std::string ReadWholeFile(const std::filesystem::path& path, std::error_code& error);

struct Deadline {
  std::chrono::steady_clock::time_point until;
  const std::atomic<bool>* cancel{nullptr};
  bool expired{false};
  std::uint32_t steps{0};
};

bool StopAtDeadline(TSParseState* state);
bool StopQueryAtDeadline(TSQueryCursorState* state);

// How many in-progress matches one query cursor may carry. tree-sitter holds a
// capture list per match still in flight and, left at its UINT32_MAX default,
// allocates one for every partial match a pathological file starts -- an
// unbounded allocation driven by file contents, on work that is otherwise
// budgeted. With a limit set it recycles the capture list of the state that
// began earliest instead, and ts_query_cursor_did_exceed_match_limit says so
// afterwards, which is a bounded run that admits to being incomplete rather
// than an unbounded one that does not.
//
// 4096 against measurement, not taste: with the limit dialled down over this
// repository's own sources, and over six thousand third-party C, C++, Rust, Go
// and Python files -- nothing anywhere complains at a limit of 16, and dropping
// to 8 makes exactly one file do so. Real code peaks in the low tens of
// simultaneous states; the limit sits two and a half orders of magnitude above
// that, where only a file built to nest matches can reach it, and 4096 recycled
// capture lists is a bounded few hundred KB rather than an open tab.
inline constexpr std::uint32_t kMaxQueryMatchStates = 4096;

// The cursor byte range for a buffer of `bytes`, or false when there is none.
// Every byte offset tree-sitter deals in -- the query cursor's range, the
// parser's input callback, every node's start and end -- is a uint32, so a
// buffer past 4 GiB cannot be addressed at all, and the truncating cast that
// used to stand at these call sites turned it into work over an arbitrary
// prefix: silently, and over a prefix with nothing to do with what the caller
// asked about. `end` is written only when the whole buffer fits, so a caller
// that forgets to look at the answer cannot act on a wrapped range.
bool TreeSitterByteRange(std::size_t bytes, std::uint32_t& end);

std::string_view NodeTextIn(const void* ctx, TSNode node, std::string& scratch);

using TreePtr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

struct ParsedBuffer {
  TreePtr tree{nullptr, ts_tree_delete};

  std::string grammar_error;
  bool timed_out{false};

  explicit operator bool() const { return tree != nullptr; }
};

ParsedBuffer ParseBuffer(TSParser* parser, const CompiledQuery& compiled,
                         std::string_view language, std::string_view text,
                         std::chrono::milliseconds budget,
                         const std::atomic<bool>* cancel = nullptr);

}

#endif
