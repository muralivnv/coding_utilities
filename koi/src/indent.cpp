#include "indent.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "query.h"
#include "unicode.h"

// The tree-sitter half of auto-indentation: helix's `indents.scm` dialect, run
// over koi's tree, adapted to the one-shot Captures() interface.
//
// The model is *scope containment*. Every node a query captures `@indent` opens
// a scope that spans the lines after the one it begins on, up to the one it ends
// on; the indent level of a line is how many such scopes contain it, collapsing
// scopes that open on the same physical line to one level, minus any `@outdent`
// whose token begins the line. `@align` replaces the level count with a column,
// `@extend` stretches a scope past where its node stops, `@opaque` says the line
// is literal content and must be left alone.
//
// Two places where this cannot be a transcription of helix's Rust, and why:
//
//   * There is no tree here, only spans. `Syntax::Captures` hands back
//     `{from, to}` per captured node and nothing else, and that is enough:
//     node spans nest, so "contains this byte" and "is an ancestor of the node
//     at this byte" are the same test, and the walk from a node to the root is
//     the set of captures whose span covers it. Nodes the query never captured
//     contribute nothing to the sum, so not seeing them costs nothing. Two
//     captures with the same span are read as one node, which is what a
//     `[(a) (b)] @indent` alternation over a node and its only child would
//     produce anyway.
//
//   * `@extend` is implemented as what it is documented to mean -- the node's
//     range reaches the line being indented -- and not as helix's repositioning
//     of the node the walk starts from. Under containment the start node no
//     longer selects the captures (containment does), so repositioning to a
//     *preceding* node cannot change any answer: a node that ends before the
//     cursor never contains the target line, and its ancestors are already in
//     the walk. Extending the scope's end is the same rule stated where the
//     containment test can read it, and it is what makes `def f():` and `foo:`
//     own the line under them. The chain that is walked, the two conditions that
//     decide an extension, and the way `@extend.prevent-once` cancels the first
//     of them are helix's, unchanged.

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kIndentQueryFile = "indents.scm";
constexpr std::array<std::string_view, 1> kIndentQueryFiles{kIndentQueryFile};

// How many nearby lines the hybrid heuristic will measure before giving up and
// using the tree's absolute answer. Helix's number, for helix's reason: each one
// is a whole query, and a file of continuation lines would otherwise walk to the
// top of the buffer looking for one to be relative to.
constexpr int kMaxBaselineAttempts = 4;

// How many lines that search may *walk* before giving up, comparable or not.
// Blank lines are skipped without a query, so the four attempts above bound the
// queries and bound nothing else: a caret under thirty thousand blank lines
// measured every one of them on the way to the top of the buffer, at two piece
// tree descents each, with the deadline nominally in force -- the walk runs no
// query, so nothing on it was ever checked against a deadline. It is now, every
// iteration; this cap is what stands behind the check, for the same reason the
// render guards below do. 512 because the answer past it is not worth having:
// the hybrid exists to inherit the indentation of a line the user can *see*
// near the one being typed, and half a thousand blank lines away is not that.
constexpr Index kMaxBaselineLines = 512;

// Rendering guards. None of these is reachable from code anyone writes -- they
// bound what a hostile or generated buffer can make one keystroke allocate.
constexpr Index kMaxIndentLevels = 64;
constexpr Index kMaxAlignBytes = 1024;
constexpr Index kMaxWhitespaceScan = 4096;

std::string IndentUnit(const IndentStyle& style) {
  if (!style.insert_spaces) return std::string{"\t"};
  return std::string(static_cast<std::size_t>(std::clamp<Index>(style.tab_width, 1, 16)), ' ');
}

Index TabWidthOf(const IndentStyle& style) { return std::max<Index>(1, style.tab_width); }

// `base` with `levels` indent units added, or -- for a negative count -- cut back
// to the visual column that many units shallower. The cut is forward from the
// start and not backwards from the end because a tab's width depends on where it
// begins, so the last character of the string is not the one whose width is
// known.
std::string AddIndentLevel(std::string base, Index levels, const IndentStyle& style) {
  const Index tab_width = TabWidthOf(style);
  if (levels >= 0) {
    const std::string unit = IndentUnit(style);
    for (Index i = 0; i < std::min(levels, kMaxIndentLevels); ++i) base += unit;
    return base;
  }
  const Index have = DisplayWidth(base, tab_width);
  const Index want = std::max<Index>(0, have + (levels * tab_width));
  Index column = 0;
  std::size_t cut = 0;
  while (cut < base.size()) {
    const Index next =
        (base[cut] == '\t') ? (((column / tab_width) + 1) * tab_width) : (column + 1);
    if (next > want) break;
    column = next;
    ++cut;
  }
  base.resize(cut);
  return base;
}

// The same visual width as `text`, spelled in tabs and spaces alone. An `@align`
// is stored as the document text left of its anchor, so that it survives a
// change of tab width; this is what turns that text into whitespace when the
// line is finally written.
std::string WhitespaceWithSameWidth(std::string_view text, Index tab_width) {
  std::string out;
  out.reserve(text.size());
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t next = NextGraphemeInString(text, at);
    if (next <= at) break;
    const std::string_view cluster = text.substr(at, next - at);
    if (cluster == "\t") {
      out += '\t';
    } else {
      out.append(static_cast<std::size_t>(std::max(0, GraphemeWidth(cluster))), ' ');
    }
    at = next;
  }
  return out;
}

// The line, as the three offsets every caller here wants: where it begins, where
// its text ends short of the terminator, and where its first non-blank byte is.
// `content == end` is a line that is blank or is nothing but whitespace.
struct LineText {
  Index start{0};
  Index end{0};
  Index content{0};
};

// LineRange and not LineContentRange, which is what this used to ask: a blank
// line's content range is empty, and an empty range has thrown away the one
// thing it was asked for -- where the line begins -- so the blank case paid a
// second descent to LineStart to get it back. It is the case that matters: the
// hybrid's search walks blank lines and nothing else without stopping, so the
// second descent was most of what a walk over them cost. LineRange keeps the
// terminator, which is why the strip below is here; it is empty only for a line
// out of range and for the trailing empty final line, where LineStart is asked
// once and no walk pays it repeatedly.
LineText MeasureLine(const PieceTable& table, Index line) {
  LineText out;
  const Interval full = LineRange(table, line);
  out.start = full.empty() ? LineStart(table, line) : full.front();
  out.end = full.empty() ? out.start : (full.back() + 1);
  char last = 0;
  if ((out.end > out.start) && ByteAt(table, out.end - 1, last) && (last == '\n')) {
    --out.end;
    if ((out.end > out.start) && ByteAt(table, out.end - 1, last) && (last == '\r')) --out.end;
  }
  out.content = out.start;
  char c = 0;
  while ((out.content < out.end) && ByteAt(table, out.content, c) && ((c == ' ') || (c == '\t'))) {
    ++out.content;
  }
  return out;
}

// How many indent units deep a line's leading whitespace is. Both `@extend`
// conditions are stated in these, so it has to measure the same way for a line
// of tabs as for a line of spaces.
Index IndentLevelForLine(const PieceTable& table, Index line, const IndentStyle& style) {
  const Index tab_width = TabWidthOf(style);
  const Index start = LineStart(table, line);
  Index column = 0;
  char c = 0;
  for (Index at = start; ((at - start) < kMaxWhitespaceScan) && ByteAt(table, at, c); ++at) {
    if (c == '\t') {
      column = ((column / tab_width) + 1) * tab_width;
    } else if (c == ' ') {
      ++column;
    } else {
      break;
    }
  }
  return column / tab_width;
}

// What a capture name means to this algorithm. Every other name is silently
// ignored, which is not laxity: the vendored corpus names helper captures --
// `@expr-start`, `@_body`, `@val`, `@key`, `@item`, `@in`, `@outer` -- purely so
// that a predicate has something to refer to, and a match carrying one is a
// perfectly good match whose helper capture defines no indent.
enum class Role : std::uint8_t {
  kIgnored,
  kIndent,
  kIndentAlways,
  kOutdent,
  kOutdentAlways,
  kAlign,
  kAnchor,
  kExtend,
  kPreventOnce,
  kOpaque,
};

Role RoleOf(std::string_view name) {
  if (name == "indent") return Role::kIndent;
  if (name == "indent.always") return Role::kIndentAlways;
  if (name == "outdent") return Role::kOutdent;
  if (name == "outdent.always") return Role::kOutdentAlways;
  if (name == "align") return Role::kAlign;
  if (name == "anchor") return Role::kAnchor;
  if (name == "extend") return Role::kExtend;
  if (name == "extend.prevent-once") return Role::kPreventOnce;
  if (name == "opaque") return Role::kOpaque;
  return Role::kIgnored;
}

// One captured node and everything the algorithm reads off it. Captures with the
// same span are folded together here -- see the file header on why a span stands
// in for a node.
//
// `from == to` is a real and load-bearing case here and nowhere else in koi. A
// query runs between two keystrokes, on a document nobody has finished writing:
// `if (c)` with the block's `}` already below it parses as a complete
// `if_statement` whose consequence is an `expression_statement` spanning no bytes
// at all, holding one token -- the `;` the parser supplied -- at the caret. That
// node is the body the newline opens, the header rule captures it, and every
// place below that reads a span has to survive it having no width: it opens its
// scope at its parent's line (`header`), it owns the line under it (`unterminated`
// is set, because a node whose only token was invented certainly ends in one),
// and it can never *close* a line, which is why the outdent roles are refused to
// it in Collect.
struct Scope {
  Index from{0};
  Index to{0};
  Index parent_from{0};
  bool unterminated{false};

  bool indent{false};
  bool indent_always{false};
  bool outdent{false};
  bool outdent_always{false};
  // `(#set! "scope" "header")`: open the scope at the parent's first line.
  bool header{false};

  bool has_align{false};
  Index anchor{0};

  // The `@extend` / `@extend.prevent-once` captures on this node, in the order
  // the query wrote them. Order matters: a node that carries both consumes its
  // own prevention rather than passing it up.
  std::vector<Role> extends;
};

struct Collected {
  std::vector<Scope> scopes;
  std::vector<std::pair<Index, Index>> opaque;
};

std::size_t ScopeFor(std::vector<Scope>& scopes, const Capture& capture) {
  for (std::size_t i = 0; i < scopes.size(); ++i) {
    if ((scopes[i].from == capture.from) && (scopes[i].to == capture.to)) return i;
  }
  // `parent_from` deliberately starts at the scope's own start rather than at
  // this capture's: the parent belongs to the capture that makes the scope a
  // header, and MarkHeader is the only thing that writes it.
  scopes.push_back(Scope{capture.from, capture.to, capture.from, capture.unterminated});
  return scopes.size() - 1;
}

// Marks a scope as opening at its parent's line, and takes the parent from the
// capture that said so.
//
// Not from whichever capture reached the span first, which is what ScopeFor
// hands over: one node's span is another's when the child fills its parent
// exactly (an `expression_statement` around a lone `call_expression`), the two
// have different parents, and only the header pattern's capture carries a
// parent worth reading -- Captures fills the field for nothing else. A
// non-header capture arriving first would otherwise fix the scope's
// `parent_from` at its own start and open the header a line late.
//
// First header capture wins over later ones, which is what a span reached only
// by header patterns did before and continues to do.
void MarkHeader(Scope& scope, const Capture& capture) {
  if (scope.header) return;
  scope.header = true;
  scope.parent_from = capture.parent_from;
}

// Folds one Captures() run into scopes. Captures arrive grouped by the match
// that produced them -- the id is handed out once per match and never reused
// within a cursor run -- which is what pairs an `@align` with the `@anchor` it
// is meaningless without.
void Collect(std::span<const Capture> captures, const CompiledQuery& compiled, Collected& out) {
  out.scopes.clear();
  out.opaque.clear();

  std::size_t at = 0;
  while (at < captures.size()) {
    std::size_t end = at;
    while ((end < captures.size()) && (captures[end].match_id == captures[at].match_id)) ++end;

    const Capture* anchor = nullptr;
    for (std::size_t i = at; (i < end) && (anchor == nullptr); ++i) {
      if (RoleOf(captures[i].name) == Role::kAnchor) anchor = &captures[i];
    }
    const bool header = ScopeIsHeader(compiled, captures[at].pattern_index);

    for (std::size_t i = at; i < end; ++i) {
      const Capture& capture = captures[i];
      const Role role = RoleOf(capture.name);
      if ((role == Role::kIgnored) || (role == Role::kAnchor)) continue;
      if (role == Role::kOpaque) {
        out.opaque.emplace_back(capture.from, capture.to);
        continue;
      }
      // An `@outdent` names the token that *begins* a line and pulls it back. A
      // span with no bytes in it is not a token anyone typed -- the only
      // zero-width tokens a grammar produces are the ones it invents to recover,
      // and a `}` that is not there dedents nothing. Refused here rather than
      // ignored at the counters, so that an invented closer cannot fold into the
      // `@indent` scope it shares a span with and cancel it.
      const bool empty = (capture.to <= capture.from);
      Scope& scope = out.scopes[ScopeFor(out.scopes, capture)];
      switch (role) {
        case Role::kIndent:
          scope.indent = true;
          if (header) MarkHeader(scope, capture);
          break;
        case Role::kIndentAlways:
          scope.indent_always = true;
          if (header) MarkHeader(scope, capture);
          break;
        case Role::kOutdent: scope.outdent = scope.outdent || !empty; break;
        case Role::kOutdentAlways: scope.outdent_always = scope.outdent_always || !empty; break;
        case Role::kAlign:
          // An `@align` with no `@anchor` in its match names no column and is
          // dropped, exactly as helix drops it: there is nothing to align to.
          if (anchor != nullptr) {
            scope.has_align = true;
            scope.anchor = anchor->from;
          }
          break;
        case Role::kExtend:
        case Role::kPreventOnce: scope.extends.push_back(role); break;
        default: break;
      }
    }
    at = end;
  }
}

// The indent of one line, before it is rendered. `align` is the document text
// left of the anchor rather than a column, so that a change of tab width does
// not move an aligned line -- and so that two lines can be compared for having
// the same alignment, which is what the hybrid heuristic needs.
struct Indentation {
  Index indent{0};
  Index indent_always{0};
  Index outdent{0};
  Index outdent_always{0};
  bool has_align{false};
  std::string align;
  // Whether a scope that *begins* the line asked about is what dedents it -- a
  // `}`, a `case`, an `else`. Recorded rather than derived, because an `@align`
  // clears the counters it would be read off: an aligned line is an absolute
  // column arrived at with the outdent already in it, and the question of
  // whether the line's own first token put it there still has the same answer.
  bool leading_outdent{false};

  Index Net() const { return (indent + indent_always) - (outdent + outdent_always); }
};

std::string Render(const Indentation& indent, const IndentStyle& style) {
  std::string base = indent.has_align ? WhitespaceWithSameWidth(indent.align, TabWidthOf(style))
                                      : std::string{};
  return AddIndentLevel(std::move(base), indent.Net(), style);
}

// What is being asked: the indent of `line`, at a position inside the first
// token on it -- or, when `new_line`, the indent of the line a newline typed at
// `byte_pos` would open, which is `line + 1` and does not exist yet.
struct Request {
  Index line{0};
  Index byte_pos{0};
  bool new_line{false};
};

// Line numbers in post-insertion coordinates: a node that begins at or after the
// caret will be on the line the newline opens, and one that ends past the caret
// will end a line lower than it does now. A node that ends *exactly* at the
// caret is the interesting case -- `foo();` ends there and owns nothing below,
// while the `compound_statement` of a `int main() {` whose `}` the parser
// invented at the caret ends there and owns everything below. `unterminated` is
// the difference; the rest of the algorithm cannot tell them apart.
Index StartLineOf(const PieceTable& table, Index from, const Request& request) {
  Index line = LineAt(table, from);
  if (request.new_line && (from >= request.byte_pos)) ++line;
  return line;
}

Index EndLineOf(const PieceTable& table, const Scope& scope, const Request& request) {
  Index line = LineAt(table, scope.to);
  if (!request.new_line) return line;
  if ((scope.to > request.byte_pos) ||
      ((scope.to == request.byte_pos) && scope.unterminated)) {
    ++line;
  }
  return line;
}

// One run of the engine over one document: the compiled query, the style, and
// the deadline every query inside it shares.
struct Engine {
  const PieceTable& table;
  Syntax& syntax;
  std::shared_ptr<CompiledQuery> compiled;
  IndentStyle style;
  std::chrono::steady_clock::time_point until;
  bool spent{false};

  std::vector<Capture> captures;
  Collected collected;
  // Where Captures writes when it fails, and no further: a failure here is a
  // parse the buffer never got -- gave up on its budget, or too large to have
  // one at all -- and the caller's answer to that is its own heuristic, not a
  // message. The one failure worth reporting, a query that will not compile,
  // cannot happen at this point: Prepare compiled the same files off the same
  // thread-local cache before this Engine existed. See Prepare.
  std::string error;
};

bool HasIndentQueryFile(std::string_view language) {
  return !FindRuntimeFile(fs::path{"queries"} / language / kIndentQueryFile).empty();
}

std::optional<Indentation> IndentAt(Engine& engine, const Request& request) {
  if (engine.spent) return std::nullopt;
  if (std::chrono::steady_clock::now() >= engine.until) {
    engine.spent = true;
    return std::nullopt;
  }

  const PieceTable& table = engine.table;
  const Index doc_len = DocLength(table);
  const Index pos = std::clamp<Index>(request.byte_pos, 0, doc_len);

  // The range to match over, and the reason it can be this small: a query
  // pattern is started by its *root* node, and every ancestor of the caret --
  // which is the whole chain this algorithm reads -- covers the last byte the
  // buffer holds before the caret. So one byte there and one at the caret reach
  // all of them, including the ones that end above the current line and only own
  // it through `@extend`. Whitespace is skipped on both sides so that the range
  // lands on real tokens rather than on the blank run between them.
  Index back = pos;
  char c = 0;
  while ((back > 0) && ((pos - back) < kMaxWhitespaceScan) && ByteAt(table, back - 1, c) &&
         ((c == ' ') || (c == '\t') || (c == '\n') || (c == '\r'))) {
    --back;
  }
  // Where the last token before the caret ends. The chain of nodes helix walks
  // from the deepest preceding node up to the one containing the caret all end
  // here, because that walk starts at a *last* descendant.
  const Index prev_end = back;
  const Index scan_from = (back > 0) ? (back - 1) : 0;

  Index ahead = pos;
  while ((ahead < doc_len) && ((ahead - pos) < kMaxWhitespaceScan) && ByteAt(table, ahead, c) &&
         ((c == ' ') || (c == '\t') || (c == '\n') || (c == '\r'))) {
    ++ahead;
  }
  const Index scan_to =
      std::max(scan_from, std::min(doc_len, std::max<Index>(pos + 1, ahead + 1)));

  bool exhausted = false;
  // Zero-width captures kept: see the note on `Scope`. The body of a brace-less
  // `if (c)` is a node with no bytes in it until the statement under it is typed,
  // and it is the whole reason the line being opened is one level in.
  //
  // Under this run's own deadline and not under a frame budget of its own: one
  // call here is up to five queries -- the answer plus the hybrid's baselines --
  // and a fresh 25 ms for each of them bounds the run at five times what the
  // caller handed over. See Syntax::Captures.
  if (!engine.syntax.Captures(table, kIndentQueryFiles, Interval(scan_from, scan_to),
                              engine.captures, engine.error, &exhausted, true, &engine.until)) {
    engine.spent = true;
    return std::nullopt;
  }
  if (exhausted) {
    engine.spent = true;
    return std::nullopt;
  }
  // Spent by a query that finished inside it. The check at the top of this
  // function is what refuses the *next* request; saying so here as well is what
  // keeps a caller that reads `spent` from having to guess, and it deliberately
  // does not touch the answer this call already has: the captures came back
  // whole -- `exhausted` above is the run that did not -- so the query was paid
  // for and declining it now would throw away work already done.
  if (std::chrono::steady_clock::now() >= engine.until) engine.spent = true;
  Collect(engine.captures, *engine.compiled, engine.collected);
  const std::vector<Scope>& scopes = engine.collected.scopes;

  // Inside an `@opaque` node that began on an earlier line -- a docstring, a raw
  // string, a heredoc -- the leading whitespace is content, not indentation.
  // Hand back exactly what is there.
  for (const auto& [from, to] : engine.collected.opaque) {
    if ((from <= pos) && (pos < to) && (LineAt(table, from) < LineAt(table, pos))) {
      const LineText line = MeasureLine(table, request.line);
      Indentation out;
      out.has_align = true;
      out.align = ReadDocRange(table, Interval(line.start, line.content));
      return out;
    }
  }

  // A newline typed after a header (`if (c)`, `while x:`) opens a body that is a
  // *following* sibling: past the caret, so no containment test can reach it.
  // The query says which siblings those are -- `(#set! "scope" "header")` is
  // written on exactly the brace-less body rules -- so descend into the nearest
  // one and let its own scope govern.
  const Scope* body = nullptr;
  if (request.new_line) {
    for (const Scope& scope : scopes) {
      if (!scope.header || !(scope.indent || scope.indent_always)) continue;
      if (scope.from < pos) continue;
      if ((body == nullptr) || (scope.from < body->from) ||
          ((scope.from == body->from) && (scope.to < body->to))) {
        body = &scope;
      }
    }
  }

  // `@extend`: the chain of captured nodes that end where the last token before
  // the caret ends, innermost first. One of them may own the line being indented
  // even though its node stops above it -- that is what keeps `if x:` and
  // `foo:` in charge of the lines under them -- and `@extend.prevent-once` on a
  // `return` or a `pass` is what ends that ownership.
  std::vector<const Scope*> extended;
  if (body == nullptr) {
    Index chain_end = -1;
    for (const Scope& scope : scopes) {
      if ((scope.to <= pos) && (scope.to >= prev_end)) chain_end = std::max(chain_end, scope.to);
    }
    if (chain_end >= 0) {
      std::vector<const Scope*> chain;
      for (const Scope& scope : scopes) {
        if (scope.to == chain_end) chain.push_back(&scope);
      }
      std::ranges::sort(chain, [](const Scope* a, const Scope* b) { return a->from > b->from; });

      bool stop_extend = false;
      for (const Scope* scope : chain) {
        bool node_captured = false;
        bool extend_node = false;
        for (const Role role : scope->extends) {
          if (role == Role::kPreventOnce) {
            stop_extend = true;
            continue;
          }
          node_captured = true;
          // Extended when the caret is on the line the node ends on, or when the
          // caret's line is indented deeper than the node's first line.
          if (LineAt(table, scope->to) == request.line) {
            extend_node = true;
          } else if (IndentLevelForLine(table, request.line, engine.style) >
                     IndentLevelForLine(table, LineAt(table, scope->from), engine.style)) {
            extend_node = true;
          }
        }
        if (node_captured && stop_extend) {
          stop_extend = false;
        } else if (extend_node && !stop_extend) {
          // Every node on the chain that qualifies, and not just the first.
          // Helix stops at one because one is all its repositioning can use --
          // the walk from there covers the rest anyway. Here an extension is a
          // property of the scope, and an outer scope that also reaches the line
          // is a level of its own: `def f():` around `if x:` around the line
          // being typed is two, and stopping at the `if` would dedent out of the
          // function.
          extended.push_back(scope);
        }
      }
    }
  }

  // The walk: every capture whose span covers the node this started from. For
  // the ordinary case that node is the caret itself; a descent or an extension
  // replaces it, exactly as they replace helix's start node.
  Index walk_from = pos;
  Index walk_to = pos;
  if (body != nullptr) {
    walk_from = body->from;
    walk_to = body->to;
  } else if (!extended.empty()) {
    // The innermost extension, which the chain visited first. Its ancestors --
    // every outer extension included -- are what the walk picks up from there.
    walk_from = extended.front()->from;
    walk_to = extended.front()->to;
  }

  const Index target_line = request.line + (request.new_line ? 1 : 0);

  Indentation out;
  // A scope contributes at most one level per physical line it opens on: two
  // brackets opened on one line are one level, not two.
  std::vector<Index> counted;
  const Scope* aligned = nullptr;

  for (const Scope& scope : scopes) {
    // Inclusive at both ends, which is wider than helix's node set on purpose.
    // Helix walks up from `descendant_for_byte_range`, so a sibling that merely
    // *begins* where the caret is -- the `}` of an auto-paired `{|}`, the `)` a
    // Return is being pressed in front of -- is never on its chain. Here it is,
    // and the two answers that matter are built on it: `{|}` puts the closer on
    // a line of its own at the brace's column (commands.cpp's closing_line
    // leans on that dedent), and a closer already sitting on the line below
    // gets its own column rather than the body's. The case where the extra
    // sibling used to do harm was an aligned list, where its `@outdent` cut the
    // alignment back a level; the align branch below now clears the outdents,
    // so the admission is inert exactly there.
    if ((scope.from > walk_from) || (scope.to < walk_to)) continue;

    const Index node_start = StartLineOf(table, scope.from, request);
    Index end = EndLineOf(table, scope, request);
    if (std::ranges::find(extended, &scope) != extended.end()) end = std::max(end, target_line);
    const Index start = scope.header ? StartLineOf(table, scope.parent_from, request) : node_start;

    const bool contains = (start < target_line) && (target_line <= end);
    // A token-shaped outdent (`}`, `else`, `case`) sits on the line it dedents.
    const bool opens_target = (node_start == target_line);

    const bool indents = scope.indent || scope.indent_always;
    const bool outdents = scope.outdent || scope.outdent_always;
    // A node carrying both cancels itself and must not open a scope -- rust's
    // `(call_expression function: (field_expression ...) arguments: (_) @outdent)`
    // over an argument list that is also `@indent` is the shape this is for.
    if (!(indents && outdents)) {
      if (scope.indent && contains && (std::ranges::find(counted, start) == counted.end())) {
        counted.push_back(start);
        ++out.indent;
      }
      if (scope.indent_always && contains) ++out.indent_always;
      if (scope.outdent && opens_target) ++out.outdent;
      if (scope.outdent_always && opens_target) ++out.outdent_always;
    }

    // Innermost containing alignment wins. Nesting makes "innermost" the one
    // that begins latest, so this does not depend on the order captures arrive.
    if (scope.has_align && contains &&
        ((aligned == nullptr) || (scope.from > aligned->from) ||
         ((scope.from == aligned->from) && (scope.to < aligned->to)))) {
      aligned = &scope;
    }
  }

  out.leading_outdent = (out.outdent + out.outdent_always) > 0;

  if (aligned != nullptr) {
    const Index anchor_line_start = LineStart(table, LineAt(table, aligned->anchor));
    const Index cut = std::max(anchor_line_start, aligned->anchor - kMaxAlignBytes);
    // Snapped forward off the middle of a cluster, and only when the cap is what
    // made the cut: a line start is a boundary already, and everything below
    // measures the text a grapheme at a time. Half of a CJK codepoint is not a
    // grapheme NextGraphemeInString can advance over, so the leftover bytes
    // either stall the walk or are measured as something they are not -- and the
    // answer is a column, so a wrong width is a line in the wrong place. Costs a
    // segmentation only on a line with more than a kilobyte left of its anchor.
    const Index from = (cut > anchor_line_start) ? SnapToGraphemeBoundary(table, cut) : cut;
    out.has_align = true;
    out.align = ReadDocRange(table, Interval(from, aligned->anchor));
    // An alignment is an absolute column and already carries every level the
    // scopes it sits inside contributed; stacking them on top would double them.
    // The outdents go with them, and for the same reason read backwards: the
    // `)` that dedents this line dedents it out of the very scope whose column
    // the anchor already is, so subtracting a level from an anchor is
    // subtracting it twice. Helix never has to say this because its `@indent`
    // on the list survives to cancel the `@outdent` one for one; here the
    // indents are gone by this point and a surviving outdent would cut the
    // alignment string back by a level.
    out.indent = 0;
    out.indent_always = 0;
    out.outdent = 0;
    out.outdent_always = 0;
  }
  return out;
}

// Everything that has to be true before a single query runs. False with `error`
// untouched means "this language has no tree indentation", which is not a
// failure and must not reach a status line; false with `error` set means the
// query itself is broken, which must.
//
// This is the only place in the engine that writes `error`, and that is the
// classification the callers below rely on: what comes back from here is the
// user's own .scm and is worth a status line, and everything a query run can
// fail with afterwards -- a spent budget, a buffer whose parse gave up, a
// document with no tree at all -- is the engine declining and is not. See
// indent.h.
bool Prepare(const PieceTable& table, Syntax& syntax, const IndentStyle& style,
             std::chrono::milliseconds budget, std::string& error,
             std::optional<Engine>& out) {
  const std::string_view language = syntax.Language();
  if (language.empty() || !HasIndentQuery(language)) return false;

  std::shared_ptr<CompiledQuery> compiled = CompileQuery(language, kIndentQueryFiles, error);
  if (compiled == nullptr) return false;

  out.emplace(Engine{table, syntax, std::move(compiled), style,
                     std::chrono::steady_clock::now() + budget});
  return true;
}

}

bool HasIndentQuery(std::string_view language) {
  // Memoised per language rather than per call: this is asked once per cursor
  // per Enter, and the answer is a walk of the runtime roots.
  //
  // Only `true` is remembered, which is CompileQuery's rule for a read that
  // failed, for its reason: a memo that never expires and holds a "no" keeps a
  // language broken for the life of the thread, so an `indents.scm` dropped into
  // ~/.config/ronin/koi/queries under a running koi would stay invisible until
  // restart. A "yes" cannot go stale the same way -- the compile behind it is
  // cached anyway, and a query file deleted mid-session leaves CompileQuery to
  // say so.
  //
  // What that costs is a language with no query re-walking the runtime roots on
  // every ask: two or three fs::exists calls, once per Enter per caret in a
  // markdown buffer, which is a stat against a keystroke and is not worth a
  // cache with an invalidation rule of its own.
  thread_local std::vector<std::string> present;
  for (const std::string& known : present) {
    if (known == language) return true;
  }
  if (!HasIndentQueryFile(language)) return false;
  present.emplace_back(language);
  return true;
}

std::optional<std::string> TreeIndentForNewline(const PieceTable& table, Syntax& syntax,
                                                Index cursor, const IndentStyle& style,
                                                std::string& error,
                                                std::chrono::milliseconds budget, bool* aligned) {
  std::optional<Engine> engine;
  if (!Prepare(table, syntax, style, budget, error, engine)) return std::nullopt;

  const Index caret = std::clamp<Index>(cursor, 0, DocLength(table));
  // Inside a `<script>` or a fenced block, the document's own query is the wrong
  // one and there is no path to the injected language's yet. Declining is the
  // honest answer; indenting JavaScript by html's rules is not -- and that holds
  // for `kUnknown` too, where the document is known to have regions and not
  // where. Declining costs the caller's bracket heuristic; the other way round
  // costs the user's whitespace.
  if (syntax.InInjectedRegion(table, caret) != Injected::kNo) return std::nullopt;

  const Index line = LineAt(table, caret);
  // `caret` and not `cursor`: the line and the injection guard above were both
  // asked about the clamped position, and handing the raw one to the engine
  // would have it decide containment against a byte the document does not have.
  // IndentAt clamps it again for the scan it runs, but `Request::byte_pos` is
  // also what StartLineOf and EndLineOf move node spans into post-insertion
  // coordinates against: past the end nothing is bumped, so no scope reaches the
  // line the newline opens and every answer is level 0; before the start
  // everything is, which is a different wrong answer. Not reachable from
  // commands.cpp, which asks about a live cursor.
  const std::optional<Indentation> self = IndentAt(*engine, Request{line, caret, true});
  // `error` deliberately left as Prepare found it: see the note on Engine::error.
  if (!self.has_value()) return std::nullopt;
  // Set on both paths below: the hybrid one only ever compares lines whose
  // alignment matches this one's, so an aligned answer stays an aligned answer
  // however it is finally spelled.
  if (aligned != nullptr) *aligned = self->has_align;

  // Hybrid: apply the difference the tree implies to the leading whitespace a
  // real line already has, so that manual indentation and an incomplete query
  // both survive. A line whose alignment differs from the new one's cannot be
  // compared -- the two are measured from different columns -- so it is skipped.
  int attempts = 0;
  Index walked = 0;
  for (Index at = line; at >= 0; --at) {
    // Both bounds before the line is even measured, because measuring it is the
    // cost: a blank line is skipped below without running a query, so a walk
    // over nothing but blank lines used to run to line 0 with the deadline
    // untouched -- the deadline was only ever consulted inside IndentAt. The cap
    // is belt and braces behind it; see kMaxBaselineLines.
    if (++walked > kMaxBaselineLines) break;
    if (engine->spent || (std::chrono::steady_clock::now() >= engine->until)) break;

    const LineText measured = MeasureLine(table, at);
    if (measured.content >= measured.end) continue;

    const std::optional<Indentation> computed =
        IndentAt(*engine, Request{at, measured.content, false});
    if (!computed.has_value()) break;
    if ((computed->has_align == self->has_align) && (computed->align == self->align)) {
      return AddIndentLevel(ReadDocRange(table, Interval(measured.start, measured.content)),
                            self->Net() - computed->Net(), style);
    }
    if (++attempts == kMaxBaselineAttempts) break;
  }
  return Render(*self, style);
}

std::optional<std::string> TreeIndentForLine(const PieceTable& table, Syntax& syntax, Index line,
                                             const IndentStyle& style, std::string& error,
                                             std::chrono::milliseconds budget,
                                             bool* outdent_token) {
  std::optional<Engine> engine;
  if (!Prepare(table, syntax, style, budget, error, engine)) return std::nullopt;

  const LineText measured = MeasureLine(table, line);
  const Index at = (measured.content < measured.end) ? measured.content : measured.start;
  if (syntax.InInjectedRegion(table, at) != Injected::kNo) return std::nullopt;

  const std::optional<Indentation> indent = IndentAt(*engine, Request{line, at, false});
  // As above: the engine declining is not something to tell anybody about.
  if (!indent.has_value()) return std::nullopt;
  // The outdent counters are only ever raised by a scope that both *contains* the
  // query position -- the line's first non-blank byte -- and *begins* on the line
  // being asked about. That is the token beginning the line and nothing else: a
  // `)` further along the line begins after the position and never reaches the
  // containment test. Read off the flag rather than the counters because an
  // `@align` clears those: the `)` closing a wrapped argument list is still the
  // token that dedents its line, and the answer above is now the column it
  // belongs in, so this is exactly the line the editor may put right.
  if (outdent_token != nullptr) *outdent_token = indent->leading_outdent;
  return Render(*indent, style);
}

std::string IndentedOnce(std::string_view indent, const IndentStyle& style) {
  return AddIndentLevel(std::string{indent}, 1, style);
}

std::string OutdentedOnce(std::string_view indent, const IndentStyle& style) {
  return AddIndentLevel(std::string{indent}, -1, style);
}

}
