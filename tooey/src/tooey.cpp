#define TB_IMPL
#define TB_OPT_ATTR_W 32
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#include <termbox2.h>
#pragma GCC diagnostic pop
#undef TB_IMPL

#include "printx.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <unigbrk.h>
#include <unistr.h>
#include <uniwidth.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "args.h"
#include "format.h"
#include "layout.h"
#include "mmap_stream.h"
#include "subprocess.h"
#include "text.h"

constexpr const char* kVersion = PROJECT_VERSION;

namespace tooey {

using namespace std::string_view_literals;

// ============================================================================
// 1. TYPES & DATA STRUCTURES
// ============================================================================
namespace theme {
constexpr uint32_t kPrimary         = TB_DEFAULT;
constexpr uint32_t kPrompt          = 0x84c2FF;
constexpr uint32_t kSelectionMarker = 0xED365B;
constexpr uint32_t kSelectionBg     = 0x444444;
constexpr uint32_t kSelectionFg     = TB_DEFAULT;
constexpr uint32_t kInfo            = 0xDAA520;
constexpr uint32_t kMuted           = 0x5F9EA0;
constexpr uint32_t kHairline        = 0x696969;
constexpr uint32_t kBg              = TB_DEFAULT;
}  // namespace theme

constexpr std::string_view kQueryPlaceholder{"{{@QUERY@}}"};
constexpr std::string_view kSelectedLinePlaceholder{"{{@SELECTION@}}"};
constexpr uint32_t kTruncationMark{0x2026};  // horizontal ellipsis
constexpr uint32_t kSelectionBar{0x258C};    // left half block, notches the current row
constexpr uint32_t kHairlineGlyph{0x2500};   // the readout rule
constexpr int kGutter{1};                    // breathing room, and what replaces the rules
constexpr int kTopPad{1};                    // blank row above the prompt
constexpr int kLoadPollMs{50};               // how often to look again once the producer goes quiet
constexpr int kLoadDrawMs{33};               // redraw ceiling while input streams in (~30fps)
constexpr size_t kContextLen{3};

enum class ListType { kInputStream, kQueryProcess, kAction, kUnknown };

// A captured stream plus the views into it. The views survive moves of this struct
// because MmapStream only moves a raw pointer.
struct StreamLines {
  std::optional<common::MmapStream> stream{std::nullopt};
  std::vector<std::string_view> lines{};
  size_t parsed_upto{0};  // an offset, so a buffer relocation leaves it correct
  int load_fd{-1};        // >= 0 while more input may still arrive

  bool IsLoading() const { return load_fd >= 0; }

  // Takes whatever load_fd can give without blocking and appends the records that
  // completed. Returns true if anything arrived.
  bool Pump(char delim);
};

struct StateInputStream {
  StreamLines input{};
  size_t selection_index{0};
};

struct StateQueryProcess {
  StreamLines output{};
  size_t selection_index{0};
};

struct StateAction {
  std::size_t selection_index{0};
};

struct State {
  ListType next_state{ListType::kUnknown};
  ListType previous_state{ListType::kUnknown};
  std::string query;
  size_t cursor_pos{0};
  std::string selection;
  size_t selection_index{0};
  bool execute_action_immediately{false};
  size_t immediate_action_index{0};
  bool finalize_exit{false};
  bool selected_with_enter{false};
  bool run_query_process{false};
  bool cancelled{false};
  bool reload_list{false};
};

struct EventHandlingContext {
  std::string& query;
  size_t& cursor_pos;
  size_t& selection_index;
  size_t& preview_scroll_offset;
  size_t max_scroll;
  const std::vector<std::string_view>& items;
  const Config& cfg;
  const Layout& layout;
  bool is_query_process;
};

struct ExecuteActionCommandResult {
  StreamLines err{};
  std::optional<State> state{std::nullopt};
  bool error{false};
};

struct Termbox {
  Termbox(const Termbox&) = delete;
  Termbox& operator=(const Termbox&) = delete;
  Termbox(Termbox&&) noexcept = delete;
  Termbox& operator=(Termbox&&) noexcept = delete;
  ~Termbox() { tb_shutdown(); }

  // `height` > 0 draws in that many rows at the bottom of the terminal instead of
  // taking it over. tb_width()/tb_height() then report the viewport, so every
  // layout and drawing path stays unchanged.
  static bool Init(int height = 0) {
    if (instance_)
      return true;
    int tb_code = tb_init_inline_file("/dev/tty", height);
    if (tb_code != 0) {
      rostd::printf<"Termbox failed to initialize (error code: %d)\n">(tb_code);
      return false;
    }
    tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
    tb_set_input_mode(TB_INPUT_MOUSE | TB_INPUT_ESC);
    instance_ = std::unique_ptr<Termbox>(new Termbox{});
    return true;
  }

  static void Shutdown() { instance_.reset(); }

 private:
  Termbox() = default;
  static inline std::unique_ptr<Termbox> instance_{nullptr};
};

// ============================================================================
// 2. DRAWING & UI RENDERING
// ============================================================================
static void PrintUt8String(int x, int y, int max_x, uint32_t fg, uint32_t bg, std::string_view str, bool ansi = false) {
  int current_x = x;
  const int limit_x = std::min(max_x, tb_width());
  const char* ptr = str.data();
  const char* end = ptr + str.size();

  AnsiColorState color_state{fg, bg, 0};

  while (ptr < end && current_x < limit_x) {
    if (ansi) {
      size_t consumed = ParseAnsiSequence(ptr, end, fg, bg, color_state);
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
      int tab_spaces = 8 - ((current_x - x) % 8);
      for (int i = 0; i < tab_spaces && current_x < limit_x; ++i) {
        tb_set_cell(current_x++, y, ' ', color_state.fg | color_state.style, color_state.bg);
      }
      ptr++;
      continue;
    }

    const DecodedChar dc = DecodeUtf8(ptr, end);
    const int char_width = CellWidth(dc.cp);
    if (char_width < 0) {
      ptr += dc.len;
      continue;
    }

    tb_set_cell(current_x, y, dc.cp, color_state.fg | color_state.style, color_state.bg);
    current_x += char_width;
    ptr += dc.len;
  }

  // Mark content we had to cut, so a clipped line reads differently from a short
  // one. Only the leftover bytes matter here; a line that ends exactly at the edge
  // is not marked.
  if ((ptr < end) && (limit_x > x)) {
    tb_set_cell(limit_x - 1, y, kTruncationMark, theme::kMuted, color_state.bg);
  }
}

static void DrawPromptLine(std::string_view query, std::string_view prompt, size_t cursor_pos, const Rect& bounds) {
  const int text_x = bounds.x + kGutter;
  const int text_y = bounds.y + kTopPad;
  const int limit_x = bounds.x + bounds.w;

  PrintUt8String(text_x, text_y, limit_x, theme::kPrompt, theme::kBg, prompt);
  const int prompt_cells = CellWidthUtf8String(prompt);

  if (!query.empty()) {
    PrintUt8String(text_x + prompt_cells, text_y, limit_x, theme::kPrimary, theme::kBg, query);
  }

  const int cursor_cells = CellWidthUtf8String(query.substr(0, std::min(cursor_pos, query.size())));
  tb_set_cursor(text_x + prompt_cells + cursor_cells, text_y);
}

// A readout rather than a corner counter: the value plus a hairline running out to the
// edge. Sits on the row the prompt's blank line used to occupy, so it costs no list
// space.
static void DrawStatusLine(size_t index, size_t total, bool loading, const Rect& bounds) {
  const int y = bounds.y + kTopPad + 1;
  const int limit_x = bounds.x + bounds.w;
  int x = bounds.x + kGutter;

  const char* value = loading ? common::FormatIntoCString<"%d/%d \xE2\x80\xA6">((total > 0) ? index + 1 : 0, total)
                              : common::FormatIntoCString<"%d/%d">((total > 0) ? index + 1 : 0, total);
  PrintUt8String(x, y, limit_x, theme::kInfo, theme::kBg, value);
  x += CellWidthUtf8String(value);

  for (int i = x + kGutter; i < limit_x - kGutter; ++i) {
    tb_set_cell(i, y, kHairlineGlyph, theme::kHairline, theme::kBg);
  }
}

// Static rows of caller-supplied text: the header above the list, the footer along the
// bottom. Escapes follow --ansi, the same switch the item and preview text obey, so one
// flag decides whether tooey renders escapes anywhere. Rows past the strip are dropped
// rather than pushing the list around.
static void DrawTextRows(const std::vector<std::string_view>& rows, bool ansi, const Rect& bounds) {
  const int limit_y = bounds.y + bounds.h;
  int y = bounds.y;

  for (const std::string_view row : rows) {
    if (y >= limit_y)
      return;
    PrintUt8String(bounds.x + kGutter, y++, bounds.x + bounds.w, theme::kMuted, theme::kBg, row, ansi);
  }
}

// bounds is the item strip alone, so the header and footer rows are already gone: what
// is left is exactly the rows this may fill.
static void DrawItems(const std::vector<std::string_view>& items, size_t index, bool ansi, const Rect& bounds) {
  const int available_rows = bounds.h;
  if (available_rows <= 0)
    return;

  const size_t s_available_rows = static_cast<size_t>(available_rows);
  size_t start_index = 0;

  if ((s_available_rows > kContextLen) && ((index + kContextLen) >= s_available_rows)) {
    start_index = (index + kContextLen + 1) - s_available_rows;
  } else if (s_available_rows <= kContextLen) {
    start_index = index;
  }
  const size_t end_index = std::min(items.size(), start_index + s_available_rows);

  int y = bounds.y;
  for (size_t i = start_index; i < end_index; i++, y++) {
    const bool is_selected = (i == index);

    // The current row is a dark field with bright text, and an item's own colours apply
    // over it, so a colourised item stays readable and legible spans pick up fg+.
    const uint32_t fg = is_selected ? theme::kSelectionFg : theme::kPrimary;
    const uint32_t bg = is_selected ? theme::kSelectionBg : theme::kBg;

    if (is_selected) {
      // Fill the whole row width, not just as far as the text reaches, or it reads as
      // a coloured phrase rather than a selected row.
      for (int x = bounds.x; x < bounds.x + bounds.w; ++x) {
        tb_set_cell(x, y, ' ', fg, bg);
      }
      tb_set_cell(bounds.x + kGutter, y, kSelectionBar, theme::kSelectionMarker, bg);
    }

    PrintUt8String(bounds.x + kGutter + 2, y, bounds.x + bounds.w, fg, bg, items[i], ansi);
  }
}

static void DrawPreview(const Rect& bounds, const std::vector<std::string_view>& lines, const Config& cfg,
                        size_t scroll_offset) {
  if (bounds.w <= 0 || bounds.h <= 0)
    return;
  int text_x = bounds.x, text_y = bounds.y, text_w = bounds.w, text_h = bounds.h;

  // Whitespace separates the panes; the muted body colour does the rest.
  if (cfg.preview_dir == "right") {
    text_x += kGutter;
    text_w -= kGutter;

  } else if (cfg.preview_dir == "left") {
    text_w -= kGutter;

  } else if (cfg.preview_dir == "top") {
    text_h -= 1;

  } else if (cfg.preview_dir == "bottom") {
    text_y += 1;
    text_h -= 1;
  }

  // Full strength text, same as the list: preview content is there to be read, and
  // the gutter is enough to separate the panes.
  for (size_t i = 0; i < static_cast<size_t>(text_h) && (i + scroll_offset) < lines.size(); ++i) {
    PrintUt8String(text_x, text_y + i, text_x + text_w, theme::kPrimary, theme::kBg, lines[i + scroll_offset],
                   cfg.ansi);
  }
}

// ============================================================================
// 3. EVENT HANDLING & LOGIC
// ============================================================================
static std::vector<std::string_view> ExtractInputItems(const common::MmapStream& stream, char delim) {
  const char* const begin = stream.buffer;
  const char* const end = stream.buffer + stream.size;
  std::vector<std::string_view> items;

  const char* tail = AppendCompleteItems(begin, end, delim, items);
  if (tail < end)
    items.emplace_back(tail, end - tail);  // last record, no delimiter
  return items;
}

bool StreamLines::Pump(char delim) {
  if (load_fd < 0)
    return false;
  if (!stream.has_value())
    stream.emplace();

  const common::ReadProgress progress = stream->ReadAvailable(load_fd);

  // Growing the mapping may have relocated it. mremap preserves offsets, so every
  // view is off by the same constant and one pass puts them all right. This is why
  // the buffer and its views live in the same struct.
  if (progress.delta != 0) {
    for (auto& line : lines) {
      line = std::string_view(line.data() + progress.delta, line.size());
    }
  }

  // Only the newly arrived bytes need splitting, and only up to the last delimiter:
  // a partial record waits for the rest of itself to turn up.
  const char* const base = stream->buffer;
  const char* const end = base + stream->size;
  const char* tail = AppendCompleteItems(base + parsed_upto, end, delim, lines);

  const bool done = progress.eof || progress.error;
  if (done && (tail < end)) {
    lines.emplace_back(tail, end - tail);
    tail = end;
  }

  parsed_upto = static_cast<size_t>(tail - base);
  if (done)
    load_fd = -1;
  return progress.new_bytes > 0;
}

// The query process and --select-1 both need the whole corpus, so block until it has
// all arrived. Reverting to a blocking fd is what keeps this from spinning on EAGAIN.
static void FinishLoading(StreamLines& list, char delim) {
  if (!list.IsLoading())
    return;

  const int flags = fcntl(list.load_fd, F_GETFL);
  if (flags != -1)
    fcntl(list.load_fd, F_SETFL, flags & ~O_NONBLOCK);
  while (list.IsLoading())
    list.Pump(delim);
}

static State CreateExitState(std::string_view query, size_t cursor_pos, const std::vector<std::string_view>& items,
                             size_t selection_index, bool finalize, bool with_enter) {
  return State{.next_state = ListType::kUnknown,
               .previous_state = ListType::kUnknown,
               .query = std::string{query},
               .cursor_pos = cursor_pos,
               .selection = items.empty() ? std::string{} : std::string{items[selection_index]},
               .selection_index = selection_index,
               .execute_action_immediately = false,
               .immediate_action_index = 0,
               .finalize_exit = finalize,
               .selected_with_enter = with_enter,
               .run_query_process = false};
}

static State CreateExitState(const EventHandlingContext& ctx, bool finalize, bool with_enter) {
  return CreateExitState(ctx.query, ctx.cursor_pos, ctx.items, ctx.selection_index, finalize, with_enter);
}

static std::optional<State> HandleListMouse(const tb_event& ev, EventHandlingContext& ctx) {
  if (ev.key == TB_KEY_MOUSE_WHEEL_UP) {
    if (ctx.layout.has_preview && IsInside(ctx.layout.preview, ev.x, ev.y)) {
      if (ctx.preview_scroll_offset > 0)
        ctx.preview_scroll_offset--;
    } else {
      ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), false);
    }

  } else if (ev.key == TB_KEY_MOUSE_WHEEL_DOWN) {
    if (ctx.layout.has_preview && IsInside(ctx.layout.preview, ev.x, ev.y)) {
      if (ctx.preview_scroll_offset < ctx.max_scroll)
        ctx.preview_scroll_offset++;
    } else {
      ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), true);
    }
  }

  return std::nullopt;
}

// Handles the CSI sequences termbox does not decode for us. Returns the event to
// dispatch, unchanged unless the sequence mapped onto a key. SGR mouse sequences
// are decoded by termbox itself (TB_INPUT_MOUSE) and arrive as TB_EVENT_MOUSE, so
// they never reach here.
static tb_event HandleListCsi(const std::string& csi, EventHandlingContext& ctx, tb_event current_ev) {
  tb_event synthetic_ev = current_ev;

  if (csi == "1;5D") {
    ctx.cursor_pos = PrevWord(ctx.query, ctx.cursor_pos);

  } else if (csi == "1;5C") {
    ctx.cursor_pos = NextWord(ctx.query, ctx.cursor_pos);

  } else if ((csi == "A") || (csi == "B") || (csi == "C") || (csi == "D")) {
    // A burst of input (a held key, a paste) can split an escape sequence across
    // termbox's 64 byte read buffer, and its front half arrives as a bare ESC. Turn
    // the reassembled tail back into the arrow it was, or the keypress is lost. The
    // lone ESC carried a CTRL modifier which must not leak into the arrow.
    synthetic_ev.key = (csi == "A")   ? TB_KEY_ARROW_UP
                       : (csi == "B") ? TB_KEY_ARROW_DOWN
                       : (csi == "C") ? TB_KEY_ARROW_RIGHT
                                      : TB_KEY_ARROW_LEFT;
    synthetic_ev.ch = 0;
    synthetic_ev.mod = 0;

  } else if (csi.size() >= 5 && csi.ends_with('u')) {
    int key_code = 0, mod = 0;
    if (sscanf(csi.c_str(), "%d;%du", &key_code, &mod) == 2) {
      if ((mod == 5 || mod == 3 || mod == 7) && key_code >= '1' && key_code <= '9') {
        synthetic_ev.ch = key_code;
        synthetic_ev.key = 0;
        if (mod == 5 || mod == 7)
          synthetic_ev.mod |= TB_MOD_CTRL;
        if (mod == 3 || mod == 7)
          synthetic_ev.mod |= TB_MOD_ALT;
      }
    }
  }

  return synthetic_ev;
}

static std::optional<State> HandleListKeyboard(tb_event ev, EventHandlingContext& ctx) {
  bool is_alt = (ev.mod & TB_MOD_ALT) != 0;
  bool is_ctrl = (ev.mod & TB_MOD_CTRL) != 0;

  if (ev.key == TB_KEY_ESC) {
    tb_event next_ev;
    if ((tb_peek_event(&next_ev, 50) == TB_OK) && (next_ev.type == TB_EVENT_KEY)) {
      if (next_ev.ch == '[') {
        std::string csi;
        tb_event csi_ev;
        while (tb_peek_event(&csi_ev, 25) == TB_OK && csi_ev.type == TB_EVENT_KEY) {
          if (csi_ev.ch != 0)
            csi += static_cast<char>(csi_ev.ch);
          if (csi_ev.ch >= 0x40 && csi_ev.ch <= 0x7E)
            break;
        }
        ev = HandleListCsi(csi, ctx, ev);
      } else {
        is_alt = true;
        ev = next_ev;
      }
    } else {
      State exit_state = CreateExitState(ctx, true, false);
      exit_state.selection = "";
      exit_state.cancelled = true;
      return exit_state;
    }
  }

  if (is_alt && ev.ch != 0) {
    for (size_t i = 0; i < ctx.cfg.actions.size(); ++i) {
      if (ctx.cfg.actions[i].key != ev.ch)
        continue;
      State next = CreateExitState(ctx, false, false);
      next.next_state = ListType::kAction;
      next.previous_state = ctx.is_query_process ? ListType::kQueryProcess : ListType::kInputStream;
      next.execute_action_immediately = true;
      next.immediate_action_index = i;
      return next;
    }
  }

  if (is_alt) {
    if ((ev.ch == 'e') || (ev.ch == 'E')) {
      State next = CreateExitState(ctx, false, false);
      next.next_state = ListType::kAction;
      next.previous_state = ctx.is_query_process ? ListType::kQueryProcess : ListType::kInputStream;
      return next;

    } else if ((ev.ch == 'q') || (ev.ch == 'Q')) {
      State next = CreateExitState(ctx, false, false);
      next.next_state = ListType::kQueryProcess;
      next.previous_state = ctx.is_query_process ? ListType::kQueryProcess : ListType::kInputStream;
      next.run_query_process = true;
      return next;
    }

  } else if (ev.key == TB_KEY_CTRL_A) {
    ctx.cursor_pos = 0;

  } else if (ev.key == TB_KEY_CTRL_E) {
    ctx.cursor_pos = ctx.query.size();

  } else if (ev.key == TB_KEY_CTRL_W) {
    if (ctx.cursor_pos > 0) {
      size_t prev_pos = PrevWord(ctx.query, ctx.cursor_pos);
      ctx.query.erase(prev_pos, ctx.cursor_pos - prev_pos);
      ctx.cursor_pos = prev_pos;
    }

    if (ctx.is_query_process && ctx.query.empty()) {
      State next = CreateExitState(ctx, false, false);
      next.next_state = ListType::kInputStream;
      return next;
    }

  } else if (ev.key == TB_KEY_ARROW_LEFT) {
    ctx.cursor_pos =
        (ev.mod & TB_MOD_CTRL) ? PrevWord(ctx.query, ctx.cursor_pos) : PrevGrapheme(ctx.query, ctx.cursor_pos);

  } else if (ev.key == TB_KEY_ARROW_RIGHT) {
    ctx.cursor_pos =
        (ev.mod & TB_MOD_CTRL) ? NextWord(ctx.query, ctx.cursor_pos) : NextGrapheme(ctx.query, ctx.cursor_pos);

  } else if (ev.key == TB_KEY_PGUP) {
    int page_size = std::max(1, ctx.layout.preview.h - 2);
    ctx.preview_scroll_offset =
        (ctx.preview_scroll_offset > static_cast<size_t>(page_size)) ? ctx.preview_scroll_offset - page_size : 0;

  } else if (ev.key == TB_KEY_PGDN) {
    int page_size = std::max(1, ctx.layout.preview.h - 2);
    ctx.preview_scroll_offset = std::min(ctx.preview_scroll_offset + page_size, ctx.max_scroll);

  } else if (ev.key == TB_KEY_ARROW_UP) {
    ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), false);

  } else if (ev.key == TB_KEY_ARROW_DOWN) {
    ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), true);

  } else if ((ev.key == TB_KEY_BACKSPACE) || (ev.key == TB_KEY_BACKSPACE2)) {
    if (ctx.cursor_pos > 0) {
      size_t prev_pos = PrevGrapheme(ctx.query, ctx.cursor_pos);
      ctx.query.erase(prev_pos, ctx.cursor_pos - prev_pos);
      ctx.cursor_pos = prev_pos;
    }

    if (ctx.is_query_process && ctx.query.empty()) {
      State next = CreateExitState(ctx, false, false);
      next.next_state = ListType::kInputStream;
      return next;
    }

  } else if (ev.ch != 0) {
    char buf[7];
    int len = u8_uctomb(reinterpret_cast<uint8_t*>(buf), static_cast<ucs4_t>(ev.ch), sizeof(buf));
    if (len > 0) {
      ctx.query.insert(ctx.cursor_pos, buf, len);
      ctx.cursor_pos += len;
    }

  } else if (((ev.key == TB_KEY_ENTER) || (ctx.cfg.tab_accept && (ev.key == TB_KEY_TAB))) && !ctx.items.empty()) {
    return CreateExitState(ctx, true, ev.key == TB_KEY_ENTER);
  } else if (ev.key == TB_KEY_TAB) {
    if (ev.mod & TB_MOD_SHIFT) { // up
      ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), false);
    } else { // down
      ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), true);
    }
  } else if (ev.key == TB_KEY_BACK_TAB) { // up
    ctx.selection_index = StepWrapped(ctx.selection_index, ctx.items.size(), false);
  }
  return std::nullopt;
}

static std::vector<std::string> BuildActionDisplayStrings(const std::vector<Action>& actions) {
  std::vector<std::string> strings(actions.size());
  for (size_t i = 0; i < actions.size(); ++i) {
    // An unbound action pads to the width of "alt-X  " so every name starts in the
    // same column and the menu reads as two columns rather than a ragged edge.
    if (actions[i].key != 0) {
      strings[i] =
          std::string{common::FormatIntoStringView<"alt-%c  %s">(static_cast<char>(actions[i].key), actions[i].name)};
    } else {
      strings[i] = std::string{common::FormatIntoStringView<"%7s%s">("", actions[i].name)};
    }
  }
  return strings;
}

static std::vector<std::string> BuildCommandPreviewLines(const Action& act, std::string_view query,
                                                         std::string_view selection) {
  std::string cmd{act.command};
  std::string safe_query{query};
  std::string safe_selection{selection};

  ShellEscapeInPlace(safe_query);
  ShellEscapeInPlace(safe_selection);
  SubstituteInPlace(kQueryPlaceholder, safe_query, cmd);
  SubstituteInPlace(kSelectedLinePlaceholder, safe_selection, cmd);

  std::vector<std::string> lines;
  if (act.is_become) lines.push_back("[Become Command]");
  else if (act.is_interactive) lines.push_back("[Run Interactive Command]");
  else {
    lines.push_back("[Run Command]");
  }
  lines.push_back("");

  for (auto line : cmd | std::views::split('\n')) {
    lines.emplace_back(line.begin(), line.end());
  }
  return lines;
}

static ExecuteActionCommandResult ExecuteActionCommand(const Action& act, const State& user_state, const Config& cfg) {
  std::string cmd{act.command};
  std::string safe_query{user_state.query};
  std::string safe_selection{user_state.selection};

  ShellEscapeInPlace(safe_query);
  ShellEscapeInPlace(safe_selection);
  SubstituteInPlace(kQueryPlaceholder, safe_query, cmd);
  SubstituteInPlace(kSelectedLinePlaceholder, safe_selection, cmd);

  if (act.is_become) {
    Termbox::Shutdown();
    common::BecomeCommand(cmd);
    exit(EXIT_SUCCESS);
  }

  common::CmdResult cmd_result;

  if (act.is_interactive) {
    Termbox::Shutdown();
    cmd_result = common::RunCmdInteractive(cmd);
    if (!Termbox::Init(cfg.height)) {
      ExecuteActionCommandResult result;
      State exit_st = user_state;
      exit_st.finalize_exit = true;
      exit_st.cancelled = true;
      result.state = exit_st;
      return result;
    }
  } else {
    cmd_result = common::RunCmdWithCapture(cmd, common::CaptureMode::kDevNull,
                                           common::CaptureMode::kPipe);
  }

  // Failure is the command's exit status, not the fact that it printed something:
  // a successful action is free to write to the screen. What comes back here is the
  // command's stderr, which the user has already seen go past -- showing it again is
  // for the case where the next frame wiped it before it could be read.
  if (cmd_result.exit_status != 0) {
    std::vector<std::string_view> err_lines;
    err_lines.push_back("[Action Failed]");
    err_lines.push_back("");

    if (cmd_result.output.has_value() && cmd_result.output->size > 0) {
      auto err_items = ExtractInputItems(cmd_result.output.value(), '\n');
      err_lines.insert(err_lines.end(), err_items.begin(), err_items.end());
    } else {
      // Safe to hold: the helper's buffer has static duration and this format is
      // only instantiated here, so nothing else can overwrite it under us.
      err_lines.push_back(common::FormatIntoStringView<"(no output, exit status %d)">(cmd_result.exit_status));
    }

    ExecuteActionCommandResult result;
    result.err.stream = std::move(cmd_result.output);
    result.err.lines = std::move(err_lines);
    result.error = true;
    return result;
  }

  ExecuteActionCommandResult result;
  State exit_st = user_state;
  exit_st.next_state = user_state.previous_state;
  exit_st.execute_action_immediately = false;
  // An action is usually run to change something, so the list behind it is now
  // stale. Refresh whichever source produced it.
  exit_st.reload_list = true;
  result.state = exit_st;
  return result;
}

// ============================================================================
// 4. WINDOW RUNNERS & STATE MACHINE
// ============================================================================
static StreamLines RunPreviewCmd(std::string_view query, std::string_view selection, const Config& cfg) {
  StreamLines result;
  if (cfg.preview_cmd.empty() || selection.empty())
    return result;

  std::string cmd{cfg.preview_cmd};
  std::string safe_query{query};
  std::string safe_selection{selection};

  ShellEscapeInPlace(safe_query);
  ShellEscapeInPlace(safe_selection);
  SubstituteInPlace(kQueryPlaceholder, safe_query, cmd);
  SubstituteInPlace(kSelectedLinePlaceholder, safe_selection, cmd);

  auto cmd_result = common::RunCmdWithCapture(cmd, common::CaptureMode::kPipe, common::CaptureMode::kPipe);
  if (cmd_result.output.has_value()) {
    result.lines = ExtractInputItems(cmd_result.output.value(), '\n');
    result.stream = std::move(cmd_result.output);
  }
  return result;
}

// Everything this window produces leaves through the returned State, including the
// selection index the caller needs to persist across window switches.
// `list` is what is on screen; `source` is the input stream, which keeps loading
// regardless of which list that is. They are the same object for the input list.
static State RunListWindowCore(StreamLines& list, StreamLines& source, size_t initial_selection_index,
                               std::string query, size_t initial_cursor_pos, const Config& cfg, bool is_query_process) {
  const std::vector<std::string_view>& items = list.lines;
  size_t selection_index = initial_selection_index;
  size_t last_index = 0;
  std::string last_query;
  bool preview_is_current = false;
  bool seen_any_items = !list.lines.empty();
  StreamLines preview;
  size_t preview_scroll_offset = 0;
  size_t cursor_pos = std::min(initial_cursor_pos, query.size());
  tb_event ev;

  // The header and footer cannot change while the window is up, so they are split once
  // here rather than on every frame; only their placement is recomputed, on resize.
  const std::vector<std::string_view> header_rows = SplitTextRows(cfg.header);
  const std::vector<std::string_view> footer_rows = SplitTextRows(cfg.footer);

  auto last_keypress_time = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point last_draw_time{};  // epoch: force the first frame
  bool query_changed_since_last_exec = false;
  std::string executing_query = query;

  while (true) {
    // Pull whatever input has arrived since the last frame. Items only ever get
    // appended, so the selection stays on the same item. This runs even when the
    // query process list is on screen: stop pumping and the producer would block on
    // a full pipe, so the corpus would never finish arriving.
    bool input_flowing = false;
    if (source.IsLoading()) {
      input_flowing = source.Pump(cfg.split_delim);

      // The query process ran against a partial corpus. Now that all of it is here,
      // run it once more so the results cover everything.
      if (!source.IsLoading() && is_query_process) {
        State next = CreateExitState(query, cursor_pos, items, selection_index, false, false);
        next.next_state = ListType::kQueryProcess;
        next.previous_state = ListType::kQueryProcess;
        next.run_query_process = true;
        return next;
      }
    }

    // The first items to arrive give us something to preview: until then there was no
    // selection for the preview command to run against.
    if (!seen_any_items && !items.empty()) {
      seen_any_items = true;
      preview_is_current = false;
    }

    if (items.empty()) {
      selection_index = 0;
    } else if (selection_index >= items.size()) {
      selection_index = items.size() - 1;
    }

    if (!preview_is_current || (last_index != selection_index) || (last_query != executing_query)) {
      preview_is_current = true;
      last_index = selection_index;
      last_query = executing_query;
      preview_scroll_offset = 0;

      preview = RunPreviewCmd(executing_query, items.empty() ? "" : std::string_view(items[selection_index]), cfg);
    }

    Layout layout = CalculateLayout(cfg, tb_width(), tb_height(), false);
    size_t max_scroll = GetMaxPreviewScroll(layout, cfg, preview.lines.size());
    if (preview_scroll_offset > max_scroll)
      preview_scroll_offset = max_scroll;

    const auto frame_time = std::chrono::steady_clock::now();
    const bool draw_due =
        !source.IsLoading() ||
        std::chrono::duration_cast<std::chrono::milliseconds>(frame_time - last_draw_time).count() >= kLoadDrawMs;

    if (draw_due) {
      last_draw_time = frame_time;
      const ListStrips strips =
          SplitListPane(layout.main, static_cast<int>(header_rows.size()), static_cast<int>(footer_rows.size()));

      tb_clear();
      DrawPromptLine(query, cfg.prompt, cursor_pos, layout.main);
      DrawStatusLine(selection_index, items.size(), source.IsLoading(), layout.main);
      DrawTextRows(header_rows, cfg.ansi, strips.header);
      DrawItems(items, selection_index, cfg.ansi, strips.items);
      DrawTextRows(footer_rows, cfg.ansi, strips.footer);
      if (layout.has_preview) {
        DrawPreview(layout.preview, preview.lines, cfg, preview_scroll_offset);
      }
      tb_present();
    }

    EventHandlingContext ctx{query, cursor_pos, selection_index, preview_scroll_offset, max_scroll,
                             items, cfg,        layout,          is_query_process};
    std::optional<State> next_state_opt;

    int timeout_ms = -1;
    if (query_changed_since_last_exec) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keypress_time).count();
      timeout_ms = cfg.debounce_ms - static_cast<int>(elapsed);
      if (timeout_ms <= 0)
        timeout_ms = 1;
    }

    // Three states, cheapest wait for each:
    //   loaded          -> timeout_ms stays -1, so we block forever at no cost
    //   loading, flowing-> 0: never sleep between drains, that is what throttled a
    //                      pipe to a fraction of its throughput
    //   loading, stalled-> kLoadPollMs, the only polling tooey ever does. It costs at
    //                      most that much latency in noticing a producer resumed.
    if (source.IsLoading()) {
      const int load_wait = input_flowing ? 0 : kLoadPollMs;
      timeout_ms = (timeout_ms < 0) ? load_wait : std::min(timeout_ms, load_wait);
    }

    bool event_received = false;
    if (timeout_ms < 0) {
      event_received = (tb_poll_event(&ev) == TB_OK);
    } else {
      event_received = (tb_peek_event(&ev, timeout_ms) == TB_OK);
    }

    if (event_received) {
      auto process_event = [&](tb_event ev_in) {
        if (ev_in.type == TB_EVENT_KEY)
          return HandleListKeyboard(ev_in, ctx);
        if (ev_in.type == TB_EVENT_MOUSE)
          return HandleListMouse(ev_in, ctx);
        return std::optional<State>{};
      };

      std::string pre_query = query;
      next_state_opt = process_event(ev);

      while (!next_state_opt.has_value() && tb_peek_event(&ev, 1) == TB_OK) {
        next_state_opt = process_event(ev);
      }

      if (pre_query != query) {
        query_changed_since_last_exec = true;
        last_keypress_time = std::chrono::steady_clock::now();
      }
    }

    if (next_state_opt.has_value())
      return next_state_opt.value();

    if (query_changed_since_last_exec) {
      auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_keypress_time).count();

      if (elapsed >= cfg.debounce_ms) {
        query_changed_since_last_exec = false;
        executing_query = query;

        if (!cfg.query_process_cmd.empty()) {
          State next = CreateExitState(ctx, false, false);
          next.next_state = ListType::kQueryProcess;
          next.previous_state = ctx.is_query_process ? ListType::kQueryProcess : ListType::kInputStream;
          next.run_query_process = true;
          next.selection_index = 0;  // new query, new results: start at the top
          return next;
        }
      }
    }
  }
}

static State RunWindowInputStream(StateInputStream& state_is, State& current_state, const Config& cfg) {
  State next = RunListWindowCore(state_is.input, state_is.input, state_is.selection_index, current_state.query,
                                 current_state.cursor_pos, cfg, false);
  state_is.selection_index = next.selection_index;
  return next;
}

static StateQueryProcess BuildQueryProcessState(const State& current_state, const Config& cfg,
                                                StateInputStream& state_is) {
  // Deliberately does NOT wait for stdin to finish: blocking here would freeze the
  // UI for as long as the producer keeps writing. The child gets the corpus loaded
  // so far, and RunListWindowCore re-runs it once the rest arrives.
  StateQueryProcess state_qp{};
  std::string cmd{cfg.query_process_cmd};
  std::string safe_query{current_state.query};
  std::string safe_selection{current_state.selection};

  ShellEscapeInPlace(safe_query);
  ShellEscapeInPlace(safe_selection);
  SubstituteInPlace(kQueryPlaceholder, safe_query, cmd);
  SubstituteInPlace(kSelectedLinePlaceholder, safe_selection, cmd);

  auto cmd_result = common::RunCmdWithCapture(cmd, common::CaptureMode::kPipe, common::CaptureMode::kPipe,
                                              state_is.input.stream ? &state_is.input.stream.value() : nullptr);

  if (!cmd_result.output.has_value()) {
    state_qp.output.lines.emplace_back("Query process returned no value");
  } else {
    state_qp.output.lines = ExtractInputItems(cmd_result.output.value(), cfg.split_delim);
    state_qp.output.stream = std::move(cmd_result.output);
  }
  state_qp.selection_index = current_state.selection_index;
  return state_qp;
}

// Re-runs --reload-command and replaces the input list with its output. Only
// meaningful when there is no query process; that list is rebuilt by re-running the
// query process instead.
static void ReloadInputStream(StateInputStream& state_is, const Config& cfg) {
  if (cfg.reload_cmd.empty())
    return;

  auto cmd_result =
      common::RunCmdWithCapture(std::string{cfg.reload_cmd}, common::CaptureMode::kPipe, common::CaptureMode::kDevNull);
  if (!cmd_result.output.has_value())
    return;

  // Views first, then move the stream: moving an MmapStream only moves a pointer,
  // so the views stay valid.
  state_is.input.lines = ExtractInputItems(cmd_result.output.value(), cfg.split_delim);
  state_is.input.stream = std::move(cmd_result.output);
}

static State RunWindowQueryProcess(StateQueryProcess& state_qp, StateInputStream& state_is, State& current_state,
                                   const Config& cfg) {
  State next = RunListWindowCore(state_qp.output, state_is.input, state_qp.selection_index, current_state.query,
                                 current_state.cursor_pos, cfg, true);
  state_qp.selection_index = next.selection_index;
  return next;
}

static State RunWindowAction(StateAction& state, const State& user_state, const Config& cfg) {
  const auto display_strings = BuildActionDisplayStrings(cfg.actions);
  std::vector<std::string_view> display_views;
  display_views.reserve(display_strings.size());
  for (const auto& s : display_strings)
    display_views.push_back(s);

  bool immediate_exec = user_state.execute_action_immediately;
  if (immediate_exec) {
    if (user_state.immediate_action_index < cfg.actions.size()) {
      state.selection_index = user_state.immediate_action_index;
    } else {
      immediate_exec = false;
    }
  }

  tb_event ev;
  size_t last_index = static_cast<size_t>(-1);
  std::vector<std::string> cmd_preview_strs;
  std::vector<std::string_view> preview_lines;
  size_t preview_scroll_offset = 0;

  bool action_error_mode = false;
  StreamLines action_error;

  while (true) {
    if (action_error_mode) {
      preview_lines = action_error.lines;
    } else if (last_index != state.selection_index) {
      last_index = state.selection_index;
      preview_scroll_offset = 0;

      if (state.selection_index < cfg.actions.size()) {
        cmd_preview_strs =
            BuildCommandPreviewLines(cfg.actions[state.selection_index], user_state.query, user_state.selection);
        preview_lines.clear();
        preview_lines.reserve(cmd_preview_strs.size());
        for (const auto& s : cmd_preview_strs)
          preview_lines.push_back(s);
      }
    }

    Layout layout = CalculateLayout(cfg, tb_width(), tb_height(), true);
    size_t max_scroll = GetMaxPreviewScroll(layout, cfg, preview_lines.size());
    if (preview_scroll_offset > max_scroll)
      preview_scroll_offset = max_scroll;

    // The action list is tooey's own screen, so the caller's header and footer stay off
    // it: they describe the list they were written for.
    const ListStrips strips = SplitListPane(layout.main, 0, 0);

    tb_clear();
    DrawPromptLine("", "[Action] \xE2\x9D\xAF ", 0, layout.main);
    DrawStatusLine(state.selection_index, display_views.size(), false, layout.main);
    DrawItems(display_views, state.selection_index, cfg.ansi, strips.items);
    if (layout.has_preview) {
      DrawPreview(layout.preview, preview_lines, cfg, preview_scroll_offset);
    }
    tb_present();

    std::optional<State> next_state_opt;
    bool execute = immediate_exec;
    immediate_exec = false;

    auto process_event = [&](const tb_event& ev_in) {
      if (ev_in.type == TB_EVENT_KEY) {
        // The keypress that dismisses an error is consumed by the dismissal,
        // otherwise Enter would re-run the action that just failed.
        if (action_error_mode) {
          action_error_mode = false;
          last_index = static_cast<size_t>(-1);
          return;
        }

        if (ev_in.key == TB_KEY_ESC) {
          State exit_st = user_state;
          exit_st.next_state = user_state.previous_state;
          exit_st.execute_action_immediately = false;
          next_state_opt = exit_st;

        } else if (ev_in.key == TB_KEY_ARROW_UP) {
          state.selection_index = StepWrapped(state.selection_index, display_views.size(), false);

        } else if (ev_in.key == TB_KEY_ARROW_DOWN) {
          state.selection_index = StepWrapped(state.selection_index, display_views.size(), true);

        } else if ((ev_in.key == TB_KEY_ENTER) && !display_views.empty()) {
          execute = true;

        } else if (ev_in.ch != 0) {
          for (size_t i = 0; i < cfg.actions.size(); ++i) {
            if (cfg.actions[i].key == ev_in.ch) {
              state.selection_index = i;
              execute = true;
              break;
            }
          }
        }

      } else if (ev_in.type == TB_EVENT_MOUSE) {
        if (ev_in.key == TB_KEY_MOUSE_WHEEL_UP) {
          if (layout.has_preview && IsInside(layout.preview, ev_in.x, ev_in.y)) {
            if (preview_scroll_offset > 0)
              preview_scroll_offset--;
          } else {
            state.selection_index = StepWrapped(state.selection_index, display_views.size(), false);
          }

        } else if (ev_in.key == TB_KEY_MOUSE_WHEEL_DOWN) {
          if (layout.has_preview && IsInside(layout.preview, ev_in.x, ev_in.y)) {
            if (preview_scroll_offset < max_scroll)
              preview_scroll_offset++;
          } else {
            state.selection_index = StepWrapped(state.selection_index, display_views.size(), true);
          }
        }
      }
    };

    if (!execute && tb_poll_event(&ev) == TB_OK) {
      process_event(ev);
      while (!next_state_opt.has_value() && !execute && tb_peek_event(&ev, 1) == TB_OK) {
        process_event(ev);
      }
    }

    if (next_state_opt.has_value())
      return next_state_opt.value();

    if (execute) {
      if (state.selection_index < cfg.actions.size()) {
        auto [err, exit_st, err_mode] = ExecuteActionCommand(cfg.actions[state.selection_index], user_state, cfg);
        if (err_mode) {
          action_error_mode = true;
          action_error = std::move(err);
          preview_scroll_offset = 0;
        } else if (exit_st) {
          return exit_st.value();
        }
      }
    }
  }
}

}  // namespace tooey

// ============================================================================
// MAIN HELPERS & MAIN
// ============================================================================

// Asks the terminal how tall it is and hands the percentage arithmetic to
// tooey::RowsFromPercent. Resolved here rather than at init because the terminal
// cannot change size in between, and /dev/tty is opened explicitly because stdout
// is frequently a pipe.
static int InlineRowsFromPercent(std::string_view raw) {
  int term_rows = 0;
  const int fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    struct winsize ws{};
    if (ioctl(fd, TIOCGWINSZ, &ws) == 0)
      term_rows = ws.ws_row;
    close(fd);
  }
  return tooey::RowsFromPercent(raw, term_rows);
}

static tooey::Config ParseConfig(const common::Args& cli) {
  using namespace tooey;
  Config cfg;
  cfg.actions = ParseActions(cli.MultiValue({"--action"}, true).value_or(std::vector<std::string_view>{}));
  cfg.query_process_cmd = cli.Value({"--query-process-command"}).value_or("");
  cfg.preview_cmd = cli.Value({"--preview-command"}).value_or("");
  cfg.reload_cmd = cli.Value({"--reload-command"}).value_or("");
  cfg.prompt = cli.Value({"--prompt"}).value_or("\xE2\x9D\xAF ");
  cfg.header = cli.Value({"--header"}).value_or("");
  cfg.footer = cli.Value({"--footer"}).value_or("");
  cfg.split_delim = cli.Has("--read0") ? '\0' : '\n';
  cfg.tab_accept = cli.Has("--tab-accept");
  cfg.ansi = cli.Has("--ansi");
  cfg.preview_dir = cli.Value({"--preview-dir"}).value_or("right");

  if (auto v = cli.Value({"--debounce"}).and_then(ParseInt)) {
    cfg.debounce_ms = *v;
  }

  if (auto v = cli.Value({"--initial-list-pos"}).and_then(ParseInt)) {
    cfg.initial_pos = *v;
  }

  if (auto v = cli.Value({"--preview-size"}).and_then(ParseInt)) {
    cfg.preview_size = *v;
  }

  if (auto v = cli.Value({"--height"})) {
    cfg.height = InlineRowsFromPercent(*v);
  }

  return cfg;
}

static tooey::State InitializeState(const common::Args& cli, const tooey::Config& cfg,
                                    tooey::StateInputStream& state_is, tooey::StateQueryProcess& state_qp) {
  using namespace tooey;
  State state;
  state.query = std::string(cli.Value({"--query"}).value_or(""));
  state.cursor_pos = state.query.size();

  // Open on a given row so a caller can restore where the user was. Out of range is
  // clamped by the list window, so no validation is needed here.
  if (cfg.initial_pos > 0) {
    const size_t row = static_cast<size_t>(cfg.initial_pos - 1);
    state.selection_index = row;
    state_is.selection_index = row;
  }
  bool select_1 = cli.Has("--select-1");

  if (!cfg.query_process_cmd.empty() && !state.query.empty()) {
    FinishLoading(state_is.input, cfg.split_delim);

    state.next_state = ListType::kQueryProcess;
    state_qp = BuildQueryProcessState(state, cfg, state_is);
    state.run_query_process = false;

    if (select_1 && state_qp.output.stream.has_value() && (state_qp.output.lines.size() == 1)) {
      state.selection = std::string{state_qp.output.lines[0]};
      state.finalize_exit = true;
      state.selected_with_enter = true;
    }

  } else {
    state.next_state = ListType::kInputStream;

    if (select_1)
      FinishLoading(state_is.input, cfg.split_delim);
    if (select_1 && state_is.input.lines.size() == 1) {
      state.selection = std::string{state_is.input.lines[0]};
      state.finalize_exit = true;
      state.selected_with_enter = true;
    }
  }
  return state;
}

static tooey::State RunAppLoop(tooey::State state, const tooey::Config& cfg, tooey::StateInputStream& state_is,
                               tooey::StateQueryProcess& state_qp, tooey::StateAction& state_action) {
  using namespace tooey;
  bool run = !state.finalize_exit;

  while (run) {
    if (state.reload_list) {
      state.reload_list = false;
      ReloadInputStream(state_is, cfg);
      if (state.next_state == ListType::kQueryProcess) {
        state.run_query_process = true;
      }
    }

    switch (state.next_state) {
      case ListType::kInputStream:
        state = RunWindowInputStream(state_is, state, cfg);
        break;

      case ListType::kQueryProcess:
        if (state.run_query_process) {
          state_qp = BuildQueryProcessState(state, cfg, state_is);
          state.run_query_process = false;
        }
        state = RunWindowQueryProcess(state_qp, state_is, state, cfg);
        break;

      case ListType::kAction:
        state = RunWindowAction(state_action, state, cfg);
        break;

      default:
        run = false;
        break;
    }

    if (state.finalize_exit)
      run = false;
  }

  Termbox::Shutdown();
  return state;
}

// Prints the result and returns the process exit status: 0 when an item was
// selected, 130 when the user cancelled, 1 when there was nothing to select.
static int PrintExitResults(const tooey::State& state, const common::Args& cli) {
  if (state.cancelled)
    return 130;
  if (!state.finalize_exit || state.selection.empty())
    return EXIT_FAILURE;

  // Items may legitimately contain newlines (they do with --read0), which makes
  // newline-terminated output ambiguous; --print0 gives the caller a delimiter the
  // payload cannot contain.
  const char delim = cli.Has("--print0") ? '\0' : '\n';

  if (cli.Has("--print-key")) {
    rostd::printf<"%s%c">(state.selected_with_enter ? "enter" : "tab", delim);
  }
  if (cli.Has("--print-query")) {
    rostd::printf<"%s%c">(state.query, delim);
  }
  rostd::printf<"%s%c">(state.selection, delim);
  std::fflush(stdout);
  return EXIT_SUCCESS;
}

int main(int argc, char** argv) {
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: tooey [options]

Options:
      --query                            Initial query string
      --query-process-command            Command to run on query change
      --preview-command                  Command to run to show preview
      --reload-command                   Command to regenerate the input list after an action runs.
      --prompt                           Prompt string to render
      --header                           Fixed text between the readout and the list, for a
                                         keybinding legend or the like. A line break makes it
                                         several rows, written either as a real newline or as
                                         the two characters \n, so both "a \n b" and $'a\nb'
                                         give two rows. Colour needs --ansi, as elsewhere.
      --footer                           Fixed text along the bottom of the list, same rules.
                                         A header or footer is dropped, footer first, rather
                                         than leaving no room for items.
      --height                           Percentage of the terminal height to draw in, instead of
                                         taking over the screen. Opens under the cursor and leaves
                                         everything above it visible. '40' and '40%' are the same;
                                         values are clamped to 1..100.
      --read0                            Split input items on NUL instead of newline
      --ansi                             To display input ansi codes or not
      --action                           Declare an action, see Actions below. Repeatable.
      --print-query                      Whether to print query on exit along with selection
      --print0                           Terminate printed output with NUL instead of newline
      --print-key                        Print key that is used to select an item 'enter' or 'tab'
      --tab-accept                       Whether tab should be used for selection accept along with 'enter'
      --select-1                         Automatically select the only match and exit
      --debounce                         Debounce timeout in ms for query process (default 150)
      --initial-list-pos                 Open with row N selected, 1-based. Clamped to the list,
                                         so a stale value from a shorter list is harmless.
      --preview-dir                      Direction to display preview top, bottom, left, right
      --preview-size                     Percentage of terminal area to use for preview.
                                         If --preview-dir is left/right, this will be interpreted as terminal width %.
                                         else this will be interpreted as terminal height %.

  -h, --help                             Show this help message
      --version                          Print version number

Actions:
  --action '[alt-K:]NAME=COMMAND'    run COMMAND non-interactively, then return to the list
  --action '[alt-K:]NAME==COMMAND'   become: replace tooey with COMMAND
  --action '[alt-K:]NAME=!COMMAND'   run COMMAND interactively, then return to the list

  alt-K binds the action to that key. K is a single character, so alt-b is alt+b and
  alt-B is alt+shift+b: they are two different bindings. Digits work the same way.
  Drop the prefix and the action is reachable only from the action list.

  alt+e opens the action list and alt+q re-runs the query process; binding either key
  yourself takes precedence over those.

  In COMMAND, {{@SELECTION@}} and {{@QUERY@}} expand to the shell-quoted current item
  and query.

  An interactive action owns the terminal while it runs, so an editor, a pager or a `read -p`
  confirmation works with no redirection: tooey stands down, the command draws, tooey
  redraws. Its stderr is shown as it arrives and kept, so one that exits non-zero can
  show what it said. A become action instead keeps tooey's stdout, which is frequently
  a pipe, so one that needs the screen has to redirect it.

    tooey --action 'alt-o:Open=$EDITOR {{@SELECTION@}}' \
          --action 'alt-D:Delete=rm -i -- {{@SELECTION@}}' \
          --action 'alt-l:Log==git log --oneline -- {{@SELECTION@}}'

Keys: up/down move and wrap at either end, enter accepts, esc cancels.

Exit status: 0 an item was selected, 1 nothing was selected, 130 cancelled.
  )CLI";

  common::Args cli(argc, argv);
  if (cli.Has("-h") || cli.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }

  if (cli.Has("--version")) {
    rostd::printf<"%s">(kVersion);
    return EXIT_SUCCESS;
  }

  using namespace tooey;

  // A preview or action command is free to exit without draining what we hand it,
  // and our own final print can land on a closed pipe. Neither should kill us.
  std::signal(SIGPIPE, SIG_IGN);

  const Config cfg = ParseConfig(cli);

  StateInputStream state_is{};
  // With no pipe there is nothing to pick from, and reading the terminal would
  // just block until Ctrl-D with an empty screen. Otherwise stream it: the list is
  // shown and usable while the producer is still writing.
  if (!isatty(STDIN_FILENO)) {
    const int flags = fcntl(STDIN_FILENO, F_GETFL);
    if (flags != -1)
      fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    // Let the producer run ahead of us rather than stalling every 64 KB. Fails
    // harmlessly on a non-pipe stdin or if it exceeds fs/pipe-max-size.
    fcntl(STDIN_FILENO, F_SETPIPE_SZ, 1 << 20);
    state_is.input.load_fd = STDIN_FILENO;
    state_is.input.Pump(cfg.split_delim);  // whatever is already buffered
  }

  StateQueryProcess state_qp{};
  StateAction state_action{};

  const State initial_state = InitializeState(cli, cfg, state_is, state_qp);

  // --select-1 can settle everything without ever showing the UI.
  if (!initial_state.finalize_exit && !Termbox::Init(cfg.height))
    return EXIT_FAILURE;

  const State final_state = RunAppLoop(initial_state, cfg, state_is, state_qp, state_action);
  return PrintExitResults(final_state, cli);
}
