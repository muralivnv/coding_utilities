#ifndef KOI_RENDER_H_
#define KOI_RENDER_H_

#include <cstdint>
#include <string>
#include <vector>

#include "editor.h"

namespace koi {

using Attr = std::uint64_t;

inline constexpr Attr kAttrBold = 0x01000000;
inline constexpr Attr kAttrUnderline = 0x02000000;
inline constexpr Attr kAttrReverse = 0x04000000;
inline constexpr Attr kAttrItalic = 0x08000000;
inline constexpr Attr kAttrBlink = 0x10000000;
inline constexpr Attr kAttrHiBlack = 0x20000000;
inline constexpr Attr kAttrDim = 0x80000000;
inline constexpr Attr kAttrStrikeout = 0x0000000100000000;
inline constexpr Attr kAttrDefault = 0x0000;

struct Glyph {

  std::string text;
  Attr fg{0};
  Attr bg{0};
};

struct Surface {
  int width{0};
  int height{0};
  std::vector<Glyph> cells;

  int cursor_x{-1};
  int cursor_y{-1};
  bool cursor_visible{false};
  bool cursor_insert{false};

  void Reset(int w, int h);
  Glyph& At(int x, int y) { return cells[static_cast<std::size_t>((y * width) + x)]; }
  const Glyph& At(int x, int y) const {
    return cells[static_cast<std::size_t>((y * width) + x)];
  }
  bool Holds(int x, int y) const {
    return (x >= 0) && (y >= 0) && (x < width) && (y < height);
  }

  std::string Row(int y) const;
};

void RenderTo(Editor& ed, Surface& out, int width, int height);

void FitFocusedViewport(Editor& ed, int width, int height);

// The scrolloff margin for this frame, and the one place align_view_once is
// consumed.
Index EffectiveScrolloff(Editor& ed);

int GutterWidth(const Editor& ed, int pane_width);

WrapMetrics WrapOf(const Editor& ed, int gutter, int width);

// The metrics the focused pane is drawn with, read off the viewport rather
// than off a pane rectangle: FitFocusedViewport has already taken the gutter
// out of view.columns, so this is the same wrap the renderer hands DrawLine
// for that pane. Exported so that everything reading the focused pane's rows
// -- the fit, the leap -- asks one definition, and a change to how the
// renderer wraps cannot leave a second copy of it behind somewhere else.
WrapMetrics WrapForFocusedViewport(const Editor& ed);

Rect PaneContent(const Editor& ed, const Rect& area, int screen_w);

int WindowAtPoint(const Editor& ed, int x, int y, int width, int height, Rect& out_area);

int DividerAtPoint(const Editor& ed, int x, int y, int width, int height);

bool DragDivider(Editor& ed, int node, int x, int y, int width, int height);

int TextWidthOf(const Rect& area, int screen_w);

Index PositionAtScreen(const Editor& ed, const WrapMetrics& wrap, int gutter, int click_x,
                       int click_y);

}

#endif
