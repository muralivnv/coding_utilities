// Tests for sidebar.cpp: the rows it builds, the filter over them, and the
// truncation that has to fit a pane.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

// What the terminal would actually put on screen for a sidebar row: the SGR
// sequences the sidebar writes cost no columns, everything else costs what
// DisplayWidth says. A row whose visible width exceeds the pane wraps, and a
// wrap pushes every section below it down a line.
int VisibleWidth(std::string_view line) {
  std::string plain;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\033') {
      while ((i < line.size()) && (line[i] != 'm')) ++i;
      continue;
    }
    plain += line[i];
  }
  return static_cast<int>(DisplayWidth(plain, 0));
}

}  // namespace

void SidebarEmphasisSurvivesTheFilter() {
  TEST_CASE("sidebar: the current file's pin is bold, not a control picture");

  const Scratch scratch{"koi-sidebar-emphasis"};
  // Exactly the name budget at 40 columns (40 - 4 of prefix - 6 of ":NNNNN"),
  // so the row only fits while the emphasis costs nothing.
  const std::string here = scratch.Write("a_rather_long_current_name.cpp", "int a;\n").string();
  const std::string other = scratch.Write("beta.cpp", "int b;\n").string();

  std::string error;
  const auto store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  store->RecordVisit(other, 7, 1);
  store->RecordVisit(here, 12, 1);
  store->SetPin(1, here, 12, 1);
  EXPECT_EQ(MostRecentFile(*store), here);

  constexpr int kColumns = 40;
  const std::vector<std::string> lines = SidebarLines(*store, kColumns);

  size_t bold_rows = 0;
  const std::string* pin_row = nullptr;
  for (const std::string& line : lines) {
    // U+241B, the control picture the escape filter substitutes for ESC.
    EXPECT_TRUE(line.find("␛") == std::string::npos);
    EXPECT_TRUE(VisibleWidth(line) <= kColumns);
    if (line.find("\033[1m") != std::string::npos) {
      ++bold_rows;
      pin_row = &line;
    }
  }

  // One emphasised row: the pin that is the file being edited.
  EXPECT_EQ(bold_rows, size_t{1});
  EXPECT_TRUE(pin_row != nullptr);
  if (pin_row == nullptr) return;
  EXPECT_TRUE(pin_row->find("\033[1ma_rather_long_current_name.cpp\033[0m") != std::string::npos);
  EXPECT_TRUE(pin_row->find(":12") != std::string::npos);
  EXPECT_EQ(VisibleWidth(*pin_row), 4 + 30 + 3);

  // A pin that is not the current file stays unemphasised.
  store->SetPin(2, other, 7, 1);
  size_t still_bold = 0;
  for (const std::string& line : SidebarLines(*store, kColumns)) {
    if (line.find("\033[1m") != std::string::npos) ++still_bold;
    EXPECT_TRUE(line.find("␛") == std::string::npos);
  }
  EXPECT_EQ(still_bold, size_t{1});
}

void TruncateNeverExceedsTheWidth() {
  TEST_CASE("sidebar: a cut name is never wider than the width asked for");

  const auto width = [](std::string_view text) { return static_cast<int>(DisplayWidth(text, 0)); };

  // A tab is worth nothing to GraphemeWidth -- deliberately, its width is
  // positional -- and a jump to the next tab stop to DisplayWidth. Deciding
  // "does this fit" with the second and doing the cut with the first made every
  // tab free: the cut loop walked off the end of the name and handed back the
  // whole of it with an ellipsis stuck on, 33 columns wide for a budget of 6.
  const std::string tabs = "a\tb\tc\td\te";
  EXPECT_EQ(width(tabs), 33);
  for (int w = 0; w <= 40; ++w) EXPECT_TRUE(width(TruncateToWidth(tabs, w)) <= std::max(w, 1));
  EXPECT_EQ(TruncateToWidth(tabs, 6), std::string("a…"));
  EXPECT_EQ(TruncateToWidth(tabs, 10), std::string("a\tb…"));
  EXPECT_EQ(TruncateToWidth(tabs, 18), std::string("a\tb\tc…"));
  EXPECT_EQ(TruncateToWidth(tabs, 33), tabs);  // fits exactly: no cut, no ellipsis

  // Nothing but tabs. The first one alone overruns what the ellipsis leaves, so
  // there is nothing to keep -- and the ellipsis is the one column that is
  // allowed to exceed a budget of zero.
  EXPECT_EQ(TruncateToWidth("\t\t\t", 6), std::string("…"));
  EXPECT_EQ(TruncateToWidth("\t", 8), std::string("\t"));
  EXPECT_EQ(TruncateToWidth("\t", 7), std::string("…"));

  // Everything without a tab in it is cut exactly where it always was.
  EXPECT_EQ(TruncateToWidth("abc", 0), std::string());
  EXPECT_EQ(TruncateToWidth("abc", 6), std::string("abc"));
  EXPECT_EQ(TruncateToWidth("abcdef", 1), std::string("…"));
  EXPECT_EQ(TruncateToWidth("abcdefghij", 6), std::string("abcde…"));
  // Two columns to the cluster: an odd budget leaves a column spare rather than
  // cutting one in half.
  EXPECT_EQ(TruncateToWidth("漢字漢字漢字", 6), std::string("漢字…"));
  EXPECT_EQ(TruncateToWidth("漢字漢字漢字", 7), std::string("漢字漢…"));
  // One grapheme cluster of four codepoints and two columns, kept or dropped
  // whole.
  const std::string emoji = "👩‍👩‍👧‍👦x👍y";
  EXPECT_EQ(TruncateToWidth(emoji, 6), emoji);
  EXPECT_EQ(TruncateToWidth(emoji, 5), std::string("👩‍👩‍👧‍👦x…"));
  EXPECT_EQ(TruncateToWidth(emoji, 3), std::string("👩‍👩‍👧‍👦…"));
  EXPECT_EQ(TruncateToWidth(emoji, 2), std::string("…"));
}

void SidebarRowsFitTheirPane() {
  TEST_CASE("sidebar: a tab in a name does not push the row past the pane");

  const Scratch scratch{"koi-sidebar-tabs"};
  // Symbol names come out of the project database, which is filled from
  // whatever the file-filter command found -- a tab in one is repository
  // controlled, and it used to make the symbol row several times the pane wide.
  const std::string file = scratch.Write("tabbed.cpp", "int a;\n").string();

  std::string error;
  const auto store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  store->RecordVisit(file, 3, 1);
  store->RecordSymbolVisit("a\tb\tc\td\te", file, 3);

  constexpr int kColumns = 42;
  bool saw_symbol = false;
  for (const std::string& line : SidebarLines(*store, kColumns)) {
    EXPECT_TRUE(VisibleWidth(line) <= kColumns);
    if (line.find("tabbed.cpp:3") != std::string::npos) saw_symbol = true;
  }
  EXPECT_TRUE(saw_symbol);
}

void SidebarRendering() {
  TEST_CASE("sidebar: what it draws");

  const Scratch scratch{"koi-sidebar-test"};
  const std::string a = scratch.Write("alpha.cpp", "int a;\n").string();
  const std::string b = scratch.Write("beta.cpp", "int b;\n").string();
  const std::string c = scratch.Write("a_very_long_file_name_indeed.cpp", "int c;\n").string();

  std::string error;
  const auto store = ProjectStore::Open(scratch.dir / "state.db", error);
  EXPECT_TRUE(store != nullptr);
  if (store == nullptr) return;

  {
    const std::vector<std::string> lines = SidebarLines(*store, 42);
    EXPECT_EQ(lines.size(), static_cast<size_t>(2 + kPinSlots + kHotSymbolSlots + 1));
    EXPECT_TRUE(lines[0].find("Pins") != std::string::npos);
    for (size_t i = 1; i <= static_cast<size_t>(kPinSlots); ++i) {
      EXPECT_TRUE(lines[i].find("—") != std::string::npos);
    }
  }

  store->RecordVisit(a, 10, 1);
  store->RecordVisit(b, 20, 1);
  store->RecordVisit(c, 30, 1);
  store->SetPin(1, a, 10, 1);
  store->RecordSymbolVisit("Widget", b, 12);

  const std::vector<std::string> lines = SidebarLines(*store, 42);
  const auto has = [&lines](std::string_view text) {
    return std::ranges::any_of(lines, [text](const std::string& line) {
      return line.find(text) != std::string::npos;
    });
  };
  EXPECT_TRUE(has(std::string{"alpha.cpp"} + "\033[2m" + ":10"));
  EXPECT_TRUE(has("Widget"));
  EXPECT_TRUE(has("beta.cpp:12"));
  EXPECT_TRUE(!has(a));

  EXPECT_EQ(MostRecentFile(*store), c);
  // Only pins and symbols are drawn, so a visited-but-unpinned file appears
  // nowhere.
  EXPECT_TRUE(!has("a_very_long_file_name_indeed.cpp"));
  EXPECT_TRUE(!has("beta.cpp:20"));

  for (const std::string& line : SidebarLines(*store, 24)) {
    EXPECT_TRUE(line.find('\n') == std::string::npos);
  }

  // Everything above pins a file that is not the most recent one, so nothing
  // above renders an emphasised row at all. Pin the current file too.
  store->SetPin(2, c, 30, 1);
  const std::vector<std::string> emphasised = SidebarLines(*store, 42);
  const auto bold = [](const std::string& line) {
    return line.find("\033[1m") != std::string::npos;
  };
  EXPECT_EQ(std::ranges::count_if(emphasised, bold), 1);
  for (const std::string& line : emphasised) {
    EXPECT_TRUE(line.find("␛") == std::string::npos);
    EXPECT_TRUE(VisibleWidth(line) <= 42);
  }
}

}  // namespace koi
