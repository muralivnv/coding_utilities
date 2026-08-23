// Tests for theme.cpp: loading a theme, resolving a scope through it, and the
// builtin the resolution falls back to.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void Themes() {
  TEST_CASE("themes");

  Color color;
  EXPECT_TRUE(ParseColor("#ff8665", color));
  EXPECT_EQ(color.rgb, 0xFF8665u);
  EXPECT_EQ(unsigned{color.alpha}, 0xFFu);
  EXPECT_TRUE(ParseColor("#474B43A4", color));
  EXPECT_EQ(color.rgb, 0x474B43u);
  EXPECT_EQ(unsigned{color.alpha}, 0xA4u);
  EXPECT_TRUE(ParseColor("light-yellow", color));
  EXPECT_EQ(unsigned{color.alpha}, 0xFFu);
  EXPECT_TRUE(!ParseColor("#abc", color));
  EXPECT_TRUE(!ParseColor("chartreuse", color));
  EXPECT_TRUE(!ParseColor("#474B43AG", color));

  Theme theme;
  std::string error;
  EXPECT_TRUE(LoadTheme("ronin", theme, error));
  EXPECT_TRUE(error.empty());

  const Style keyword = theme.Get("keyword.control");
  EXPECT_TRUE(keyword.fg.set);
  EXPECT_EQ(keyword.fg.rgb, 0xFF8665u);

  const Style ret = theme.Get("keyword.control.return");
  EXPECT_EQ(ret.fg.rgb, 0xFFA07Au);
  EXPECT_TRUE((ret.mods & kModUnderlined) != 0);
  EXPECT_EQ(theme.Get("keyword.control.conditional").fg.rgb, 0xFF8665u);

  EXPECT_TRUE(!theme.Get("nonsense.scope").fg.set);

  EXPECT_TRUE(theme.Get("diff.plus").fg.set);

  EXPECT_TRUE(LoadTheme("calm", theme, error));
  EXPECT_TRUE(!LoadTheme("no-such-theme", theme, error));
  EXPECT_TRUE(!error.empty());
  EXPECT_EQ(theme.name, std::string{"calm"});

  Theme ronin;
  EXPECT_TRUE(LoadTheme("ronin", ronin, error));
  for (const std::string_view scope : UiScopes()) {
    const Style style = ronin.Get(scope);
    if (scope == std::string_view{"ui.virtual.wrap"}) continue;
    if (scope == std::string_view{"ui.virtual.whitespace"}) continue;
    if (scope == std::string_view{"ui.cursorline.secondary"}) continue;
    EXPECT_TRUE(style.fg.set || style.bg.set || (style.mods != 0));
  }

  // A leap label is one cell of chrome drawn over the text, so a foreground
  // alone would leave it a letter among letters. Every theme koi ships, and
  // the one it falls back on with no theme file at all, gives it both.
  for (const std::string_view name : {"ronin", "calm"}) {
    Theme shipped;
    EXPECT_TRUE(LoadTheme(name, shipped, error));
    const Style label = shipped.Get("ui.virtual.jump-label");
    EXPECT_TRUE(label.fg.set);
    EXPECT_TRUE(label.bg.set);
  }
  const Style builtin_label = BuiltinTheme().Get("ui.virtual.jump-label");
  EXPECT_TRUE(builtin_label.fg.set);
  EXPECT_TRUE(builtin_label.bg.set);

  // The smart-jump destination colour needs only a foreground, but every
  // shipped theme and the builtin must give it one, or the path a message
  // names dresses exactly like the file name beside it.
  for (const std::string_view name : {"ronin", "calm"}) {
    Theme shipped;
    EXPECT_TRUE(LoadTheme(name, shipped, error));
    EXPECT_TRUE(shipped.Get("ui.jump.next").fg.set);
  }
  EXPECT_TRUE(BuiltinTheme().Get("ui.jump.next").fg.set);
}

namespace {

// A themes/ directory under the runtime root koi looks in first --
// $HOME/.config/ronin/koi, and $HOME here is this run's private one. It is the
// only way to put a *user's* theme in front of LoadTheme: one written before a
// scope existed, which is what every theme older than a feature is.
struct FakeThemeDir {
  std::filesystem::path dir;

  FakeThemeDir() {
    const char* home = std::getenv("HOME");
    if ((home == nullptr) || (*home == '\0')) return;
    dir = std::filesystem::path{home} / ".config" / "ronin" / "koi" / "themes";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
  }
  ~FakeThemeDir() { RemoveAllQuietly(dir); }
  FakeThemeDir(const FakeThemeDir&) = delete;
  FakeThemeDir& operator=(const FakeThemeDir&) = delete;

  bool Ready() const { return !dir.empty() && std::filesystem::exists(dir); }

  void Write(std::string_view name, std::string_view source) const {
    if (!dir.empty()) WriteFixtureFile(dir / (std::string{name} + ".toml"), source);
  }
};

}  // namespace

void ThemesFallBackToTheBuiltinForScopesTheyNeverNamed() {
  Presser press;

  const FakeThemeDir themes;
  if (!themes.Ready()) {
    EXPECT_TRUE(themes.Ready());
    return;
  }
  const Style builtin = BuiltinTheme().Get("ui.virtual.jump-label");

  TEST_CASE("theme: a scope a theme never names is drawn in the builtin's style, not in none");
  {
    // A theme written before jump labels existed. Nothing under `ui.virtual`,
    // and no bare `ui` either, so Theme::Get's prefix walk has nowhere to land.
    themes.Write("audit-no-label", R"THEME(
"ui.text"          = { fg = "#bfbfbf" }
"ui.background"    = { bg = "#101010" }
"ui.cursor.match"  = { bg = "#6c4e1e" }
"ui.statusline"    = { fg = "#bfbfbf", bg = "#313131" }
"keyword"          = { fg = "#ff8665" }
)THEME");

    Theme theme;
    std::string error;
    EXPECT_TRUE(LoadTheme("audit-no-label", theme, error));
    EXPECT_TRUE(error.empty());

    const Style label = theme.Get("ui.virtual.jump-label");
    EXPECT_TRUE(Paints(label));
    EXPECT_TRUE(label.fg.set);
    EXPECT_TRUE(label.bg.set);
    EXPECT_EQ(label.fg.rgb, builtin.fg.rgb);
    EXPECT_EQ(label.bg.rgb, builtin.bg.rgb);
    EXPECT_EQ(label.mods, builtin.mods);
    // What the theme did say is untouched, backfill or no backfill.
    EXPECT_EQ(theme.Get("ui.background").bg.rgb, 0x101010u);
    EXPECT_EQ(theme.Get("keyword").fg.rgb, 0xFF8665u);
    // And a scope neither the theme nor the builtin paints stays unpainted:
    // the fallback fills gaps, it does not invent chrome.
    EXPECT_TRUE(!Paints(theme.Get("ui.virtual.whitespace")));

    // The labels are legible on the screen, which is the point of all this: a
    // label cell no longer reads as one of the document's own letters.
    Editor ed;
    ed.theme = theme;
    std::string text;
    for (int i = 0; i < 6; ++i) text += "ab..\n";
    ResetToOriginal(ed.doc.table, text);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    constexpr int kWidth = 40;
    constexpr int kHeight = 10;
    FitFocusedViewport(ed, kWidth, kHeight);
    const int gutter = GutterWidth(ed, kWidth);

    Surface plain;
    RenderTo(ed, plain, kWidth, kHeight);
    press(ed, "y");
    press(ed, "a");
    press(ed, "b");
    EXPECT_TRUE(ed.leap.stage == LeapState::Stage::kLabel);
    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);

    int seen = 0;
    for (const LeapLabel& one : ed.leap.labels) {
      const Index line = LineAt(ed.doc.table, one.at);
      const int x = gutter + static_cast<int>(one.at - LineStart(ed.doc.table, line));
      const int y = static_cast<int>(line - ed.doc.view.top_line);
      const Glyph& cell = frame.At(x, y);
      EXPECT_EQ(cell.text, std::string(1, one.key));
      EXPECT_EQ(cell.bg, static_cast<Attr>(builtin.bg.rgb));
      // Not the colours the character it replaced was drawn in: that is
      // exactly the confusion the fallback exists to prevent.
      EXPECT_TRUE((cell.fg != plain.At(x, y).fg) || (cell.bg != plain.At(x, y).bg));
      ++seen;
    }
    EXPECT_TRUE(seen > 0);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("theme: a theme that names the scope keeps its own style");
  {
    themes.Write("audit-own-label", R"THEME(
"ui.text"                = { fg = "#bfbfbf" }
"ui.virtual.jump-label"  = { fg = "#010203", bg = "#040506" }
)THEME");

    Theme theme;
    std::string error;
    EXPECT_TRUE(LoadTheme("audit-own-label", theme, error));
    const Style label = theme.Get("ui.virtual.jump-label");
    EXPECT_EQ(label.fg.rgb, 0x010203u);
    EXPECT_EQ(label.bg.rgb, 0x040506u);
    EXPECT_EQ(label.mods, std::uint16_t{0});
  }

  TEST_CASE("theme: a style the prefix walk finds is an answer, and is left alone");
  {
    // The contract the backfill is written to: "did not define" means the walk
    // lands on nothing that paints, not that the exact key is absent. A theme
    // that coloured `ui.virtual` said something about everything under it, and
    // an exact scope copied in from the builtin would shadow it.
    themes.Write("audit-ui-virtual", R"THEME(
"ui.text"     = { fg = "#bfbfbf" }
"ui.virtual"  = { fg = "#0a0b0c", bg = "#0d0e0f" }
)THEME");

    Theme theme;
    std::string error;
    EXPECT_TRUE(LoadTheme("audit-ui-virtual", theme, error));
    const Style label = theme.Get("ui.virtual.jump-label");
    EXPECT_EQ(label.fg.rgb, 0x0A0B0Cu);
    EXPECT_EQ(label.bg.rgb, 0x0D0E0Fu);
    EXPECT_TRUE(theme.scopes.find("ui.virtual.jump-label") == theme.scopes.end());
    // The same walk, and the same answer, for the other virtual scopes.
    EXPECT_EQ(theme.Get("ui.virtual.whitespace").fg.rgb, 0x0A0B0Cu);
  }

  TEST_CASE("theme: the shipped themes are unchanged by the fallback");
  {
    // They name every chrome scope they mean to, so the backfill has nothing
    // to do: what Get answers is still the theme's own entry rather than a
    // builtin style copied in over the top of it.
    for (const std::string_view name : {"ronin", "calm"}) {
      Theme shipped;
      std::string error;
      EXPECT_TRUE(LoadTheme(name, shipped, error));
      const auto own = shipped.scopes.find("ui.virtual.jump-label");
      EXPECT_TRUE(own != shipped.scopes.end());
      if (own == shipped.scopes.end()) continue;
      const Style label = shipped.Get("ui.virtual.jump-label");
      EXPECT_TRUE(label.fg.set);
      EXPECT_TRUE(label.bg.set);
      EXPECT_EQ(label.fg.rgb, own->second.fg.rgb);
      EXPECT_EQ(label.bg.rgb, own->second.bg.rgb);
    }
    // ronin leaves the wrap indicator unpainted and the builtin does not paint
    // it either, so it stays that way: the fallback fills gaps, it does not
    // invent chrome. (calm colours all of `ui.virtual`, which is an answer.)
    Theme ronin;
    std::string error;
    EXPECT_TRUE(LoadTheme("ronin", ronin, error));
    EXPECT_TRUE(!Paints(ronin.Get("ui.virtual.wrap")));
  }
}

void ThemeAlphaIsCompositedAtLoad() {
  const FakeThemeDir themes;
  if (!themes.Ready()) {
    EXPECT_TRUE(themes.Ready());
    return;
  }
  const auto load = [](std::string_view name) {
    Theme theme;
    std::string error;
    EXPECT_TRUE(LoadTheme(name, theme, error));
    EXPECT_TRUE(error.empty());
    return theme;
  };
  // Nothing a loaded theme hands out may still be waiting to be composited:
  // the renderer writes `rgb` into a cell and never looks at the alpha.
  const auto all_opaque = [](const Theme& theme) {
    for (const auto& [scope, style] : theme.scopes) {
      if ((style.fg.alpha != 0xFF) || (style.bg.alpha != 0xFF)) return false;
    }
    return true;
  };

  TEST_CASE("theme: an #rrggbbaa colour is composited over the background, not stripped");
  {
    themes.Write("audit-alpha", R"THEME(
"ui.background"      = { bg = "#101010" }
"ui.selection"       = { bg = "#80c0e080" }
"ui.text.focus"      = { fg = "#ffffff40", bg = "#202020" }
"ui.help"            = { fg = "#ffffff40" }
"ui.popup"           = { bg = "#ffffff00" }
"ui.window"          = { fg = "#abcdefff" }
"ui.menu"            = { fg = "#123456" }
"ui.menu.scroll"     = { bg = "ghost" }

[palette]
ghost = "#ffffff80"
)THEME");
    const Theme theme = load("audit-alpha");

    // 0x80 over 0x10 at a=128 is (0x80*128 + 0x10*127 + 127)/255 = 72; the
    // other two channels the same way give 0x68 and 0x78.
    EXPECT_EQ(theme.Get("ui.selection").bg.rgb, 0x486878u);
    // A foreground goes over the background of its own scope, which is what is
    // painted under the glyph: (0xFF*64 + 0x20*191 + 127)/255 = 0x58 a channel,
    // where over `ui.background` it would have come out 0x4C.
    EXPECT_EQ(theme.Get("ui.text.focus").fg.rgb, 0x585858u);
    EXPECT_EQ(theme.Get("ui.text.focus").bg.rgb, 0x202020u);
    // The same foreground on a scope with no background of its own falls back
    // to the theme's: (0xFF*64 + 0x10*191 + 127)/255 = 0x4C.
    EXPECT_EQ(theme.Get("ui.help").fg.rgb, 0x4C4C4Cu);
    // The ends of the range: fully transparent is the background exactly,
    // fully opaque is untouched, and so is a six-digit colour.
    EXPECT_EQ(theme.Get("ui.popup").bg.rgb, 0x101010u);
    EXPECT_EQ(theme.Get("ui.window").fg.rgb, 0xABCDEFu);
    EXPECT_EQ(theme.Get("ui.menu").fg.rgb, 0x123456u);
    // A palette entry carries its alpha to wherever it is used:
    // (0xFF*128 + 0x10*127 + 127)/255 = 0x88.
    EXPECT_EQ(theme.Get("ui.menu.scroll").bg.rgb, 0x888888u);
    EXPECT_EQ(theme.Get("ui.background").bg.rgb, 0x101010u);
    EXPECT_TRUE(all_opaque(theme));
  }

  TEST_CASE("theme: the background composited against is the one the theme ends up with");
  {
    // Which is known only after the inherits chain has run: the parent's
    // background for a child that keeps it, the child's for one that does not.
    themes.Write("audit-alpha-parent", R"THEME(
"ui.background"  = { bg = "#101010" }
"ui.text"        = { fg = "#bfbfbf" }
)THEME");
    themes.Write("audit-alpha-heir", R"THEME(
inherits = "audit-alpha-parent"
"ui.selection"   = { bg = "#80c0e080" }
)THEME");
    themes.Write("audit-alpha-repaint", R"THEME(
inherits = "audit-alpha-parent"
"ui.background"  = { bg = "#202020" }
"ui.selection"   = { bg = "#80c0e080" }
)THEME");

    const Theme heir = load("audit-alpha-heir");
    EXPECT_EQ(heir.Get("ui.background").bg.rgb, 0x101010u);
    EXPECT_EQ(heir.Get("ui.selection").bg.rgb, 0x486878u);

    // Over 0x20 instead: (0x80*128 + 0x20*127 + 127)/255 = 0x50, then 0x70,
    // then 0x80. Compositing before the child was read would have given the
    // parent's answer.
    const Theme repaint = load("audit-alpha-repaint");
    EXPECT_EQ(repaint.Get("ui.background").bg.rgb, 0x202020u);
    EXPECT_EQ(repaint.Get("ui.selection").bg.rgb, 0x507080u);
  }

  TEST_CASE("theme: an alpha with nothing known underneath is dropped, not guessed at");
  {
    // `ui.background` is the bottom of the stack and the terminal's own colour
    // is under it, so there is nothing to composite against -- for the
    // background itself, or for anything else in a theme that leaves the
    // background unpainted.
    themes.Write("audit-alpha-floor", R"THEME(
"ui.background"  = { bg = "#40506080" }
"ui.selection"   = { bg = "#ffffff80" }
)THEME");
    const Theme floor = load("audit-alpha-floor");
    EXPECT_EQ(floor.Get("ui.background").bg.rgb, 0x405060u);
    // And that colour, as authored, is what the rest is composited over:
    // (0xFF*128 + 0x40*127 + 127)/255 = 0xA0, then 0xA8, then 0xB0.
    EXPECT_EQ(floor.Get("ui.selection").bg.rgb, 0xA0A8B0u);
    EXPECT_TRUE(all_opaque(floor));

    themes.Write("audit-alpha-bare", R"THEME(
"ui.background"  = { fg = "#bfbfbf" }
"ui.selection"   = { bg = "#474B43A4" }
)THEME");
    const Theme bare = load("audit-alpha-bare");
    EXPECT_TRUE(!bare.Get("ui.background").bg.set);
    EXPECT_EQ(bare.Get("ui.selection").bg.rgb, 0x474B43u);
    EXPECT_TRUE(all_opaque(bare));
  }

  TEST_CASE("theme: ronin's translucent selection reaches the screen as the blend it asks for");
  {
    // #474B43A4 over ronin's own background, #252524:
    // (0x47*164 + 0x25*91 + 127)/255 = 0x3B, then 0x3D, then 0x38.
    constexpr std::uint32_t kBlend = 0x3B3D38u;
    const Theme ronin = load("ronin");
    EXPECT_EQ(ronin.Get("ui.background").bg.rgb, 0x252524u);
    EXPECT_EQ(ronin.Get("ui.selection").bg.rgb, kBlend);
    EXPECT_EQ(ronin.Get("ui.selection.primary").bg.rgb, kBlend);
    EXPECT_EQ(ronin.Get("ui.menu.scroll").bg.rgb, kBlend);
    EXPECT_EQ(ronin.Get("ui.menu.scroll").fg.rgb, 0xBFBFBFu);
    EXPECT_TRUE(all_opaque(ronin));
    EXPECT_TRUE(all_opaque(load("calm")));

    // On the screen, not just in the map: the cells under a selection carry
    // the blend, and none of them the alpha-stripped colour.
    Editor ed;
    ed.theme = ronin;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Replace(ed.doc.table, {Selection{0, 5, -1}});
    constexpr int kWidth = 40;
    constexpr int kHeight = 6;
    FitFocusedViewport(ed, kWidth, kHeight);
    Surface frame;
    RenderTo(ed, frame, kWidth, kHeight);

    int blended = 0;
    int stripped = 0;
    for (int x = 0; x < kWidth; ++x) {
      const Attr bg = frame.At(x, 0).bg;
      if (bg == static_cast<Attr>(kBlend)) ++blended;
      if (bg == static_cast<Attr>(0x474B43u)) ++stripped;
    }
    EXPECT_TRUE(blended > 0);
    EXPECT_EQ(stripped, 0);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

}  // namespace koi
