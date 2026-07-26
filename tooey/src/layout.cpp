#include "layout.h"

#include <algorithm>

#include "text.h"

namespace tooey {

Layout CalculateLayout(const Config& cfg, int term_w, int term_h, bool force_preview) {
  if (cfg.preview_cmd.empty() && !force_preview) {
    return {Rect{0, 0, term_w, term_h}, Rect{0, 0, 0, 0}, false};
  }

  int p_size = (cfg.preview_size <= 0 || cfg.preview_size >= 100) ? kDefaultPreviewSize : cfg.preview_size;

  if (cfg.preview_dir == "left") {
    int pw = (term_w * p_size) / 100;
    return {Rect{pw, 0, term_w - pw, term_h}, Rect{0, 0, pw, term_h}, true};

  } else if (cfg.preview_dir == "top") {
    int ph = (term_h * p_size) / 100;
    return {Rect{0, ph, term_w, term_h - ph}, Rect{0, 0, term_w, ph}, true};

  } else if (cfg.preview_dir == "bottom") {
    int ph = (term_h * p_size) / 100;
    return {Rect{0, 0, term_w, term_h - ph}, Rect{0, term_h - ph, term_w, ph}, true};
  }

  int pw = (term_w * p_size) / 100;
  return {Rect{0, 0, term_w - pw, term_h}, Rect{term_w - pw, 0, pw, term_h}, true};
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

int RowsFromPercent(std::string_view raw, int term_rows) {
  const auto pct = ParseInt(raw);
  if (!pct || *pct <= 0)
    return 0;
  const int percent = std::min(*pct, 100);

  if (term_rows <= 0)
    return 0;  // size unknown, so stay fullscreen rather than guess a row count

  return std::max(term_rows * percent / 100, kMinInlineRows);
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

    if (pos + 1 < spec.size() && spec[pos + 1] == '=') {
      act.is_become = true;
      act.command = spec.substr(pos + 2);
    } else {
      act.command = spec.substr(pos + 1);
    }

    result.push_back(act);
  }

  return result;
}

}  // namespace tooey
