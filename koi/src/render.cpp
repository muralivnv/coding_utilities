#include "render.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "commands.h"
#include "format.h"
#include "navigate.h"
#include "search.h"
#include "syntax.h"
#include "theme.h"
#include "unicode.h"

namespace koi {
namespace {

void SetCursor(int x, int y);
void HideCursor();
void RenderInto(Editor& ed, int width, int height);

constexpr std::string_view kInsertCaret = "▏";

struct Palette {
  Style text;
  Style background;
  Style linenr;
  Style linenr_selected;
  Style selection;
  Style selection_primary;
  Style cursor;
  Style cursor_primary;
  Style cursor_match;
  Style cursorline_primary;
  Style cursorline_secondary;
  Style statusline;
  Style statusline_normal;
  Style statusline_insert;
  Style status_error;
  Style status_warning;
  Style status_info;
  Style popup;
  Style window;
  Style wrap;
  Style whitespace;
  Style jump_label;
  Style jump_next;
  Style excerpt_match;
};

Palette Resolve(const Theme& theme) {
  Palette ui;
  ui.text = theme.Get("ui.text");
  ui.background = theme.Get("ui.background");
  ui.linenr = theme.Get("ui.linenr");
  ui.linenr_selected = theme.Get("ui.linenr.selected");
  ui.selection = theme.Get("ui.selection");
  ui.selection_primary = theme.Get("ui.selection.primary");
  ui.cursor = theme.Get("ui.cursor");
  ui.cursor_primary = theme.Get("ui.cursor.primary");
  ui.cursor_match = theme.Get("ui.cursor.match");
  ui.cursorline_primary = theme.Get("ui.cursorline.primary");
  ui.cursorline_secondary = theme.Get("ui.cursorline.secondary");
  ui.statusline = theme.Get("ui.statusline");
  ui.statusline_normal = theme.Get("ui.statusline.normal");
  ui.statusline_insert = theme.Get("ui.statusline.insert");
  ui.status_error = theme.Get("error");
  ui.status_warning = theme.Get("warning");
  ui.status_info = theme.Get("info");
  ui.popup = theme.Get("ui.popup");
  ui.window = theme.Get("ui.window");
  ui.wrap = theme.Get("ui.virtual.wrap");
  ui.whitespace = theme.Get("ui.virtual.whitespace");
  ui.jump_label = theme.Get("ui.virtual.jump-label");
  ui.jump_next = theme.Get("ui.jump.next");
  ui.excerpt_match = theme.Get(kExcerptMatchScope);
  return ui;
}

Attr ToTb(std::uint32_t rgb) {
  return (rgb == 0) ? kAttrHiBlack : static_cast<Attr>(rgb);
}

Attr Fg(const Style& style, const Palette& ui) {
  if (style.fg.set) return ToTb(style.fg.rgb);
  if (ui.text.fg.set) return ToTb(ui.text.fg.rgb);
  return 0;
}

Attr Bg(const Style& style, const Palette& ui) {
  if (style.bg.set) return ToTb(style.bg.rgb);
  if (ui.background.bg.set) return ToTb(ui.background.bg.rgb);
  return 0;
}

// What a box drawn over code outlines itself in. `ui.window` is the wrong scope
// for it: that one is sized for a window split, where a line between two panes
// of text wants to disappear, and a theme that dims it is right to. A box's edge
// is the opposite job -- it is the only thing telling the box from what it
// covers. So it takes the colour the theme already trusts to be legible on its
// own background, whichever direction that theme runs.
Attr BorderFg(const Palette& ui) { return Fg(ui.text, ui); }

Attr CaretBarFg(const Style& cursor, const Palette& ui) {
  if (cursor.bg.set) return ToTb(cursor.bg.rgb);
  if (cursor.fg.set) return ToTb(cursor.fg.rgb);
  return Fg(ui.text, ui);
}

Attr Attrs(const Style& style) {
  Attr attrs = 0;
  if ((style.mods & kModBold) != 0) attrs |= kAttrBold;
  if ((style.mods & kModDim) != 0) attrs |= kAttrDim;
  if ((style.mods & kModItalic) != 0) attrs |= kAttrItalic;
  if ((style.mods & kModUnderlined) != 0) attrs |= kAttrUnderline;
  if ((style.mods & (kModSlowBlink | kModRapidBlink)) != 0) attrs |= kAttrBlink;
  if ((style.mods & kModReversed) != 0) attrs |= kAttrReverse;
  if ((style.mods & kModCrossedOut) != 0) attrs |= kAttrStrikeout;
  return attrs;
}

// Which selection covers pos, or -1 for none. Not just whether one does: the
// theme may tint the primary selection apart from the rest, which needs the
// index to compare against SelectionSet::PrimaryIndex.
//
// Binary search on To(), which assumes the ranges are sorted and disjoint --
// the invariant SelectionSet::Normalize maintains.
int SelectedIn(const SelectionSet& sel, Index pos) {
  const auto& ranges = sel.Ranges();
  auto it = std::ranges::upper_bound(ranges, pos, {}, &Selection::To);
  if ((it == ranges.end()) || (pos < it->From())) return -1;
  return static_cast<int>(std::distance(ranges.begin(), it));
}

bool InAnyRange(const std::vector<Interval>& ranges, Index pos) {
  auto it = std::lower_bound(ranges.begin(), ranges.end(), pos,
                             [](const Interval& range, Index p) {
                               return range.empty() || (range.back() < p);
                             });
  return (it != ranges.end()) && !it->empty() && (pos >= it->front());
}

enum class CursorKind { kNone, kSecondary, kPrimary };

CursorKind CursorAt(const PieceTable& table, const SelectionSet& sel, Index pos) {
  const auto& ranges = sel.Ranges();

  auto it = std::lower_bound(ranges.begin(), ranges.end(), pos,
                             [](const Selection& s, Index p) { return s.To() < p; });
  if (it == ranges.end()) return CursorKind::kNone;

  for (int step = 0; (step < 2) && (it != ranges.end()); ++step, ++it) {
    if (CursorOf(table, *it) != pos) continue;
    const auto at = static_cast<std::size_t>(std::distance(ranges.begin(), it));
    return (at == sel.PrimaryIndex()) ? CursorKind::kPrimary : CursorKind::kSecondary;
  }
  return CursorKind::kNone;
}

struct Clip {
  int x{0};
  int y{0};
  int w{0};
  int h{0};
};
Clip g_clip;

Surface* g_out = nullptr;

void Put(int x, int y, std::string_view text, Attr fg, Attr bg) {
  if ((x < 0) || (y < 0) || (x >= g_clip.w) || (y >= g_clip.h)) return;
  const int sx = g_clip.x + x;
  const int sy = g_clip.y + y;
  if ((g_out == nullptr) || !g_out->Holds(sx, sy)) return;
  Glyph& cell = g_out->At(sx, sy);
  // The one gate between a document and the terminal. Present() hands
  // cell.text to tb_set_cell as a codepoint and termbox re-encodes it onto the
  // wire, so an unfiltered ESC here is an escape sequence the terminal obeys --
  // a file could set the window title, clear the screen, conceal what it really
  // says, or write the clipboard. Every caller funnels through Put, so this is
  // the only place that has to remember.
  cell.text.assign(PrintableCluster(text));
  cell.fg = fg;
  cell.bg = bg;
}

void Cell(int x, int y, char ch, Attr fg, Attr bg) {
  Put(x, y, std::string_view{&ch, 1}, fg, bg);
}

void DrawCluster(int x, int y, std::string_view cluster, Attr fg, Attr bg) {
  Put(x, y, cluster.empty() ? std::string_view{" "} : cluster, fg, bg);
}

int DrawText(int x, int y, int limit, std::string_view text, Attr fg, Attr bg) {
  size_t i = 0;
  while ((x < limit) && (i < text.size())) {
    const size_t next = NextGraphemeInString(text, i);
    const std::string_view cluster{text.data() + i, next - i};
    const int w = std::max(1, GraphemeWidth(cluster));
    if (x + w > limit) {
      while (x < limit) Cell(x++, y, ' ', fg, bg);
      break;
    }
    DrawCluster(x, y, cluster, fg, bg);
    x += w;
    i = next;
  }
  return x;
}

int DisplayWidthOf(std::string_view text) {
  int width = 0;
  size_t i = 0;
  while (i < text.size()) {
    const size_t next = NextGraphemeInString(text, i);
    width += std::max(1, GraphemeWidth(std::string_view{text.data() + i, next - i}));
    i = next;
  }
  return width;
}

// DrawText for a text that says most at its end: too long for the column, it
// keeps its tail behind an ellipsis rather than its head. A path clipped the
// other way reads `/home/murali/.local/share/chezmoi/dot_` on every row and
// names no file.
int DrawTextTail(int x, int y, int limit, std::string_view text, Attr fg, Attr bg) {
  const int room = limit - x;
  if (room <= 0) return x;
  if (DisplayWidthOf(text) <= room) return DrawText(x, y, limit, text, fg, bg);
  // Walk off the front until what is left fits beside the ellipsis. A wide
  // cluster straddling the cut goes with the front, so the tail is never wider
  // than the room measured for it.
  int drop = DisplayWidthOf(text) - room + 1;
  size_t i = 0;
  while ((i < text.size()) && (drop > 0)) {
    const size_t next = NextGraphemeInString(text, i);
    drop -= std::max(1, GraphemeWidth(std::string_view{text.data() + i, next - i}));
    i = next;
  }
  return DrawText(DrawText(x, y, limit, "…", fg, bg), y, limit, text.substr(i), fg, bg);
}

struct PaneRef {
  const Document& doc;
  const Viewport& view;
  const SelectionSet& sel;
  std::size_t buffer;
  bool focused;
};

int GutterWidthOf(const Document& doc) {
  Index lines = LineCount(doc.table);
  int digits = 1;
  while (lines >= 10) {
    lines /= 10;
    ++digits;
  }
  return std::max(3, digits) + 2;
}

int GutterFor(const Document& doc, int pane_width) {
  const int full = GutterWidthOf(doc);
  return (pane_width >= (full + 8)) ? full : 0;
}

WrapMetrics WrapFor(const Settings& settings, const Document& doc, int gutter, int width) {
  WrapMetrics wrap;
  wrap.enabled = settings.soft_wrap;
  wrap.width = std::max(1, width - gutter);
  // An indicator at least as wide as the pane would put every continuation row
  // past the right edge, where LayoutLine goes on counting rows and DrawLine
  // draws nothing of the text on them -- rows the leap would still offer and
  // label. Indent by no more than what leaves a column to indent into. Capped
  // here rather than in LayoutLine so that the fit, the renderer and the leap
  // are all handed the same value; collapsing the line to one row instead
  // would make LeapVisibleRanges offer all of it, which is worse.
  wrap.indent = std::min<Index>(DisplayWidthOf(settings.wrap_indicator), wrap.width - 1);
  wrap.max_wrap = settings.max_wrap;
  wrap.tab_width = doc.tab_width;
  return wrap;
}

bool StatusOverlayFits(int width, int height) {
  return (std::min(width - 6, 100) >= 8) && (height >= 5);
}

void DrawStatus(const Editor& ed, const PaneRef& pane, const Palette& ui, int y, int width,
                bool* message_clipped = nullptr, bool overlay_takes_message = false) {
  const bool focused = pane.focused;
  StatusLine bar = StatusBar(ed, pane.doc, pane.sel, pane.buffer, focused);
  const Style& mode =
      (ed.mode == Mode::kInsert) ? ui.statusline_insert : ui.statusline_normal;

  const Attr bar_fg =
      Fg(ui.statusline, ui) | Attrs(ui.statusline) | (focused ? 0 : kAttrDim);
  const Attr bar_bg = Bg(ui.statusline, ui);

  const bool block = focused && (ed.settings.mode_indicator == "block");
  Attr accent_fg = focused ? (block ? (Fg(mode, ui) | Attrs(mode))
                              : mode.bg.set ? Bg(mode, ui)
                                            : (Fg(mode, ui) | Attrs(mode)))
                           : bar_fg;
  const Attr accent_bg = (focused && block) ? Bg(mode, ui) : bar_bg;
  if (!block && (accent_fg == accent_bg)) accent_fg = bar_fg | kAttrBold;

  const auto severity = [&](const Style& style) -> std::pair<Attr, Attr> {
    return {Fg(style, ui) | Attrs(style), style.bg.set ? ToTb(style.bg.rgb) : bar_bg};
  };
  const auto paint = [&](StatusTone tone) -> std::pair<Attr, Attr> {
    switch (tone) {
      case StatusTone::kAccent: return {accent_fg, accent_bg};
      case StatusTone::kStrong: return {bar_fg | kAttrBold, bar_bg};
      case StatusTone::kDim: return {bar_fg | kAttrDim, bar_bg};
      case StatusTone::kError: return severity(ui.status_error);
      case StatusTone::kWarning: return severity(ui.status_warning);
      case StatusTone::kInfo: return severity(ui.status_info);
      case StatusTone::kNext: return severity(ui.jump_next);
      case StatusTone::kNormal: break;
    }
    return {bar_fg, bar_bg};
  };

  int right_width = 0;
  for (const StatusSpan& span : bar.right) right_width += DisplayWidthOf(span.text);
  const int right_start = width - right_width - 1;

  for (int fill = 0; fill < width; ++fill) Cell(fill, y, ' ', bar_fg, bar_bg);

  int x = 1;
  const int limit = (right_start > x) ? (right_start - 1) : width;

  {
    int left_width = 0;
    for (const StatusSpan& span : bar.left) left_width += DisplayWidthOf(span.text);
    int over = left_width - std::max(0, right_start - 1 - x);

    const bool has_message = (bar.message_from < bar.left.size());
    std::vector<StatusSpan> untrimmed;
    if (has_message && overlay_takes_message) untrimmed = bar.left;

    const auto shrink_dim_spans = [&bar](int& still_over) {
      while (still_over > 0) {
        StatusSpan* widest = nullptr;
        for (StatusSpan& span : bar.left) {
          if (span.tone != StatusTone::kDim) continue;
          if ((widest == nullptr) || (DisplayWidthOf(span.text) > DisplayWidthOf(widest->text))) {
            widest = &span;
          }
        }
        if ((widest == nullptr) || (DisplayWidthOf(widest->text) <= 2)) break;

        std::string& text = widest->text;
        const int want = std::max(1, DisplayWidthOf(text) - still_over - 1);
        std::string kept;
        int keep_from = DisplayWidthOf(text) - want;
        std::size_t i = 0;
        int seen = 0;
        while (i < text.size()) {
          const std::size_t next = NextGraphemeInString(text, i);
          const std::string_view cluster{text.data() + i, next - i};
          if (seen >= keep_from) kept.append(cluster);
          seen += std::max(1, GraphemeWidth(cluster));
          i = next;
        }
        const int before = DisplayWidthOf(text);
        text = "…" + kept;
        still_over -= (before - DisplayWidthOf(text));
        if (before == DisplayWidthOf(text)) break;
      }
    };
    shrink_dim_spans(over);

    if ((over > 0) && has_message) {
      if (message_clipped != nullptr) *message_clipped = true;
      if (overlay_takes_message) {
        bar.left = std::move(untrimmed);
        bar.left.resize(bar.message_from);
        int left_over = -std::max(0, right_start - 1 - x);
        for (const StatusSpan& span : bar.left) left_over += DisplayWidthOf(span.text);
        shrink_dim_spans(left_over);
      }
    }
    if ((over > 0) && has_message && !overlay_takes_message &&
        ((bar.message_from + 1) < bar.left.size())) {
      std::string& text = bar.left[bar.message_from + 1].text;
      const int want = std::max(1, DisplayWidthOf(text) - over - 1);
      std::string kept;
      int seen = 0;
      std::size_t i = 0;
      while ((i < text.size()) && (seen < want)) {
        const std::size_t next = NextGraphemeInString(text, i);
        const std::string_view cluster{text.data() + i, next - i};
        if ((seen + std::max(1, GraphemeWidth(cluster))) > want) break;
        kept.append(cluster);
        seen += std::max(1, GraphemeWidth(cluster));
        i = next;
      }
      if (i < text.size()) text = kept + "…";
    }
  }

  for (const StatusSpan& span : bar.left) {
    const auto [fg, bg] = paint(span.tone);
    x = DrawText(x, y, limit, span.text, fg, bg);
  }

  if (right_start >= x) {
    int rx = right_start;
    for (const StatusSpan& span : bar.right) {
      const auto [fg, bg] = paint(span.tone);
      rx = DrawText(rx, y, width, span.text, fg, bg);
    }
  }
}

struct Highlights {
  Index from{0};
  std::vector<CaptureId> captures;
  // Styles for capture ids past the ones ResolveCaptureStyles knew about.
  //
  // A syntax with injections learns new scopes as it meets them -- the first
  // ```rust block in a markdown file adds Rust's -- and that happens during
  // Paint, long after the document resolved its styles. Without this the newly
  // injected language draws as plain text until something unrelated forces a
  // re-resolve, which is a bug that only shows on some files and only sometimes.
  std::vector<Style> beyond;

  Style StyleAt(const Document& doc, Index pos, const Palette& ui) const {
    const Index at = pos - from;
    if ((at < 0) || (at >= std::ssize(captures))) return ui.text;
    const CaptureId id = captures[static_cast<size_t>(at)];
    if (id == kNoCapture) return ui.text;

    const std::size_t slot = static_cast<std::size_t>(id) - 1;
    if (slot < doc.capture_styles.size()) return ui.text.Over(doc.capture_styles[slot]);
    const std::size_t extra = slot - doc.capture_styles.size();
    if (extra < beyond.size()) return ui.text.Over(beyond[extra]);
    return ui.text;
  }
};

void DrawLine(const Editor& ed, const PaneRef& pane, const Palette& ui,
              const Highlights& highlights, Index line, int y0, int gutter, int width,
              int text_rows, const WrapMetrics& wrap, const std::vector<Index>& row_starts,
              std::string& scratch, Index bracket_a = -1, Index bracket_b = -1,
              const Style* cursorline = nullptr, const LeapState* leap = nullptr) {
  const Document& doc = pane.doc;
  const Interval content = LineContentRange(doc.table, line);
  const Index line_start = LineStart(doc.table, line);
  ReadDocRangeInto(doc.table, content, scratch);

  static std::vector<Interval> matches;
  matches.clear();
  const bool confined = SearchIsConfinedToSelections(ed);
  if (const std::string_view pattern = ActiveSearchPattern(ed); !pattern.empty()) {
    std::string ignored;
    std::ignore = FindInText(pattern, scratch, matches, ignored);

    if (confined) {
      std::erase_if(matches, [&](const Interval& match) {
        const Index from = line_start + match.front();
        const Index to = line_start + match.back() + 1;
        for (const Selection& s : pane.sel.Ranges()) {
          if ((from >= s.From()) && (to <= s.To())) return false;
        }
        return true;
      });
    }
  }

  Index column = 0;
  size_t i = 0;
  size_t row = 0;
  const Index left = wrap.enabled ? 0 : pane.view.left_column;

  while (i <= scratch.size()) {
    const Index pos = line_start + static_cast<Index>(i);
    while (((row + 1) < row_starts.size()) && (pos >= row_starts[row + 1])) {
      ++row;
      column = 0;
    }
    const int y = y0 + static_cast<int>(row);
    if (y >= text_rows) break;
    const bool visible = (y >= 0);
    const Index indent = (row == 0) ? 0 : wrap.indent;
    const int selected_in = SelectedIn(pane.sel, pos);
    const bool selected = (selected_in >= 0);
    // Unset, "ui.selection.primary" falls back to "ui.selection" through
    // Theme::Get's prefix walk, so this is the same style unless a theme says
    // otherwise.
    const Style& selection_style =
        (selected && (static_cast<std::size_t>(selected_in) == pane.sel.PrimaryIndex()))
            ? ui.selection_primary
            : ui.selection;
    const bool matched = InAnyRange(matches, static_cast<Index>(i));
    CursorKind cursor_kind = CursorAt(doc.table, pane.sel, pos);
    if ((ed.mode == Mode::kInsert) && (cursor_kind == CursorKind::kPrimary)) {
      cursor_kind = CursorKind::kNone;
    }
    if (!pane.focused) cursor_kind = CursorKind::kNone;
    const bool cursor = (cursor_kind != CursorKind::kNone);
    const Style& cursor_style =
        (cursor_kind == CursorKind::kPrimary) ? ui.cursor_primary : ui.cursor;
    const bool bar_caret = cursor && (ed.mode == Mode::kInsert);
    const auto caret_bg = [&] {
      return (cursorline != nullptr) ? Bg(*cursorline, ui) : Bg(ui.background, ui);
    };

    if (i == scratch.size()) {
      const int x = static_cast<int>(gutter + indent + column - left);
      if (visible && (column >= left) && (x < width) && (cursor || selected)) {
        const Style& style = cursor ? cursor_style : selection_style;
        if (bar_caret) {
          DrawCluster(x, y, kInsertCaret, CaretBarFg(cursor_style, ui), caret_bg());
        } else {
          Cell(x, y, ' ', Fg(style, ui) | Attrs(style), Bg(style, ui));
        }
      }
      break;
    }

    const size_t next = NextGraphemeInString(scratch, i);
    const std::string_view cluster{scratch.data() + i, next - i};
    Index advance = 0;
    const bool is_tab = (scratch[i] == '\t');
    advance = is_tab ? (doc.tab_width - (column % doc.tab_width))
                     : std::max(1, GraphemeWidth(cluster));

    Style style = highlights.StyleAt(doc, pos, ui);
    const bool glyph_tab = is_tab && ed.settings.render_tabs;
    if (glyph_tab) style = style.Over(ui.whitespace);
    if (cursorline != nullptr) style = style.Over(*cursorline);
    if (cursor) {
      style = style.Over(cursor_style);
    } else {
      if (selected) style = style.Over(selection_style);
      if ((pos == bracket_a) || (pos == bracket_b) || matched) {
        style = style.Over(ui.cursor_match);
      }
    }
    // A leap paints over everything, the cursor included: while it is up, the
    // only question on the screen is which target to take, and a label hidden
    // under the block cursor is a key nobody can see to press. The caret has
    // not moved and comes back the moment the mode ends.
    char leap_key = 0;
    Style under = style;
    if (leap != nullptr) {
      if (InAnyRange(leap->spans, pos)) style = style.Over(ui.cursor_match);
      if (const char key = LeapLabelAt(*leap, pos); key != 0) {
        leap_key = key;
        under = style;
        style = style.Over(ui.jump_label);
      }
    }
    const Attr fg = Fg(style, ui) | Attrs(style);
    const Attr bg = Bg(style, ui);
    // What the rest of a labelled cluster is blanked in: the style the text
    // under the label already had, not the label's own. A label is one cell of
    // chrome, and a tab's advance is up to tab_width columns -- painting those
    // in the jump-label colours turns a one-key label into a highlighted block
    // as wide as the tab stop. Identical to fg/bg when nothing is labelled.
    const Attr pad_fg = (leap_key != 0) ? (Fg(under, ui) | Attrs(under)) : fg;
    const Attr pad_bg = (leap_key != 0) ? Bg(under, ui) : bg;

    for (Index step = 0; visible && (step < advance); ++step) {
      const Index screen_column = column + step;
      if (screen_column < left) continue;
      const int x = static_cast<int>(gutter + indent + screen_column - left);
      if (x >= width) break;
      if ((step == 0) && (leap_key != 0)) {
        // One column of the cluster the label stands on, and the rest of it
        // blanked by the steps below -- which is what keeps a label over a
        // double-width glyph from leaving half of one behind it.
        Cell(x, y, leap_key, fg, bg);
      } else if (glyph_tab) {
        DrawCluster(x, y, (step == 0) ? ed.settings.tab_glyph : ed.settings.tab_pad, pad_fg,
                    pad_bg);
      } else if (step == 0 && !is_tab) {
        if ((x + static_cast<int>(advance)) > width) {
          Cell(x, y, ' ', fg, bg);
        } else {
          DrawCluster(x, y, cluster, fg, bg);
        }
      } else {
        Cell(x, y, ' ', pad_fg, pad_bg);
      }
    }
    column += advance;
    i = next;
    if (!wrap.enabled && ((gutter + column - left) >= width)) break;
  }
}
struct Panes {
  Rect screen;
  std::vector<Rect> areas;
  std::vector<int> order;
};

Panes PanesOf(const Editor& ed, int width, int height) {
  // The smart-jump and picker prompts draw at the caret, so they take no row
  // and opening them reflows nothing.
  const int prompt_rows = (ed.prompt_active && (ed.prompt_kind != PromptKind::kSmartJump) &&
                           (ed.prompt_kind != PromptKind::kPicker))
                              ? 1
                              : 0;
  const Rect screen{0, 0, width, std::max(0, height - prompt_rows)};
  return Panes{screen, LayoutWindows(ed, screen), WindowOrder(ed)};
}

Rect FocusedArea(const Editor& ed, int width, int height) {
  const Panes panes = PanesOf(ed, width, height);
  if (panes.order.empty()) return panes.areas.empty() ? panes.screen : panes.areas.front();
  for (std::size_t i = 0; (i < panes.order.size()) && (i < panes.areas.size()); ++i) {
    if (panes.order[i] == ed.focused) return panes.areas[i];
  }
  return panes.areas.empty() ? panes.screen : panes.areas.front();
}

}

int TextWidthOf(const Rect& area, int screen_w) {
  const bool neighbour_right = (area.x + area.w) < screen_w;
  return std::max(1, area.w - (neighbour_right ? 1 : 0));
}

Rect PaneContent(const Editor& ed, const Rect& area, int screen_w) {
  return Rect{area.x, area.y, TextWidthOf(area, screen_w), area.h};
}

int GutterWidth(const Editor& ed, int pane_width) { return GutterFor(ed.doc, pane_width); }

WrapMetrics WrapOf(const Editor& ed, int gutter, int width) {
  return WrapFor(ed.settings, ed.doc, gutter, width);
}

WrapMetrics WrapForFocusedViewport(const Editor& ed) {
  // A gutter of zero, because view.columns is the text width already.
  return WrapFor(ed.settings, ed.doc, 0, static_cast<int>(std::max<Index>(1, ed.doc.view.columns)));
}

Index EffectiveScrolloff(Editor& ed) {
  // Consumed either way: align_view_once is a one-shot, and swallowing it
  // without using it is the same bug as never clearing it.
  const bool aligning = std::exchange(ed.align_view_once, false);
  if (aligning) return 0;
  return ed.settings.scrolloff;
}

void FitFocusedViewport(Editor& ed, int width, int height) {
  ed.screen_w = width;
  ed.screen_h = height;
  const Rect area = FocusedArea(ed, width, height);
  const Rect content = PaneContent(ed, area, width);
  const int gutter = GutterWidth(ed, content.w);
  ed.doc.view.rows = std::max(1, content.h - 1);
  ed.doc.view.columns = std::max(1, content.w - gutter);

  ed.doc.view.scrolloff = EffectiveScrolloff(ed);
  // Off the viewport just written, not off the rectangle again: the scroll and
  // everything that later reads this pane's rows then wrap at one width.
  ed.doc.view = ScrollToCursor(ed.doc, ed.doc.view, WrapForFocusedViewport(ed));
}

Index PositionAtScreen(const Editor& ed, const WrapMetrics& wrap, int gutter, int click_x,
                       int click_y) {
  const Index line_count = LineCount(ed.doc.table);
  std::vector<Index> row_starts;
  std::string scratch;
  int line_y = wrap.enabled ? -static_cast<int>(ed.doc.view.top_row) : 0;

  for (Index line = ed.doc.view.top_line; line < line_count; ++line) {
    LayoutLine(ed.doc.table, line, wrap, row_starts, scratch);
    const int rows = static_cast<int>(row_starts.size());
    if (click_y < (line_y + rows)) {
      const size_t r = static_cast<size_t>(std::clamp(click_y - line_y, 0, rows - 1));
      const Index indent = (r == 0) ? 0 : wrap.indent;
      const Index left = wrap.enabled ? 0 : ed.doc.view.left_column;
      const Index column = std::max<Index>(0, click_x - gutter - indent + left);
      const Interval content = LineContentRange(ed.doc.table, line);
      const Index line_end =
          content.empty() ? LineStart(ed.doc.table, line) : (content.back() + 1);
      const Index row_end = ((r + 1) < row_starts.size()) ? row_starts[r + 1] : line_end;
      return ByteForColumnFrom(ed.doc.table, row_starts[r], row_end, column, ed.doc.tab_width);
    }
    line_y += rows;
  }
  return std::max<Index>(0, DocLength(ed.doc.table) - 1);
}

namespace {

// `defer_status` leaves this pane's status row to the caller, which is what a
// prompt drawn at the caret needs: whether the bar says ed.status depends on
// whether the box found room for its branch row, and the box is fitted around
// the caret this draws.
void RenderPane(const Editor& ed, const PaneRef& pane, const Palette& ui, int width, int height,
                int& caret_x, int& caret_y, bool* status_clipped = nullptr,
                bool overlay_takes_message = false, bool defer_status = false) {
  const Document& doc = pane.doc;
  const int gutter = GutterFor(doc, width);
  const int chrome_rows = (height >= 2) ? 1 : 0;
  const Index text_rows = std::max(1, height - chrome_rows);
  const Index line_count = LineCount(doc.table);
  const Index cursor_pos = CursorOf(doc.table, pane.sel.Primary());
  const Index cursor_line = LineAt(doc.table, cursor_pos);

  Index bracket_a = -1;
  Index bracket_b = -1;
  if (const Index match = MatchingBracket(doc.table, cursor_pos);
      pane.focused && (match >= 0)) {
    bracket_a = cursor_pos;
    bracket_b = match;
  }
  const bool want_cursorline =
      pane.focused && ed.settings.cursorline &&
      (Paints(ui.cursorline_primary) || Paints(ui.cursorline_secondary));
  // Only the lines this frame draws can carry a secondary band, so only the
  // selections reaching into them are worth a LineAt -- a whole-file
  // multi-cursor otherwise costs one piece-table lookup per cursor per frame.
  // The slice is bounded by each selection's *range*, not by its cursor:
  // CursorOf always lands inside [From(), To()] (backward selections put it on
  // From(), forward ones one grapheme back from To()), so a selection whose
  // range misses the visible bytes cannot put a cursor on a visible line, and
  // a slice that keeps every range touching them is trivially a superset.
  // Lines that slip in just off screen are simply never looked up.
  std::vector<Index> cursor_lines;
  if (want_cursorline && Paints(ui.cursorline_secondary) && (line_count > 0) &&
      (pane.view.top_line < line_count)) {
    // The loop below draws at most text_rows lines: a partially scrolled first
    // line still leaves line_y >= 1 behind it, and every later line costs a row.
    const Index last_visible = std::min(line_count - 1, pane.view.top_line + text_rows);
    const Index window_from = LineStart(doc.table, pane.view.top_line);
    const Interval window_tail = LineRange(doc.table, last_visible);
    const Index window_to =
        window_tail.empty() ? DocLength(doc.table) : (window_tail.back() + 1);

    const std::vector<Selection>& ranges = pane.sel.Ranges();
    const auto start_of = [](const Selection& s) { return s.From(); };
    auto first = std::ranges::lower_bound(ranges, window_from, {}, start_of);
    // While the set holds its non-overlapping invariant at most one range can
    // straddle the top of the window, but stepping back over every range that
    // reaches into it costs nothing when they are disjoint and keeps this
    // right if they ever are not.
    while ((first != ranges.begin()) && (std::prev(first)->To() >= window_from)) --first;
    const auto last = std::ranges::upper_bound(ranges, window_to, {}, start_of);

    for (auto it = first; it != last; ++it) {
      cursor_lines.push_back(LineAt(doc.table, CursorOf(doc.table, *it)));
    }
    std::ranges::sort(cursor_lines);
    const auto extra = std::ranges::unique(cursor_lines);
    cursor_lines.erase(extra.begin(), extra.end());
  }

  const WrapMetrics wrap = WrapFor(ed.settings, doc, gutter, width);

  // The overlay belongs to the pane the leap was armed in, and only while it
  // still describes that pane's text -- a buffer swapped in or rebuilt under
  // an armed leap leaves byte positions that name a document nobody is looking
  // at, and those must never be painted as targets.
  const LeapState* leap = (pane.focused && LeapIsLive(ed)) ? &ed.leap : nullptr;

  Highlights highlights;
  if ((doc.syntax != nullptr) || IsExcerptView(doc)) {
    const Index last = std::min(line_count - 1, pane.view.top_line + text_rows);
    const Index from = LineStart(doc.table, pane.view.top_line);
    const Interval tail = LineRange(doc.table, last);
    const Index to = tail.empty() ? DocLength(doc.table) : (tail.back() + 1);
    highlights.from = from;
    if (doc.syntax != nullptr) {
      doc.syntax->Sync(doc.table);
      if (to > from) doc.syntax->Paint(doc.table, Interval(from, to), highlights.captures);
      // Resolved after Paint, because Paint is what discovers an injected
      // language and appends its scopes.
      const std::span<const std::string> names = doc.syntax->CaptureNames();
      for (std::size_t i = doc.capture_styles.size(); i < names.size(); ++i) {
        highlights.beyond.push_back(ed.theme.Get(names[i]));
      }
    } else if (to > from) {
      constexpr CaptureId kHeaderCapture = 1;
      constexpr CaptureId kMatchCapture = 2;
      highlights.captures.assign(static_cast<std::size_t>(to - from), kNoCapture);
      static std::string visible;
      ReadDocRangeInto(doc.table, Interval(from, to), visible);
      std::vector<Interval> spans;
      const auto paint = [&highlights](std::size_t lo, std::size_t hi, CaptureId id) {
        for (std::size_t x = lo; x < hi; ++x) highlights.captures[x] = id;
      };
      std::size_t at = 0;
      while (at < visible.size()) {
        std::size_t eol = visible.find('\n', at);
        if (eol == std::string::npos) eol = visible.size();
        const std::string_view line{visible.data() + at, eol - at};
        switch (ClassifyExcerptLine(doc.excerpts, line, spans)) {
          case ExcerptLine::kHeader: paint(at, eol, kHeaderCapture); break;
          case ExcerptLine::kWholeLineMatch: paint(at, eol, kMatchCapture); break;
          case ExcerptLine::kSpanMatches:
            for (const Interval& span : spans) {
              paint(at + static_cast<std::size_t>(span.front()),
                    at + static_cast<std::size_t>(span.back() + 1), kMatchCapture);
            }
            break;
          case ExcerptLine::kPlain: break;
        }
        at = eol + 1;
      }
    }
  }

  caret_x = -1;
  caret_y = -1;

  std::string scratch;
  std::vector<Index> row_starts;
  int line_y = wrap.enabled ? -static_cast<int>(pane.view.top_row) : 0;
  for (Index line = pane.view.top_line; (line < line_count) && (line_y < text_rows); ++line) {
    LayoutLine(doc.table, line, wrap, row_starts, scratch);

    const bool current = (line == cursor_line);
    const Index shown = (current || !ed.settings.relative_line_numbers)
                            ? (line + 1)
                            : std::abs(line - cursor_line);
    const std::string label = std::to_string(shown);
    const int pad = gutter - 1 - static_cast<int>(label.size());

    const Style* cursorline = nullptr;
    if (want_cursorline) {
      if (current) {
        if (Paints(ui.cursorline_primary)) cursorline = &ui.cursorline_primary;
      } else if (std::ranges::binary_search(cursor_lines, line)) {
        cursorline = &ui.cursorline_secondary;
      }
    }

    for (size_t r = 0; r < row_starts.size(); ++r) {
      const int row_y = line_y + static_cast<int>(r);
      if ((row_y < 0) || (row_y >= text_rows)) continue;
      const Style& number = current ? ui.linenr_selected : ui.linenr;
      const Attr gutter_bg =
          (cursorline != nullptr) ? Bg(*cursorline, ui) : Bg(ui.linenr, ui);
      for (int x = 0; x < gutter; ++x) {
        Cell(x, row_y, ' ', Fg(ui.linenr, ui), gutter_bg);
      }
      if (cursorline != nullptr) {
        const Attr band_fg = Fg(*cursorline, ui) | Attrs(*cursorline);
        const Attr band_bg = Bg(*cursorline, ui);
        for (int x = gutter; x < width; ++x) Cell(x, row_y, ' ', band_fg, band_bg);
      }
      if (r == 0) {
        for (size_t c = 0; c < label.size(); ++c) {
          const int x = pad + static_cast<int>(c);
          if ((x >= 0) && (x < gutter)) {
            Cell(x, row_y, static_cast<uint32_t>(label[c]),
                        Fg(number, ui) | Attrs(number), gutter_bg);
          }
        }
      } else {
        DrawText(gutter, row_y, width, ed.settings.wrap_indicator,
                 Fg(ui.wrap.fg.set ? ui.wrap : ui.linenr, ui),
                 (cursorline != nullptr) ? Bg(*cursorline, ui) : Bg(ui.background, ui));
      }
    }

    if (current) {
      const size_t r = static_cast<size_t>(RowOfPosition(row_starts, cursor_pos));
      const Index column =
          ColumnBetween(doc.table, row_starts[r], cursor_pos, doc.tab_width);
      const Index indent = (r == 0) ? 0 : wrap.indent;
      caret_x = static_cast<int>(gutter + indent + column -
                                 (wrap.enabled ? 0 : pane.view.left_column));
      caret_y = line_y + static_cast<int>(r);
    }

    DrawLine(ed, pane, ui, highlights, line, line_y, gutter, width, text_rows, wrap,
             row_starts, scratch, bracket_a, bracket_b, cursorline, leap);
    line_y += static_cast<int>(row_starts.size());
  }

  if ((chrome_rows > 0) && !defer_status) {
    DrawStatus(ed, pane, ui, height - chrome_rows, width, status_clipped, overlay_takes_message);
  }

}

std::vector<std::string> WrapPlainText(std::string_view text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) return lines;
  std::string line;
  int line_w = 0;
  const auto flush = [&] {
    lines.push_back(std::move(line));
    line.clear();
    line_w = 0;
  };
  std::size_t i = 0;
  while (i < text.size()) {
    if (text[i] == '\n') {
      flush();
      ++i;
      continue;
    }
    std::size_t end = i;
    while ((end < text.size()) && (text[end] != ' ') && (text[end] != '\n') &&
           (text[end] != '\t')) {
      end = NextGraphemeInString(text, end);
    }
    std::string_view word = text.substr(i, end - i);
    const int word_w = DisplayWidthOf(word);
    const int lead = line.empty() ? 0 : 1;
    if ((line_w + lead + word_w) > width) {
      if (!line.empty()) flush();
      while (DisplayWidthOf(word) > width) {
        std::size_t cut = 0;
        int w = 0;
        while (cut < word.size()) {
          const std::size_t next = NextGraphemeInString(word, cut);
          const int piece = std::max(1, GraphemeWidth(word.substr(cut, next - cut)));
          if ((w + piece) > width) break;
          w += piece;
          cut = next;
        }
        lines.emplace_back(word.substr(0, cut));
        word.remove_prefix(cut);
      }
      line = std::string{word};
      line_w = DisplayWidthOf(word);
    } else {
      if (lead != 0) line += ' ';
      line += word;
      line_w += lead + word_w;
    }
    i = end;
    while ((i < text.size()) && ((text[i] == ' ') || (text[i] == '\t'))) ++i;
  }
  if (!line.empty() || lines.empty()) lines.push_back(std::move(line));
  return lines;
}

bool DrawStatusOverlay(const Editor& ed, const Palette& ui, int width, int height) {
  if (!StatusOverlayFits(width, height)) return false;
  const int inner_max = std::min(width - 6, 100);

  std::vector<std::string> lines = WrapPlainText(ed.status.text(), inner_max);
  int inner = 8;
  for (const std::string& line : lines) inner = std::max(inner, DisplayWidthOf(line));

  const int max_rows = std::max(1, (height / 2) - 2);
  if (static_cast<int>(lines.size()) > max_rows) {
    lines.resize(static_cast<std::size_t>(max_rows));
    lines.back() = "…";
  }
  const int rows = static_cast<int>(lines.size());

  const bool error = ed.status.level() == StatusLevel::kError;
  const Style& sev = error ? ui.status_error : ui.status_warning;
  const Attr border_fg = Fg(sev, ui) | Attrs(sev);
  const Attr text_fg = Fg(ui.text, ui);
  const Attr bg = Bg(ui.popup, ui);

  const int box_w = inner + 4;
  const int x0 = std::max(0, (width - box_w) / 2);
  const int y0 = height - 3 - rows;

  const auto hline = [&](int y, std::string_view left, std::string_view mid,
                         std::string_view right) {
    Put(x0, y, left, border_fg, bg);
    for (int x = 1; x < (box_w - 1); ++x) Put(x0 + x, y, mid, border_fg, bg);
    Put(x0 + box_w - 1, y, right, border_fg, bg);
  };
  const std::string title = error ? " error " : " warning ";
  hline(y0, "┌", "─", "┐");
  DrawText(x0 + 2, y0, x0 + 2 + DisplayWidthOf(title), title, border_fg | kAttrBold, bg);
  for (int r = 0; r < rows; ++r) {
    const int y = y0 + 1 + r;
    for (int x = 0; x < box_w; ++x) Cell(x0 + x, y, ' ', text_fg, bg);
    Put(x0, y, "│", border_fg, bg);
    Put(x0 + box_w - 1, y, "│", border_fg, bg);
    DrawText(x0 + 2, y, x0 + 2 + inner, lines[static_cast<std::size_t>(r)], text_fg, bg);
  }
  hline(y0 + 1 + rows, "└", "─", "┘");
  const std::string hint = "use :messages to see all messages";
  const int hint_w = DisplayWidthOf(hint);
  if (hint_w < (box_w - 4)) {
    DrawText(x0 + box_w - 2 - hint_w, y0 + 1 + rows, x0 + box_w - 2, hint,
             border_fg | kAttrDim, bg);
  }
  return true;
}

void DrawCompletions(const Editor& ed, const Palette& ui, int width, int height) {
  const std::vector<const TypableDef*> matches = PromptCompletions(ed);
  if (matches.empty() || (width <= 0)) return;

  constexpr int kMaxRows = 12;
  const int room = std::max(0, height - 1);
  const int rows = std::min({kMaxRows, static_cast<int>(matches.size()), room});
  if (rows <= 0) return;

  // Measured over the rows drawn, not over every match: a help column aligned
  // to the widest name in a list of seventy-six leaves the twelve on screen
  // reading as two lists with a void between them.
  int widest = 0;
  int helpest = 0;
  for (int r = 0; r < rows; ++r) {
    const TypableDef* def = matches[static_cast<std::size_t>(r)];
    widest = std::max(widest, DisplayWidthOf(def->name) + DisplayWidthOf(def->args) +
                                  (def->args.empty() ? 0 : 1));
    helpest = std::max(helpest, DisplayWidthOf(def->help));
  }
  widest = std::min(widest, std::max(1, width / 2));

  const std::string more =
      (static_cast<int>(matches.size()) > rows)
          ? ("+" + std::to_string(static_cast<int>(matches.size()) - rows))
          : std::string{};
  // The badge takes room of its own rather than being stamped over the first
  // row's help.
  const int badge = more.empty() ? 0 : (DisplayWidthOf(more) + 2);
  // The block is only as wide as what it says, and its own edge is what holds
  // it apart from the code either side of it. No border: those are the picker's,
  // and being the one surface that wears them is how a picker is told apart.
  const int block = std::min(width, widest + 3 + helpest + 1 + badge);

  const Attr fg = Fg(ui.statusline, ui);
  const Attr bg = Bg(ui.statusline, ui);
  const int top = height - 1 - rows;
  for (int r = 0; r < rows; ++r) {
    const TypableDef* def = matches[static_cast<std::size_t>(r)];
    const int y = top + r;
    for (int x = 0; x < block; ++x) Cell(x, y, ' ', fg, bg);

    int x = DrawText(1, y, block, def->name, fg | kAttrBold, bg);
    if (!def->args.empty()) {
      x = DrawText(x + 1, y, block, def->args, fg | kAttrDim, bg);
    }

    if (!def->help.empty()) {
      const int at = std::max(x + 1, widest + 3);
      if (at < block) DrawText(at, y, block, def->help, fg | kAttrDim, bg);
    }
  }

  if (!more.empty()) {
    const int at = block - DisplayWidthOf(more) - 1;
    if (at > 0) DrawText(at, top, block, more, fg | kAttrDim, bg);
  }
}

// A command or a pattern can be longer than its row is wide, so the row is a
// window onto it and scrolls the way a pane does: the sigil holds the first
// columns the way a gutter does, the input scrolls beside it, and the window
// moves only far enough to hold the caret -- which keeps the text on both
// sides of a mid-string caret up instead of snapping to the end. `width` is
// the columns available from `x0`; the terminal cursor lands on the caret.
void DrawPromptInput(Editor& ed, const Palette& ui, int x0, int y, int width, Attr fg,
                     Attr bg) {
  const std::string sigil{PromptSigil(ed)};
  const std::string& shown = ed.prompt_input;

  const int gutter = std::min(width, DisplayWidthOf(sigil));
  DrawText(x0, y, x0 + gutter, sigil, fg, bg);

  const std::size_t caret_at = std::min(ed.prompt_cursor, shown.size());
  const int caret_column = DisplayWidthOf(std::string_view{shown}.substr(0, caret_at));
  const int room = std::max(1, width - gutter);
  int& scroll = ed.prompt_scroll;
  if (scroll > caret_column) scroll = caret_column;
  if (caret_column >= (scroll + room)) scroll = caret_column - room + 1;
  scroll = std::clamp(scroll, 0, std::max(0, DisplayWidthOf(shown) - room + 1));

  const int limit = x0 + width;
  int x = x0 + gutter;
  int column = 0;
  size_t i = 0;
  while ((x < limit) && (i < shown.size())) {
    const size_t next = NextGraphemeInString(shown, i);
    const std::string_view cluster{shown.data() + i, next - i};
    const int cluster_width = std::max(1, GraphemeWidth(cluster));
    if ((column + cluster_width) > scroll) {
      if ((column < scroll) || ((x + cluster_width) > limit)) {
        // Half of a wide glyph is not a glyph: a cluster hanging off either
        // edge of the window is blanked, never split.
        for (int step = std::max(0, scroll - column); (step < cluster_width) && (x < limit);
             ++step) {
          Cell(x++, y, ' ', fg, bg);
        }
      } else {
        DrawCluster(x++, y, cluster, fg, bg);
        for (int step = 1; step < cluster_width; ++step) Cell(x++, y, ' ', fg, bg);
      }
    }
    column += cluster_width;
    i = next;
  }
  while (x < limit) Cell(x++, y, ' ', fg, bg);
  g_out->cursor_insert = true;
  SetCursor(std::clamp(x0 + gutter + caret_column - scroll, x0, std::max(x0, limit - 1)), y);
}

// One feedback row: the branch glyph, then the message around its marked
// destination, which wears ui.jump.next. A pad space each side, so the text
// the row floats over never touches the glyphs. `bg` is the caller's: the
// box's own interior under the box, chrome elsewhere.
void DrawBranchRow(Editor& ed, const Palette& ui, const FlatStatus& said, int x0, int y,
                   int width, bool below, Attr bg) {
  const Attr border = BorderFg(ui);
  const StatusLevel level = ed.status.level();
  const Style& sev = (level == StatusLevel::kError)     ? ui.status_error
                     : (level == StatusLevel::kWarning) ? ui.status_warning
                                                        : ui.status_info;
  const Attr sfg = Fg(sev, ui) | Attrs(sev);
  int x = DrawText(x0, y, width, " ", sfg, bg);
  x = DrawText(x, y, width, below ? "╰─▸ " : "╭─▸ ", border, bg);
  x = DrawText(x, y, width, said.before, sfg, bg);
  x = DrawText(x, y, width, said.target, Fg(ui.jump_next, ui) | Attrs(ui.jump_next), bg);
  x = DrawText(x, y, width, said.after, sfg, bg);
  DrawText(x, y, width, " ", sfg, bg);
}

// What the picker asks to hang off the box, before any of it has been placed.
// `band_grow` is what the band takes when the side the box lands on has room
// past `band_rows` -- buffers is the one list worth reading whole; everywhere
// else it is the baseline itself. `rule_rows` is the line between the block and
// the band, so context lines and list rows do not read as one list: one row,
// and only when there is a block.
struct PickerWant {
  int context_rows{0};
  int rule_rows{0};
  int band_rows{0};
  int band_grow{0};
  // The row under the list the count takes for itself, where the rows have no
  // right edge to hang it off (DrawPickerBand). Sources with a block only.
  int count_rows{0};
};

// Two borders and the input between them -- the one part of the stack that
// cannot be given up a row at a time.
constexpr int kPromptBoxRows = 3;

// Where the box landed and what the stack gave up to land there. The side, the
// rows drawn and the window the keys walk all come out of this one
// computation, so the band cannot draw one thing while the letters mean
// another. Everything stacks away from the caret -- box, branch row, context
// block, rule, band -- sharing the branch row's left edge. `ok` false means not
// even the box fits and the caller keeps a bottom row.
struct PromptFit {
  bool ok{false};
  bool below{false};
  int x0{0};
  int y0{0};
  int inner{0};
  int box_w{0};
  // The feedback the bar would have shown, flattened the way the bar flattens
  // it, and whether the stack found room for the branch row that says it. That
  // row is the last one given up, and when it goes the bar says it instead
  // (Editor::prompt_box_said).
  FlatStatus said;
  int said_rows{0};
  int context_rows{0};
  int rule_rows{0};
  int band_rows{0};
  int count_rows{0};
  // The card's border rows. The far one is always spent; the near one only when
  // a branch row stands between the box and the card, because otherwise the
  // box's border row on that side is the card's.
  int frame_near{0};
  int frame_far{0};
  int x{0};
  int context_top{0};
  int rule_top{0};
  int band_top{0};
};

// The smart-jump prompt sits at the caret instead of the bottom row: its loop
// is type, read, decide, several times a second, and the status line is
// peripheral to where the eyes already are. The box takes the rows below the
// caret and flips above when below would reach the focused status line.
//
// A side too short shrinks the stack rather than refusing it: a box that will
// not draw leaves the letters and enter live over rows nobody can see. The block
// and its rule go first -- they are the one part with nothing behind them --
// then band rows down to one, then the branch row, which the bar can say. Only
// a side with no room for the box at all is refused.
PromptFit FitPromptBox(const Editor& ed, int caret_x, int caret_y, const Rect& content, int width,
                       const PickerWant& want) {
  PromptFit fit;
  if ((caret_x < 0) || (caret_y < 0)) return fit;

  const int inner = std::min(width - 4, std::max(32, width / 3));
  if (inner < 12) return fit;

  fit.said = FlattenStatus(ed.status);
  // While a band hangs off the smart-jump box, the card is the feedback: the
  // count row says the total and the marked first row is the destination, so a
  // branch row under the box would say both twice. What the card cannot say --
  // a parse error, `not been there` -- has no band behind it and keeps the row.
  if ((ed.prompt_kind == PromptKind::kSmartJump) && (want.band_rows > 0)) fit.said = {};
  const int status_y = content.y + std::max(0, content.h - 1);
  // What each side has room for: down to the row above the focused status
  // line, up to the top of the pane.
  const int below_room = status_y - caret_y - 1;
  const int above_room = caret_y - content.y;
  // The side is chosen with the baseline band and never the grown one, so a
  // list growing into the room it found cannot move the box off it. Below is
  // preferred; above is taken when the whole stack fits there and not below;
  // when neither side fits whole the roomier one wins.
  const int frame = (want.band_rows > 0) ? 1 : 0;
  const int whole = kPromptBoxRows + (fit.said.empty() ? 0 : (1 + frame)) + want.context_rows +
                    want.rule_rows + want.band_rows + want.count_rows + frame;
  const bool below =
      (whole <= below_room) || ((whole > above_room) && (below_room >= above_room));
  const int room = below ? below_room : above_room;

  const int band_min = (want.band_rows > 0) ? 1 : 0;
  int said_rows = fit.said.empty() ? 0 : 1;
  int near = (said_rows > 0) ? frame : 0;
  int left = room - kPromptBoxRows - said_rows - near - frame;
  if (left < band_min) {
    said_rows = 0;
    near = 0;
    left = room - kPromptBoxRows - frame;
  }
  if (left < band_min) return fit;

  // The count's row is given up after the block and before the last list row: a
  // card holding a count and no list says nothing about anything.
  const int count = ((want.count_rows > 0) && (left > band_min)) ? want.count_rows : 0;
  left -= count;

  int band = std::min(std::max(want.band_rows, band_min), left);
  int context = 0;
  int rule = 0;
  if (band >= want.band_rows) {
    // The block takes what the band leaves, down to nothing: a grown
    // excerpt-context on a short side shows the lines nearest the target
    // (DrawPickerContext windows them) instead of vanishing whole.
    context = std::max(0, std::min(want.context_rows, left - band - want.rule_rows));
    rule = (context > 0) ? want.rule_rows : 0;
    // Only a stack that fit whole grows, into whatever the block left over.
    band = std::max(band, std::min(want.band_grow, left - context - rule));
  }

  fit.ok = true;
  fit.below = below;
  fit.inner = inner;
  fit.box_w = inner + 4;
  fit.said_rows = said_rows;
  fit.frame_near = near;
  fit.frame_far = (band > 0) ? frame : 0;
  fit.context_rows = context;
  fit.rule_rows = rule;
  fit.band_rows = band;
  fit.count_rows = count;
  fit.x0 = std::clamp(caret_x - 2, 0, std::max(0, width - fit.box_w));
  fit.y0 = below ? (caret_y + 1) : (caret_y - kPromptBoxRows);
  fit.x = fit.x0 + 1;
  fit.context_top = below ? (fit.y0 + kPromptBoxRows + said_rows + near)
                          : (fit.y0 - said_rows - near - context);
  // Away from the box either way: box, block, rule, band. The count's row is
  // the one after the last band row in both orientations, so above the caret it
  // is what the box's border row meets and the band starts a row higher.
  fit.rule_top = below ? (fit.context_top + context) : (fit.context_top - rule);
  fit.band_top = below ? (fit.rule_top + rule) : (fit.rule_top - band - count);
  return fit;
}

// The box the fit placed. The match feedback hangs off it on a branch row of
// its own, so the bar never has to be read at all -- below the box, or climbing
// out of its top when the box flipped above the caret.
void DrawPromptBox(Editor& ed, const Palette& ui, const PromptFit& fit, int width) {
  const int x0 = fit.x0;
  const int y0 = fit.y0;
  const int box_w = fit.box_w;

  // The editor's own fill, not the popup's: the box and the card it carries are
  // one surface, and two greys meeting at their shared border row reads as a
  // bleed rather than as a join. The border is what holds the box apart from
  // the code, which is why it is lifted.
  const Attr bg = Bg(ui.background, ui);
  const Attr fg = Fg(ui.text, ui);
  const Attr border = BorderFg(ui);

  const auto hline = [&](int y, std::string_view left, std::string_view mid,
                         std::string_view right) {
    Put(x0, y, left, border, bg);
    for (int x = 1; x < (box_w - 1); ++x) Put(x0 + x, y, mid, border, bg);
    Put(x0 + box_w - 1, y, right, border, bg);
  };
  hline(y0, "╭", "─", "╮");
  for (int x = 0; x < box_w; ++x) Cell(x0 + x, y0 + 1, ' ', fg, bg);
  Put(x0, y0 + 1, "│", border, bg);
  Put(x0 + box_w - 1, y0 + 1, "│", border, bg);
  DrawPromptInput(ed, ui, x0 + 2, y0 + 1, fit.inner, fg, bg);
  hline(y0 + 2, "╰", "─", "╯");

  if (fit.said_rows > 0) {
    DrawBranchRow(ed, ui, fit.said, x0 + 1, fit.below ? (y0 + 3) : (y0 - 1), width, fit.below, bg);
  }
}

// Where the card the block, the rule and the band share sits, and how wide.
// The width the filter computed, clipped to the screen, at the box's left edge
// -- pulled left where that width would run off the right, and all the way to
// the margin for a card as wide as the screen, which is what every picker over
// file content asks for (kPickerCardWide). Nothing here measures a row, so
// nothing here can resize the card on a step.
struct PickerCard {
  int x{0};
  int w{0};
};

PickerCard PickerCardAt(const PickerState& state, int x0, int width) {
  const int want = std::max(state.card_w, 8);
  // A card the screen cannot hold at the box's edge is pulled left until it
  // fits, which for a card wider than the screen is the margin: it stops
  // touching the box, and the band draws its plain indent instead of the
  // connector (RenderInto asks PickerCardAt whether it still touches).
  const int w = std::min(want, width);
  return {std::clamp(x0, 0, std::max(0, width - w)), w};
}

// The rule between the block and the band, in the card's own language: the
// popup fill, the band's indent, a dimmed line in the border's colour. Without
// it the context lines and the list rows read as one list.
void DrawPickerRule(const PickerState& state, const Palette& ui, int x0, int y, int width) {
  const Attr bg = Bg(ui.background, ui);
  const Attr fg = Fg(ui.text, ui);
  const Attr border = BorderFg(ui);
  const PickerCard card = PickerCardAt(state, x0, width);
  const int card_w = card.w;
  const int bx = card.x;
  for (int x = 0; x < card_w; ++x) Cell(bx + x, y, ' ', fg, bg);
  // From the text column the rows share to the card's far edge.
  for (int x = bx + 5; x < (bx + card_w - 1); ++x) Put(x, y, "─", border | kAttrDim, bg);
}

// The picker's band hangs off the box the way the branch row does: the same
// left edge, the popup fill only as wide as what it says, the connector on
// the row touching the box -- `touches` is false when the context block is
// between the two and carries it instead. Digits label the alt accelerators;
// the selected row wears ui.cursorline.primary, the band the editor puts under
// the cursor's own line. A dimmed index/shown/total count rides the last row, or
// `count_row` gives it one of its own under the list.
void DrawPickerBand(PickerState& state, const Palette& ui, int x0, int y0, int rows, int width,
                    bool below, bool touches, bool count_row) {
  const Attr bg = Bg(ui.background, ui);
  const Attr fg = Fg(ui.text, ui);
  const Attr border = BorderFg(ui);

  // i/n/m -- where the selection is in the filtered list, how much of the list
  // the filter left, how many candidates there were -- and while a child is
  // still feeding it, that it is still coming: a count that stood still would
  // read as the whole answer. Its width is arithmetic rather than a string
  // measured, because the row it rides is laid out around it. The index is
  // padded to n's width, so stepping from row 9 to row 10 does not widen the
  // count and move every path under it.
  const std::size_t shown = state.shown.size();
  const std::size_t total = PickerTotal(state);
  const std::size_t index = (shown == 0) ? 0 : (std::min(state.selected, shown - 1) + 1);
  const std::string_view note = PickerCountNote(state);
  const int index_w = PickerCountDigits(shown);
  const int count_w = (2 * index_w) + PickerCountDigits(total) + 2 + DisplayWidthOf(note);

  // One width for the whole card, taken from the shown rows when the filter
  // last changed: stepping into a longer file must not resize it (see
  // PickerCardWidth). Long rows clip.
  const PickerCard card = PickerCardAt(state, x0, width);
  const int band_w = card.w;
  const int bx = card.x;
  const int limit = bx + band_w - 1;
  // Where a row points takes a column of its own, two past the widest lead the
  // window holds -- for every source that draws a block, whether the row leads
  // with a name, with the line it points at, or with a matched line of the
  // project. The card is the screen for those, and a detail at its right edge
  // is a screen's width of nothing between the two halves of the row.
  const bool tailed = PickerShowsContext(state.source);
  // Past the indent, the row's letter and the space after it -- one column each
  // way whatever the row holds, so it is the same column on every row.
  const int lead_x = bx + 7;
  int lead_w = 0;
  if (tailed) {
    // The rows this frame draws and no others: the shown list is a scan's, with
    // no ceiling on its length, and this runs on every keystroke. Measured
    // before anything is drawn, so a row growing the column grows it for the
    // whole band and not from itself down. Neither shrinks -- PickerRefilter
    // clears both when the list is rebuilt.
    for (int r = 0; r < rows; ++r) {
      const std::size_t at = state.offset + static_cast<std::size_t>(r);
      if (at >= shown) break;
      const std::string_view detail = PickerRowDetail(state, at);
      // A row that is only a place -- a file match, a stored line that came
      // back empty -- is its path and nothing else (FillSmartBand), and a
      // content row with no `path:line:` head is only its line. Either takes
      // the whole row below, so it is measured into neither column: a deep path
      // that drove the lead column would push every other row's detail right
      // for a column it does not use.
      if (detail.empty()) continue;
      state.lead_w = std::max(state.lead_w, DisplayWidthOf(PickerRowLead(state, at)));
      state.detail_w = std::max(state.detail_w, DisplayWidthOf(detail));
    }
    // The lead takes what the widest detail leaves, so a short path hands its
    // slack to the lines -- but never below a fixed share of the row. One
    // nested path is otherwise enough to squeeze every line on the band down to
    // a few columns, and a band of clipped lines says nothing about which row
    // to take. The detail keeps the rest and clips from its front (below),
    // where a path can afford to.
    // Measured off the tightest row, which is the one the count rides when the
    // stack was too short to give the count a row of its own: the column is one
    // for the whole band, and a floor taken from the roomier rows leaves that
    // row a bare ellipsis where its path goes. One column narrower down the
    // band costs the other rows nothing they can read.
    const int tight = count_row ? limit : (limit - count_w - 1);
    const int avail = std::max(0, tight - lead_x - 2);
    // Never past `avail`, so the lead's own draw limit (lead_x + lead_w) stays
    // two columns short of the count and cannot be stamped over.
    lead_w = std::clamp(state.lead_w, 0, std::max(avail - state.detail_w, (avail * 11) / 20));
  }

  for (int r = 0; r < rows; ++r) {
    const int y = y0 + r;
    const std::size_t at = state.offset + static_cast<std::size_t>(r);
    const bool selected = (at < state.shown.size()) && (at == state.selected);
    const Attr row_bg = selected ? Bg(ui.cursorline_primary, ui) : bg;
    for (int x = 0; x < band_w; ++x) Cell(bx + x, y, ' ', fg, row_bg);
    // The count's own row is the one the box meets when the card hangs above.
    const bool joins = touches && (below ? (r == 0) : (!count_row && (r == (rows - 1))));
    int x = DrawText(bx + 1, y, limit, joins ? (below ? "╰─▸ " : "╭─▸ ") : "    ", border, row_bg);
    // A bar down the row's left edge, in what the row's text wears: the row
    // enter opens, findable without reading anything.
    if (selected) Put(bx + 1, y, "▌", Fg(ui.jump_next, ui), row_bg);
    // The row the count rides gives its right edge up rather than being stamped
    // over there: an untailed row always, and a tailed one only where the stack
    // was too short to give the count a row of its own.
    const int text_limit =
        ((tailed && count_row) || (r != (rows - 1))) ? limit : (limit - count_w - 1);
    if (at < state.shown.size()) {
      // Whatever holds the row -- an entry, or a byte offset into content's
      // corpus -- and whatever it leads with, resolved in one place
      // (PickerRowLead, PickerRowDetail).
      const std::string_view text = PickerRowLead(state, at);
      const std::string_view detail = PickerRowDetail(state, at);
      // The row enter would open wears what the branch row's destination
      // wears: ui.jump.next.
      const Attr text_fg = selected ? (Fg(ui.jump_next, ui) | Attrs(ui.jump_next)) : fg;
      // The letter names the window's row for its alt chord: i j k l a
      // wherever the band is scrolled. A band grown past those is walked rather
      // than lettered (PickerAccept), so the rows past them wear a space -- a
      // label nothing honours is worse than none.
      const bool lettered = r < static_cast<int>(kPickerRows);
      x = DrawText(x, y, limit, lettered ? kPickerRowKeys.substr(r, 1) : std::string_view{" "},
                   fg | kAttrDim, row_bg);
      if (tailed) {
        // The lead keeps its column whatever it holds and the detail starts two
        // past it, so the details read down the band as one column. A lead
        // longer than the column clips there; the detail always draws -- a row
        // whose line cannot say where it points is a row that cannot be taken.
        if (detail.empty()) {
          // Two shapes land here, and they clip from opposite ends. A place
          // row's lead is its path (FillSmartBand), so it has the row to itself
          // and keeps its end: clipped at the lead column it says
          // `/home/murali/` and names no file. A content row whose line carries
          // no `path:line:` head leads with the raw line instead
          // (PickerRowLead), and there the front is what names it -- so that
          // one draws head-first, the way it did before the column.
          if (state.source == PickerState::Source::kContent) {
            DrawText(lead_x, y, text_limit, text, text_fg, row_bg);
          } else {
            DrawTextTail(lead_x, y, text_limit, text, text_fg, row_bg);
          }
        } else {
          const int dx = lead_x + lead_w + 2;
          DrawText(lead_x, y, dx - 2, text, text_fg, row_bg);
          DrawTextTail(dx, y, text_limit, detail, fg | kAttrDim, row_bg);
        }
      } else {
        x = DrawText(x + 1, y, text_limit, text, text_fg, row_bg);
        DrawText(x, y, text_limit, detail, text_fg | kAttrDim, row_bg);
      }
    }
    if (!count_row && (r == (rows - 1))) {
      const int cx = limit - count_w;
      // Formatted where it is drawn: what FormatIntoStringView hands back is a
      // view of a buffer the next call to it takes, so nothing here holds one.
      if (cx > bx) {
        DrawText(cx, y, limit,
                 common::FormatIntoStringView<"%*lu/%lu/%lu%s">(index_w, index, shown, total, note),
                 fg, row_bg);
      }
    }
  }

  // A row of its own under the list, at the column the rows and the rule share.
  // The rows have no right edge left to hang it off: their detail column is a
  // measured one now, and a count out at the card's far edge would be the only
  // thing left reading across a screen.
  if (count_row) {
    const int y = y0 + rows;
    for (int x = 0; x < band_w; ++x) Cell(bx + x, y, ' ', fg, bg);
    const bool joins = touches && !below;
    const int x = DrawText(bx + 1, y, limit, joins ? "╭─▸ " : "    ", border, bg);
    DrawText(x, y, limit,
             common::FormatIntoStringView<"%*lu/%lu/%lu%s">(index_w, index, shown, total, note), fg,
             bg);
  }
}

// The block drawn between the box and the band: the lines around the selected
// row's target, a dimmed line-number gutter, and the target line in
// ui.excerpt.match. That is the `--no-syntax --highlight-line` look
// (cli.cpp), drawn in the editor -- no grammar runs, because this redraws on
// every keystroke and every step. The band's left edge and its indent, so the
// two read as one card.
void DrawPickerContext(const PickerState& state, const Palette& ui, int x0, int y0, int rows,
                       int width, bool below, bool touches) {
  if (state.context.empty()) return;
  // What the fill decided the target was, so nothing here has to know what holds
  // the row -- content's is a byte offset into a corpus.
  const Index target = state.context_target;

  const Attr bg = Bg(ui.background, ui);
  const Attr fg = Fg(ui.text, ui);
  const Attr border = BorderFg(ui);
  const Attr match = Fg(ui.excerpt_match, ui) | Attrs(ui.excerpt_match);

  // A block granted fewer rows than the lines read shows the ones nearest the
  // target: centred on it, pulled back inside what was read at either end.
  const std::size_t have = state.context.size();
  std::size_t start = 0;
  if (static_cast<std::size_t>(rows) < have) {
    const std::size_t at = (target > state.context_first)
                               ? static_cast<std::size_t>(target - state.context_first)
                               : 0;
    const std::size_t half = static_cast<std::size_t>(rows) / 2;
    start = std::min((at > half) ? (at - half) : 0, have - static_cast<std::size_t>(rows));
  }

  // No heading over the lines: the selected row says which file this is, in the
  // detail column, for every source that has a block at all.
  const int last = static_cast<int>(state.context_first + static_cast<Index>(start)) + rows - 1;
  const int gutter = PickerCountDigits(static_cast<std::size_t>(std::max(1, last)));
  // The band's width, not the block's own: the lines here change with every
  // step and the card they share may not change with them. Long lines clip.
  const PickerCard card = PickerCardAt(state, x0, width);
  const int card_w = card.w;
  const int bx = card.x;
  const int limit = bx + card_w - 1;

  for (int r = 0; r < rows; ++r) {
    const int y = y0 + r;
    for (int x = 0; x < card_w; ++x) Cell(bx + x, y, ' ', fg, bg);
    const bool joins = touches && (below ? (r == 0) : (r == (rows - 1)));
    int x = DrawText(bx + 1, y, limit, joins ? (below ? "╰─▸ " : "╭─▸ ") : "    ", border, bg);
    const auto at = start + static_cast<std::size_t>(r);
    if (at >= state.context.size()) continue;
    const Index number = state.context_first + static_cast<Index>(at);
    // Right-aligned in the gutter by the field width: the row's fill is already
    // drawn, so the padding the format writes is the fill it writes over.
    x = DrawText(x, y, limit, common::FormatIntoStringView<"%*td">(gutter, number), fg | kAttrDim,
                 bg);
    DrawText(x + 1, y, limit, state.context[at], (number == target) ? match : fg, bg);
  }
}

// The box around the block, the rule and the band, in the prompt box's own
// language and joined to it on the row the two share: the box's border row is
// the card's on that side, so the pair reads as one surface instead of a
// bordered box sitting on an unbordered slab.
void DrawPickerFrame(const PickerState& state, const Palette& ui, const PromptFit& fit, int width) {
  if (fit.frame_far == 0) return;
  const Attr bg = Bg(ui.background, ui);
  const Attr fg = Fg(ui.text, ui);
  const Attr border = BorderFg(ui);
  const PickerCard card = PickerCardAt(state, fit.x0, width);
  const int bx = card.x;
  const int right = bx + card.w - 1;

  // The count's row sits under the last list row in both orientations, so the
  // band block is the rows plus it.
  int lo = fit.band_top;
  int hi = fit.band_top + fit.band_rows + fit.count_rows - 1;
  if (fit.context_rows > 0) {
    lo = std::min(lo, fit.context_top);
    hi = std::max(hi, fit.context_top + fit.context_rows - 1);
  }
  if (fit.rule_rows > 0) {
    lo = std::min(lo, fit.rule_top);
    hi = std::max(hi, fit.rule_top);
  }
  for (int y = lo; y <= hi; ++y) {
    Put(bx, y, "│", border, bg);
    Put(right, y, "│", border, bg);
  }

  // The shared row, which the two boxes have to agree about. Three cases per
  // card edge, and the box is not always the wider of the two -- the box is
  // sized from the caret's screen (`inner`), the card from its rows, so either
  // can be wider: the edges coincide (a junction, the wall passing through),
  // the card's edge falls inside the box's span (a tee off the box's own
  // border), or it falls outside it (the card's own corner). Columns of the box
  // the card does not reach keep what the box drew there.
  const int near_y = fit.below ? (lo - 1) : (hi + 1);
  const bool joined = (fit.frame_near == 0);
  const int box_l = fit.x0;
  const int box_r = fit.x0 + fit.box_w - 1;
  const auto edge = [&](int c, std::string_view corner, std::string_view junction,
                        std::string_view tee) {
    if (!joined) return corner;
    if ((c == box_l) || (c == box_r)) return junction;
    return ((c > box_l) && (c < box_r)) ? tee : corner;
  };
  for (int c = bx; c <= right; ++c) {
    std::string_view glyph = "─";
    if (c == bx) {
      glyph = fit.below ? edge(c, "╭", "├", "┬") : edge(c, "╰", "├", "┴");
    } else if (c == right) {
      glyph = fit.below ? edge(c, "╮", "┤", "┬") : edge(c, "╯", "┤", "┴");
    } else if (joined && ((c == box_l) || (c == box_r))) {
      glyph = fit.below ? "┴" : "┬";
    }
    Cell(c, near_y, ' ', fg, bg);
    Put(c, near_y, glyph, border, bg);
  }

  const int far_y = fit.below ? (hi + 1) : (lo - 1);
  for (int c = bx; c <= right; ++c) {
    std::string_view glyph = "─";
    if (c == bx) {
      glyph = fit.below ? "╰" : "╭";
    } else if (c == right) {
      glyph = fit.below ? "╯" : "╮";
    }
    Cell(c, far_y, ' ', fg, bg);
    Put(c, far_y, glyph, border, bg);
  }
}

// A smart-jump arrival answers the walk's only question -- press again or
// stay -- right where the eyes are: the prompt's rounded box at the caret,
// sized to what it holds, naming where the next press goes. Below the caret,
// above when below would reach the status line. A bare row was tried first
// and read as part of the code it floated over; the border is what keeps it
// apart. No room, or no caret to hang from, and it does not draw -- false
// then, and the bar says the message instead (Editor::jump_branch_said).
bool DrawJumpBranch(Editor& ed, const Palette& ui, int caret_x, int caret_y,
                    const Rect& content, int width) {
  if ((caret_x < 0) || (caret_y < 0)) return false;
  const FlatStatus said = FlattenStatus(ed.status);
  if (said.empty()) return false;

  const int text_w =
      DisplayWidthOf(said.before) + DisplayWidthOf(said.target) + DisplayWidthOf(said.after);
  const int box_w = std::min(width, text_w + 4);
  if (box_w < 8) return false;

  const int status_y = content.y + std::max(0, content.h - 1);
  int y0 = caret_y + 1;
  if ((y0 + 2) >= status_y) y0 = caret_y - 3;
  if (y0 < content.y) return false;

  const int x0 = std::clamp(caret_x - 2, 0, std::max(0, width - box_w));

  const Attr bg = Bg(ui.background, ui);
  const Attr border = BorderFg(ui);
  const StatusLevel level = ed.status.level();
  const Style& sev = (level == StatusLevel::kError)     ? ui.status_error
                     : (level == StatusLevel::kWarning) ? ui.status_warning
                                                        : ui.status_info;
  const Attr sfg = Fg(sev, ui) | Attrs(sev);

  const auto hline = [&](int y, std::string_view left, std::string_view mid,
                         std::string_view right) {
    Put(x0, y, left, border, bg);
    for (int x = 1; x < (box_w - 1); ++x) Put(x0 + x, y, mid, border, bg);
    Put(x0 + box_w - 1, y, right, border, bg);
  };
  hline(y0, "╭", "─", "╮");
  for (int x = 0; x < box_w; ++x) Cell(x0 + x, y0 + 1, ' ', sfg, bg);
  Put(x0, y0 + 1, "│", border, bg);
  Put(x0 + box_w - 1, y0 + 1, "│", border, bg);
  const int limit = x0 + box_w - 2;
  int x = DrawText(x0 + 2, y0 + 1, limit, said.before, sfg, bg);
  x = DrawText(x, y0 + 1, limit, said.target, Fg(ui.jump_next, ui) | Attrs(ui.jump_next), bg);
  DrawText(x, y0 + 1, limit, said.after, sfg, bg);
  hline(y0 + 2, "╰", "─", "╯");
  return true;
}

void RenderInto(Editor& ed, int width, int height) {
  const Palette ui = Resolve(ed.theme);

  g_clip = Clip{0, 0, width, height};
  {
    const Attr fill_fg = Fg(ui.text, ui);
    const Attr fill_bg = Bg(ui.background, ui);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) Cell(x, y, ' ', fill_fg, fill_bg);
    }
  }

  const Panes panes = PanesOf(ed, width, height);
  const std::vector<Rect>& areas = panes.areas;
  const std::vector<int>& order = panes.order;

  int caret_x = -1;
  int caret_y = -1;
  Rect focused_content{0, 0, width, height};
  bool status_clipped = false;

  // A prompt that lives in a box at the caret decides what its branch row says
  // -- and so what the bar may not say twice -- only once the box has been
  // fitted, which needs the caret this loop finds. The focused bar waits for
  // that; every other pane's is drawn with the rest of it.
  const bool prompt_at_caret = ed.prompt_active &&
                               ((ed.prompt_kind == PromptKind::kSmartJump) ||
                                (ed.prompt_kind == PromptKind::kPicker));
  // An arrival's branch box is the same story: whether it found room at the
  // caret is known only once the box has been fitted, and the bar's message is
  // that answer's other half.
  const bool branch_at_caret = ed.jump_branch && !ed.prompt_active;
  std::optional<PaneRef> focused_pane;

  // Not over a leap: the popup is drawn after the panes and would cover labels
  // whose keys are still armed, hiding targets the user can still take by
  // pressing them. The message keeps its place in ed.status and in :messages
  // and the bar shows it as soon as the mode ends.
  const bool overlay_takes_message = !ed.prompt_active && LeapHint(ed).empty() &&
                                     !ed.status.empty() &&
                                     (ed.status.level() != StatusLevel::kInfo) &&
                                     StatusOverlayFits(width, height);

  const auto pane_of = [&ed, &order](std::size_t i) -> PaneRef {
    if (order.empty() || (order[i] == ed.focused)) {
      return PaneRef{ed.doc, ed.doc.view, ed.doc.selections, ed.active, true};
    }
    WindowNode& node = ed.windows[static_cast<std::size_t>(order[i])];
    const std::size_t buffer = (node.buffer < BufferCount(ed)) ? node.buffer : ed.active;
    const Document& doc = BufferAt(ed, buffer);
    SyncWindowSelections(doc.table, node);
    const Index last_line = std::max<Index>(0, LineCount(doc.table) - 1);
    if ((node.view.top_line < 0) || (node.view.top_line > last_line)) {
      node.view.top_line = std::clamp<Index>(node.view.top_line, 0, last_line);
      node.view.top_row = 0;
    }
    if (node.view.left_column < 0) node.view.left_column = 0;
    return PaneRef{doc, node.view, node.selections, buffer, false};
  };

  for (std::size_t i = 0; i < areas.size(); ++i) {
    const Rect& area = areas[i];

    if ((area.w <= 0) || (area.h <= 0)) continue;

    const PaneRef pane = pane_of(i);

    const int text_w = TextWidthOf(area, width);
    const Rect content = PaneContent(ed, area, width);
    if (pane.focused) {
      focused_content = content;
      focused_pane.emplace(pane);
    }
    g_clip = Clip{content.x, content.y, content.w, content.h};
    int pane_x = -1;
    int pane_y = -1;
    RenderPane(ed, pane, ui, content.w, content.h, pane_x, pane_y,
               pane.focused ? &status_clipped : nullptr,
               pane.focused && overlay_takes_message,
               pane.focused && (prompt_at_caret || branch_at_caret));

    if (text_w < area.w) {
      g_clip = Clip{area.x + text_w, area.y, area.w - text_w, area.h};
      const Attr rule_fg = Fg(ui.window.fg.set ? ui.window : ui.linenr, ui);
      const Attr rule_bg = Bg(ui.window.bg.set ? ui.window : ui.background, ui);
      for (int row = 0; row < area.h; ++row) Put(0, row, "│", rule_fg, rule_bg);
    }
    if (pane.focused && (pane_x >= 0) && (pane_y >= 0)) {
      caret_x = content.x + pane_x;
      caret_y = content.y + pane_y;
    }
  }

  g_clip = Clip{0, 0, width, height};

  ed.status_overlay =
      overlay_takes_message && status_clipped && DrawStatusOverlay(ed, ui, width, height);

  ed.jump_branch_said = false;
  if (branch_at_caret) {
    ed.jump_branch_said = DrawJumpBranch(ed, ui, caret_x, caret_y, focused_content, width);
    // Then the bar, which reads that flag: the box drew and the bar stays out
    // of it, or the box could not and the message is the bar's after all.
    if (focused_pane.has_value() && (focused_content.h >= 2)) {
      g_clip = Clip{focused_content.x, focused_content.y, focused_content.w, focused_content.h};
      DrawStatus(ed, *focused_pane, ui, focused_content.h - 1, focused_content.w);
      g_clip = Clip{0, 0, width, height};
    }
  }

  if (ed.prompt_active) {
    DrawCompletions(ed, ui, width, height);
    // What hangs off the box: a picker's filtered list, or -- while the
    // smart-jump prompt is open and its query has matched -- the top of the
    // ranking, built into the same thing and drawn by the same code.
    PickerState* band = nullptr;
    if (ed.prompt_kind == PromptKind::kPicker) {
      band = ed.picker.get();
    } else if (ed.prompt_kind == PromptKind::kSmartJump) {
      band = ed.smart_band.get();
    }
    PickerWant want;
    if (band != nullptr) {
      const auto shown = static_cast<int>(band->shown.size());
      // At least one row, so an empty filter still has somewhere to say 0/N.
      want.band_rows = std::clamp<int>(shown, 1, static_cast<int>(kPickerRows));
      // Buffers is the one list worth reading whole: it grows into whatever the
      // side the box takes has room for, past the baseline the side was chosen
      // with.
      want.band_grow = (band->source == PickerState::Source::kBuffers)
                           ? std::max(shown, want.band_rows)
                           : want.band_rows;
      // However many lines were read: a target near the top of its file, or in
      // a file that has gone away, is a shorter card or none.
      want.context_rows = static_cast<int>(band->context.size());
      want.rule_rows = (want.context_rows > 0) ? 1 : 0;
      // A source with a block draws its details in a measured column, so the
      // count has no right edge left to ride and takes a row instead.
      want.count_rows = PickerShowsContext(band->source) ? 1 : 0;
    }
    const PromptFit fit = prompt_at_caret
                              ? FitPromptBox(ed, caret_x, caret_y, focused_content, width, want)
                              : PromptFit{};
    // The bar asks this before it decides whether to say ed.status itself, and
    // only the fit knows whether the branch row survived the side it landed on.
    // A smart-jump band stands in for the row it displaced: the card said it,
    // so the bar echoing the feedback would say it a second time anyway.
    ed.prompt_box_said =
        fit.ok && ((fit.said_rows > 0) ||
                   ((ed.prompt_kind == PromptKind::kSmartJump) && (fit.band_rows > 0)));
    if (prompt_at_caret && focused_pane.has_value() && (focused_content.h >= 2)) {
      g_clip = Clip{focused_content.x, focused_content.y, focused_content.w, focused_content.h};
      DrawStatus(ed, *focused_pane, ui, focused_content.h - 1, focused_content.w);
      g_clip = Clip{0, 0, width, height};
    }
    if (band != nullptr) {
      // What stepping and the letters walk: the rows the band drew, which is
      // what the renderer alone knows and may be none of them. A window past
      // what is on screen would leave the accelerators opening rows nobody can
      // see. A resize that changed it leaves the window where the selection is,
      // not where the last screen had put it.
      band->window = static_cast<std::size_t>(fit.ok ? fit.band_rows : 0);
      PickerScrollToSelected(*band);
    }
    if (fit.ok) {
      DrawPromptBox(ed, ui, fit, width);
      if ((fit.band_rows > 0) && (band != nullptr)) {
        // A card too wide for the room at the box's edge is pulled left, and a
        // connector drawn from a card that no longer starts under the box
        // points at the code between the two. The plain indent is what is
        // honest there; the box stays where the caret put it either way.
        // The card's left edge is the box's, not one column in from it: the two
        // share a border row, and an edge a column out from under the box
        // leaves its corner dangling over the code with a gap beside it. A card
        // that takes the screen lands on the margin either way.
        DrawPickerContext(*band, ui, fit.x0, fit.context_top, fit.context_rows, width, fit.below,
                          false);
        if (fit.rule_rows > 0) DrawPickerRule(*band, ui, fit.x0, fit.rule_top, width);
        DrawPickerBand(*band, ui, fit.x0, fit.band_top, fit.band_rows, width, fit.below, false,
                       fit.count_rows > 0);
        DrawPickerFrame(*band, ui, fit, width);
      }
    } else {
      // PanesOf reserves no row for a prompt that lives in a box, so the bottom
      // row is the focused pane's status line: the fallback input sits above it
      // rather than over it.
      const int y = prompt_at_caret ? std::max(0, height - 2) : (height - 1);
      DrawPromptInput(ed, ui, 0, y, width, Fg(ui.text, ui), Bg(ui.background, ui));
    }
  } else {
    g_out->cursor_insert = (ed.mode == Mode::kInsert);
    if ((ed.mode == Mode::kInsert) && (caret_x >= 0) && (caret_y >= 0)) {
      SetCursor(caret_x, caret_y);
    } else {
      HideCursor();
    }
  }
}
void SetCursor(int x, int y) {
  if (g_out == nullptr) return;
  g_out->cursor_x = x;
  g_out->cursor_y = y;
  g_out->cursor_visible = true;
}

void HideCursor() {
  if (g_out == nullptr) return;
  g_out->cursor_visible = false;
}

}

void Surface::Reset(int w, int h) {
  width = std::max(0, w);
  height = std::max(0, h);
  cells.assign(static_cast<std::size_t>(width) * height, Glyph{});
}

std::string Surface::Row(int y) const {
  std::string out;
  if ((y < 0) || (y >= height)) return out;
  for (int x = 0; x < width; ++x) {
    const Glyph& cell = At(x, y);
    out += cell.text.empty() ? " " : cell.text;
  }
  while (!out.empty() && (out.back() == ' ')) out.pop_back();
  return out;
}

void RenderTo(Editor& ed, Surface& out, int width, int height) {
  ed.screen_w = width;
  ed.screen_h = height;
  out.Reset(width, height);
  g_out = &out;
  RenderInto(ed, width, height);
  g_out = nullptr;
}

int DividerAtPoint(const Editor& ed, int x, int y, int width, int height) {
  return DividerAt(ed, x, y, PanesOf(ed, width, height).screen);
}

bool DragDivider(Editor& ed, int node, int x, int y, int width, int height) {
  return MoveDivider(ed, node, x, y, PanesOf(ed, width, height).screen);
}

int WindowAtPoint(const Editor& ed, int x, int y, int width, int height, Rect& out_area) {
  const Panes panes = PanesOf(ed, width, height);
  for (std::size_t i = 0; i < panes.areas.size(); ++i) {
    const Rect& r = panes.areas[i];
    if ((r.w <= 0) || (r.h <= 0)) continue;
    if ((x < r.x) || (y < r.y) || (x >= (r.x + r.w)) || (y >= (r.y + r.h))) continue;
    out_area = r;

    return panes.order.empty() ? 0 : panes.order[i];
  }
  return -1;
}

}
