#ifndef KOI_UNICODE_H_
#define KOI_UNICODE_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <array>
#include <string_view>

#include "piece_doc.h"

namespace koi {

Index DocLength(const PieceTable& table);

std::string ReadDocRange(const PieceTable& table, Interval range);

bool IsGraphemeBoundary(const PieceTable& table, Index doc_pos);

Index NextGraphemeBoundary(const PieceTable& table, Index doc_pos);
Index PrevGraphemeBoundary(const PieceTable& table, Index doc_pos);

Index SnapToGraphemeBoundary(const PieceTable& table, Index doc_pos);

Index CountGraphemes(const PieceTable& table, Interval range);

void ReadDocRangeInto(const PieceTable& table, Interval range, std::string& out);

std::size_t NextGraphemeInString(std::string_view s, std::size_t pos);
std::size_t PrevGraphemeInString(std::string_view s, std::size_t pos);

int GraphemeWidth(std::string_view cluster);

Index DisplayWidth(std::string_view text, Index tab_width, Index start_column = 0);

Interval LineContentRange(const PieceTable& table, Index line);

Index ColumnForByte(const PieceTable& table, Index doc_pos, Index tab_width);

Index ColumnForByteFrom(const PieceTable& table, Index line_start, Index pos, Index tab_width);

Index ByteForColumn(const PieceTable& table, Index line, Index column, Index tab_width);

bool IsWellFormedUtf8(std::string_view s);

// -- what a terminal is allowed to see ---------------------------------------
//
// koi validates that text is well-formed UTF-8 and never that it is printable,
// and 0x1B is a perfectly well-formed scalar. Everything that reaches a
// terminal -- a cell, an --render-mode line, a sidebar row -- has to come
// through one of these two, or a file gets to drive the terminal: set its
// title, clear and redraw the screen, conceal text so what is read is not what
// is there, or write the clipboard through OSC 52.
//
// Substituted, not dropped: U+2400..U+241F are the Unicode pictures for the C0
// controls, U+2421 is the one for DEL, and C1 becomes U+FFFD. A byte that is
// in the file should look like it is.

// One grapheme cluster, for the per-cell path. Tab is substituted too: a cell
// is one column and a literal tab in one is never what was meant.
std::string_view PrintableCluster(std::string_view cluster);

// Bulk text, for the line-at-a-time paths. Tab is left alone here -- it is
// meaningful indentation in a rendered line, and a terminal treats it as a
// cursor move within the line rather than as an introducer.
//
// `more` says another chunk of the same text follows this call. A caller that
// hands over the whole of it at once leaves it false, gets the guarantee above
// unconditionally, and can ignore the result, which is then always 0.
//
// A caller that feeds the text in pieces must set it, and must prepend the
// returned count of trailing bytes -- 0..3, the start of a codepoint the chunk
// boundary cut in half -- to the bytes it passes next. Those bytes have not
// been written to `out`. This is not an optimisation: 0xC2 at the end of one
// chunk and 0x9B at the start of the next are U+009B, an eight-bit CSI, and
// filtering the two halves separately is what lets it through whole. The last
// chunk is passed with `more` false, which is what turns any leftover into
// U+FFFD instead of holding it back for a call that never comes.
std::size_t AppendPrintable(std::string& out, std::string_view text, bool more = false);

inline void AppendUtf8(std::string& out, std::uint32_t cp) {
  // Nothing above U+10FFFF and no surrogate has a UTF-8 encoding, and the
  // branches below would emit one anyway -- five-byte sequences past the top,
  // and the CESU-8 spelling of a surrogate. Both are ill-formed, and the only
  // thing standing between them and a document today is that every caller
  // happens to feed a real key code. Substituting keeps that a property of this
  // function rather than of its callers.
  if ((cp > 0x10FFFF) || ((cp >= 0xD800) && (cp <= 0xDFFF))) cp = 0xFFFD;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

}

#endif
