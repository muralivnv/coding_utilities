#include "layout.h"

#include <algorithm>

#include "text.h"

namespace tooey {

namespace {

// The rectangle the picker draws in. With no position that is the whole terminal,
// which is what every caller got before there was a box and what --height's inline
// mode still hands us -- there tb_width()/tb_height() already report the strip, so
// the box is the strip and nothing here changes.
//
// With one, --width and --height size it and the position names the vertical edge.
// All three centre it horizontally: a picker off to one side is the eye travel the
// box exists to remove, so there is no reason to offer a left or a right. A size
// larger than the terminal is clipped to it rather than refused, which is also how
// a percentage under the floor lands on a terminal too small to hold the floor.
//
// ParseConfig only lets the three names through, so the last branch is "centered"
// and there is no unrecognised value to fall back for.
Rect BoxOf(const Config& cfg, int term_w, int term_h) {
  const int full_w = std::max(0, term_w);
  const int full_h = std::max(0, term_h);
  if (cfg.position.empty())
    return Rect{0, 0, full_w, full_h};

  const int w = (cfg.width > 0) ? std::min(cfg.width, full_w) : full_w;
  const int h = (cfg.height > 0) ? std::min(cfg.height, full_h) : full_h;
  const int y = (cfg.position == "top") ? 0 : (cfg.position == "bottom") ? (full_h - h) : (full_h - h) / 2;
  return Rect{(full_w - w) / 2, y, w, h};
}

}  // namespace

Layout CalculateLayout(const Config& cfg, int term_w, int term_h, bool force_preview) {
  const Rect box = BoxOf(cfg, term_w, term_h);

  if (cfg.preview_cmd.empty() && !force_preview) {
    return {box, Rect{0, 0, 0, 0}, false};
  }

  int p_size = (cfg.preview_size <= 0 || cfg.preview_size >= 100) ? kDefaultPreviewSize : cfg.preview_size;

  if (cfg.preview_dir == "left") {
    int pw = (box.w * p_size) / 100;
    return {Rect{box.x + pw, box.y, box.w - pw, box.h}, Rect{box.x, box.y, pw, box.h}, true};

  } else if (cfg.preview_dir == "top") {
    int ph = (box.h * p_size) / 100;
    return {Rect{box.x, box.y + ph, box.w, box.h - ph}, Rect{box.x, box.y, box.w, ph}, true};

  } else if (cfg.preview_dir == "bottom") {
    int ph = (box.h * p_size) / 100;
    return {Rect{box.x, box.y, box.w, box.h - ph}, Rect{box.x, box.y + box.h - ph, box.w, ph}, true};
  }

  int pw = (box.w * p_size) / 100;
  return {Rect{box.x, box.y, box.w - pw, box.h}, Rect{box.x + box.w - pw, box.y, pw, box.h}, true};
}

ListStrips SplitListPane(const Rect& main, int header_rows, int footer_rows) {
  const int top = main.y + kChromeRows;
  int available = main.h - kChromeRows;
  if (available <= 0)
    return {Rect{main.x, top, main.w, 0}, Rect{main.x, top, main.w, 0}, Rect{main.x, top, main.w, 0}};

  int hr = (header_rows > 0 && header_rows < available) ? header_rows : 0;
  available -= hr;
  int fr = (footer_rows > 0 && footer_rows < available) ? footer_rows : 0;
  available -= fr;

  return {Rect{main.x, top, main.w, hr}, Rect{main.x, top + hr, main.w, available},
          Rect{main.x, main.y + main.h - fr, main.w, fr}};
}

std::vector<std::string_view> SplitTextRows(std::string_view text) {
  std::vector<std::string_view> rows;
  if (text.empty())
    return rows;

  size_t row_start = 0;
  for (size_t i = 0; i < text.size();) {
    size_t sep_len = 0;
    if (text[i] == '\n') {
      sep_len = 1;
    } else if ((text[i] == '\\') && (i + 1 < text.size()) && (text[i + 1] == 'n')) {
      sep_len = 2;
    } else {
      ++i;
      continue;
    }

    rows.push_back(text.substr(row_start, i - row_start));
    i += sep_len;
    row_start = i;
  }

  // Whatever follows the last break is a row too, blank included: a trailing break asked
  // for an empty line and gets one.
  rows.push_back(text.substr(row_start));
  return rows;
}

size_t StepWrapped(size_t index, size_t count, bool forward) {
  if (count == 0)
    return 0;
  if (forward)
    return (index + 1 == count) ? 0 : index + 1;
  return (index == 0) ? count - 1 : index - 1;
}

size_t GetMaxPreviewScroll(const Layout& layout, const Config& cfg, size_t total_lines) {
  if (!layout.has_preview || total_lines == 0)
    return 0;

  int view_h = layout.preview.h;
  if ((cfg.preview_dir == "top") || (cfg.preview_dir == "bottom"))
    view_h -= 1;
  if (view_h <= 0)
    return 0;

  return total_lines > static_cast<size_t>(view_h) ? total_lines - static_cast<size_t>(view_h) : 0;
}

namespace {

int FromPercent(std::string_view raw, int total, int floor) {
  const auto pct = ParseInt(raw);
  if (!pct || *pct <= 0)
    return 0;
  const int percent = std::min(*pct, 100);

  if (total <= 0)
    return 0;  // size unknown, so stay fullscreen rather than guess a count

  return std::max(total * percent / 100, floor);
}

}  // namespace

int RowsFromPercent(std::string_view raw, int term_rows) {
  return FromPercent(raw, term_rows, kMinInlineRows);
}

int ColumnsFromPercent(std::string_view raw, int term_cols) {
  return FromPercent(raw, term_cols, kMinBoxColumns);
}

std::vector<Action> ParseActions(const std::vector<std::string_view>& actions) {
  std::vector<Action> result;
  result.reserve(actions.size());

  constexpr std::string_view kKeyPrefix{"alt-"};

  for (auto spec : actions) {
    Action act;

    // Optional "alt-<char>:" binding. Only alt combinations are bound, so the prefix
    // names the one modifier there is; an uppercase char is the shifted key, because
    // that is exactly what the terminal sends for alt+shift.
    if (spec.starts_with(kKeyPrefix) && (spec.size() > kKeyPrefix.size() + 1) && (spec[kKeyPrefix.size() + 1] == ':')) {
      act.key = static_cast<unsigned char>(spec[kKeyPrefix.size()]);
      spec.remove_prefix(kKeyPrefix.size() + 2);
    }

    size_t pos = spec.find('=');
    if (pos == std::string_view::npos)
      continue;

    act.name = spec.substr(0, pos);

    if ((pos + 1) < spec.size()) {
      size_t cmd_start = pos + 1;
      if (spec[cmd_start] == '=') {
        act.is_become = true;
        ++cmd_start; 
      } else if (spec[cmd_start] == '!') {
        act.is_interactive = true;
        ++cmd_start;
      }
      act.command = spec.substr(cmd_start);
    }
    result.push_back(act);
  }

  return result;
}

}  // namespace tooey
