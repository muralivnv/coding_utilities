#include "text.h"

#include <unigbrk.h>
#include <unistr.h>
#include <uniwidth.h>

#include <charconv>
#include <cstring>

namespace tooey {

size_t ParseAnsiSequence(const char* ptr, const char* end, uint32_t default_fg, uint32_t default_bg,
                         AnsiColorState& state) {
  // Check for the CSI (Control Sequence Introducer) prefix: ESC [
  if (ptr >= end || *ptr != '\x1B' || (ptr + 1 >= end) || ptr[1] != '[') {
    return 0;
  }

  const char* ansi_ptr = ptr + 2;

  // Use a stack-allocated array to prevent heap allocations in the render loop.
  // 16 parameters is more than enough for valid SGR sequences (e.g., RGB takes 5).
  std::array<int, 16> params{};
  size_t param_count = 0;
  int current_param = 0;
  bool has_param = false;

  auto push_param = [&]() {
    if (param_count < params.size()) {
      params[param_count++] = current_param;
    }
    current_param = 0;
    has_param = false;
  };

  while (ansi_ptr < end) {
    char c = *ansi_ptr;

    if (c >= '0' && c <= '9') {
      current_param = current_param * 10 + (c - '0');
      has_param = true;
    } else if (c == ';') {
      push_param();
    } else if (c >= 0x40 && c <= 0x7E) {
      // Any character in this range is a final dispatch character (terminates the CSI).
      if (has_param || param_count == 0) {
        push_param();
      }

      // We only care about 'm' (Select Graphic Rendition). We just consume and ignore others (like
      // cursor moves).
      if (c == 'm') {
        for (size_t i = 0; i < param_count; ++i) {
          int p = params[i];

          switch (p) {
            case 0:
              state = {default_fg, default_bg, 0};
              break;
            case 1:
              state.style |= TB_BOLD;
              break;
            case 3:
              state.style |= TB_ITALIC;
              break;
            case 4:
              state.style |= TB_UNDERLINE;
              break;
            case 7:
              state.style |= TB_REVERSE;
              break;
            case 22:
              state.style &= ~TB_BOLD;
              break;
            case 23:
              state.style &= ~TB_ITALIC;
              break;
            case 24:
              state.style &= ~TB_UNDERLINE;
              break;
            case 27:
              state.style &= ~TB_REVERSE;
              break;
            case 39:
              state.fg = default_fg;
              break;
            case 49:
              state.bg = default_bg;
              break;
            case 38:
            case 48: {
              bool is_fg = (p == 38);
              if (i + 1 < param_count && params[i + 1] == 2 && i + 4 < param_count) {
                // Truecolor RGB: 38;2;R;G;B
                uint32_t rgb = ((params[i + 2] & 0xFF) << 16) | ((params[i + 3] & 0xFF) << 8) | (params[i + 4] & 0xFF);
                if (is_fg)
                  state.fg = rgb;
                else
                  state.bg = rgb;
                i += 4;
              } else if (i + 1 < param_count && params[i + 1] == 5 && i + 2 < param_count) {
                // 256-color palette: 38;5;N
                const int idx = params[i + 2];
                if (idx >= 0 && idx <= 255) {
                  const uint32_t rgb = Ansi256ToRgb(idx);
                  if (is_fg)
                    state.fg = rgb;
                  else
                    state.bg = rgb;
                }
                i += 2;
              }
              break;
            }
            default:
              if (p >= 30 && p <= 37)
                state.fg = kAnsiColors[p - 30];
              else if (p >= 40 && p <= 47)
                state.bg = kAnsiColors[p - 40];
              else if (p >= 90 && p <= 97)
                state.fg = kAnsiBrightColors[p - 90];
              else if (p >= 100 && p <= 107)
                state.bg = kAnsiBrightColors[p - 100];
              break;
          }
        }
      }

      // Successfully parsed the sequence, return total bytes consumed.
      return (ansi_ptr + 1) - ptr;
    }

    ansi_ptr++;
  }

  // If we hit the end of the string without finding a terminator, consume what we scanned.
  return ansi_ptr - ptr;
}

DecodedChar DecodeUtf8(const char* ptr, const char* end) {
  ucs4_t uc = 0;
  const int len = u8_mbtouc(&uc, reinterpret_cast<const uint8_t*>(ptr), static_cast<size_t>(end - ptr));
  if (len <= 0)
    return {0xFFFD, 1};
  return {static_cast<uint32_t>(uc), len};
}

int CellWidth(uint32_t cp) {
  return uc_width(static_cast<ucs4_t>(cp), "UTF-8");
}

int CellWidthUtf8String(std::string_view str, bool ansi) {
  int width = 0;
  const char* ptr = str.data();
  const char* end = ptr + str.size();

  AnsiColorState dummy_state{};
  while (ptr < end) {
    if (ansi) {
      size_t consumed = ParseAnsiSequence(ptr, end, 0, 0, dummy_state);
      if (consumed > 0) {
        ptr += consumed;
        continue;
      }
    }

    if (*ptr == '\r') {
      ptr++;
      continue;
    }

    if (*ptr == '\t') {
      width += 8 - (width % 8);
      ptr++;
      continue;
    }

    const DecodedChar dc = DecodeUtf8(ptr, end);
    const int char_width = CellWidth(dc.cp);
    width += (char_width < 0) ? 0 : char_width;
    ptr += dc.len;
  }
  return width;
}

size_t PrevGrapheme(std::string_view str, size_t pos) {
  if (pos == 0 || str.empty())
    return 0;
  const auto* s = reinterpret_cast<const uint8_t*>(str.data());
  const uint8_t* prev = u8_grapheme_prev(s + pos, s);
  return prev ? static_cast<size_t>(prev - s) : 0;
}

size_t NextGrapheme(std::string_view str, size_t pos) {
  if (pos >= str.size())
    return str.size();
  const auto* s = reinterpret_cast<const uint8_t*>(str.data());
  const uint8_t* next = u8_grapheme_next(s + pos, s + str.size());
  return next ? static_cast<size_t>(next - s) : str.size();
}

size_t PrevWord(std::string_view str, size_t pos) {
  if (pos == 0)
    return 0;
  size_t p = pos;
  while (p > 0 && str[PrevGrapheme(str, p)] == ' ')
    p = PrevGrapheme(str, p);
  while (p > 0 && str[PrevGrapheme(str, p)] != ' ')
    p = PrevGrapheme(str, p);
  return p;
}

size_t NextWord(std::string_view str, size_t pos) {
  size_t len = str.size();
  size_t p = pos;
  while (p < len && str[p] != ' ')
    p = NextGrapheme(str, p);
  while (p < len && str[p] == ' ')
    p = NextGrapheme(str, p);
  return p;
}

const char* AppendCompleteItems(const char* begin, const char* end, char delim, std::vector<std::string_view>& out) {
  const char* ptr = begin;
  while (ptr < end) {
    const char* next = static_cast<const char*>(memchr(ptr, delim, end - ptr));
    if (next == nullptr)
      break;
    if (next != ptr)
      out.emplace_back(ptr, next - ptr);
    ptr = next + 1;
  }
  return ptr;
}

namespace {

consteval auto BuildSafeCharsTable() {
  std::array<bool, 256> table{};
  std::string_view safe_chars =
      "%+,-./"
      "0123456789:=@ABCDEFGHIJKLMNOPQRSTUVWXYZ_abcdefghijklmnopqrstuvwxyz";
  for (char c : safe_chars)
    table[static_cast<unsigned char>(c)] = true;
  return table;
}

}  // namespace

void ShellEscapeInPlace(std::string& s) {
  static constexpr auto kIsSafeChar = BuildSafeCharsTable();
  if (s.empty()) {
    s = "''";
    return;
  }

  bool needs_quoting = false;
  size_t single_quote_count = 0;
  for (char c : s) {
    if (!kIsSafeChar[static_cast<unsigned char>(c)])
      needs_quoting = true;
    if (c == '\'')
      single_quote_count++;
  }

  if (!needs_quoting)
    return;

  size_t old_size = s.size();
  size_t new_size = old_size + 2 + (single_quote_count * 4);
  s.resize(new_size);

  s[new_size - 1] = '\'';
  size_t write_idx = new_size - 2;

  for (ptrdiff_t read_idx = old_size - 1; read_idx >= 0; --read_idx) {
    if (s[read_idx] == '\'') {
      s[write_idx--] = '\'';
      s[write_idx--] = '"';
      s[write_idx--] = '\'';
      s[write_idx--] = '"';
      s[write_idx--] = '\'';
    } else {
      s[write_idx--] = s[read_idx];
    }
  }
  s[0] = '\'';
}

void SubstituteInPlace(std::string_view search, std::string_view replace, std::string& s) {
  if (search.empty() || s.empty())
    return;

  std::string result;
  result.reserve(s.length() + (replace.length() * 2));

  size_t start = 0;
  size_t pos;
  while ((pos = s.find(search, start)) != std::string::npos) {
    result.append(s, start, pos - start);
    result.append(replace);
    start = pos + search.length();
  }
  result.append(s, start, std::string::npos);

  s = std::move(result);
}

std::optional<int> ParseInt(std::string_view sv) {
  int val = 0;
  auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), val);
  if (ec == std::errc{})
    return val;
  return std::nullopt;
}

std::generator<std::string_view> ChunkView(std::string_view str, int term_width, bool ansi) {
  const char* ptr = str.data();
  const char* end = ptr + str.size();
  const char* chunk_start = ptr;
  int current_width = 0; // Width of the text in the current chunk
  int x_pos = 0;         // Absolute visual X coordinate for tab alignment
  AnsiColorState dummy_state{};

  while (ptr < end) {
    const char* next_ptr = ptr;
    int item_width = 0;

    if (ansi) {
      size_t consumed = ParseAnsiSequence(ptr, end, 0, 0, dummy_state);
      if (consumed > 0) {
        ptr += consumed;
        continue;
      }
    }

    // Ignore carriage returns
    if (*ptr == '\r') {
      ptr++;
      continue;
    }

    // Calculate width for Tabs vs standard UTF-8 characters
    if (*ptr == '\t') {
      item_width = 8 - (x_pos % 8);
      next_ptr = ptr + 1;
    } else {
      const DecodedChar dc = DecodeUtf8(ptr, end);
      const int char_width = CellWidth(dc.cp);
      item_width = (char_width < 0) ? 0 : char_width;
      next_ptr = ptr + dc.len;
    }

    // Yield the current chunk if the next character would overflow the screen.
    if (current_width + item_width > term_width && current_width > 0) {
      co_yield std::string_view(chunk_start, ptr - chunk_start);
      chunk_start = ptr;
      current_width = 0;
      x_pos = 2; // Continuation lines start with "↪ " (2 cells wide)     
      continue; 
    }

    current_width += item_width;
    x_pos += item_width;
    ptr = next_ptr;
  }

  // Yield any leftover text at the end of the string
  if (chunk_start < end) {
    co_yield std::string_view(chunk_start, end - chunk_start);
  }
}

}  // namespace tooey
