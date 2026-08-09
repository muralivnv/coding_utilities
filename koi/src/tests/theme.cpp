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
  EXPECT_TRUE(ParseColor("#474B43A4", color));
  EXPECT_EQ(color.rgb, 0x474B43u);
  EXPECT_TRUE(ParseColor("light-yellow", color));
  EXPECT_TRUE(!ParseColor("#abc", color));
  EXPECT_TRUE(!ParseColor("chartreuse", color));

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

}  // namespace koi
