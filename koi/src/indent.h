#ifndef KOI_INDENT_H_
#define KOI_INDENT_H_

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "piece_doc.h"
#include "syntax.h"

namespace koi {

// What one *keystroke* may spend on tree indentation, across every query it runs
// -- the answer for the new line plus the nearby lines the hybrid heuristic
// measures against it, for every cursor the keystroke has.
//
// One budget per command and not per cursor, which is the thing to keep true
// when calling this. It used to be per cursor, and a budget that each of N
// carets mints fresh bounds nothing anybody waits on: twenty carets closing
// twenty deeply nested blocks measured 261 ms of query for one `}`, all of it
// between the key going down and the character appearing on the screen -- ten
// times the number this constant says. So the commands here take the clock once
// and hand each caret what is *left* of it (see the `budget` parameters below);
// a caret that arrives with none left declines, which costs it the bracket
// heuristic and not the keystroke.
//
// The size is syntax.cpp's frame budget rather than textobject.cpp's 500 ms, and
// for the opposite reason to the one that made textobject's large: this runs
// between two keystrokes and there is nothing to show for the wait -- the
// bracket heuristic is right behind it and answers instantly. Matching the frame
// budget also means a call that hands its deadline down to Syntax::Captures is
// the deadline that fires, and not one of five fresh 25 ms frame budgets under
// it. Exposed so that a caller can shrink it; nothing in the editor grows it.
inline constexpr auto kIndentBudget = std::chrono::milliseconds{25};

// How a level of indentation is spelled in one document: `insert_spaces`
// decides tabs or spaces, `tab_width` how wide either is. Both come off
// Document, where DetectIndentation sets them from what the file actually uses;
// this exists so indent.cpp does not have to know editor.h.
struct IndentStyle {
  Index tab_width{4};
  bool insert_spaces{true};
};

// Whether `language` has an `indents.scm` on the runtime path at all. A language
// without one -- diff, markdown -- is not a failure and has no error to show:
// the caller keeps its own heuristic. Answered from a per-language memo, so it
// is a lookup and not a stat once a language has been asked about.
bool HasIndentQuery(std::string_view language);

// The indentation for the line a newline typed at `cursor` would open, or
// nullopt when the tree engine cannot answer -- no `indents.scm` for the
// language, a position inside a region another grammar owns, a query that would
// not compile, a run that hit its budget, or a buffer the parser gave up on.
//
// Only the query that will not compile writes `error`, and it is the only one of
// them there is anything to say about: it is the user's own file and the message
// names the line it broke on. The rest are the engine declining, which the
// caller answers by keeping the heuristic it already had -- so a caller that
// shows `error` is not told about markdown, about a spent budget, or about a
// buffer too big to parse, on every keystroke. Untouched when nothing is wrong,
// which is what lets one string be passed across a loop of cursors.
//
// `budget` is what *this call* may spend, and every query it runs shares it --
// the one that answers, the ones the hybrid measures baselines with, and the
// walk between them. A caller looping over cursors passes the remainder of its
// own deadline rather than kIndentBudget each time; see kIndentBudget.
//
// `hybrid`: the tree decides the *difference* between this line and a nearby
// one, which is then applied to that line's real leading whitespace, so
// deliberate manual indentation and an incomplete query both survive. Falling
// back to the tree's absolute answer only when no nearby line is comparable.
//
// `aligned`, when asked for, says the answer is an absolute column an `@align`
// fixed rather than a count of levels. It is what a caller splitting an
// auto-paired closer onto a line of its own needs: a level count is a depth the
// caret goes one deeper than, while a column is where *both* lines belong --
// the next argument lines up under the first, and so does the `)`. Left alone
// when the call declines.
std::optional<std::string> TreeIndentForNewline(const PieceTable& table, Syntax& syntax,
                                                Index cursor, const IndentStyle& style,
                                                std::string& error,
                                                std::chrono::milliseconds budget = kIndentBudget,
                                                bool* aligned = nullptr);

// The indentation `line` should have as the document stands, for a caller that
// re-indents a line already on screen. Absolute rather than hybrid: the point of
// re-indenting a line is to overrule what is there, so there is nothing to be
// relative to. Same nullopt contract, and the same `budget` contract, as above:
// the caller re-indenting several carets' lines in one keystroke hands each of
// them what is left of the one deadline it took before the loop.
//
// `outdent_token`, when asked for, says whether the token that *begins* the line
// is the reason the answer is where it is -- a `}`, a `case`, an
// `access_specifier`, an `else` -- rather than the line merely sitting at that
// depth. It is helix's `is_outdent_token_at`, and it is what separates a line the
// editor may move on its own from one that belongs to whoever typed it. Left
// alone when the call declines.
std::optional<std::string> TreeIndentForLine(const PieceTable& table, Syntax& syntax, Index line,
                                             const IndentStyle& style, std::string& error,
                                             std::chrono::milliseconds budget = kIndentBudget,
                                             bool* outdent_token = nullptr);

// `indent` with one level added or taken off, in this document's units. Both
// work on the rendered string rather than on a level count, because the string
// may have come from an `@align`, where there is no level count to adjust: a
// caret being pushed inside an auto-paired bracket is one level in from wherever
// the closer landed, however that column was arrived at -- which is what the
// newline path uses IndentedOnce for. OutdentedOnce is its inverse and is what a
// line whose leading token dedents it will need.
std::string IndentedOnce(std::string_view indent, const IndentStyle& style);
std::string OutdentedOnce(std::string_view indent, const IndentStyle& style);

}

#endif
