#ifndef TOOEY_LAYOUT_H_
#define TOOEY_LAYOUT_H_

#include <cstdint>
#include <string_view>
#include <vector>

namespace tooey {

inline constexpr int kDefaultPreviewSize{50};
inline constexpr int kMinInlineRows{3};

struct Action {
  std::string_view name{};
  std::string_view command{};
  uint32_t key{0};  // the char after alt-, 0 when the action is reachable only from the list
  bool is_become{false};
};

struct Config {
  std::vector<Action> actions;
  std::string_view query_process_cmd{};
  std::string_view preview_cmd{};
  std::string_view reload_cmd{};
  std::string_view prompt{"\xE2\x9D\xAF "};  // U+276F
  std::string_view header{};                 // fixed rows between the readout and the list
  std::string_view footer{};                 // fixed rows along the bottom of the list pane
  std::string_view preview_dir{"right"};
  int preview_size{kDefaultPreviewSize};
  char split_delim{'\n'};
  bool tab_accept{false};
  bool ansi{false};
  int debounce_ms{150};
  int initial_pos{0};  // 1-based row to open on; 0 keeps the top
  int height{0};       // inline rows, resolved from --height's percentage; 0 = fullscreen
};

struct Rect {
  int x{}, y{}, w{}, h{};
};

// Rows the prompt, the readout and the blank line above them take off the top of the
// list pane before any header.
inline constexpr int kChromeRows{3};

struct Layout {
  Rect main;
  Rect preview;
  bool has_preview{false};
};

// Splits term_w x term_h between the list and the preview according to
// cfg.preview_dir and cfg.preview_size. With no preview command the list gets
// everything, unless force_preview asks for the split anyway (the action window
// previews its command without one being configured).
Layout CalculateLayout(const Config& cfg, int term_w, int term_h, bool force_preview = false);

// The three horizontal strips the list pane is cut into below the readout. A strip
// with h == 0 is absent and draws nothing, so callers need no separate flag.
struct ListStrips {
  Rect header;
  Rect items;
  Rect footer;
};

// Divides what is left of the list pane between the header, the items and the footer.
// A strip is granted its rows only while at least one item row survives, and the
// footer gives way first: --height can hand us a kMinInlineRows pane, and a picker
// with a header but nothing to pick is worse than one with no header. Strips are
// all-or-nothing, since half a footer reads as a rendering bug.
ListStrips SplitListPane(const Rect& main, int header_rows, int footer_rows);

// A header or footer split into the rows it draws as, so a multi-line legend can be
// passed as one argument. Breaks on a newline byte or on the two characters \n, because
// "a \n b" in double quotes carries the backslash through to us -- only $'a\nb' becomes
// a real newline in the shell, and the quoting a caller reaches for first should work.
// A row is a view into the argument, never a copy. Empty text draws no rows at all.
std::vector<std::string_view> SplitTextRows(std::string_view text);

// Running off either end lands on the other. This is the only navigation behaviour
// there is, so a long list needs no separate jump-to-top key.
size_t StepWrapped(size_t index, size_t count, bool forward);

constexpr bool IsInside(const Rect& r, int x, int y) noexcept {
  return (x >= r.x) && (x < r.x + r.w) && (y >= r.y) && (y < r.y + r.h);
}

// Largest scroll offset that still shows content, so scrolling cannot run off the
// end of the preview.
size_t GetMaxPreviewScroll(const Layout& layout, const Config& cfg, size_t total_lines);

// --height is a percentage of the terminal height, never a row count: panes vary
// too much for one absolute number to mean anything. Returns 0 -- meaning stay
// fullscreen -- for an unparseable or non-positive percentage, or when term_rows is
// unknown, rather than guessing a row count. A trailing '%' is accepted and ignored,
// so both `--height 40` and `--height 40%` work.
int RowsFromPercent(std::string_view raw, int term_rows);

// Parses --action specs of the form [alt-<c>:]<name>=<command>, where `==` instead
// of `=` marks the command as one that replaces tooey rather than running under it.
// A spec with no `=` is skipped.
std::vector<Action> ParseActions(const std::vector<std::string_view>& actions);

}  // namespace tooey

#endif  // TOOEY_LAYOUT_H_
