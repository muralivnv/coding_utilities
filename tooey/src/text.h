#ifndef TOOEY_TEXT_H_
#define TOOEY_TEXT_H_

#include <termbox2.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <generator>

namespace tooey {

// Standard xterm 256-color basic 16 palette.
// Note: Black (index 0) is set to 0x010101 (very dark gray) instead of 0x000000 
// because termbox reads a plain 0 as "terminal default" and drops the color.
//                                                      black     red       green     yellow
//                                                      blue      magenta   cyan      white
inline constexpr std::array<uint32_t, 8> kAnsiColors = {0x010101, 0xCD0000, 0x00CD00, 0xCDCD00,
                                                        0x0000EE, 0xCD00CD, 0x00CDCD, 0xE5E5E5};

//                                                            br_black  br_red    br_green  br_yellow
//                                                            br_blue   br_magenta br_cyan  br_white
inline constexpr std::array<uint32_t, 8> kAnsiBrightColors = {0x7F7F7F, 0xFF0000, 0x00FF00, 0xFFFF00,
                                                              0x5C5CFF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};

// The 6x6x6 colour cube steps used by the xterm 256 colour palette.
inline constexpr std::array<uint32_t, 6> kAnsiCubeSteps = {0, 95, 135, 175, 215, 255};

constexpr uint32_t Ansi256ToRgb(int index) {
  if (index < 8)
    return kAnsiColors[index];
  if (index < 16)
    return kAnsiBrightColors[index - 8];

  if (index < 232) {
    const int i = index - 16;
    return (kAnsiCubeSteps[(i / 36) % 6] << 16) | (kAnsiCubeSteps[(i / 6) % 6] << 8) | kAnsiCubeSteps[i % 6];
  }

  const uint32_t grey = 8 + (10 * static_cast<uint32_t>(index - 232));
  return (grey << 16) | (grey << 8) | grey;
}

// The defaults match theme::kPrimary and theme::kBg, which are both TB_DEFAULT:
// tooey tints what it owns and never repaints the terminal's own colours.
struct AnsiColorState {
  uint32_t fg{TB_DEFAULT};
  uint32_t bg{TB_DEFAULT};
  uint32_t style{0};
};

// Consumes one CSI sequence at `ptr`, folding an SGR into `state`, and returns the
// bytes consumed -- 0 when `ptr` does not begin a CSI sequence, which is how callers
// tell "this was an escape" from "this is text". Non-SGR sequences are consumed and
// ignored so a stray cursor move cannot leak into the output as visible bytes.
size_t ParseAnsiSequence(const char* ptr, const char* end, uint32_t default_fg, uint32_t default_bg,
                         AnsiColorState& state);

// One decoded code point and the byte count it occupied. u8_mbtouc never reports
// failure for a non-empty input: malformed bytes come back as U+FFFD with the
// number of bytes to skip, so callers do not need a separate error path.
struct DecodedChar {
  uint32_t cp;
  int len;
};

DecodedChar DecodeUtf8(const char* ptr, const char* end);

// uc_width, not libc wcwidth: the latter is locale dependent and returns -1 for
// everything above ASCII unless setlocale(LC_CTYPE) has been called. Combining
// marks correctly measure 0 here, so summing per code point stays correct.
int CellWidth(uint32_t cp);

// Display columns, counting a tab to the next multiple of 8 and skipping SGR
// sequences when `ansi` is set, so a colourised line still measures its visible
// width.
int CellWidthUtf8String(std::string_view str, bool ansi = false);

// Cursor movement steps over grapheme clusters, not code points. Stepping by code
// point splits "e" from its combining accent and tears apart emoji ZWJ sequences
// and flags, so one keypress would delete half a character.
size_t PrevGrapheme(std::string_view str, size_t pos);
size_t NextGrapheme(std::string_view str, size_t pos);
size_t PrevWord(std::string_view str, size_t pos);
size_t NextWord(std::string_view str, size_t pos);

// Splits [begin, end) on `delim`, appending the complete records to `out`. Returns
// the start of the trailing incomplete record, which is `end` when the input ended
// on a delimiter. Empty records are skipped.
const char* AppendCompleteItems(const char* begin, const char* end, char delim, std::vector<std::string_view>& out);

// Wraps `s` in single quotes when it holds anything a shell would interpret,
// rendering embedded single quotes as '"'"'. A caller can splice the result
// straight into a command line.
void ShellEscapeInPlace(std::string& s);

// Replaces every occurrence of `search` in `s` with `replace`.
void SubstituteInPlace(std::string_view search, std::string_view replace, std::string& s);

// Leading integer of `sv`, or nullopt when it does not start with one. A trailing
// non-digit tail is ignored, which is what lets "40%" parse as 40.
std::optional<int> ParseInt(std::string_view sv);

// Given an input string, this function yields chunk view taking into account screen real-estate and string UTF8
std::generator<std::string_view> ChunkView(std::string_view str, int term_width, bool ansi);

}  // namespace tooey

#endif  // TOOEY_TEXT_H_
