#ifndef KOI_SYNTAX_H_
#define KOI_SYNTAX_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"

namespace koi {

using CaptureId = std::uint16_t;
inline constexpr CaptureId kNoCapture = 0;

struct Capture {
  Index from{0};
  Index to{0};
  std::string_view name;

  // Which match of which pattern this capture fell out of. Two captures with
  // the same `match_id` were produced together by one match, which is the only
  // thing that pairs an `@align` with its `@anchor`: two such captures that
  // merely overlap say nothing, and an indent query has several of both live
  // over the same byte. `pattern_index` is what a pattern's `#set!` properties
  // are keyed by -- see PropertiesFor in query.h.
  //
  // Both are handed out by the query cursor that produced them, so they are
  // comparable within the results of one Captures() call and meaningless
  // across two. Defaulted so that a caller with no use for either -- text
  // objects want the span and the name -- keeps writing `Capture{from, to,
  // name}`.
  std::uint32_t match_id{0};
  std::uint32_t pattern_index{0};

  // Where the captured node's parent begins, or the node's own start when it is
  // the root. An indent scope written `(#set! "scope" "header")` opens at the
  // *parent's* first line rather than the node's own -- which is the only thing
  // that lets a brace-less body (`if (c)` on one line, `stmt;` on the next)
  // contain the very line being indented -- and a capture that carries nothing
  // but its own span cannot reach it.
  //
  // Filled only for a `keep_zero_width` call, and within it only for captures
  // whose pattern carries that `(#set! "scope" "header")`; every other capture
  // carries its own `from`, which is what a node with no parent carries anyway.
  // Not an economy: reaching a parent means walking down from the root of the
  // tree, and paying that per capture on a document-wide text-object query
  // spends the frame budget on an answer that caller never reads -- and hands
  // it back a third fewer captures for the trouble.
  Index parent_from{0};

  // Whether the node stops where it does because the parser ran out of document
  // rather than because it was closed: the last token under it is one the
  // grammar inserted, not one the buffer contains. An indent query runs
  // mid-keystroke, where `int main() {` parses as a compound_statement whose `}`
  // was invented at the caret; that node ends exactly where the newline is about
  // to go, and this is what tells it apart from a `foo();` that genuinely ends
  // there and owns nothing below it.
  //
  // Filled only for a `keep_zero_width` call, and false otherwise. Cheap to
  // compute -- it walks down the last-child chain, not up from the root -- but
  // gated with `parent_from` so that the two structure fields hold to one rule
  // between them: they say something only for the caller that asked for them.
  bool unterminated{false};
};

// Which grammar's rules a byte falls under, as far as anything knows -- see
// Syntax::InInjectedRegion. Three answers and not two because the third is a
// real state with no safe default: "there are injected regions in this document
// and I cannot say where they are" is not the same claim as "this byte is the
// host's", and collapsing it into either answer hands the caller a certainty
// nobody has.
enum class Injected : std::uint8_t {
  kNo,
  kYes,
  kUnknown,
};

struct Syntax {
  virtual ~Syntax() = default;

  virtual std::string_view Language() const = 0;

  virtual std::span<const std::string> CaptureNames() const = 0;

  virtual void Sync(const PieceTable& table) = 0;

  virtual void Paint(const PieceTable& table, Interval range, std::vector<CaptureId>& out) = 0;

  // Whether the *parse* gave up on this buffer: it wants more than the parse
  // budget, or more bytes than tree-sitter can address at all. While it holds
  // there is no tree, so Paint colours nothing and Captures fails with a
  // reason, and it is what stops Sync re-parsing a buffer already known to be
  // beyond it. Written by a parse and by nothing else. A query that ran out of
  // budget used to raise it too, which left "the parse gave up -- file too
  // large" standing behind a tree that was perfectly good; that half is
  // QueryTruncated below.
  virtual bool TimedOut() const = 0;

  // Whether the last query run on this object's own frame budget came back
  // short of what the tree holds -- the 25 ms a draw is allowed ran out, or a
  // cursor ran out of capture lists and recycled one that was still in use.
  // Either way spans that belong on screen are missing and the bytes they
  // covered are painted plain.
  //
  // Raised by a paint, or by a Captures call running on the frame budget rather
  // than on a deadline of its own, and lowered by the next parse of a changed
  // buffer. A Captures call under a caller's own deadline reports its cut
  // through `budget_exhausted` and does not raise this -- see there -- but a
  // recycled capture list raises it whoever was holding the clock: running out
  // of match slots is a fact about how densely this document matches, not about
  // who ran out of time.
  virtual bool QueryTruncated() const = 0;

  // Whether the last paint stopped looking for injected regions before it ran
  // out of them, because the document put more of them in one call than a frame
  // is willing to parse, or because the query that finds them ran out of match
  // slots and dropped some. What was not found is painted with the base grammar
  // alone. Reported like QueryTruncated(): raised by a paint, and lowered by
  // the next parse of a changed buffer.
  virtual bool InjectionsTruncated() const = 0;

  // How many injected-region parses have run since this Syntax was opened.
  // Observable so that a test can say what no timing assertion can: that
  // scrolling over a <script> body reuses its tree instead of re-parsing it.
  virtual Index InjectionParses() const = 0;

  // `budget_exhausted`, when given, says whether the match stopped partway and
  // `out` is therefore short -- because time ran out, or because the cursor ran
  // out of capture lists and recycled one still in use. Returning true with it
  // raised is a real state -- half the captures over the asked-for range -- and
  // a caller that cannot use a partial answer (the indent engine cannot: a
  // missing ancestor is a missing indent level, not a missing highlight) has no
  // other way to tell that from a range that genuinely had nothing more in it.
  // The two causes share one flag because they leave the caller in one state;
  // which of them it was is only ever an answer about the document, and
  // QueryTruncated is where that is kept.
  //
  // `keep_zero_width` keeps captures whose node spans no bytes. Off by default
  // because for everything that selects or paints text -- text objects,
  // highlights, symbols -- an empty span is a range nobody can put a cursor in
  // and dropping it is what keeps those callers from having to check. An indent
  // query is the one place it is the answer: mid-keystroke the parser completes
  // `if (c)` with a zero-width body whose only token is one it invented, and
  // that node is precisely the brace-less body the new line belongs to. It
  // carries `unterminated`, like every node whose last token the grammar
  // supplied.
  //
  // It doubles as the switch for the two structure fields on `Capture`,
  // `parent_from` and `unterminated`, which are filled for this call and
  // defaulted for every other -- see their declarations. One flag rather than
  // two because it is one question: an indent query is the caller that asks
  // about the shape of the tree, everything else asks about spans of text.
  //
  // `deadline`, when given, is an *upper bound* on when this call's query may
  // still be running: the match stops at whichever comes first, it or the frame
  // budget this would otherwise run under alone. For a caller that runs several
  // of these under one budget of its own -- the indent engine runs up to five
  // per cursor -- a per-call frame budget bounds no such caller, because every
  // call mints a fresh one; handing the caller's own deadline in is what makes
  // the sum of them add up to what the caller meant to spend. It can only ever
  // *shorten* the run, so it cannot be used to hold a frame open, and it applies
  // to this call and no other: the next Captures or Paint starts a frame budget
  // of its own as before. A cut run is reported through `budget_exhausted` as
  // any other is, and does *not* raise QueryTruncated(): the caller running out
  // of time says nothing about the document.
  virtual bool Captures(const PieceTable& table, std::span<const std::string_view> query_files,
                        Interval range, std::vector<Capture>& out, std::string& error,
                        bool* budget_exhausted = nullptr, bool keep_zero_width = false,
                        const std::chrono::steady_clock::time_point* deadline = nullptr) = 0;

  virtual bool InLiteralOrComment(Index pos) = 0;

  // Whether `pos` sits in a region another grammar owns -- a <script> body, a
  // fenced block. Asked by auto-indent, which has one query per document and so
  // has nothing to say about bytes the document's own grammar only sees as
  // `raw_text`: an html indent query indents a `<script>` body by html's rules,
  // which is worse than not indenting it at all.
  //
  // Answered from the spans the paints so far have found, carried forward
  // through the document's edit journal -- so a region stays known across the
  // edits and the Sync/Captures calls that follow the paint that found it, and
  // an edit before it moves it rather than losing it. `table` is what makes that
  // possible: the answer is wanted mid-keystroke, before anything has synced,
  // and the journal is the only thing that says where the bytes went.
  //
  // Two bounds remain, both about regions this has never been *told* about.
  // Discovery happens in Paint and nowhere else, so a region no frame has drawn
  // reads as the host's -- the same bound InLiteralOrComment's layer
  // consultation works under, and for the same reason; in the editor a frame is
  // drawn between every pair of keystrokes. And a paint refreshes only the
  // window it drew: outside it, what an earlier paint found still stands,
  // mapped.
  //
  // `kUnknown` is what is left when the journal cannot carry the spans -- it has
  // been trimmed past them, or the document moved backwards under them. It says
  // regions are known to exist and their whereabouts are not; a caller that
  // would do the wrong thing inside one must treat it as `kYes`. It is cleared
  // by the next paint, and a document with no injected region in it never
  // reports it.
  virtual Injected InInjectedRegion(const PieceTable& table, Index pos) = 0;
};

std::string_view LanguageForPath(const std::filesystem::path& path);

std::string_view CommentTokenFor(std::string_view language);

std::span<const std::string_view> KnownLanguages();

// The grammar to parse an injected region with, given what the document called
// it -- a fenced block's info string, an html script/style type -- or empty for
// anything that is not a plausible grammar name. Unknown languages are not an
// error: a fence can say anything, and `mermaid` simply has no grammar here.
std::string GrammarFor(std::string_view written);

std::shared_ptr<Syntax> OpenSyntax(const std::filesystem::path& path, std::string& error);

std::shared_ptr<Syntax> OpenSyntaxForLanguage(std::string_view language, std::string& error);

std::vector<std::filesystem::path> RuntimeRoots();

std::filesystem::path FindRuntimeFile(const std::filesystem::path& relative);

}

#endif
