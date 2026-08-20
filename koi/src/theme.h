#ifndef KOI_THEME_H_
#define KOI_THEME_H_

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace koi {

enum StyleMod : std::uint16_t {
  kModBold = 1u << 0,
  kModDim = 1u << 1,
  kModItalic = 1u << 2,
  kModUnderlined = 1u << 3,
  kModSlowBlink = 1u << 4,
  kModRapidBlink = 1u << 5,
  kModReversed = 1u << 6,
  kModHidden = 1u << 7,
  kModCrossedOut = 1u << 8,
};

struct Color {
  bool set{false};
  std::uint32_t rgb{0};
  // From an `#rrggbbaa` colour, 255 being opaque. A terminal cell holds one
  // colour and has nowhere to put transparency, so this means something only
  // between ParseColor and the compositing at the end of LoadTheme: every
  // colour a loaded theme hands to the renderer is opaque.
  std::uint8_t alpha{255};
};

struct Style {
  Color fg;
  Color bg;
  std::uint16_t mods{0};

  Style Over(const Style& other) const;
};

// Whether a style would change anything on the screen. Layering one that sets
// nothing over the text leaves the text exactly as it was, which for a piece
// of chrome means it was never drawn.
inline bool Paints(const Style& s) { return s.fg.set || s.bg.set || (s.mods != 0); }

struct Theme {
  std::string name;
  std::map<std::string, Style, std::less<>> scopes;

  Style Get(std::string_view scope) const;
};

bool LoadTheme(std::string_view name, Theme& out, std::string& error);

Theme BuiltinTheme();

bool ParseColor(std::string_view text, Color& out);

std::vector<std::string_view> UiScopes();

}

#endif
