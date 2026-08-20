#include "theme.h"

#include <toml++/toml.hpp>

#include <array>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <utility>
#include <vector>

#include "syntax.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

struct NamedColor {
  std::string_view name;
  std::uint32_t rgb;
};

constexpr std::array kNamedColors{
    NamedColor{"black", 0x000000},         NamedColor{"red", 0xCC0000},
    NamedColor{"green", 0x4E9A06},         NamedColor{"yellow", 0xC4A000},
    NamedColor{"blue", 0x3465A4},          NamedColor{"magenta", 0x75507B},
    NamedColor{"cyan", 0x06989A},          NamedColor{"gray", 0x555753},
    NamedColor{"light-red", 0xEF2929},     NamedColor{"light-green", 0x8AE234},
    NamedColor{"light-yellow", 0xFCE94F},  NamedColor{"light-blue", 0x729FCF},
    NamedColor{"light-magenta", 0xAD7FA8}, NamedColor{"light-cyan", 0x34E2E2},
    NamedColor{"light-gray", 0xD3D7CF},    NamedColor{"white", 0xEEEEEC},
};

struct NamedMod {
  std::string_view name;
  StyleMod bit;
};

constexpr std::array kNamedMods{
    NamedMod{"bold", kModBold},
    NamedMod{"dim", kModDim},
    NamedMod{"italic", kModItalic},
    NamedMod{"underlined", kModUnderlined},
    NamedMod{"underline", kModUnderlined},
    NamedMod{"slow_blink", kModSlowBlink},
    NamedMod{"rapid_blink", kModRapidBlink},
    NamedMod{"reversed", kModReversed},
    NamedMod{"hidden", kModHidden},
    NamedMod{"crossed_out", kModCrossedOut},
};

bool HexDigit(char c, std::uint32_t& out) {
  if ((c >= '0') && (c <= '9')) { out = static_cast<std::uint32_t>(c - '0'); return true; }
  if ((c >= 'a') && (c <= 'f')) { out = static_cast<std::uint32_t>(c - 'a' + 10); return true; }
  if ((c >= 'A') && (c <= 'F')) { out = static_cast<std::uint32_t>(c - 'A' + 10); return true; }
  return false;
}

bool ResolveColor(std::string_view text, const std::map<std::string, Color, std::less<>>& palette,
                  Color& out) {
  if (const auto found = palette.find(text); found != palette.end()) {
    out = found->second;
    return true;
  }
  return ParseColor(text, out);
}

void ReadStyle(const toml::node& node, const std::map<std::string, Color, std::less<>>& palette,
               Style& out) {
  if (const auto* text = node.as_string()) {
    ResolveColor(text->get(), palette, out.fg);
    return;
  }
  const toml::table* table = node.as_table();
  if (table == nullptr) return;

  if (const auto* fg = table->get_as<std::string>("fg")) {
    ResolveColor(fg->get(), palette, out.fg);
  }
  if (const auto* bg = table->get_as<std::string>("bg")) {
    ResolveColor(bg->get(), palette, out.bg);
  }
  if (const auto* mods = table->get_as<toml::array>("modifiers")) {
    for (const toml::node& entry : *mods) {
      const auto* name = entry.as_string();
      if (name == nullptr) continue;
      for (const NamedMod& known : kNamedMods) {
        if (known.name == name->get()) out.mods |= known.bit;
      }
    }
  }
}

bool LoadThemeInto(std::string_view name, Theme& out, std::unordered_set<std::string>& seen,
                   std::string& error) {
  if (!seen.insert(std::string{name}).second) return true;

  const fs::path path = FindRuntimeFile(fs::path{"themes"} / (std::string{name} + ".toml"));
  if (path.empty()) {
    error = "no theme " + std::string{name} + " on the runtime path";
    return false;
  }
  std::ifstream in{path};
  if (!in) {
    error = "cannot read " + path.string();
    return false;
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();

  toml::table root;
  try {
    root = toml::parse(buffer.str());
  } catch (const toml::parse_error& err) {
    error = path.filename().string() + ": " + std::string{err.description()};
    return false;
  }

  if (const auto* parent = root.get_as<std::string>("inherits")) {
    std::string ignored;
    LoadThemeInto(parent->get(), out, seen, ignored);
  }

  std::map<std::string, Color, std::less<>> palette;
  if (const toml::table* entries = root.get_as<toml::table>("palette")) {
    for (const auto& [key, value] : *entries) {
      const auto* text = value.as_string();
      Color color;
      if ((text != nullptr) && ParseColor(text->get(), color)) {
        palette.emplace(std::string{key.str()}, color);
      }
    }
  }

  for (const auto& [key, value] : root) {
    const std::string scope{key.str()};
    if ((scope == "palette") || (scope == "inherits")) continue;
    Style style = out.scopes.contains(scope) ? out.scopes[scope] : Style{};
    ReadStyle(value, palette, style);
    out.scopes[scope] = style;
  }
  return true;
}

// src-over, per channel: what the author would see with the colour laid over
// `under` at that alpha.
std::uint32_t Composite(std::uint32_t over, std::uint32_t under, std::uint32_t alpha) {
  std::uint32_t out = 0;
  for (const int shift : {16, 8, 0}) {
    const std::uint32_t a = (over >> shift) & 0xFFu;
    const std::uint32_t b = (under >> shift) & 0xFFu;
    out |= (((a * alpha) + (b * (0xFFu - alpha)) + 127u) / 0xFFu) << shift;
  }
  return out;
}

void Flatten(Color& color, const Color& under) {
  if (color.set && (color.alpha != 0xFF) && under.set) {
    color.rgb = Composite(color.rgb, under.rgb, color.alpha);
  }
  color.alpha = 0xFF;
}

// An alpha in a theme file says what the colour should end up *looking* like,
// and a terminal cell can only be told one opaque colour, so the compositing
// has to happen here rather than at the cell. Once, and last: the background a
// colour is laid over is not known until the inherits chain, the palette and
// the builtin fallback have all had their say.
//
// A background is laid over the theme's background. A foreground is laid over
// its own scope's background where the scope has one, because that is what is
// actually painted under the glyph, and over the theme's background otherwise.
// `ui.background` is the bottom of the stack -- what is under it is whatever
// the terminal draws, which koi neither knows nor controls -- so its alpha is
// dropped rather than composited against a guess, and so is every alpha in a
// theme that gives `ui.background` no background at all.
void FlattenAlpha(Theme& theme) {
  // Taken as authored, alpha and all: laid over itself it comes out unchanged,
  // which is the "dropped" above, and what everything else is laid over.
  const Color base = theme.Get("ui.background").bg;
  for (auto& [scope, style] : theme.scopes) {
    Flatten(style.bg, base);
    Flatten(style.fg, style.bg.set ? style.bg : base);
  }
}

}

Style Style::Over(const Style& other) const {
  Style merged = *this;
  if (other.fg.set) merged.fg = other.fg;
  if (other.bg.set) merged.bg = other.bg;
  merged.mods |= other.mods;
  return merged;
}

bool ParseColor(std::string_view text, Color& out) {
  if (text.starts_with('#')) {
    const std::string_view digits = text.substr(1);
    if ((digits.size() != 6) && (digits.size() != 8)) return false;
    std::uint32_t rgb = 0;
    for (size_t i = 0; i < 6; ++i) {
      std::uint32_t nibble = 0;
      if (!HexDigit(digits[i], nibble)) return false;
      rgb = (rgb << 4) | nibble;
    }
    std::uint32_t alpha = 0xFF;
    if (digits.size() == 8) {
      alpha = 0;
      for (size_t i = 6; i < 8; ++i) {
        std::uint32_t nibble = 0;
        if (!HexDigit(digits[i], nibble)) return false;
        alpha = (alpha << 4) | nibble;
      }
    }
    out = Color{true, rgb, static_cast<std::uint8_t>(alpha)};
    return true;
  }
  for (const NamedColor& known : kNamedColors) {
    if (known.name == text) {
      out = Color{true, known.rgb};
      return true;
    }
  }
  return false;
}

Style Theme::Get(std::string_view scope) const {
  std::string_view prefix = scope;
  while (true) {
    if (const auto found = scopes.find(prefix); found != scopes.end()) return found->second;
    const size_t dot = prefix.rfind('.');
    if (dot == std::string_view::npos) return Style{};
    prefix = prefix.substr(0, dot);
  }
}

bool LoadTheme(std::string_view name, Theme& out, std::string& error) {
  Theme loaded;
  loaded.name = name;
  std::unordered_set<std::string> seen;
  if (!LoadThemeInto(name, loaded, seen, error)) return false;

  // A theme is a set of overrides, not a promise to name every scope koi will
  // ever draw -- and a theme file is a copy somebody made on whatever day they
  // made it. What it leaves out reaches Theme::Get's prefix walk, which for a
  // chrome scope with no ancestor in the file ends at a Style that sets
  // nothing: chrome painted in the text's own colours, which for a jump label
  // is a key indistinguishable from the letter it stands on.
  //
  // The test is what the theme *paints*, not which keys it spells out: a theme
  // that gave `ui.virtual` a colour meant that colour for everything under it,
  // and an exact scope backfilled from the builtin would shadow it. Only a
  // walk that lands on nothing at all is a scope the theme has no opinion
  // about. Decided against the theme as loaded and applied afterwards, so no
  // backfilled ancestor can decide the answer for a scope below it.
  const Theme base = BuiltinTheme();
  std::vector<std::pair<std::string, Style>> missing;
  for (const std::string_view scope : UiScopes()) {
    if (Paints(loaded.Get(scope))) continue;
    if (const Style fallback = base.Get(scope); Paints(fallback)) {
      missing.emplace_back(std::string{scope}, fallback);
    }
  }
  for (auto& [scope, style] : missing) loaded.scopes.insert_or_assign(std::move(scope), style);

  FlattenAlpha(loaded);
  out = std::move(loaded);
  return true;
}

Theme BuiltinTheme() {
  Theme theme;
  theme.name = "builtin";
  const auto rgb = [](std::uint32_t value) { return Color{true, value}; };
  const auto set = [&theme](std::string_view scope, Style style) {
    theme.scopes[std::string{scope}] = style;
  };

  set("ui.text", Style{rgb(0xBFBFBF), {}, 0});
  set("ui.background", Style{{}, rgb(0x252524), 0});
  set("ui.linenr", Style{rgb(0x7D7D7D), {}, 0});
  set("ui.linenr.selected", Style{rgb(0xBFBFBF), {}, kModBold});
  set("ui.selection", Style{{}, rgb(0x474B43), 0});
  set("ui.cursor", Style{rgb(0xBFBFBF), {}, kModReversed});
  set("ui.cursor.primary", Style{rgb(0x65B0B2), {}, kModReversed});
  set("ui.cursor.match", Style{{}, rgb(0x6C4E1E), 0});
  set("ui.virtual.jump-label", Style{rgb(0x252524), rgb(0xFFCE78), kModBold});
  set("ui.excerpt.header", Style{rgb(0xD19A66), {}, kModBold});
  set("ui.excerpt.match", Style{rgb(0xEF2929), {}, kModBold});
  set("ui.statusline", Style{rgb(0xBFBFBF), rgb(0x313131), 0});
  set("ui.statusline.normal", Style{rgb(0x252524), rgb(0x8EC87C), 0});
  set("ui.statusline.insert", Style{rgb(0x252524), rgb(0xD19A66), 0});
  set("comment", Style{rgb(0x636C6E), {}, 0});
  set("string", Style{rgb(0x8EC87C), {}, 0});
  set("constant", Style{rgb(0x8EC87C), {}, 0});
  set("keyword", Style{rgb(0xBFBFBF), {}, 0});
  set("keyword.control", Style{rgb(0xFF8665), {}, 0});
  set("operator", Style{rgb(0xFF8665), {}, 0});
  set("function", Style{rgb(0xFFCE78), {}, 0});
  set("type", Style{rgb(0x65B0B2), {}, 0});
  set("error", Style{rgb(0xFF8665), {}, 0});
  return theme;
}

std::vector<std::string_view> UiScopes() {
  return {
      "ui.background",       "ui.text",           "ui.linenr",
      "ui.linenr.selected",  "ui.selection",      "ui.selection.primary",
      "ui.cursor",           "ui.cursor.primary", "ui.cursor.match",
      "ui.cursorline.primary", "ui.cursorline.secondary",
      "ui.excerpt.header",  "ui.excerpt.match",
      "ui.statusline",       "ui.statusline.normal", "ui.statusline.insert",
      "ui.virtual.wrap",     "ui.virtual.whitespace", "ui.virtual.jump-label",
  };
}

}
