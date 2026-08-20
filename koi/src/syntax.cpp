#include "syntax.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <map>

#include "query.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

struct Span {
  std::uint32_t start{0};
  std::uint32_t end{0};
  std::uint32_t pattern{0};
  CaptureId id{kNoCapture};
};

constexpr Index kReadChunk = 4096;

struct Reader {
  const PieceTable* table{nullptr};
  Index length{0};
  std::string chunk;
  Reader(const PieceTable* t, Index len) : table(t), length(len) {
    chunk.reserve(kReadChunk);
  }
};

const char* ReadChunk(void* payload, uint32_t byte_index, TSPoint, uint32_t* bytes_read) {
  auto* reader = static_cast<Reader*>(payload);
  const auto at = static_cast<Index>(byte_index);
  if (at >= reader->length) {
    *bytes_read = 0;
    return "";
  }
  const Index end = std::min(reader->length, at + kReadChunk);
  ReadDocRangeInto(*reader->table, Interval(at, end), reader->chunk);
  *bytes_read = static_cast<uint32_t>(reader->chunk.size());
  return reader->chunk.data();
}

std::string_view DocNodeText(const void* ctx, TSNode node, std::string& scratch) {
  const auto* table = static_cast<const PieceTable*>(ctx);
  const auto from = static_cast<Index>(ts_node_start_byte(node));
  const auto to = static_cast<Index>(ts_node_end_byte(node));
  if (to <= from) {
    scratch.clear();
    return scratch;
  }
  ReadDocRangeInto(*table, Interval(from, to), scratch);
  return scratch;
}

TSPoint ToTsPoint(const Point& p) {
  return TSPoint{static_cast<uint32_t>(p.row), static_cast<uint32_t>(p.column)};
}

TSInputEdit ToTsEdit(const Edit& edit) {
  TSInputEdit out{};
  out.start_byte = static_cast<uint32_t>(edit.start_byte);
  out.old_end_byte = static_cast<uint32_t>(edit.old_end_byte);
  out.new_end_byte = static_cast<uint32_t>(edit.new_end_byte);
  out.start_point = ToTsPoint(edit.start_point);
  out.old_end_point = ToTsPoint(edit.old_end_point);
  out.new_end_point = ToTsPoint(edit.new_end_point);
  return out;
}

// Whether the last token under `node` is one the parser inserted to finish a
// construct the buffer never finished -- a MISSING `}` at the caret, which is
// what every unterminated block looks like while it is being typed. Walking the
// last-child chain rather than asking ts_node_has_error is deliberate: an error
// anywhere inside a block says nothing about whether the block was closed, and
// only the closing end of it decides whether the node owns the lines below.
bool EndsInInventedToken(TSNode node) {
  for (;;) {
    if (ts_node_is_missing(node)) return true;
    const uint32_t children = ts_node_child_count(node);
    if (children == 0) return false;
    node = ts_node_child(node, children - 1);
  }
}

constexpr auto kIncrementalBudget = std::chrono::milliseconds{500};
constexpr auto kFullParseBudget = std::chrono::milliseconds{500};

// A parse being inside its budget says nothing about the query that follows
// it. Matching is quadratic in tree depth over a byte range that every one of
// those ancestors contains -- which is what a viewport in the middle of a
// generated file is. Measured: 12 KB of 3,000-deep nested conditionals parse in
// 18 ms and then cost 1.03 s to highlight the 400 bytes on screen. Every frame,
// and Paint runs inside DrawPane, where no key is read until it returns.
//
// One budget for a whole Paint rather than one per cursor. A frame runs the
// base query, the injection query, and a query per injected region; a per-exec
// deadline would multiply by kMaxInjectedRegions and stop bounding the frame.
// Small, because this is the drawing path: past it the rest of the window is
// painted uncoloured, which is what already happens beyond every other cap
// here.
constexpr auto kQueryBudget = std::chrono::milliseconds{25};

constexpr int kMaxFullParseFailures = 3;

constexpr std::array<std::string_view, 1> kHighlightQueryFiles{"highlights.scm"};
constexpr std::array<std::string_view, 1> kInjectionQueryFiles{"injections.scm"};

// What a fenced block's info string can call a language, versus what koi calls
// its grammar. Only the spellings that actually turn up in the wild -- the
// lookup falls through to the name as written, which is right for `cpp`,
// `rust`, `python` and most of the rest.
struct LanguageAlias {
  std::string_view written;
  std::string_view grammar;
};

constexpr std::array kLanguageAliases{
    LanguageAlias{"sh", "bash"},         LanguageAlias{"shell", "bash"},
    LanguageAlias{"zsh", "bash"},        LanguageAlias{"console", "bash"},
    LanguageAlias{"c++", "cpp"},         LanguageAlias{"h", "cpp"},
    LanguageAlias{"hpp", "cpp"},         LanguageAlias{"js", "javascript"},
    LanguageAlias{"jsx", "javascript"},  LanguageAlias{"mjs", "javascript"},
    LanguageAlias{"ts", "typescript"},   LanguageAlias{"py", "python"},
    LanguageAlias{"rs", "rust"},         LanguageAlias{"yml", "yaml"},
    LanguageAlias{"md", "markdown"},     LanguageAlias{"markdown", "markdown"},
    LanguageAlias{"golang", "go"},       LanguageAlias{"patch", "diff"},
};

// The longest a grammar name may be. The longest koi ships is `javascript`, at
// ten; 32 is room for whatever it grows into and still short enough that a
// path, which is what a hostile info string looks like, does not fit.
constexpr std::size_t kMaxGrammarName = 32;

// How many distinct injected grammars one open document may accumulate. See
// LayerFor: the names come from the document, so the map they key is only
// bounded if something bounds it.
constexpr int kMaxLayers = 256;

// An injected region: where it is in the document, and what to parse it as.
struct Injection {
  Index from{0};
  Index to{0};
  std::string language;
};

// Where an injected region sits, and nothing else about it. What grammar owns
// it and what it parsed as are questions for `region_trees_`, whose entries are
// tied to the revision they were parsed at and thrown away by the next edit; an
// extent is the one thing about a region that an edit *moves* rather than
// invalidates, so it is kept apart and carried forward. See InInjectedRegion.
struct InjectedSpan {
  Index from{0};
  Index to{0};
};

// Maps one span through one edit, widening wherever the edit and the span
// overlap. The direction is deliberate: the only consumer that reads a `kNo`
// acts on it by indenting those bytes with the host's query, so a region
// reported one byte short indents JavaScript by html's rules, while a region
// reported one byte long merely declines to indent a byte of html. Widening is
// the cheap mistake.
//
//  * an edit wholly before the span moves it by the edit's delta;
//  * an edit wholly after it leaves it alone;
//  * an insertion at either edge counts as inside -- text typed at the first or
//    last byte of a <script> body lands in that body -- and grows it;
//  * anything overlapping takes the union of what the span covered and what the
//    edit wrote: `from` down to the edit's start, `to` up to whichever of the
//    shifted end and the edit's new end is further along.
//
// A span the edit deleted outright comes back empty, which is how the caller
// drops it.
InjectedSpan MapSpanThrough(const InjectedSpan& span, const Edit& edit) {
  const Index start = edit.start_byte;
  const Index old_end = edit.old_end_byte;
  const Index new_end = edit.new_end_byte;
  const Index delta = new_end - old_end;

  if (start == old_end) {  // an insertion has no extent to overlap with
    if (start < span.from) return InjectedSpan{span.from + delta, span.to + delta};
    if (start > span.to) return span;
    return InjectedSpan{span.from, span.to + delta};
  }
  if (old_end <= span.from) return InjectedSpan{span.from + delta, span.to + delta};
  if (start >= span.to) return span;
  return InjectedSpan{std::min(span.from, start), std::max(span.to + delta, new_end)};
}

// What one Paint is willing to spend on injected regions, as bytes of document
// handed to another grammar rather than as a count of regions.
//
// The count used to be the whole bound, at 512, on the reasoning that a region
// is a paragraph or a fenced block and a viewport cannot hold many. It can. A
// markdown pipe table puts one region on every cell -- fifteen columns over
// forty rows is six hundred of them on one screen -- and `--render-mode` paints
// a whole file in a single call, where a document of seven hundred paragraphs
// has seven hundred. Both crossed 512 and the rest of the screen came back
// plain, silently: the cap was a *count*, so a thousand twenty-byte paragraphs
// tripped it while a single 196 KB <script> did not, which is exactly backwards
// from what either costs.
//
// So: budget the bytes, which is what a parse is paid in, and keep the count
// only as a backstop against pathological region *density*. A megabyte is far
// more than any viewport of small regions reaches -- the table above is six
// kilobytes of cells -- so a normal frame never meets either limit, and what
// does meet them is a whole-file paint of something enormous, where stopping is
// the right answer and saying so is the missing half of it.
//
// The two are charged differently, because they bound different things. The
// count bounds the *list* one frame builds, and a region takes a slot in it
// whatever its language turns out to be. The bytes bound the text handed to
// another parser, so only a region with a grammar to hand it to pays them: a
// ```mermaid fence is free. See CollectInjections.
constexpr Index kMaxInjectedBytes = 1 << 20;
constexpr int kMaxInjectedRegions = 8192;

// Per region, not per frame. This is only what stops one pathological block
// from stalling a draw; what bounds the frame is kMaxInjectedBytes above, plus
// the trees kept between frames below.
constexpr auto kInjectionBudget = std::chrono::milliseconds{50};

// Injected trees kept alive between frames, across the whole document and not
// just the viewport: scrolling a long markdown file meets an unbounded number
// of regions at one revision, and every one of them would otherwise hold a tree
// until the next edit. Kept comfortably above kMaxInjectedRegions -- the cap is
// a whole-map clear, so anything smaller would let one frame's worth of regions
// throw away the frame before it -- and it rises with that constant for the
// same reason.
constexpr Index kMaxCachedRegionTrees = 2 * kMaxInjectedRegions;

// Injected *extents* kept, which is a different list from the trees above and
// far cheaper per entry -- two offsets, no parse. Scrolling a long markdown file
// accumulates one per region met, so it needs a bound of its own; it is the same
// bound for the same reason, a whole-list drop that must not be reachable by one
// frame's worth of regions.
constexpr Index kMaxInjectedSpans = kMaxCachedRegionTrees;

class TreeSitterSyntax final : public Syntax {
 public:
  // One injected grammar: its query, its parser, and where its captures land in
  // the shared id space.
  struct Layer {
    std::shared_ptr<CompiledQuery> compiled;
    TSParser* parser{nullptr};
    std::vector<CaptureId> ids;
  };

  // One injected region's parse, kept between frames. `Sync` gives the base
  // tree journal replay and `ts_tree_edit`; this is the same discipline for the
  // injected ones, which had none: every Paint read a whole region out of the
  // piece tree and parsed it from scratch, so scrolling an HTML page re-parsed
  // the entire <script> body per frame, and a body too big for the budget
  // stayed unpainted forever while still burning it.
  struct CachedRegion {
    Index from{0};
    Index to{0};
    Index revision{-1};
    std::string language;
    // Set when the parse ran out of budget. Held against this revision only, so
    // the region is retried after the next edit but not on the next frame.
    bool gave_up{false};
    TreePtr tree{nullptr, ts_tree_delete};
  };

  TreeSitterSyntax(std::string language, std::shared_ptr<CompiledQuery> compiled)
      : language_{std::move(language)}, compiled_{std::move(compiled)} {
    parser_ = ts_parser_new();
    ts_parser_set_language(parser_, LanguageOf(*compiled_));
    cursor_ = ts_query_cursor_new();
    // Set once, because this cursor is shared by every paint of every layer and
    // by the injection scan. Left at tree-sitter's UINT32_MAX default it keeps a
    // capture list for every partial match the document starts, which is an
    // allocation the file decides the size of on the one path that runs inside
    // DrawPane -- see kMaxQueryMatchStates. With it set, a run past the cap
    // recycles instead, and every drain below reads back whether it had to.
    ts_query_cursor_set_match_limit(cursor_, kMaxQueryMatchStates);
    base_ids_ = InternNames(*compiled_);
  }

  ~TreeSitterSyntax() override {
    if (tree_ != nullptr) ts_tree_delete(tree_);
    if (cursor_ != nullptr) ts_query_cursor_delete(cursor_);
    if (parser_ != nullptr) ts_parser_delete(parser_);
    for (auto& [name, layer] : layers_) {
      if (layer.parser != nullptr) ts_parser_delete(layer.parser);
    }
  }

  TreeSitterSyntax(const TreeSitterSyntax&) = delete;
  TreeSitterSyntax& operator=(const TreeSitterSyntax&) = delete;

  std::string_view Language() const override { return language_; }
  // Not the base query's list: an injected layer's captures live in the same id
  // space, appended as its language is first seen. Grows during Paint, which is
  // why callers resolve styles from this rather than caching a length.
  std::span<const std::string> CaptureNames() const override { return names_; }
  bool TimedOut() const override { return timed_out_; }
  bool QueryTruncated() const override { return query_truncated_; }
  bool InjectionsTruncated() const override { return injections_truncated_; }
  Index InjectionParses() const override { return injection_parses_; }

  bool Captures(const PieceTable& table, std::span<const std::string_view> query_files,
                Interval range, std::vector<Capture>& out, std::string& error,
                bool* budget_exhausted = nullptr, bool keep_zero_width = false,
                const std::chrono::steady_clock::time_point* deadline = nullptr) override {
    if (budget_exhausted != nullptr) *budget_exhausted = false;
    out.clear();
    error.clear();
    Sync(table);
    if (tree_ == nullptr) {
      error = timed_out_ ? "parse gave up -- file too large" : "no parse tree for this buffer";
      return false;
    }
    const std::shared_ptr<CompiledQuery> query = CompileQuery(language_, query_files, error);
    if (query == nullptr) return false;
    const std::span<const std::string> names = CaptureNamesOf(*query);

    const Index doc_len = DocLength(table);
    const Index scan_from = range.empty() ? 0 : std::clamp<Index>(range.front(), 0, doc_len);
    const Index scan_to =
        range.empty() ? doc_len : std::clamp<Index>(range.back() + 1, scan_from, doc_len);

    TSQueryCursor* cursor = ts_query_cursor_new();
    ts_query_cursor_set_match_limit(cursor, kMaxQueryMatchStates);
    // Both ends fit a uint32 because there is a tree above: Sync refuses a
    // buffer tree-sitter cannot address at all, so a document with a tree is one
    // whose every offset survives the cast. Clamped to `doc_len` just above, so
    // the same holds for a range the caller made up.
    ts_query_cursor_set_byte_range(cursor, static_cast<uint32_t>(scan_from),
                                   static_cast<uint32_t>(scan_to));
    StartQueryBudget(deadline);
    ExecUnderBudget(cursor, QueryOf(*query), ts_tree_root_node(tree_));

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor, &match)) {
      if (!PredicatesHold(*query, match, DocNodeText, &table)) continue;
      // The structure fields cost real work and only one caller reads them --
      // see the declaration for the contract. `ts_node_parent` in particular
      // re-descends from the root, so asking it per capture on a document-wide
      // text-object query spends the frame budget walking for an answer nobody
      // looks at, and returns a short list of what the caller did want.
      // Narrowed twice: to the caller that asked for zero-width spans (the
      // indent engine), and within it to the patterns whose `scope` is
      // `header`, the only ones for which a parent means anything.
      const bool structure = keep_zero_width;
      const bool wants_parent = structure && ScopeIsHeader(*query, match.pattern_index);
      for (uint16_t i = 0; i < match.capture_count; ++i) {
        const TSQueryCapture& capture = match.captures[i];
        const auto from = static_cast<Index>(ts_node_start_byte(capture.node));
        const auto to = static_cast<Index>(ts_node_end_byte(capture.node));
        // A node spanning no bytes is one the parser completed with a token it
        // invented, and only the indent engine has any use for it -- see the
        // declaration.
        if (((to <= from) && !keep_zero_width) || (capture.index >= names.size())) continue;
        Index parent_from = from;
        if (wants_parent) {
          const TSNode parent = ts_node_parent(capture.node);
          if (!ts_node_is_null(parent)) parent_from = static_cast<Index>(ts_node_start_byte(parent));
        }
        out.push_back(Capture{from, to, names[capture.index], match.id, match.pattern_index,
                              parent_from, structure && EndsInInventedToken(capture.node)});
      }
    }
    // Both ways this run can come back short. A recycled capture list is a
    // dropped match, and a dropped match is a capture the indent engine never
    // sees and reads as a scope that is not there -- a silently wrong column
    // rather than a decline. `budget_exhausted` is what turns either into one,
    // so they are one answer to the caller; the difference between them is a
    // fact about the document, and that is what QueryTruncated keeps. Read
    // before the delete: the flag belongs to the cursor.
    const bool dropped = ts_query_cursor_did_exceed_match_limit(cursor);
    ts_query_cursor_delete(cursor);
    if (dropped) query_dropped_matches_ = true;
    const bool cut = query_deadline_.expired || dropped;
    NoteQueryBudget();
    if (budget_exhausted != nullptr) *budget_exhausted = cut;
    return true;
  }

  // Asked per bracket by auto-indent, which skips the ones that are only text.
  //
  // The host tree alone cannot answer this inside an injected region: every
  // byte of a <script> body hangs off raw_text/script_element/element, and no
  // name in that chain says "string" or "comment", so a brace inside a JS
  // string counted as a real block delimiter and shifted every line after it.
  // The layer that does know is the one Paint parsed and `region_trees_` keeps.
  //
  // The two answers are OR'd rather than the layer's overriding: a region can
  // itself sit inside a host literal -- html injected into a JS template
  // string -- and those bytes really are in a string, which is what the host
  // walk has always said about them. Consulting the layer can only turn a
  // `false` into a `true`; no byte that used to be skipped stops being skipped.
  bool InLiteralOrComment(Index pos) override {
    if ((tree_ == nullptr) || (pos < 0)) return false;
    if (InInjectedLiteralOrComment(pos)) return true;
    const auto at = static_cast<uint32_t>(pos);
    return NamesLiteralOrComment(
        ts_node_descendant_for_byte_range(ts_tree_root_node(tree_), at, at + 1));
  }

  // The half of the question above that is only about ownership: which grammar's
  // rules apply to this byte, without asking what it parsed as.
  //
  // Answered from `injected_spans_` rather than from `region_trees_`, which this
  // used to read. Two things that map is not: it is emptied by every Sync, so
  // one caret's query blinded the guard for the next caret of the same command;
  // and its entries are only valid at the revision they were parsed at, so
  // between an edit and the paint that follows it every offset in it is one edit
  // out of date -- which is exactly the state auto-indent asks this in. Spans
  // survive both: nothing but a paint replaces them, and an edit moves them.
  Injected InInjectedRegion(const PieceTable& table, Index pos) override {
    AdvanceSpans(table);
    if (spans_unmapped_) return Injected::kUnknown;
    if (pos < 0) return Injected::kNo;
    // Sorted and disjoint, so the only candidate is the last span starting at or
    // before `pos`.
    const auto after = std::ranges::upper_bound(injected_spans_, pos, {}, &InjectedSpan::from);
    if (after == injected_spans_.begin()) return Injected::kNo;
    return (pos < (after - 1)->to) ? Injected::kYes : Injected::kNo;
  }

  void Sync(const PieceTable& table) override {
    // Before the early returns: the spans are the one piece of state here that
    // an unchanged tree does not make current, and a Sync that decides it has
    // nothing to parse has still seen the table.
    AdvanceSpans(table);
    if (revision_ == table.revision) {
      if ((tree_ != nullptr) || timed_out_) return;
    }
    if (full_parse_failures_ >= kMaxFullParseFailures) return;

    // Every path from here either replaces `tree_` or moves `revision_` to the
    // table's -- the replay branch below, the `next != nullptr` branch, and the
    // failure branch alike -- and an edit anywhere in the document moves the
    // byte offsets an injected region's cached tree was parsed at. The
    // `revision == revision_` test in PaintInjections is what actually makes
    // reuse safe; clearing here is belt and braces, so the map cannot carry
    // entries from a dead revision forward.
    //
    // A tree is worth nothing at moved offsets, so dropping it is right; where
    // a region *is* survives the edit that moved it, and that half lives in
    // `injected_spans_`, which AdvanceSpans above has already carried forward.
    region_trees_.clear();
    // Lowered here and raised only by a paint, exactly as `timed_out_` is
    // lowered only by a parse: giving up on part of a document is a fact about
    // the revision that was painted, and it holds until the buffer changes.
    injections_truncated_ = false;
    query_truncated_ = false;

    // Every byte offset tree-sitter deals in is a uint32 -- the input callback
    // it reads through, every node's start and end, every cursor range -- so a
    // buffer past 4 GiB cannot be addressed, and parsing one would quietly
    // produce a tree over an arbitrary prefix with captures landing at wrapped
    // offsets. Refused at the one place a tree comes from, which is what makes
    // every static_cast<uint32_t> downstream of a tree safe by construction:
    // nothing here runs without one. It takes the same path a parse that ran out
    // of budget takes, because it is the same answer -- this file is more than
    // the engine will read -- and like that one it is held against the revision,
    // so an edit that shortens the buffer is tried afresh.
    std::uint32_t doc_end = 0;
    if (!TreeSitterByteRange(static_cast<std::size_t>(DocLength(table)), doc_end)) {
      if (tree_ != nullptr) {
        ts_tree_delete(tree_);
        tree_ = nullptr;
      }
      revision_ = table.revision;
      timed_out_ = true;
      painted_revision_ = -1;
      return;
    }

    const bool replayable = (tree_ != nullptr) && (revision_ >= table.journal_base);
    if (replayable) {
      for (Index r = revision_; r < table.revision; ++r) {
        const Index at = r - table.journal_base;
        if ((at < 0) || (at >= static_cast<Index>(table.journal.size()))) break;
        const TSInputEdit edit = ToTsEdit(table.journal[static_cast<size_t>(at)]);
        ts_tree_edit(tree_, &edit);
      }
    } else if (tree_ != nullptr) {
      ts_tree_delete(tree_);
      tree_ = nullptr;
    }

    Reader reader{&table, DocLength(table)};
    TSInput input{};
    input.payload = &reader;
    input.read = ReadChunk;
    input.encoding = TSInputEncodingUTF8;

    const bool incremental = (tree_ != nullptr);
    const auto budget = incremental ? kIncrementalBudget : kFullParseBudget;

    Deadline deadline{std::chrono::steady_clock::now() + budget, nullptr, false, 0};
    TSParseOptions options{};
    options.payload = &deadline;
    options.progress_callback = StopAtDeadline;

    TSTree* next = ts_parser_parse_with_options(parser_, tree_, input, options);
    timed_out_ = deadline.expired;
    painted_revision_ = -1;
    if (next != nullptr) {
      if (tree_ != nullptr) ts_tree_delete(tree_);
      tree_ = next;
      revision_ = table.revision;
      full_parse_failures_ = 0;
    } else {
      ts_parser_reset(parser_);
      if (tree_ != nullptr) {
        ts_tree_delete(tree_);
        tree_ = nullptr;
      }
      revision_ = table.revision;
      if (!incremental) ++full_parse_failures_;
    }
  }

  void Paint(const PieceTable& table, Interval range, std::vector<CaptureId>& out) override {
    const Index start = range.empty() ? 0 : range.front();
    const Index end = range.empty() ? 0 : (range.back() + 1);
    const Index length = std::max<Index>(0, end - start);

    if ((tree_ != nullptr) && (painted_revision_ == revision_) && (painted_start_ == start) &&
        (painted_end_ == end)) {
      out = painted_;
      return;
    }

    out.assign(static_cast<size_t>(length), kNoCapture);
    if ((tree_ == nullptr) || (QueryOf(*compiled_) == nullptr) || (length == 0)) return;

    StartQueryBudget();
    PaintTree(*compiled_, ts_tree_root_node(tree_), base_ids_, DocNodeText, &table, 0, start, end,
              start, out);
    PaintInjections(table, start, end, out);
    NoteQueryBudget();

    // Memoised even when the budget ran out. A window that cost 25 ms and came
    // back half-painted must not cost 25 ms again on the next frame that shows
    // the same bytes at the same revision: the partial paint is what this
    // document looks like until it is edited, and Sync clears the memo.
    painted_ = out;
    painted_revision_ = revision_;
    painted_start_ = start;
    painted_end_ = end;
  }

 private:
  // Opens a query budget for one call into this object. Every cursor started
  // between here and the matching NoteQueryBudget() shares it, so the cost is
  // bounded per frame and not per cursor.
  //
  // `caller` is a deadline this call may not run past whatever the frame budget
  // says -- see Captures. Taken as the earlier of the two rather than as a
  // replacement: a caller may shorten a frame and may not lengthen one, so a
  // generous deadline handed in from outside cannot turn the 25 ms bound below
  // into a stall. Written afresh here on every call, so what a caller hands in
  // governs its own call and nothing after it.
  void StartQueryBudget(const std::chrono::steady_clock::time_point* caller = nullptr) {
    const auto frame = std::chrono::steady_clock::now() + kQueryBudget;
    query_budget_borrowed_ = (caller != nullptr) && (*caller < frame);
    query_deadline_ = Deadline{query_budget_borrowed_ ? *caller : frame, nullptr, false, 0};
    query_dropped_matches_ = false;
  }

  // Closes it, and tells the truth about it. Only ever raises the flag: what
  // lowers it is the next parse in Sync, exactly as for a parse that gave up.
  // A query that came back short is a fact about this revision, and it holds
  // until the buffer changes.
  //
  // `query_truncated_` and not `timed_out_`. The two used to be the same flag,
  // which meant one cut paint left every later reader of TimedOut() -- Captures'
  // own "parse gave up -- file too large", and Sync's refusal to try a buffer
  // again -- being told the parse failed on a document that had parsed fine.
  // What the parse did belongs to the parse.
  //
  // A run the *caller's* deadline cut raises nothing: it stopped because whoever
  // asked had spent its own time, not because this document is more than a frame
  // can match, and the same query on the same revision with a budget of its own
  // would finish. The caller is told through `budget_exhausted`, which is its
  // answer to give. A run that recycled a capture list is the other way round --
  // no clock decided it -- so it is raised whoever the deadline belonged to.
  void NoteQueryBudget() {
    if ((query_deadline_.expired && !query_budget_borrowed_) || query_dropped_matches_) {
      query_truncated_ = true;
    }
  }

  // `query_options_` is a member and not a local because the cursor keeps a
  // *pointer* to it -- the progress callback is read out of it on every
  // ts_query_cursor_next_match, long after the exec has returned.
  void ExecUnderBudget(TSQueryCursor* cursor, const TSQuery* query, TSNode root) {
    query_options_.payload = &query_deadline_;
    query_options_.progress_callback = StopQueryAtDeadline;
    ts_query_cursor_exec_with_options(cursor, query, root, &query_options_);
  }

  // One query over one tree, painted into `out`, which is indexed from `start`.
  //
  // `origin` is where the tree's byte 0 sits in the document: zero for the base
  // tree, and the region's start for an injected one, which is parsed from a
  // copy of its own text so that its offsets begin at zero.
  //
  // `out_base` is the document offset `out[0]` stands for, which is the
  // viewport's start and *not* `start`: an injected region is painted over a
  // window that begins before it.
  void PaintTree(const CompiledQuery& query, TSNode root, const std::vector<CaptureId>& ids,
                 NodeText text, const void* ctx, Index origin, Index start, Index end,
                 Index out_base, std::vector<CaptureId>& out) {
    if (QueryOf(query) == nullptr) return;
    const Index lo = std::max<Index>(0, start - origin);
    const Index hi = std::max<Index>(0, end - origin);
    if (hi <= lo) return;

    // Both fit a uint32: there is a tree here, and Sync makes no tree for a
    // buffer whose length tree-sitter cannot address, so every offset within the
    // document that produced it survives the cast. `end` is the viewport's, and
    // both callers Sync the same table immediately before painting it.
    ts_query_cursor_set_byte_range(cursor_, static_cast<uint32_t>(lo), static_cast<uint32_t>(hi));
    ExecUnderBudget(cursor_, QueryOf(query), root);

    spans_.clear();
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor_, &match)) {
      if (!PredicatesHold(query, match, text, ctx)) continue;
      for (uint16_t i = 0; i < match.capture_count; ++i) {
        const TSQueryCapture& capture = match.captures[i];
        if (capture.index >= ids.size()) continue;
        spans_.push_back(Span{ts_node_start_byte(capture.node), ts_node_end_byte(capture.node),
                              match.pattern_index, ids[capture.index]});
      }
    }
    // Read here rather than once per Paint: the cursor is shared and each exec
    // clears the flag, so a layer that dropped matches would be forgotten by the
    // next layer's run. Accumulated into the budget this paint opened -- a
    // highlight missing because its match was recycled is missing for the same
    // reason as one the deadline cut, and the same word covers both.
    if (ts_query_cursor_did_exceed_match_limit(cursor_)) query_dropped_matches_ = true;

    // Later and narrower wins, which is what puts a keyword over the string
    // that contains it. Stable across layers too: the base is painted first and
    // an injected layer paints over it, so the inner language always shows.
    std::ranges::sort(spans_, [](const Span& a, const Span& b) {
      if (a.start != b.start) return a.start < b.start;
      if (a.end != b.end) return a.end > b.end;
      return a.pattern < b.pattern;
    });

    for (const Span& span : spans_) {
      const Index from = std::max<Index>(start, origin + static_cast<Index>(span.start));
      const Index to = std::min<Index>(end, origin + static_cast<Index>(span.end));
      for (Index at = from; at < to; ++at) out[static_cast<size_t>(at - out_base)] = span.id;
    }
  }

  // Interns a query's capture names into the shared id space, so that
  // `markup.italic` from the inline grammar and `markup.italic` from anywhere
  // else are the same id and resolve to the same theme entry.
  std::vector<CaptureId> InternNames(const CompiledQuery& query) {
    std::vector<CaptureId> ids;
    for (const std::string& name : CaptureNamesOf(query)) {
      const auto found = ids_by_name_.find(name);
      if (found != ids_by_name_.end()) {
        ids.push_back(found->second);
        continue;
      }
      names_.push_back(name);
      const auto id = static_cast<CaptureId>(names_.size());  // 0 is kNoCapture
      ids_by_name_.emplace(name, id);
      ids.push_back(id);
    }
    return ids;
  }

  Layer* LayerFor(const std::string& language) {
    if (const auto found = layers_.find(language); found != layers_.end()) {
      return found->second.compiled ? &found->second : nullptr;
    }
    // Every miss costs a runtime-path sweep and a permanent entry, and the
    // names come from the document -- a file of ```aaa, ```aab, ... fences
    // would grow this without end. A document that genuinely injects 256
    // distinct grammars does not exist; past the cap the base grammar paints
    // the region, which is what an unknown language gets anyway.
    if (std::ssize(layers_) >= kMaxLayers) return nullptr;
    Layer layer;
    std::string error;
    // Cached negatively as well as positively: a markdown file full of ```text
    // blocks must not retry the runtime-path lookup on every frame.
    layer.compiled = CompileQuery(language, kHighlightQueryFiles, error);
    if (layer.compiled != nullptr) {
      layer.parser = ts_parser_new();
      if (!ts_parser_set_language(layer.parser, LanguageOf(*layer.compiled))) {
        ts_parser_delete(layer.parser);
        layer.parser = nullptr;
        layer.compiled = nullptr;
      } else {
        layer.ids = InternNames(*layer.compiled);
      }
    }
    const bool usable = (layer.compiled != nullptr);
    auto [at, _] = layers_.emplace(language, std::move(layer));
    return usable ? &at->second : nullptr;
  }

  // Reads the one directive koi understands, `(#set! injection.language "x")`,
  // off a pattern.
  //
  // Through PropertiesFor, which is where `#set!` is parsed for everybody: this
  // used to walk the predicate steps itself, looking for the literal "set!" with
  // two string steps behind it, and two parsers for one directive drift. They
  // already disagreed in two places -- the hand-scan matched a "set!" anywhere in
  // the run, including one that was some other predicate's *argument*, and it
  // refused the capture-scoped `(#set! @content injection.language "x")` that
  // PropertiesFor reads pattern-wide. Neither shape is in a shipped
  // injections.scm, so the answer for every file koi ships is unchanged.
  //
  // The first `injection.language` on the pattern wins, which is the step order
  // PropertiesFor preserves and the one the scan it replaces read. The view is
  // into the CompiledQuery, which outlives this call: `injections_` is a member.
  std::string_view LiteralLanguageOf(const CompiledQuery& query, uint32_t pattern) const {
    for (const QueryProperty& property : PropertiesFor(query, pattern)) {
      if (property.key == "injection.language") return property.value;
    }
    return {};
  }

  // Does any name on this node's ancestor chain call it a literal or a comment?
  static bool NamesLiteralOrComment(TSNode node) {
    for (; !ts_node_is_null(node); node = ts_node_parent(node)) {
      const std::string_view type{ts_node_type(node)};
      if ((type.find("string") != std::string_view::npos) ||
          (type.find("comment") != std::string_view::npos) ||
          (type.find("char_literal") != std::string_view::npos)) {
        return true;
      }
    }
    return false;
  }

  // The same question, put to the injected layer that owns `pos` -- in that
  // layer's own coordinates, since a region is parsed from a copy of its bytes
  // and its offsets start at zero.
  //
  // `region_trees_` is keyed on the region's start byte and injected regions do
  // not overlap (they are one level deep, and each is one node of the host
  // tree), so the only candidate is the last region starting at or before
  // `pos`: one lookup, not a walk over as many as kMaxCachedRegionTrees
  // entries, for a function auto-indent calls once per bracket on the line.
  //
  // Every miss falls through to the host walk, which is exactly what this
  // function used to do for these bytes -- never a worse answer than before:
  //
  //  * no entry: the region has no grammar koi ships, or nothing has painted
  //    this window yet. Paint is what fills the cache, and typing between two
  //    frames leaves it empty; Sync clears it on every edit.
  //  * `tree == nullptr`: the parse gave up on this region inside the budget.
  //  * a revision that is not the one the base tree stands at: an entry left
  //    under a reused key by an older revision, whose byte offsets have since
  //    moved. Sync clears the map, so this is belt and braces.
  //  * `pos` outside the cached extent, on the region's own key or on the
  //    tree's: the document may have shrunk under a cache that outlived it.
  bool InInjectedLiteralOrComment(Index pos) const {
    auto it = region_trees_.upper_bound(pos);
    if (it == region_trees_.begin()) return false;
    const CachedRegion& cached = (--it)->second;
    if ((cached.tree == nullptr) || (cached.revision != revision_)) return false;
    if ((pos < cached.from) || (pos >= cached.to)) return false;

    const TSNode root = ts_tree_root_node(cached.tree.get());
    const auto at = static_cast<uint32_t>(pos - cached.from);
    if (at >= ts_node_end_byte(root)) return false;
    return NamesLiteralOrComment(ts_node_descendant_for_byte_range(root, at, at + 1));
  }

  // Carries `injected_spans_` from the revision its offsets are in to the
  // table's, through the same journal Sync replays into the base tree. Cheap
  // enough to call from the guard itself -- the common case is one edit and a
  // handful of spans, and the alternative is answering from stale offsets.
  void AdvanceSpans(const PieceTable& table) {
    if (spans_revision_ == table.revision) return;
    if (injected_spans_.empty()) {
      // Nothing to carry. Deliberately not a place that lowers `spans_unmapped_`
      // -- the list is empty *because* the carrying failed, and only a paint
      // knows anything again. A document that injects nothing never raised it,
      // which is what keeps most documents from ever meeting `kUnknown`.
      spans_revision_ = table.revision;
      return;
    }

    // Replayable on the same terms as the tree: the journal must still reach
    // back to where the spans stand. It cannot reach *forward* -- a document
    // that moved backwards under this object (a reload) leaves offsets nothing
    // here can undo -- and either way the honest answer is that the regions are
    // still there and their whereabouts are not.
    bool mapped = (spans_revision_ >= table.journal_base) && (spans_revision_ <= table.revision);
    if (mapped) {
      for (Index r = spans_revision_; r < table.revision; ++r) {
        const Index at = r - table.journal_base;
        if ((at < 0) || (at >= std::ssize(table.journal))) {
          mapped = false;
          break;
        }
        const Edit& edit = table.journal[static_cast<std::size_t>(at)];
        for (InjectedSpan& span : injected_spans_) span = MapSpanThrough(span, edit);
      }
    }
    spans_revision_ = table.revision;
    if (mapped) {
      NormalizeSpans();
      return;
    }
    injected_spans_.clear();
    spans_unmapped_ = true;
  }

  // Drops what an edit emptied and restores the sorted-disjoint invariant the
  // lookup depends on: widening two neighbours through one edit can leave them
  // overlapping.
  //
  // Checked before it is done, because the check is a scan and the repair is a
  // sort, and the caller that runs per caret per keystroke almost never needs
  // the repair: mapping moves spans monotonically, so only an edit that spans
  // the gap between two of them can put a pair out of shape. The paint path,
  // which appends a window's worth of new regions at the end, always does.
  void NormalizeSpans() {
    bool intact = true;
    for (std::size_t at = 0; at < injected_spans_.size(); ++at) {
      const InjectedSpan& span = injected_spans_[at];
      if ((span.to <= span.from) || ((at > 0) && (span.from < injected_spans_[at - 1].to))) {
        intact = false;
        break;
      }
    }
    if (intact) return;

    std::erase_if(injected_spans_, [](const InjectedSpan& span) { return span.to <= span.from; });
    std::ranges::sort(injected_spans_, {}, &InjectedSpan::from);
    std::size_t kept = 0;
    for (const InjectedSpan& span : injected_spans_) {
      if ((kept > 0) && (span.from <= injected_spans_[kept - 1].to)) {
        injected_spans_[kept - 1].to = std::max(injected_spans_[kept - 1].to, span.to);
        continue;
      }
      injected_spans_[kept++] = span;
    }
    injected_spans_.resize(kept);
  }

  // What one paint of [start, end) leaves the guard knowing. Every region the
  // collection found goes in, whether or not koi has a grammar for it and
  // whether or not the parse that follows succeeds: which grammar owns a byte is
  // not a question about how well it was painted, and a ```mermaid fence is as
  // wrong to indent by markdown's rules as a <script> is by html's.
  //
  // `complete` is the collection's own account of itself. When it holds, this
  // window's regions are the whole truth about this window and whatever was
  // known of it before is replaced -- which is what retires the span of a
  // <script> the user has since turned into a <div>. When it does not (the
  // region budget cut the collection short, or its cursor recycled a match away
  // before the collection ever saw it), the two are unioned instead:
  // dropping a region the collection never got to would be the expensive
  // mistake. Outside the window nothing is touched either way, which is what
  // lets scrolling accumulate the document rather than forget it.
  void RecordInjectedSpans(Index start, Index end, bool complete) {
    spans_unmapped_ = false;
    if (complete) {
      std::vector<InjectedSpan> kept;
      kept.reserve(injected_spans_.size());
      for (const InjectedSpan& span : injected_spans_) {
        if (span.from < start) kept.push_back(InjectedSpan{span.from, std::min(span.to, start)});
        if (span.to > end) kept.push_back(InjectedSpan{std::max(span.from, end), span.to});
      }
      injected_spans_ = std::move(kept);
    }
    if ((std::ssize(injected_spans_) + std::ssize(injections_found_)) > kMaxInjectedSpans) {
      injected_spans_.clear();
    }
    for (const Injection& region : injections_found_) {
      injected_spans_.push_back(InjectedSpan{region.from, region.to});
    }
    NormalizeSpans();
  }

  // The regions inside [start, end) that another grammar owns.
  void CollectInjections(const PieceTable& table, Index start, Index end,
                         std::vector<Injection>& out) {
    out.clear();
    if (!injections_tried_) {
      injections_tried_ = true;
      std::string error;
      injections_ = CompileQuery(language_, kInjectionQueryFiles, error);
    }
    if ((injections_ == nullptr) || (QueryOf(*injections_) == nullptr)) return;

    const std::span<const std::string> names = CaptureNamesOf(*injections_);
    ts_query_cursor_set_byte_range(cursor_, static_cast<uint32_t>(start),
                                   static_cast<uint32_t>(end));
    ExecUnderBudget(cursor_, QueryOf(*injections_), ts_tree_root_node(tree_));

    // Counted whole, and counted whether or not the region's tree is already in
    // `region_trees_`. A cached region does cost close to nothing this frame,
    // but charging only for cache misses would make the set of regions that get
    // painted depend on what happened to be cached -- the same screen at the
    // same revision styling differently from one frame to the next, with
    // whichever answer came first frozen into Paint's memo. The budget is meant
    // to bound the frame that hurts, which is the cold one, and a cache hit can
    // only make a frame cheaper than its bound, never dearer. It is the whole
    // region and not the part on screen because that is what gets parsed: the
    // copy below is never clipped to the viewport.
    Index injected_bytes = 0;
    std::string scratch;
    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor_, &match)) {
      if ((std::ssize(out) >= kMaxInjectedRegions) || (injected_bytes >= kMaxInjectedBytes)) {
        injections_truncated_ = true;
        break;
      }
      if (!PredicatesHold(*injections_, match, DocNodeText, &table)) continue;

      Injection found;
      bool have_content = false;
      std::string_view dynamic;
      for (uint16_t i = 0; i < match.capture_count; ++i) {
        const TSQueryCapture& capture = match.captures[i];
        if (capture.index >= names.size()) continue;
        const std::string_view name = names[capture.index];
        if (name == "injection.content") {
          found.from = static_cast<Index>(ts_node_start_byte(capture.node));
          found.to = static_cast<Index>(ts_node_end_byte(capture.node));
          have_content = true;
        } else if (name == "injection.language") {
          // A capture, not a literal: the info string of a fenced block.
          dynamic = DocNodeText(&table, capture.node, scratch);
        }
      }
      if (!have_content || (found.to <= found.from)) continue;

      const std::string_view written =
          dynamic.empty() ? LiteralLanguageOf(*injections_, match.pattern_index) : dynamic;
      found.language = GrammarFor(written);
      // Never itself: a language whose injection query names its own grammar
      // would re-parse the same bytes forever.
      if (found.language.empty() || (found.language == language_)) continue;
      // ```mermaid is a perfectly plausible grammar name with no grammar behind
      // it. The paint loop discovers that below, by which point the region has
      // been charged for a parse that never happens -- so a chapter of diagrams
      // spends the byte budget, the real ```rust fence under it comes back
      // plain, and `injections_truncated_` blames a frame that did no work.
      // Asking here is a hash lookup after the first miss, which LayerFor caches
      // as firmly as a hit; and at the kMaxLayers cap it answers nullptr for an
      // unseen name just as flatly, which is the answer the paint loop is about
      // to get from that same full table. The two cannot end up disagreeing
      // about which regions were worth charging for.
      //
      // Kept in `out` with its language cleared rather than dropped, because
      // this list is also what RecordInjectedSpans reads. A fence's bytes belong
      // to whatever the fence says they belong to whether or not koi can colour
      // them, and InInjectedRegion is what keeps the host's indent query out of
      // them; dropping the region would quietly hand markdown authority over the
      // inside of a ```mermaid block, and html the inside of a script body whose
      // type it has no grammar for.
      if (LayerFor(found.language) == nullptr) {
        found.language.clear();
      } else {
        injected_bytes += (found.to - found.from);
      }
      out.push_back(std::move(found));
    }
    // A recycled capture list here is a region the collection was never handed,
    // so this window's list is not the whole truth about the window -- which is
    // exactly what `injections_truncated_` says, and what stops RecordInjectedSpans
    // retiring the spans of regions that are still there. The break above is the
    // other way to fall short of the same thing.
    if (ts_query_cursor_did_exceed_match_limit(cursor_)) {
      query_dropped_matches_ = true;
      injections_truncated_ = true;
    }
  }

  // Deliberately one deep. An injected layer is parsed and painted, and its own
  // injections are not followed: the second level is markdown-in-html-in-markdown
  // territory, it doubles the worst-case parse cost of a frame, and nothing koi
  // ships needs it.
  void PaintInjections(const PieceTable& table, Index start, Index end,
                       std::vector<CaptureId>& out) {
    CollectInjections(table, start, end, injections_found_);
    // Recorded before the early return below, and from what the collection found
    // rather than from what the loop manages to parse: a window that no longer
    // has a region in it is exactly the case that has to retire the span it used
    // to have. Only when the tree the regions came out of is the table's own --
    // a paint over a stale tree has stale offsets, and the spans are kept in the
    // table's coordinates.
    if (revision_ == table.revision) {
      AdvanceSpans(table);
      RecordInjectedSpans(start, end, !injections_truncated_);
    }
    if (injections_found_.empty()) return;

    for (const Injection& region : injections_found_) {
      // The frame is over. Not just the queries: an injected region that has
      // not been parsed yet would spend up to kInjectionBudget on a tree
      // nothing is left to paint with.
      if (query_deadline_.expired) break;

      // Cleared by the collection, which already looked for a grammar and found
      // none. The region is in this list only so that its extent reached
      // RecordInjectedSpans above. LayerFor would refuse the empty name too, but
      // it would refuse it by interning it: a permanent entry under "" holding
      // one of the kMaxLayers slots for something that was never a language.
      if (region.language.empty()) continue;

      const Index from = std::max(start, region.from);
      const Index to = std::min(end, region.to);
      if (to <= from) continue;
      Layer* layer = LayerFor(region.language);
      if (layer == nullptr) continue;

      // Parsed from a copy of its own bytes rather than with included ranges on
      // the whole document: node offsets then start at zero, which is what
      // `origin` in PaintTree exists to undo, and each region's parse stays
      // independent, so an unclosed `*` in one paragraph cannot bleed into the
      // next.
      //
      // The copy is *not* bounded by the viewport -- only the discovery of a
      // region is. A 196 KB <script> whose first line is on screen is copied
      // and parsed whole, which is why the tree is kept between frames below
      // rather than rebuilt on each one.
      ReadDocRangeInto(table, Interval(region.from, region.to), region_text_);
      const std::string_view text{region_text_};

      if (std::ssize(region_trees_) > kMaxCachedRegionTrees) region_trees_.clear();

      // Keyed on where the region starts, and usable only at the revision it
      // was parsed at. A cached tree whose `from` has since shifted is
      // unreachable rather than wrong: nothing looks under the old key any
      // more, and the entry that does sit under a reused key fails the revision
      // test, because an edit anywhere moves `revision_`.
      CachedRegion& cached = region_trees_[region.from];
      const bool fresh = (cached.tree != nullptr) && (cached.revision == revision_) &&
                         (cached.from == region.from) && (cached.to == region.to) &&
                         (cached.language == region.language);
      if (!fresh) {
        // A region that ran out of budget is not retried until the next edit.
        // Scrolling over one must not spend the budget again on every frame to
        // paint nothing, which is what it used to do.
        if (cached.gave_up && (cached.revision == revision_)) continue;
        ++injection_parses_;
        ParsedBuffer parsed =
            ParseBuffer(layer->parser, *layer->compiled, region.language, text, kInjectionBudget);
        cached.from = region.from;
        cached.to = region.to;
        cached.revision = revision_;
        cached.language = region.language;
        cached.gave_up = !parsed;
        cached.tree = std::move(parsed.tree);
        if (cached.tree == nullptr) continue;
      }
      // The text is re-read even on a hit: `NodeTextIn` reads node text out of
      // it to settle the query's predicates, and only the parse is cached.
      //
      // Running out of *query* budget in here deliberately leaves `gave_up`
      // alone. `gave_up` means the region has no tree and asking again at this
      // revision would only burn the parse budget again; a region whose query
      // was cut short still has a perfectly good tree, and the next frame may
      // well reach further into it. Repainting the same window is what the
      // memo in Paint stops, not this flag.
      PaintTree(*layer->compiled, ts_tree_root_node(cached.tree.get()), layer->ids, NodeTextIn,
                &text, region.from, from, to, start, out);
    }
  }

  std::string language_;
  std::shared_ptr<CompiledQuery> compiled_;
  TSParser* parser_{nullptr};
  TSQueryCursor* cursor_{nullptr};
  TSTree* tree_{nullptr};
  Index revision_{-1};
  bool timed_out_{false};
  bool query_truncated_{false};
  // Live only between StartQueryBudget() and NoteQueryBudget(); the calls do
  // not nest, and Captures' own cursor is gone before either returns.
  Deadline query_deadline_{};
  // Whether the deadline in force is a caller's rather than this frame's, which
  // is what decides who a run that stopped short belongs to. See NoteQueryBudget.
  bool query_budget_borrowed_{false};
  // Whether any cursor run under the budget now open had to recycle a capture
  // list. A member and not a local because a paint runs one cursor several times
  // and tree-sitter clears the flag on every exec; see PaintTree.
  bool query_dropped_matches_{false};
  TSQueryCursorOptions query_options_{};
  int full_parse_failures_{0};
  std::vector<Span> spans_;

  // The shared capture-id space. `names_[id - 1]` is the scope a theme resolves;
  // `base_ids_` and each layer's `ids` map that query's own capture indices into
  // it, so one scope means one id no matter which grammar produced it.
  std::vector<std::string> names_;
  std::map<std::string, CaptureId, std::less<>> ids_by_name_;
  std::vector<CaptureId> base_ids_;

  std::shared_ptr<CompiledQuery> injections_;
  bool injections_tried_{false};
  std::vector<Injection> injections_found_;
  std::string region_text_;
  std::map<std::string, Layer, std::less<>> layers_;
  // Keyed on the region's start byte. Emptied by Sync; see PaintInjections.
  std::map<Index, CachedRegion> region_trees_;
  // Where the regions the paints so far have found sit, sorted and disjoint, in
  // the coordinates of `spans_revision_` -- a *table* revision, not this
  // object's, because these are carried through the journal rather than rebuilt
  // by a parse. `spans_unmapped_` says the carrying failed and the offsets have
  // been dropped; see AdvanceSpans and InInjectedRegion.
  std::vector<InjectedSpan> injected_spans_;
  Index spans_revision_{-1};
  bool spans_unmapped_{false};
  Index injection_parses_{0};
  bool injections_truncated_{false};

  std::vector<CaptureId> painted_;
  Index painted_revision_{-1};
  Index painted_start_{0};
  Index painted_end_{0};
};



}

std::string GrammarFor(std::string_view written) {
  // A grammar name reaches dlopen: it is spliced into "libtree-sitter-<name>.so"
  // and into "queries/<name>/highlights.scm", and both are then looked for under
  // every runtime root. The name comes out of the document -- a fenced block's
  // info string, an html script/style type -- so it is checked here rather than
  // trusted: a grammar names a *file*, never a path, and `../../../../etc/passwd`
  // is not a language.
  if (written.empty() || (written.size() > kMaxGrammarName)) return {};
  std::string lowered{written};
  for (char& c : lowered) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  // `+` and `#` are in the set for c++ and c#: kLanguageAliases has to still be
  // able to see the first, and the second is a language name people write.
  const bool plain = std::ranges::all_of(lowered, [](unsigned char c) {
    return (std::isalnum(c) != 0) || (c == '_') || (c == '-') || (c == '+') || (c == '#');
  });
  if (!plain) return {};
  for (const LanguageAlias& alias : kLanguageAliases) {
    if (alias.written == lowered) return std::string{alias.grammar};
  }
  return lowered;
}

std::shared_ptr<Syntax> OpenSyntax(const fs::path& path, std::string& error) {
  return OpenSyntaxForLanguage(LanguageForPath(path), error);
}

std::shared_ptr<Syntax> OpenSyntaxForLanguage(std::string_view language, std::string& error) {
  error.clear();
  if (language.empty()) return nullptr;

  std::shared_ptr<CompiledQuery> compiled = CompileQuery(language, kHighlightQueryFiles, error);
  if (compiled == nullptr) return nullptr;

  TSParser* probe = ts_parser_new();
  const bool accepted = ts_parser_set_language(probe, LanguageOf(*compiled));
  ts_parser_delete(probe);
  if (!accepted) {
    error = "grammar rejected by this tree-sitter: " + std::string{language};
    return nullptr;
  }
  return std::make_shared<TreeSitterSyntax>(std::string{language}, std::move(compiled));
}

}
