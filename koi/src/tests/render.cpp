// Tests for render.cpp: what actually reaches the terminal -- the frame, the
// gaps in it, and the bytes it is allowed to write.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void Rendering() {
  const auto draw = [](Editor& ed, int w, int h) {
    Surface frame;
    FitFocusedViewport(ed, w, h);
    RenderTo(ed, frame, w, h);
    return frame;
  };

  TEST_CASE("render: a single window draws its text, gutter and status line");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    const Surface frame = draw(ed, 40, 6);

    EXPECT_EQ(frame.width, 40);
    EXPECT_EQ(frame.height, 6);

    bool all_painted = true;
    for (int y = 0; y < frame.height; ++y) {
      for (int x = 0; x < frame.width; ++x) {
        if (frame.At(x, y).text.empty()) all_painted = false;
      }
    }
    EXPECT_TRUE(all_painted);
    EXPECT_TRUE(frame.Row(0).find("alpha") != std::string::npos);
    EXPECT_TRUE(frame.Row(1).find("bravo") != std::string::npos);
    EXPECT_TRUE(frame.Row(2).find("charlie") != std::string::npos);

    EXPECT_TRUE(frame.Row(0).find('1') < frame.Row(0).find("alpha"));
  }

  TEST_CASE("render: insert mode paints secondary carets without eating the glyph");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\n");
    std::vector<Selection> carets;
    carets.push_back(Selection{0, 0, -1});
    carets.push_back(Selection{6, 6, -1});
    carets.push_back(Selection{12, 12, -1});
    ed.doc.selections.Replace(ed.doc.table, std::move(carets));
    ed.mode = Mode::kInsert;
    const Surface frame = draw(ed, 40, 6);

    const auto cell_of = [](const Surface& f, int row, std::string_view want) {
      for (int x = 0; x < f.width; ++x) {
        if (f.At(x, row).text == want) return x;
      }
      return -1;
    };
    EXPECT_EQ(cell_of(frame, 1, "▏"), -1);
    EXPECT_EQ(cell_of(frame, 2, "▏"), -1);
    EXPECT_TRUE(frame.Row(1).find("bravo") != std::string::npos);
    EXPECT_TRUE(frame.Row(2).find("charlie") != std::string::npos);
    const int b = static_cast<int>(frame.Row(1).find("bravo"));
    const int c = static_cast<int>(frame.Row(2).find("charlie"));
    EXPECT_TRUE((frame.At(b, 1).fg != frame.At(b + 1, 1).fg) ||
                (frame.At(b, 1).bg != frame.At(b + 1, 1).bg));
    EXPECT_TRUE((frame.At(c, 2).fg != frame.At(c + 1, 2).fg) ||
                (frame.At(c, 2).bg != frame.At(c + 1, 2).bg));

    const int a = static_cast<int>(frame.Row(0).find("alpha"));
    EXPECT_EQ(frame.At(a, 0).text, std::string{"a"});
    EXPECT_TRUE((frame.At(a, 0).fg & kAttrReverse) == 0);
    EXPECT_EQ(frame.cursor_x, a);
    EXPECT_EQ(frame.cursor_y, 0);
    EXPECT_TRUE(frame.cursor_insert);

    ed.mode = Mode::kNormal;
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    const Surface normal = draw(ed, 40, 6);
    EXPECT_EQ(normal.At(static_cast<int>(normal.Row(1).find("bravo")), 1).text, std::string{"b"});
    EXPECT_TRUE((normal.At(static_cast<int>(normal.Row(1).find("bravo")), 1).fg & kAttrReverse) !=
                0);
  }

  TEST_CASE("render: an error too wide for the status line goes up as an overlay");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    ed.status.Fail("tiny");
    Surface frame = draw(ed, 60, 24);
    EXPECT_FALSE(ed.status_overlay);

    std::string words;
    for (int i = 0; i < 12; ++i) words += "hunk" + std::to_string(i) + " no longer matches ";
    ed.status.Fail(words);
    frame = draw(ed, 60, 24);
    EXPECT_TRUE(ed.status_overlay);
    std::string all;
    for (int y = 0; y < frame.height; ++y) all += frame.Row(y) + "\n";
    EXPECT_TRUE(all.find(" error ") != std::string::npos);
    EXPECT_TRUE(all.find("hunk0") != std::string::npos);
    EXPECT_TRUE(all.find("hunk11") != std::string::npos);
    EXPECT_TRUE(all.find("┌") != std::string::npos);
    EXPECT_TRUE(all.find("└") != std::string::npos);
    const std::string bar_row = frame.Row(frame.height - 1);
    EXPECT_TRUE(bar_row.find("hunk0") == std::string::npos);
    EXPECT_TRUE(bar_row.find("…") == std::string::npos);
    EXPECT_TRUE(bar_row.find("1:1") != std::string::npos);

    Surface cramped = draw(ed, 60, 4);
    EXPECT_FALSE(ed.status_overlay);
    const std::string cramped_bar = cramped.Row(cramped.height - 1);
    EXPECT_TRUE(cramped_bar.find("hunk0") != std::string::npos);
    EXPECT_TRUE(cramped_bar.find("…") != std::string::npos);

    ed.status.clear();
    ed.status.Warn(words);
    frame = draw(ed, 60, 24);
    EXPECT_TRUE(ed.status_overlay);
    all.clear();
    for (int y = 0; y < frame.height; ++y) all += frame.Row(y) + "\n";
    EXPECT_TRUE(all.find(" warning ") != std::string::npos);

    ed.status.Fail(words);
    frame = draw(ed, 60, 24);
    EXPECT_TRUE(ed.status_overlay);
    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;
    Key d;
    EXPECT_TRUE(ParseKey("d", d));
    HandleKeyInput(ed, maps, d, pending);
    EXPECT_FALSE(ed.status_overlay);
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}),
              std::string{"lpha\nbravo\ncharlie\n"});
    frame = draw(ed, 60, 24);
    EXPECT_FALSE(ed.status_overlay);
    all.clear();
    for (int y = 0; y < frame.height; ++y) all += frame.Row(y) + "\n";
    EXPECT_TRUE(all.find("┌") == std::string::npos);

    bool logged = false;
    for (const StatusRecord& entry : ed.status.log()) {
      logged = logged || (entry.text == words);
    }
    EXPECT_TRUE(logged);
  }

  TEST_CASE("render: nothing is drawn outside the surface");
  {

    Editor ed;
    std::string wide;
    for (int i = 0; i < 40; ++i) wide += "the quick brown fox ";
    wide += "\n";
    ResetToOriginal(ed.doc.table, wide + wide + wide);
    ed.doc.selections.Set(Selection{0, 0, -1});
    for (const int w : {1, 2, 5, 13, 80}) {
      for (const int h : {1, 2, 3, 24}) {
        const Surface frame = draw(ed, w, h);
        EXPECT_EQ(frame.width, w);
        EXPECT_EQ(frame.height, h);
        EXPECT_EQ(frame.cells.size(), static_cast<std::size_t>(w) * h);

        if (frame.cursor_visible) {
          EXPECT_TRUE(frame.cursor_x >= 0);
          EXPECT_TRUE(frame.cursor_y >= 0);
          EXPECT_TRUE(frame.cursor_x < w);
          EXPECT_TRUE(frame.cursor_y < h);
        }
      }
    }
  }

  TEST_CASE("render: a split draws both halves, and neither writes into the other");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\nbbbb\ncccc\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    SplitWindow(ed, true);
    const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, 60, 10});
    EXPECT_EQ(areas.size(), std::size_t{2});

    const Surface frame = draw(ed, 60, 10);

    bool all_painted = true;
    for (int y = 0; y < frame.height; ++y) {
      for (int x = 0; x < frame.width; ++x) {
        if (frame.At(x, y).text.empty()) all_painted = false;
      }
    }
    EXPECT_TRUE(all_painted);

    if (areas.size() == 2) {
      const int seam = areas[0].w;

      const std::string row = frame.Row(0);

      EXPECT_TRUE(frame.width == 60);

      for (int y = 0; y < areas[0].h; ++y) {
        EXPECT_EQ(frame.At(areas[0].x + areas[0].w - 1, y).text, std::string{"│"});
      }

      EXPECT_TRUE(row.find('1') < static_cast<std::size_t>(seam));
      EXPECT_TRUE(row.find('1', static_cast<std::size_t>(seam)) != std::string::npos);
    }
  }

  TEST_CASE("render: only the focused split paints a caret and its cursorline");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.theme.scopes["ui.cursorline.primary"] = Style{{}, Color{true, 0x303030}, 0};
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);

    const Surface solo = draw(ed, 60, 10);
    for (int y = 0; y < solo.height; ++y) {
      EXPECT_TRUE(solo.Row(y).find("╭") == std::string::npos);
      EXPECT_TRUE(solo.Row(y).find("│") == std::string::npos);
    }
    EXPECT_TRUE(solo.Row(0).find("alpha") != std::string::npos);

    SplitWindow(ed, true);
    const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, 60, 10});
    const std::vector<int> order = WindowOrder(ed);
    EXPECT_EQ(areas.size(), std::size_t{2});
    const auto area_of_focus = [&] {
      for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == ed.focused) return areas[i];
      }
      return areas[0];
    };

    const auto banded = [](const Surface& s, const Rect& area) {
      const int x = area.x + area.w - 2;
      return s.At(x, area.y).bg != s.At(x, area.y + 1).bg;
    };

    const Rect before = area_of_focus();
    const Rect other = (before.x == areas[0].x) ? areas[1] : areas[0];
    const Surface split = draw(ed, 60, 10);
    EXPECT_TRUE(banded(split, before));
    EXPECT_FALSE(banded(split, other));

    const auto blocked = [](const Surface& s, const Rect& area) {
      for (int x = area.x; x < (area.x + area.w); ++x) {
        if ((s.At(x, area.y).fg & kAttrReverse) != 0) return true;
      }
      return false;
    };
    EXPECT_FALSE(split.cursor_visible);
    EXPECT_TRUE(blocked(split, before));
    EXPECT_FALSE(blocked(split, other));

    FocusWindow(ed, true);
    const Rect after = area_of_focus();
    EXPECT_TRUE(after.x != before.x);
    const Surface moved = draw(ed, 60, 10);
    EXPECT_TRUE(banded(moved, after));
    EXPECT_FALSE(banded(moved, before));
    EXPECT_TRUE(blocked(moved, after));
    EXPECT_FALSE(blocked(moved, before));
  }

  TEST_CASE("render: the secondary cursorline band matches the full selection walk");
  {
    // RenderPane walks only the selections whose range reaches the visible
    // bytes, not all of them -- a whole-file multi-cursor used to cost one
    // piece-table lookup per cursor per frame. The rows it lands on have to
    // stay the old full walk's answer, so that walk is redone here and the
    // two are compared.
    //
    // A band cannot be read off a single Glyph, so every check is a
    // difference: the same frame drawn with and then without a painted
    // `ui.cursorline.secondary` differs exactly on the rows that band reached.
    // BuiltinTheme has no `ui.cursorline` parent for Get to fall back to, so
    // erasing the key really does unpaint it.
    const Style band_primary{{}, Color{true, 0x303030}, 0};
    const Style band_secondary{{}, Color{true, 0x204060}, 0};

    std::string text;
    for (int i = 0; i < 200; ++i) text += "line " + std::to_string(i) + "\n";

    const auto join = [](const std::vector<int>& rows) {
      std::string out;
      for (const int row : rows) {
        if (!out.empty()) out += ",";
        out += std::to_string(row);
      }
      return out;
    };

    const auto painted = [&](Editor& ed, int w, int h, bool refit) {
      const auto frame = [&] {
        Surface s;
        if (refit) FitFocusedViewport(ed, w, h);
        RenderTo(ed, s, w, h);
        return s;
      };
      ed.theme.scopes["ui.cursorline.secondary"] = band_secondary;
      const Surface with = frame();
      ed.theme.scopes.erase("ui.cursorline.secondary");
      const Surface without = frame();
      ed.theme.scopes["ui.cursorline.secondary"] = band_secondary;
      std::vector<int> rows;
      for (int y = 0; (y + 1) < h; ++y) {
        if (with.At(w - 1, y).bg != without.At(w - 1, y).bg) rows.push_back(y);
      }
      return rows;
    };

    // The walk as it was: every selection's cursor line, then the visible ones,
    // minus the primary's -- that row takes the primary band, which is painted
    // in both frames and so never shows up as a difference.
    const auto wanted = [](const Editor& ed, int h) {
      std::vector<Index> all;
      for (const Selection& s : ed.doc.selections.Ranges()) {
        all.push_back(LineAt(ed.doc.table, CursorOf(ed.doc.table, s)));
      }
      std::ranges::sort(all);
      const auto extra = std::ranges::unique(all);
      all.erase(extra.begin(), extra.end());

      const Index primary =
          LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary()));
      const Index count = LineCount(ed.doc.table);
      std::vector<int> rows;
      for (int y = 0; (y + 1) < h; ++y) {
        const Index line = ed.doc.view.top_line + y;
        if (line >= count) break;
        if (line == primary) continue;
        if (std::ranges::binary_search(all, line)) rows.push_back(y);
      }
      return rows;
    };

    {
      // A cursor on every line of the file, seen from four scroll positions.
      Editor ed;
      ed.theme = BuiltinTheme();
      ed.theme.scopes["ui.cursorline.primary"] = band_primary;
      ResetToOriginal(ed.doc.table, text);
      ed.doc.selections.Set(Selection{0, 0, -1});
      ed.doc.selections.EnsureBlockCursors(ed.doc.table);
      RunCommands(ed, {"select_all", "split_selection_on_newline"});
      EXPECT_TRUE(ed.doc.selections.Size() >= std::size_t{200});

      for (const std::size_t at : {std::size_t{0}, std::size_t{7}, std::size_t{101},
                                   std::size_t{199}}) {
        ed.doc.selections.SetPrimary(at);
        const std::vector<int> rows = painted(ed, 60, 12, true);
        EXPECT_EQ(join(rows), join(wanted(ed, 12)));
        // Not a vacuous match: the window is 11 rows, and all but the primary's
        // (and, at the end of the file, the cursorless line past the last
        // newline) carry a band.
        EXPECT_TRUE(rows.size() >= std::size_t{7});
      }
    }

    {
      // Sparse cursors around a pinned window: lines 50..60 on a 12-row screen.
      // Rendered without FitFocusedViewport so nothing can scroll the window
      // out from under the cases below.
      Editor ed;
      ed.theme = BuiltinTheme();
      ed.theme.scopes["ui.cursorline.primary"] = band_primary;
      ResetToOriginal(ed.doc.table, text);
      const auto start = [&ed](Index line) { return LineStart(ed.doc.table, line); };
      ed.doc.view.top_line = 50;
      ed.doc.view.top_row = 0;

      ed.doc.selections.Replace(
          ed.doc.table, {
                            // Far above the window.
                            Selection{start(10), start(10) + 1, -1},
                            // Anchored off screen and reaching in: From() is
                            // above the window, the cursor is on its top line.
                            Selection{start(20), start(50) + 2, -1},
                            // Backward, so the cursor is From() rather than To().
                            Selection{start(55) + 4, start(55) + 1, -1},
                            // Cursor on the newline of the last visible line.
                            Selection{start(60) + 6, start(61), -1},
                            // Just below the window, and far below it.
                            Selection{start(62), start(62) + 1, -1},
                            Selection{start(150), start(150) + 1, -1},
                        });
      EXPECT_EQ(ed.doc.selections.Size(), std::size_t{6});
      ed.doc.selections.SetPrimary(2);
      EXPECT_TRUE(ed.doc.selections.Primary().Backward());

      const std::vector<int> rows = painted(ed, 60, 12, false);
      EXPECT_EQ(ed.doc.view.top_line, Index{50});
      // Row 0 is line 50 (the straddler's cursor), row 10 is line 60. Row 5 is
      // line 55, the primary, which wears the primary band instead.
      EXPECT_EQ(join(rows), std::string{"0,10"});
      EXPECT_EQ(join(rows), join(wanted(ed, 12)));

      // A selection covering the whole window with its cursor off screen paints
      // nothing: the windowed walk still visits it, and LineAt puts it out of
      // view. Forward (cursor near To()) and backward (cursor at From()) both.
      ed.doc.selections.Replace(ed.doc.table, {Selection{start(20), start(70), -1}});
      EXPECT_EQ(join(painted(ed, 60, 12, false)), std::string{});
      EXPECT_EQ(join(wanted(ed, 12)), std::string{});
      ed.doc.selections.Replace(ed.doc.table, {Selection{start(70), start(20), -1}});
      EXPECT_EQ(join(painted(ed, 60, 12, false)), std::string{});
      EXPECT_EQ(join(wanted(ed, 12)), std::string{});
    }

    {
      // What made this reachable with no user configuration: calm sets only
      // "ui.cursorline", and Theme::Get walks the scope back over the dots, so
      // Get("ui.cursorline.secondary") resolves to it and the band paints.
      Editor ed;
      ed.theme = BuiltinTheme();
      ed.theme.scopes["ui.cursorline"] = band_primary;
      EXPECT_TRUE(ed.theme.Get("ui.cursorline.secondary").bg.set);
      ResetToOriginal(ed.doc.table, text);
      ed.doc.selections.Replace(ed.doc.table, {Selection{0, 1, -1},
                                               Selection{LineStart(ed.doc.table, 2),
                                                         LineStart(ed.doc.table, 2) + 1, -1}});
      ed.doc.selections.SetPrimary(0);

      Surface frame;
      FitFocusedViewport(ed, 60, 12);
      RenderTo(ed, frame, 60, 12);
      EXPECT_EQ(ed.doc.view.top_line, Index{0});
      EXPECT_TRUE(frame.At(59, 2).bg != frame.At(59, 3).bg);
      EXPECT_EQ(frame.At(59, 1).bg, frame.At(59, 3).bg);
    }
  }

  TEST_CASE("render: a theme can tint the primary selection apart from the rest");
  {
    // `ui.selection.primary` was resolved into the palette at Resolve and then
    // read by nothing: DrawLine painted every selection with `ui.selection`, so
    // a theme that distinguishes the one the next edit belongs to -- ronin.toml
    // does -- was silently ignored.
    //
    // Read as a difference, the way the cursorline bands are: the same frame
    // drawn with and without the key set differs exactly on the primary
    // selection's cells.
    const Style plain{{}, Color{true, 0x202020}, 0};
    const Style tinted{{}, Color{true, 0x804020}, 0};

    Editor ed;
    ed.theme = BuiltinTheme();
    ed.theme.scopes["ui.selection"] = plain;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\n");
    ed.doc.selections.Replace(ed.doc.table,
                              {Selection{0, 5, -1}, Selection{6, 11, -1}});
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{2});

    // Where "alpha" and "bravo" start on screen, past the gutter.
    const Surface probe = draw(ed, 40, 6);
    const auto column_of = [&](int y, std::string_view word) {
      return static_cast<int>(probe.Row(y).find(word));
    };
    const int alpha_x = column_of(0, "alpha");
    const int bravo_x = column_of(1, "bravo");
    EXPECT_TRUE(alpha_x > 0);
    EXPECT_TRUE(bravo_x > 0);

    // Every cell that two frames disagree on, as "x,y" pairs.
    const auto differences = [](const Surface& a, const Surface& b) {
      std::vector<std::string> out;
      for (int y = 0; y < a.height; ++y) {
        for (int x = 0; x < a.width; ++x) {
          const Glyph& one = a.At(x, y);
          const Glyph& two = b.At(x, y);
          if ((one.text == two.text) && (one.fg == two.fg) && (one.bg == two.bg)) continue;
          out.push_back(std::to_string(x) + "," + std::to_string(y));
        }
      }
      return out;
    };
    // The same editor, the same primary, drawn without the key and then with
    // it: which selection is primary is held fixed across the pair, because
    // moving it would also move the caret and the status line.
    const auto pair_for = [&](std::size_t primary, const Style& set) {
      ed.doc.selections.SetPrimary(primary);
      ed.theme.scopes.erase("ui.selection.primary");
      const Surface without = draw(ed, 40, 6);
      ed.theme.scopes["ui.selection.primary"] = set;
      const Surface with = draw(ed, 40, 6);
      ed.theme.scopes.erase("ui.selection.primary");
      return std::pair{without, with};
    };
    const auto rows_of = [](const std::vector<std::string>& cells) {
      std::set<std::string> rows;
      for (const std::string& at : cells) rows.insert(at.substr(at.find(',') + 1));
      std::string out;
      for (const std::string& row : rows) {
        if (!out.empty()) out += ",";
        out += row;
      }
      return out;
    };

    // -- the control: unset, and set to what it already falls back to --------
    //
    // Theme::Get walks "ui.selection.primary" back to "ui.selection", so an
    // unset key has to leave the frame exactly as it was before any of this.
    EXPECT_EQ(ed.theme.Get("ui.selection.primary").bg.rgb, plain.bg.rgb);
    {
      const auto [without, with] = pair_for(1, plain);
      EXPECT_EQ(differences(without, with).size(), std::size_t{0});
      // Both selections carry the one background when nothing tells them apart.
      EXPECT_EQ(without.At(alpha_x + 1, 0).bg, without.At(bravo_x + 1, 1).bg);
    }

    // -- and with the key set to something else ------------------------------
    {
      // "bravo" is the primary. Every differing cell is on its row: a
      // non-primary selection is painted exactly as it always was.
      const auto [without, with] = pair_for(1, tinted);
      EXPECT_EQ(rows_of(differences(without, with)), std::string{"1"});
      // Interior cells of the selection, away from the block cursor on its
      // last grapheme, which wears the cursor style in either frame.
      EXPECT_TRUE(with.At(bravo_x, 1).bg != without.At(bravo_x, 1).bg);
      EXPECT_TRUE(with.At(bravo_x + 1, 1).bg != without.At(bravo_x + 1, 1).bg);
      EXPECT_EQ(with.At(alpha_x, 0).bg, without.At(alpha_x, 0).bg);
      EXPECT_EQ(with.At(alpha_x + 1, 0).bg, without.At(alpha_x + 1, 0).bg);
      // The tint is the colour the theme asked for, not merely "some other":
      // the same cell painted by a plain `ui.selection` of that colour agrees.
      ed.theme.scopes["ui.selection"] = tinted;
      const Surface all_tinted = draw(ed, 40, 6);
      ed.theme.scopes["ui.selection"] = plain;
      EXPECT_EQ(with.At(bravo_x, 1).bg, all_tinted.At(bravo_x, 1).bg);
      EXPECT_TRUE(without.At(bravo_x, 1).bg != all_tinted.At(bravo_x, 1).bg);
    }

    // The tint follows the primary rather than the row.
    {
      const auto [without, with] = pair_for(0, tinted);
      EXPECT_EQ(rows_of(differences(without, with)), std::string{"0"});
      EXPECT_TRUE(with.At(alpha_x + 1, 0).bg != without.At(alpha_x + 1, 0).bg);
      EXPECT_EQ(with.At(bravo_x + 1, 1).bg, without.At(bravo_x + 1, 1).bg);
    }
  }

  TEST_CASE("render: each window gets its own status line");
  {
    const Scratch scratch{"koi-render-status"};
    const std::filesystem::path a = scratch.Write("left.txt", "aaa\n");
    const std::filesystem::path b = scratch.Write("right.txt", "bbb\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, b.string()));

    const Surface frame = draw(ed, 80, 12);

    std::string everything;
    for (int y = 0; y < frame.height; ++y) everything += frame.Row(y) + "\n";

    EXPECT_TRUE(everything.find("left.txt") != std::string::npos);
    EXPECT_TRUE(everything.find("right.txt") != std::string::npos);
  }

  TEST_CASE("render: the prompt and its completions reach the bottom rows");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kCommand);
    PromptInsert(ed, "buffer-");

    const Surface frame = draw(ed, 60, 14);

    EXPECT_TRUE(frame.Row(frame.height - 1).find("buffer-") != std::string::npos);

    std::string above;
    for (int y = 0; y < (frame.height - 1); ++y) above += frame.Row(y) + "\n";
    EXPECT_TRUE(above.find("buffer-close") != std::string::npos);

    EXPECT_TRUE(frame.cursor_visible);
    EXPECT_EQ(frame.cursor_y, frame.height - 1);
  }

  // A command or a pattern longer than the terminal is wide used to be typed
  // blind: the row drew from column 0, stopped at the width, and the caret
  // stuck to the last column while the text stopped changing.
  const auto typed = [](int count) {
    std::string out;
    for (int i = 0; i < count; ++i) out += static_cast<char>('a' + (i % 26));
    return out;
  };
  // Surface::Row drops the blanks at the end of a row, so the wanted rows are
  // written out in full and trimmed the same way.
  const auto row_like = [](std::string want) {
    while (!want.empty() && (want.back() == ' ')) want.pop_back();
    return want;
  };

  TEST_CASE("render: a prompt wider than the row scrolls to keep the caret in view");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(Selection{0, 0, -1});

    const auto row_for = [&](int count) {
      PromptOpen(ed, PromptKind::kCommand);
      PromptInsert(ed, typed(count));
      const Surface frame = draw(ed, 40, 6);
      return frame.Row(frame.height - 1);
    };

    const std::string at_39 = row_for(39);
    const std::string at_100 = row_for(100);
    const std::string at_200 = row_for(200);

    // The audit's repro: these three were byte-identical.
    EXPECT_TRUE(at_39 != at_100);
    EXPECT_TRUE(at_100 != at_200);
    EXPECT_TRUE(at_39 != at_200);

    // The sigil keeps column 0, the 39 columns beside it show the tail of what
    // was typed, and the last of them is the blank the caret sits on.
    EXPECT_EQ(at_200, row_like(":" + typed(200).substr(162) + " "));
    EXPECT_TRUE(at_200.find(typed(25)) == std::string::npos);

    PromptOpen(ed, PromptKind::kCommand);
    PromptInsert(ed, typed(200));
    const Surface frame = draw(ed, 40, 6);
    EXPECT_TRUE(frame.cursor_visible);
    EXPECT_EQ(frame.cursor_x, 39);
    EXPECT_EQ(frame.cursor_y, 5);
  }

  TEST_CASE("render: the prompt window follows a caret moved back into the line");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kCommand);
    PromptInsert(ed, typed(200));
    const std::string input = typed(200);

    EXPECT_EQ(draw(ed, 40, 6).Row(5), row_like(":" + input.substr(162) + " "));

    // Twenty steps back is still inside the window, so the window holds still
    // and the caret moves within it: text on both sides of the caret is up.
    for (int i = 0; i < 20; ++i) PromptMoveLeft(ed);
    const Surface near = draw(ed, 40, 6);
    EXPECT_EQ(near.Row(5), row_like(":" + input.substr(162) + " "));
    EXPECT_EQ(near.cursor_x, 19);

    // Forty more walks the caret off the left edge, and the window follows it
    // rather than the end of the string.
    for (int i = 0; i < 40; ++i) PromptMoveLeft(ed);
    const Surface back = draw(ed, 40, 6);
    EXPECT_EQ(back.Row(5), row_like(":" + input.substr(140, 39)));
    EXPECT_EQ(back.cursor_x, 1);

    // And home brings the head of the input back.
    PromptHome(ed);
    const Surface home = draw(ed, 40, 6);
    EXPECT_EQ(home.Row(5), row_like(":" + input.substr(0, 39)));
    EXPECT_EQ(home.cursor_x, 1);
  }

  TEST_CASE("render: a prompt that fits still starts at column 0");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kCommand);
    PromptInsert(ed, "write notes.txt");

    const Surface frame = draw(ed, 40, 6);
    EXPECT_EQ(frame.Row(5), row_like(":write notes.txt" + std::string(24, ' ')));
    EXPECT_EQ(frame.cursor_x, 16);
  }

  TEST_CASE("render: the scrolled prompt never splits a wide cluster at either edge");
  {
    const std::array<std::string, 5> wide{"漢", "字", "日", "本", "語"};
    std::string input(30, 'x');
    for (int i = 0; i < 20; ++i) input += wide[static_cast<std::size_t>(i % 5)];
    input += "!";

    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kCommand);
    PromptInsert(ed, input);

    // 30 columns of ASCII, 40 of double-width and one more: 71 columns for the
    // 39 beside the sigil, so the window starts at 33 -- the back half of a
    // wide cluster, which is blanked rather than cut in two.
    const Surface tail = draw(ed, 40, 6);
    std::string want_tail = ": ";
    for (int i = 2; i < 20; ++i) want_tail += wide[static_cast<std::size_t>(i % 5)] + " ";
    want_tail += "! ";
    EXPECT_EQ(tail.Row(5), row_like(want_tail));
    EXPECT_EQ(tail.cursor_x, 39);

    // At home the half-shown cluster is the one hanging off the right edge.
    PromptHome(ed);
    const Surface head = draw(ed, 40, 6);
    std::string want_head = ":" + std::string(30, 'x');
    for (int i = 0; i < 4; ++i) want_head += wide[static_cast<std::size_t>(i % 5)] + " ";
    want_head += " ";
    EXPECT_EQ(head.Row(5), row_like(want_head));
    EXPECT_EQ(head.cursor_x, 1);

    for (const Surface* frame : {&tail, &head}) {
      EXPECT_TRUE(IsWellFormedUtf8(frame->Row(5)));
      bool whole_clusters = true;
      for (int x = 0; x < frame->width; ++x) {
        const std::string& text = frame->At(x, 5).text;
        if (text.empty()) continue;
        if (!IsWellFormedUtf8(text)) whole_clusters = false;
        if (NextGraphemeInString(text, 0) != text.size()) whole_clusters = false;
      }
      EXPECT_TRUE(whole_clusters);
    }
  }

  TEST_CASE("render: the smart-jump prompt is a box at the caret, not the bottom row");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kSmartJump);
    PromptInsert(ed, "na");

    const Surface frame = draw(ed, 60, 16);

    // Caret on row 0: borders on rows 1 and 3, the rune sigil and the input
    // between them, the terminal cursor inside the box.
    EXPECT_TRUE(frame.Row(1).find("╭") != std::string::npos);
    EXPECT_TRUE(frame.Row(2).find("ᛃ na") != std::string::npos);
    EXPECT_TRUE(frame.Row(3).find("╰") != std::string::npos);
    EXPECT_TRUE(frame.cursor_visible);
    EXPECT_EQ(frame.cursor_y, 2);

    // The bottom row keeps being the status line: no reserved prompt row, so
    // opening the prompt reflows nothing.
    EXPECT_TRUE(frame.Row(frame.height - 1).find("ᛃ") == std::string::npos);
    EXPECT_TRUE(frame.Row(frame.height - 1).find("1:1") != std::string::npos);
  }

  TEST_CASE("render: the box hangs the match feedback off itself and the bar stays quiet");
  {
    Editor ed;
    // The builtin theme, so ui.jump.next resolves to its own colour rather
    // than every foreground falling back to the terminal default together.
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kSmartJump);
    PromptInsert(ed, "na");
    ed.status = "12 zz/yy.cpp";
    ed.status.Highlight(3, 9);

    const Surface frame = draw(ed, 60, 16);
    EXPECT_TRUE(frame.Row(4).find("╰─▸ 12 zz/yy.cpp") != std::string::npos);
    EXPECT_TRUE(frame.Row(frame.height - 1).find("zz/yy.cpp") == std::string::npos);

    // The marked destination wears ui.jump.next, apart from the count.
    int digit_x = -1;
    int path_x = -1;
    for (int x = 0; x < frame.width; ++x) {
      const std::string& t = frame.At(x, 4).text;
      if ((digit_x < 0) && (t == "1")) digit_x = x;
      if ((path_x < 0) && (t == "z")) path_x = x;
    }
    EXPECT_TRUE((digit_x >= 0) && (path_x >= 0));
    EXPECT_TRUE(frame.At(path_x, 4).fg != frame.At(digit_x, 4).fg);
  }

  TEST_CASE("render: the box flips above a caret near the floor, branch out of its top");
  {
    Editor ed;
    std::string text;
    for (int i = 0; i < 40; ++i) text += "line\n";
    ResetToOriginal(ed.doc.table, text);
    const Index end = DocLength(ed.doc.table);
    ed.doc.selections.Set(Selection{end, end, -1});
    PromptOpen(ed, PromptKind::kSmartJump);
    PromptInsert(ed, "na");
    ed.status = "12 zz/yy.cpp";

    // Caret in the scrolloff band off the floor: the box takes the rows above
    // it, the feedback climbing out of the top border instead of hanging
    // below, where the rows the branch would take belong to the caret and the
    // status line.
    const Surface frame = draw(ed, 60, 16);
    const int input_y = frame.cursor_y;
    EXPECT_TRUE(input_y >= 3);
    EXPECT_TRUE(frame.Row(input_y).find("ᛃ na") != std::string::npos);
    EXPECT_TRUE(frame.Row(input_y - 2).find("╭─▸ 12 zz/yy.cpp") != std::string::npos);
    EXPECT_TRUE(frame.Row(input_y + 2).find("▸") == std::string::npos);
  }

  TEST_CASE("render: no room by the caret and the smart-jump prompt takes the row above the bar");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kSmartJump);
    PromptInsert(ed, "na");

    // Nothing was reserved for this prompt, so the bottom row is the pane's
    // status line and the fallback sits above it rather than over it.
    const Surface frame = draw(ed, 40, 4);
    EXPECT_TRUE(frame.Row(2).find("ᛃ na") != std::string::npos);
    EXPECT_EQ(frame.cursor_y, 2);
    EXPECT_TRUE(frame.Row(3).find("ᛃ") == std::string::npos);
    EXPECT_TRUE(frame.Row(3).find("1:1") != std::string::npos);
  }

  TEST_CASE("render: an arrival's feedback is the rounded box at the caret, bar quiet");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.status = "12 zz/yy.cpp";
    ed.status.Highlight(3, 9);
    ed.jump_branch = true;

    const Surface frame = draw(ed, 60, 12);
    EXPECT_TRUE(frame.Row(1).find("╭") != std::string::npos);
    EXPECT_TRUE(frame.Row(2).find("12 zz/yy.cpp") != std::string::npos);
    EXPECT_TRUE(frame.Row(3).find("╰") != std::string::npos);
    EXPECT_TRUE(frame.Row(frame.height - 1).find("zz/yy.cpp") == std::string::npos);

    // Against the floor the box sits above the caret rather than reaching the
    // status line.
    Editor low;
    low.theme = BuiltinTheme();
    std::string text;
    for (int i = 0; i < 30; ++i) text += "line\n";
    ResetToOriginal(low.doc.table, text);
    low.settings.scrolloff = 0;
    const Index end = DocLength(low.doc.table);
    low.doc.selections.Set(Selection{end, end, -1});
    low.status = "12 zz/yy.cpp";
    low.jump_branch = true;

    const Surface floor = draw(low, 60, 12);
    EXPECT_TRUE(floor.Row(7).find("╭") != std::string::npos);
    EXPECT_TRUE(floor.Row(8).find("12 zz/yy.cpp") != std::string::npos);
  }

  TEST_CASE("render: the picker's band hangs off the box, with digits and a count");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    // BuiltinTheme paints no cursorline, so the selected row's band would be
    // the popup fill and the difference below would prove nothing.
    ed.theme.scopes["ui.cursorline.primary"] = Style{{}, Color{true, 0x303030}, 0};
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);
    PromptInsert(ed, "c");

    auto state = std::make_shared<PickerState>();
    for (const std::string_view name :
         {"alpha.cpp", "bravo.cpp", "charlie.cpp", "delta.cpp", "echo.cpp"}) {
      state->rows.push_back(PickerEntry{std::string{name}, ":12", std::string{name}, 12, 1});
    }
    // Three of the five kept, the middle one selected: the count reads what the
    // filter left of the list, not how many rows the band has room for.
    state->shown = {0, 2, 4};
    state->selected = 1;
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    const Surface frame = draw(ed, 60, 16);

    // The connector on the row touching the box, the others indented to the
    // same text column, and the digit on each is what opens it.
    EXPECT_TRUE(frame.Row(4).find("╰─▸ 1 alpha.cpp:12") != std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("2 charlie.cpp:12") != std::string::npos);
    EXPECT_TRUE(frame.Row(6).find("3 echo.cpp:12") != std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("▸") == std::string::npos);
    // The count is where the selection is, of what the filter left, of what
    // there was: the second of the three kept, of five rows.
    EXPECT_TRUE(frame.Row(6).find("2/3/5") != std::string::npos);

    const auto column_of = [&frame](int y, std::string_view glyph) {
      for (int x = 0; x < frame.width; ++x) {
        if (frame.At(x, y).text == glyph) return x;
      }
      return -1;
    };
    // The selected row wears the cursorline band, and its text the colour a
    // destination wears.
    const int alpha_x = column_of(4, "a");
    const int charlie_x = column_of(5, "c");
    EXPECT_TRUE((alpha_x >= 0) && (charlie_x >= 0));
    EXPECT_TRUE(frame.At(charlie_x, 5).bg != frame.At(alpha_x, 4).bg);
    EXPECT_TRUE(frame.At(charlie_x, 5).fg != frame.At(alpha_x, 4).fg);
  }

  TEST_CASE("render: a symbol picker draws its block between the box and the band");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kDefs;
    for (const std::string_view where : {"src/a.cpp", "src/b.cpp"}) {
      PickerEntry row;
      // Defs: the line the row points at is the row, the name being in it
      // already. A symbol list leads with the name instead, same right edge.
      row.text = "int Widget = 1;";
      row.read = true;
      row.detail = std::string{where} + ":12";
      row.target = std::string{where};
      row.line = 12;
      row.name = "Widget";
      state->rows.push_back(std::move(row));
    }
    state->shown = {0, 1};
    state->context = {"void Above() {}", "  int Widget = 1;", "}"};
    state->context_first = 11;
    state->context_target = 12;
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    const Surface frame = draw(ed, 60, 16);

    // The box takes rows 1..3, so the block is 4..6, the rule is 7 and the band
    // starts at 8. A card this wide is the screen's width and starts at the
    // margin, so it hangs off nothing and draws the plain indent.
    EXPECT_TRUE(frame.Row(4).find("11 void Above() {}") != std::string::npos);
    EXPECT_TRUE(frame.Row(4).find("▸") == std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("12   int Widget = 1;") != std::string::npos);
    EXPECT_TRUE(frame.Row(6).find("13 }") != std::string::npos);
    // A rule between the two, so context lines and list rows do not read as one
    // list, and nothing of the block's own on it.
    EXPECT_TRUE(frame.Row(7).find("───") != std::string::npos);
    EXPECT_TRUE(frame.Row(7).find("Widget") == std::string::npos);
    EXPECT_TRUE(frame.Row(8).find("1 int Widget = 1;") != std::string::npos);
    EXPECT_TRUE(frame.Row(8).find("▸") == std::string::npos);

    // The line on the left, where it is at the right edge of the card -- and
    // the same right edge on both rows, the count's column kept off them.
    const int a_at = static_cast<int>(frame.Row(8).find("src/a.cpp:12"));
    const int b_at = static_cast<int>(frame.Row(9).find("src/b.cpp:12"));
    EXPECT_TRUE(a_at > static_cast<int>(frame.Row(8).find("int Widget")));
    EXPECT_EQ(a_at, b_at);
    EXPECT_TRUE(frame.Row(9).find("1/2/2") != std::string::npos);

    const auto column_of = [](const Surface& f, int y, std::string_view glyph) {
      for (int x = 0; x < f.width; ++x) {
        if (f.At(x, y).text == glyph) return x;
      }
      return -1;
    };
    // The target line is the only one marked, and it is marked in the
    // foreground -- ui.excerpt.match, the way --no-syntax previews mark it.
    const int above_x = column_of(frame, 4, "v");
    const int target_x = column_of(frame, 5, "W");
    EXPECT_TRUE((above_x >= 0) && (target_x >= 0));
    EXPECT_TRUE(frame.At(target_x, 5).fg != frame.At(above_x, 4).fg);
    EXPECT_TRUE(frame.At(target_x, 5).bg == frame.At(above_x, 4).bg);

    // Flipped, the whole stack goes with it: the box's rows are counted with
    // the block's, the rule's and the band's, so nothing lands on the status
    // line and the order away from the caret is the same one.
    ResetToOriginal(ed.doc.table, "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11\n12\n");
    ed.doc.selections.Set(Selection{24, 24, -1});
    const Surface up = draw(ed, 60, 16);
    const auto row_with = [](const Surface& f, std::string_view needle) {
      for (int y = 0; y < f.height; ++y) {
        if (f.Row(y).find(needle) != std::string::npos) return y;
      }
      return -1;
    };
    const int band = row_with(up, "1 int Widget = 1;");
    const int block = row_with(up, "12   int Widget = 1;");
    const int box = row_with(up, "ᛃ");
    EXPECT_TRUE((band >= 0) && (block >= 0) && (box >= 0));
    EXPECT_TRUE(band < block);
    EXPECT_TRUE(block < box);
    EXPECT_EQ(row_with(up, "13 }"), block + 1);
    // The rule keeps its place between the two, whichever way the stack hangs.
    EXPECT_EQ(row_with(up, "───"), band + 2);

    // A file row is its own evidence: no lines are read for it, so there is no
    // block, no rule, and the band goes back to touching the box. Text and
    // detail read as one there -- a path with the line it was left on.
    state->source = PickerState::Source::kFiles;
    ed.doc.selections.Set(Selection{0, 0, -1});
    state->context.clear();
    for (PickerEntry& row : state->rows) {
      row.text = row.target;
      row.detail = ":12";
    }
    state->card_w = PickerCardWidth(*state);
    const Surface plain = draw(ed, 60, 16);
    EXPECT_TRUE(plain.Row(4).find("╰─▸ 1 src/a.cpp:12") != std::string::npos);
    EXPECT_TRUE(plain.Row(5).find("2 src/b.cpp:12") != std::string::npos);
    EXPECT_TRUE(plain.Row(6).find("Above") == std::string::npos);
    EXPECT_TRUE(plain.Row(6).find("───") == std::string::npos);
  }

  TEST_CASE("render: a content row leads with the line and keeps its path at the edge");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    // Content's rows are byte offsets into the scan's corpus, and `rows` stays
    // empty -- so the fixture is the corpus, built the way the scan's reader
    // would have left it.
    const std::string first =
        "src/a.cpp:12:  int Widget = 1;  // " + std::string(80, 'x') + "ZZEND\n";
    const std::string corpus = first + "src/b.cpp:3:another line\n";
    auto stream = common::MmapStreamFromBytes(corpus.data(), corpus.size());
    EXPECT_TRUE(stream.has_value());
    if (!stream) return;

    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kContent;
    state->scan = std::make_unique<PickerScan>();
    state->scan->out = std::move(*stream);
    state->scan->parsed = corpus.size();
    state->scan->lines = 2;
    // Not live, so the count is the plain i/n/m without the scanning note.
    state->scan->done = true;
    state->shown = {0, first.size()};
    state->context = {"void Above() {}", "  int Widget = 1;", "}"};
    state->context_first = 11;
    state->context_target = 12;
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    // Nothing was measured to get there: the card is the screen's answer.
    EXPECT_EQ(state->card_w, kPickerCardWide);

    const Surface frame = draw(ed, 60, 16);

    // Box on rows 1..3, then the block -- the lines only, with no heading of
    // its own: the row below says which file this is.
    EXPECT_TRUE(frame.Row(4).find("11 void Above() {}") != std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("12   int Widget = 1;") != std::string::npos);
    EXPECT_TRUE(frame.Row(6).find("13 }") != std::string::npos);
    EXPECT_TRUE(frame.Row(7).find("───") != std::string::npos);

    // The row leads with the line, trimmed as a defs row's line is, and the
    // `path:line:` head is off it: what clips is the far end of the line, not
    // its first columns.
    EXPECT_TRUE(frame.Row(8).find("1 int Widget = 1;  // xxx") != std::string::npos);
    EXPECT_TRUE(frame.Row(8).find("src/a.cpp:12:") == std::string::npos);
    EXPECT_TRUE(frame.Row(8).find("ZZEND") == std::string::npos);
    EXPECT_TRUE(frame.Row(9).find("2 another line") != std::string::npos);

    // The head rides the right edge instead -- ending on the same column for
    // both rows, whatever each one's length, with the count's room kept off
    // them: a symbol band, in other words.
    const int a_at = static_cast<int>(frame.Row(8).find("src/a.cpp:12"));
    const int b_at = static_cast<int>(frame.Row(9).find("src/b.cpp:3"));
    EXPECT_TRUE(a_at > static_cast<int>(frame.Row(8).find("int Widget")));
    EXPECT_EQ(a_at + 12, b_at + 11);
    // The selection is the first row, of two shown, of two lines scanned.
    EXPECT_TRUE(frame.Row(9).find("1/2/2") != std::string::npos);

    const auto column_of = [](const Surface& f, int y, std::string_view glyph) {
      for (int x = 0; x < f.width; ++x) {
        if (f.At(x, y).text == glyph) return x;
      }
      return -1;
    };
    // The target line in the block wears ui.excerpt.match, as it does for every
    // other source -- the block does not care what holds the row.
    const int above_x = column_of(frame, 4, "v");
    const int target_x = column_of(frame, 5, "W");
    EXPECT_TRUE((above_x >= 0) && (target_x >= 0));
    EXPECT_TRUE(frame.At(target_x, 5).fg != frame.At(above_x, 4).fg);

    // The card takes the room at the box's edge and stops there: the count is
    // the card's last column, and nothing is painted past the screen.
    const int edge = column_of(frame, 8, "1");
    EXPECT_TRUE(edge > 0);
    EXPECT_EQ(frame.At(frame.width - 2, 9).text, std::string{"2"});
    EXPECT_EQ(frame.At(frame.width - 1, 9).text, std::string{" "});
  }

  TEST_CASE("render: the buffers band grows to what the screen fits");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kBuffers;
    for (int i = 0; i < 12; ++i) {
      const std::string name = "buffer" + std::to_string(i) + ".txt";
      state->rows.push_back(PickerEntry{name, {}, name, 1, 1});
      state->shown.push_back(static_cast<std::size_t>(i));
    }
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    // Room below the caret for all twelve: every buffer is on screen, which is
    // the whole point of reading a buffer list.
    const Surface tall = draw(ed, 60, 24);
    EXPECT_TRUE(tall.Row(4).find("1 buffer0.txt") != std::string::npos);
    EXPECT_TRUE(tall.Row(15).find("buffer11.txt") != std::string::npos);
    EXPECT_EQ(ed.picker->window, std::size_t{12});

    // The digits stop where the accept gate stops: a grown band is walked past
    // its fifth row, so those rows carry no number to press, and the text
    // column is the same one a numbered row uses.
    EXPECT_TRUE(tall.Row(8).find("5 buffer4.txt") != std::string::npos);
    EXPECT_TRUE(tall.Row(9).find("6 buffer5.txt") == std::string::npos);
    EXPECT_TRUE(tall.Row(15).find("12 buffer11.txt") == std::string::npos);
    EXPECT_EQ(tall.Row(15).find("buffer11.txt"), tall.Row(5).find("buffer1.txt"));

    // The index is padded to the shown count's width, so stepping past the
    // ninth row does not widen the count and shift what sits left of it.
    const std::size_t count_at = tall.Row(15).find(" 1/12/12");
    EXPECT_TRUE(count_at != std::string::npos);
    for (int i = 0; i < 9; ++i) PickerStep(ed, true);
    const Surface tenth = draw(ed, 60, 24);
    EXPECT_EQ(tenth.Row(15).find("10/12/12"), count_at);
    for (int i = 0; i < 3; ++i) PickerStep(ed, true);
    EXPECT_EQ(ed.picker->selected, std::size_t{0});

    // A short screen takes what it can and the rest is walked: the box still
    // hangs off the caret rather than falling to the bottom row.
    const Surface squat = draw(ed, 60, 12);
    EXPECT_TRUE(squat.Row(4).find("1 buffer0.txt") != std::string::npos);
    EXPECT_TRUE(ed.picker->window < std::size_t{12});
    EXPECT_TRUE(ed.picker->window >= kPickerRows);

    // Files is not buffers: five rows and a window over the rest.
    state->source = PickerState::Source::kFiles;
    const Surface files = draw(ed, 60, 24);
    EXPECT_TRUE(files.Row(8).find("5 buffer4.txt") != std::string::npos);
    EXPECT_TRUE(files.Row(9).find("buffer5.txt") == std::string::npos);
    EXPECT_EQ(ed.picker->window, kPickerRows);
  }

  TEST_CASE("render: the band draws its window, wherever the selection scrolled it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    auto state = std::make_shared<PickerState>();
    for (int i = 0; i < 9; ++i) {
      const std::string name = "file" + std::to_string(i) + ".txt";
      state->rows.push_back(PickerEntry{name, {}, name, 1, 1});
      state->shown.push_back(static_cast<std::size_t>(i));
    }
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    // Stepped to the last row: the window has scrolled to the end of the list,
    // the digits still number the five rows drawn, and the count says how far
    // in the list the band is showing from.
    for (int i = 0; i < 8; ++i) PickerStep(ed, true);
    EXPECT_EQ(ed.picker->offset, std::size_t{4});
    const Surface frame = draw(ed, 60, 20);
    EXPECT_TRUE(frame.Row(4).find("1 file4.txt") != std::string::npos);
    EXPECT_TRUE(frame.Row(8).find("5 file8.txt") != std::string::npos);
    EXPECT_TRUE(frame.Row(8).find("9/9/9") != std::string::npos);
    // Drawing settles the window, it does not move it.
    EXPECT_EQ(ed.picker->offset, std::size_t{4});
  }

  TEST_CASE("render: the count says a scan is still coming, until it is not");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kProjectSymbols;
    for (int i = 0; i < 2; ++i) {
      PickerEntry row;
      row.text = "int Sym" + std::to_string(i) + "();";
      row.read = true;
      row.detail = "s.cpp:" + std::to_string(i + 1);
      row.target = "s.cpp";
      row.line = i + 1;
      state->rows.push_back(std::move(row));
      state->shown.push_back(static_cast<std::size_t>(i));
    }
    // No pid and no fd: what makes this a live scan is that nothing has closed
    // it, which is the same thing the band asks.
    state->scan = std::make_unique<PickerScan>();
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    const Surface scanning = draw(ed, 70, 20);
    EXPECT_TRUE(scanning.Row(5).find("1/2/2 scanning") != std::string::npos);

    // The pipe closes and the count is the plain answer it was waiting to be.
    state->scan->done = true;
    const Surface done = draw(ed, 70, 20);
    EXPECT_TRUE(done.Row(5).find("scanning") == std::string::npos);
    EXPECT_TRUE(done.Row(5).find("1/2/2") != std::string::npos);

    // Unless it stopped at the corpus ceiling, where a plain 2/2 would be the
    // band claiming a slice of the project as the whole of it.
    state->scan->truncated = true;
    const Surface cut = draw(ed, 70, 20);
    EXPECT_TRUE(cut.Row(5).find("1/2/2 truncated") != std::string::npos);
  }

  TEST_CASE("render: a card with room for one of the two keeps the line, not the detail");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kDefs;
    // A path most of an 80-column screen wide: keeping its column at the right
    // edge would leave the line it belongs to nowhere to draw.
    const std::string deep = "packages/design-system/src/components/nav/sidebar/SideBarItem.tsx:1234";
    for (int i = 0; i < 2; ++i) {
      PickerEntry row;
      row.text = "int Widget" + std::to_string(i) + " = 1;";
      row.read = true;
      row.detail = deep;
      row.target = "SideBarItem.tsx";
      row.line = 1234;
      state->rows.push_back(std::move(row));
      state->shown.push_back(static_cast<std::size_t>(i));
    }
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    // The line is what the row is for, so the line is what survives: the detail
    // goes rather than the row reading as a bare digit.
    const Surface tight = draw(ed, 80, 20);
    EXPECT_TRUE(tight.Row(4).find("1 int Widget0 = 1;") != std::string::npos);
    EXPECT_TRUE(tight.Row(5).find("2 int Widget1 = 1;") != std::string::npos);
    EXPECT_TRUE(tight.Row(4).find("SideBarItem") == std::string::npos);

    // A detail with room left over for some of the line keeps its column, and
    // the line clips beside it.
    const std::string mid = std::string(48, 'p') + "/Item.tsx:12";
    for (PickerEntry& row : state->rows) row.detail = mid;
    state->card_w = PickerCardWidth(*state);
    const Surface both = draw(ed, 80, 20);
    EXPECT_TRUE(both.Row(4).find("1 int W") != std::string::npos);
    EXPECT_TRUE(both.Row(4).find("int Widget0 = 1;") == std::string::npos);
    EXPECT_TRUE(both.Row(4).find("/Item.tsx:12") != std::string::npos);

    // And the scanning note is width like any other: while it rides the count
    // the detail gives way, and the rows say more of what they are about rather
    // than blanking until the scan ends.
    state->scan = std::make_unique<PickerScan>();
    state->card_w = PickerCardWidth(*state);
    const Surface live = draw(ed, 80, 20);
    EXPECT_TRUE(live.Row(5).find("scanning") != std::string::npos);
    EXPECT_TRUE(live.Row(4).find("1 int Widget0 = 1;") != std::string::npos);
    EXPECT_TRUE(live.Row(4).find("/Item.tsx:12") == std::string::npos);
  }

  TEST_CASE("render: a clipped row stops short of the count the last row carries");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    PromptOpen(ed, PromptKind::kPicker);

    // A file row runs to the card's edge -- its detail reads as part of the path
    // rather than taking a column at the right -- so the count's row is the one
    // that has to give its column up.
    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kFiles;
    for (const char c : {'a', 'b'}) {
      const std::string path = "src/" + std::string(90, c) + ".cpp";
      state->rows.push_back(PickerEntry{path, {}, path, 1, 1});
      state->shown.push_back(state->shown.size());
    }
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    const Surface frame = draw(ed, 60, 20);
    // A gap between the two, not a count stamped over the path under it.
    EXPECT_TRUE(frame.Row(5).find("b 1/2/2") != std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("bb1/2/2") == std::string::npos);
    // The rows the count does not ride still run to the card's last column.
    EXPECT_TRUE(frame.At(frame.width - 2, 4).text == "a");
  }

  TEST_CASE("render: a card pulled off the box's edge draws no connector back to it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    std::string text;
    for (int i = 0; i < 12; ++i) text += std::string(70, '.') + "\n";
    ResetToOriginal(ed.doc.table, text);
    ed.settings.scrolloff = 0;
    ed.doc.selections.Set(Selection{4 * 71, 4 * 71, -1});
    PromptOpen(ed, PromptKind::kPicker);

    // A measured card, which is a file band: the ones over file content take
    // the screen and start at the margin, so they never touch the box at all.
    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kFiles;
    for (int i = 0; i < 2; ++i) {
      const std::string path = "src/pick" + std::to_string(i) + std::string(60, 'w') + ".cpp";
      state->rows.push_back(PickerEntry{path, ":12", path, 12, 1});
      state->shown.push_back(static_cast<std::size_t>(i));
    }
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;

    const auto rows_with = [](const Surface& f, std::string_view needle) {
      std::vector<std::string> found;
      for (int y = 0; y < f.height; ++y) {
        if (f.Row(y).find(needle) != std::string::npos) found.push_back(f.Row(y));
      }
      return found;
    };

    // At the left margin the card starts under the box and says so.
    const Surface hung = draw(ed, 100, 24);
    const std::vector<std::string> attached = rows_with(hung, "src/pick");
    EXPECT_EQ(attached.size(), std::size_t{2});
    EXPECT_TRUE(!attached.empty() && (attached[0].find("╰─▸") != std::string::npos));

    // Out at column 60 the card cannot both start under the box and end on the
    // screen, so it is pulled left -- and an arrow from there would point at
    // the code in between rather than at the box.
    ed.doc.selections.Set(Selection{(4 * 71) + 60, (4 * 71) + 60, -1});
    const Surface pulled = draw(ed, 100, 24);
    const std::vector<std::string> loose = rows_with(pulled, "src/pick");
    EXPECT_EQ(loose.size(), std::size_t{2});
    for (const std::string& row : loose) EXPECT_TRUE(row.find("▸") == std::string::npos);
    // The box did not move with the card: it is still out at the caret.
    const auto column_of = [](const Surface& f, std::string_view glyph) {
      for (int y = 0; y < f.height; ++y) {
        for (int x = 0; x < f.width; ++x) {
          if (f.At(x, y).text == glyph) return x;
        }
      }
      return -1;
    };
    EXPECT_TRUE(column_of(pulled, "ᛃ") > 50);
  }

  // A defs picker with everything a stack can hold: three context lines, the
  // rule under them, five band rows and a warning wanting a branch row of its
  // own. The rows say "pick", the block says "ctx", so counting either in a
  // frame says what was drawn.
  const auto squeezed = [](Editor& ed, Index caret_line) {
    ed.theme = BuiltinTheme();
    std::string text;
    for (int i = 0; i < 12; ++i) text += "line" + std::to_string(i) + "\n";
    ResetToOriginal(ed.doc.table, text);
    ed.settings.scrolloff = 0;
    const Index at = caret_line * 6;
    ed.doc.selections.Set(Selection{at, at, -1});
    PromptOpen(ed, PromptKind::kPicker);

    auto state = std::make_shared<PickerState>();
    state->source = PickerState::Source::kDefs;
    for (int i = 0; i < 9; ++i) {
      PickerEntry row;
      row.text = "int pick" + std::to_string(i) + "();";
      row.read = true;
      row.detail = "s.cpp:" + std::to_string(i + 1);
      row.target = "s.cpp";
      row.line = i + 1;
      row.name = "pick" + std::to_string(i);
      state->rows.push_back(std::move(row));
      state->shown.push_back(static_cast<std::size_t>(i));
    }
    state->context = {"ctx above", "ctx target", "ctx below"};
    state->context_first = 11;
    state->context_target = 12;
    state->card_w = PickerCardWidth(*state);
    ed.picker = state;
    // What a half-typed pattern leaves behind: the list stands and the box says
    // why. The row it wants is what pushes the stack past a short side.
    ed.status.Warn("bad pattern");
  };

  TEST_CASE("render: a caret with no room for the whole stack shrinks it, block first");
  {
    Editor ed;
    squeezed(ed, 6);

    // Room below the caret for the box, the branch row and four band rows, and
    // that is what draws: the block and its rule are given up first, the band
    // takes what is left, and nothing falls to the bottom row.
    const Surface frame = draw(ed, 100, 16);
    EXPECT_EQ(frame.cursor_y, 8);
    EXPECT_TRUE(frame.Row(10).find("╰─▸ bad pattern") != std::string::npos);
    EXPECT_TRUE(frame.Row(11).find("1 int pick0();") != std::string::npos);
    EXPECT_TRUE(frame.Row(14).find("4 int pick3();") != std::string::npos);
    EXPECT_EQ(ed.picker->window, std::size_t{4});
    for (int y = 0; y < frame.height; ++y) {
      EXPECT_TRUE(frame.Row(y).find("ctx ") == std::string::npos);
    }
    // The bar is still the bar: the prompt did not land on it, and with the box
    // saying the warning the bar does not say it twice.
    EXPECT_TRUE(frame.Row(15).find("ᛃ") == std::string::npos);
    EXPECT_TRUE(frame.Row(15).find("7:1") != std::string::npos);
    EXPECT_TRUE(frame.Row(15).find("bad pattern") == std::string::npos);

    // The band drew four rows, so the fifth digit names nothing and is refused
    // rather than opening a row nobody can see.
    PickerAccept(ed, 4);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);

    // Room for the whole stack, and the whole stack is back: block, rule, five
    // rows, the window with them.
    const Surface tall = draw(ed, 100, 24);
    EXPECT_EQ(ed.picker->window, kPickerRows);
    EXPECT_TRUE(tall.Row(11).find("ctx above") != std::string::npos);
    EXPECT_TRUE(tall.Row(14).find("───") != std::string::npos);
    EXPECT_TRUE(tall.Row(15).find("1 int pick0();") != std::string::npos);
    EXPECT_TRUE(tall.Row(19).find("5 int pick4();") != std::string::npos);
  }

  TEST_CASE("render: a box with no room at all keeps its keys and its message honest");
  {
    Editor ed;
    squeezed(ed, 2);

    // Neither side has room for the box: the input keeps a row above the bar,
    // the band drew nothing, and the warning the box would have said is the
    // bar's again.
    const Surface frame = draw(ed, 100, 6);
    EXPECT_EQ(frame.cursor_y, 4);
    EXPECT_TRUE(frame.Row(4).find("ᛃ") != std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("ᛃ") == std::string::npos);
    EXPECT_TRUE(frame.Row(5).find("bad pattern") != std::string::npos);
    EXPECT_EQ(ed.picker->window, std::size_t{0});
    for (int y = 0; y < frame.height; ++y) {
      EXPECT_TRUE(frame.Row(y).find("pick") == std::string::npos);
    }

    // No band, no accelerators: every digit names a row that was never drawn.
    PickerAccept(ed, 0);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.picker != nullptr);
  }

  TEST_CASE("render: a picker prompt at every size draws what its keys walk");
  {
    const std::vector<int> widths{1, 2, 5, 12, 16, 20, 24, 40, 60, 100};
    // Every caret row of a short screen, and a split, where the pane the box
    // hangs in is shorter than the screen and its bar is not the bottom row.
    for (const Index caret_line : {Index{0}, Index{4}, Index{9}}) {
      for (const bool split : {false, true}) {
        Editor ed;
        squeezed(ed, caret_line);
        if (split) SplitWindow(ed, false);
        for (const int w : widths) {
          for (int h = 1; h <= 24; ++h) {
            const Surface frame = draw(ed, w, h);
            EXPECT_EQ(frame.cells.size(), static_cast<std::size_t>(w) * h);
            if (frame.cursor_visible) {
              EXPECT_TRUE((frame.cursor_x >= 0) && (frame.cursor_x < w));
              EXPECT_TRUE((frame.cursor_y >= 0) && (frame.cursor_y < h));
            }
            // The window is what the digits accept on, so it may never be more
            // than the band drew: a row that never fit is a row the eyes never
            // saw.
            EXPECT_TRUE(ed.picker->window <= kPickerRows);
            if (w >= 40) {
              int drawn = 0;
              for (int y = 0; y < h; ++y) {
                if (frame.Row(y).find("int pick") != std::string::npos) ++drawn;
              }
              EXPECT_EQ(ed.picker->window, static_cast<std::size_t>(drawn));
            }
            // The bottom row belongs to a bar: a prompt that could not fit at
            // the caret sits above it, never on it.
            if (h >= 2) EXPECT_TRUE(frame.Row(h - 1).find("ᛃ") == std::string::npos);
            // And whoever ends up saying the warning, someone does.
            const StatusLine bar = StatusBar(ed, true);
            EXPECT_TRUE(ed.prompt_box_said || (bar.message_from < bar.left.size()));
          }
        }
      }
    }
  }

  TEST_CASE("render: the smart-jump box behaves at every size");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\necho\nfoxtrot\n");
    ed.doc.selections.Set(Selection{10, 10, -1});
    PromptOpen(ed, PromptKind::kSmartJump);
    PromptInsert(ed, "na");
    ed.status = "3 zz/yy.cpp";
    for (int w = 1; w <= 24; ++w) {
      for (int h = 1; h <= 12; ++h) {
        const Surface frame = draw(ed, w, h);
        EXPECT_EQ(frame.cells.size(), static_cast<std::size_t>(w) * h);
        if (frame.cursor_visible) {
          EXPECT_TRUE((frame.cursor_x >= 0) && (frame.cursor_x < w));
          EXPECT_TRUE((frame.cursor_y >= 0) && (frame.cursor_y < h));
        }
      }
    }
  }

  TEST_CASE("render: every size from tiny to large draws without misbehaving");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\necho\nfoxtrot\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    SplitWindow(ed, true);
    SplitWindow(ed, false);
    for (int w = 1; w <= 24; ++w) {
      for (int h = 1; h <= 12; ++h) {
        const Surface frame = draw(ed, w, h);
        EXPECT_EQ(frame.cells.size(), static_cast<std::size_t>(w) * h);
        if (frame.cursor_visible) {
          EXPECT_TRUE((frame.cursor_x >= 0) && (frame.cursor_x < w));
          EXPECT_TRUE((frame.cursor_y >= 0) && (frame.cursor_y < h));
        }
      }
    }
  }
}

void RenderingGaps() {
  const auto draw = [](Editor& ed, int w, int h) {
    Surface frame;
    FitFocusedViewport(ed, w, h);
    RenderTo(ed, frame, w, h);
    return frame;
  };

  TEST_CASE("mouse: a click lands in the window it was made in");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha bravo charlie\ndelta echo foxtrot\ngolf hotel india\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    SplitWindow(ed, true);

    constexpr int kW = 60;
    constexpr int kH = 10;
    const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, kW, kH});
    EXPECT_EQ(areas.size(), std::size_t{2});
    if (areas.size() != 2) return;

    Rect got{};
    const std::vector<int> order = WindowOrder(ed);
    const int left = WindowAtPoint(ed, areas[0].x + 1, 1, kW, kH, got);
    EXPECT_EQ(left, order[0]);
    EXPECT_TRUE(got == areas[0]);
    const int right = WindowAtPoint(ed, areas[1].x + 1, 1, kW, kH, got);
    EXPECT_EQ(right, order[1]);
    EXPECT_TRUE(got == areas[1]);
    EXPECT_EQ(WindowAtPoint(ed, kW + 5, 1, kW, kH, got), -1);
    EXPECT_EQ(WindowAtPoint(ed, 1, kH + 5, kW, kH, got), -1);

    const Index length = DocLength(ed.doc.table);
    for (int y = 0; y < (kH - 1); ++y) {
      for (int x = 0; x < kW; ++x) {
        Rect area{};
        const int leaf = WindowAtPoint(ed, x, y, kW, kH, area);
        EXPECT_TRUE(leaf >= 0);
        if (leaf < 0) continue;
        const int local_y = y - area.y;
        if (local_y >= (area.h - 1)) continue;
        const int gutter = GutterWidth(ed, area.w);
        const Index at = PositionAtScreen(ed, WrapOf(ed, gutter, area.w), gutter, x - area.x,
                                          local_y);
        EXPECT_TRUE(at >= 0);
        EXPECT_TRUE(at <= length);
        EXPECT_TRUE(IsGraphemeBoundary(ed.doc.table, at));
      }
    }

    const int gutter = GutterWidth(ed, areas[0].w);
    EXPECT_EQ(gutter, GutterWidth(ed, areas[1].w));
    const Index in_left = PositionAtScreen(ed, WrapOf(ed, gutter, areas[0].w), gutter, gutter + 3, 1);
    const Index in_right =
        PositionAtScreen(ed, WrapOf(ed, gutter, areas[1].w), gutter, gutter + 3, 1);
    EXPECT_EQ(in_left, in_right);
  }

  TEST_CASE("mouse: an unsplit editor resolves clicks, which needs no split tree");
  {

    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha bravo\ncharlie delta\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    EXPECT_TRUE(ed.windows.empty());

    Rect area{};
    EXPECT_TRUE(WindowAtPoint(ed, 0, 0, 40, 8, area) >= 0);
    EXPECT_TRUE(area.w > 0);
    EXPECT_TRUE(area.h > 0);
    EXPECT_TRUE(WindowAtPoint(ed, 3, 1, 40, 8, area) >= 0);

    EXPECT_EQ(WindowAtPoint(ed, 100, 1, 40, 8, area), -1);
    EXPECT_EQ(WindowAtPoint(ed, 1, 100, 40, 8, area), -1);

    Rect hit{};
    EXPECT_TRUE(WindowAtPoint(ed, 8, 1, 40, 8, hit) >= 0);
    const int text_w = TextWidthOf(hit, 40);
    const int gutter = GutterWidth(ed, text_w);
    const Index at = PositionAtScreen(ed, WrapOf(ed, gutter, text_w), gutter, 8, 1);
    EXPECT_TRUE(at >= 0);
    EXPECT_TRUE(at <= DocLength(ed.doc.table));
  }

  TEST_CASE("soft wrap: a narrow split wraps inside its own window");
  {
    Editor ed;
    std::string long_line;
    for (int i = 0; i < 30; ++i) long_line += "wrapme ";
    ResetToOriginal(ed.doc.table, long_line + "\nshort\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.settings.soft_wrap = true;
    SplitWindow(ed, true);

    for (const int w : {20, 40, 60, 100}) {
      const Surface frame = draw(ed, w, 12);
      EXPECT_EQ(frame.width, w);

      bool painted = true;
      for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
          if (frame.At(x, y).text.empty()) painted = false;
        }
      }
      EXPECT_TRUE(painted);
      int indicators = 0;
      for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
          if (frame.At(x, y).text == "↳") ++indicators;
        }
      }

      EXPECT_TRUE(indicators >= 2);
      if (frame.cursor_visible) {
        EXPECT_TRUE((frame.cursor_x >= 0) && (frame.cursor_x < w));
        EXPECT_TRUE((frame.cursor_y >= 0) && (frame.cursor_y < 12));
      }
    }
    ed.settings.soft_wrap = false;
  }

  // Byte -> cell -> byte, for every grapheme in the document, across the layout
  // knobs that move the answer. Five functions have to agree for this to close:
  // LayoutLine and WrappedRows decide which row a byte lands on, ScrollWrapped
  // decides which rows are on screen, DrawLine decides which column, and
  // PositionAtScreen inverts all of it. Nothing tested them against each other
  // before -- each was tested against its own expectations.
  TEST_CASE("click round-trip: the byte under the caret is the byte the caret is on");
  {
    struct Sample {
      const char* what;
      const char* text;
    };
    static constexpr std::array<Sample, 6> kSamples{{
        {"ascii", "alpha bravo charlie delta echo foxtrot\nsecond line here\n"},
        {"tabs", "a\tb\tc\tdef\tghi\n\t\tindented\n"},
        {"cjk", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e mixed \xe4\xb8\xad\xe6\x96\x87 text\n"},
        {"emoji", "hi \xf0\x9f\x8c\x8a ocean \xf0\x9f\x90\x9f fish\n"},
        {"combining", "e\xcc\x81gal a\xcc\x80 co\xcc\x82te\xcc\x81 done\n"},
        {"wide+tab", "\xe4\xb8\xad\tx\xe6\x96\x87\t\xf0\x9f\x8c\x8a\ty\n"},
    }};

    int checked = 0;
    for (const Sample& sample : kSamples) {
      for (const bool wrap : {false, true}) {
        for (const int width : {15, 24, 40}) {
          Editor ed;
          ed.theme = BuiltinTheme();
          ResetToOriginal(ed.doc.table, sample.text);
          ed.settings.soft_wrap = wrap;
          // Insert mode, because that is when the draw reports a terminal caret
          // cell to invert. Normal mode paints the block cursor as a reversed
          // glyph instead and leaves cursor_visible false; the layout being
          // checked here is the same either way.
          ed.mode = Mode::kInsert;

          const Index length = DocLength(ed.doc.table);
          for (Index p = 0; p <= length; ++p) {
            if (!IsGraphemeBoundary(ed.doc.table, p)) continue;
            ed.doc.selections.Set(Selection{p, p, -1});
            const Surface frame = draw(ed, width, 10);
            if (!frame.cursor_visible) continue;

            Rect area{};
            const int leaf =
                WindowAtPoint(ed, frame.cursor_x, frame.cursor_y, width, 10, area);
            if (leaf < 0) continue;
            const int text_w = TextWidthOf(area, width);
            const int gutter = GutterWidth(ed, text_w);
            const Index back =
                PositionAtScreen(ed, WrapOf(ed, gutter, text_w), gutter,
                                 frame.cursor_x - area.x, frame.cursor_y - area.y);
            if (back != ed.doc.selections.Primary().From()) {
              std::cerr << "FAIL [" << common::g_test_case << "] click round-trip "
                        << sample.what << " wrap=" << wrap << " width=" << width
                        << " byte=" << p << " came back as " << back << "\n";
              ++common::g_test_failures;
            }
            ++checked;
          }
        }
      }
    }
    // The guard is against the loop above quietly skipping everything -- an
    // always-invisible caret would make every assertion above vacuous.
    EXPECT_TRUE(checked > 800);
  }

  TEST_CASE("resize: the cursor stays on screen through any sequence of sizes");
  {
    Editor ed;
    std::string doc;
    for (int i = 0; i < 200; ++i) doc += "line " + std::to_string(i) + " of the document\n";
    ResetToOriginal(ed.doc.table, doc);
    ed.doc.selections.Set(Selection{0, 0, -1});
    SplitWindow(ed, false);

    for (int i = 0; i < 120; ++i) RunCommands(ed, {"move_line_down"});

    for (const std::pair<int, int>& size :
         {std::pair{80, 24}, std::pair{40, 12}, std::pair{10, 4}, std::pair{2, 2},
          std::pair{1, 1}, std::pair{200, 60}, std::pair{80, 24}}) {
      const Surface frame = draw(ed, size.first, size.second);
      EXPECT_EQ(frame.width, size.first);
      EXPECT_EQ(frame.height, size.second);
      EXPECT_EQ(frame.cells.size(), static_cast<std::size_t>(size.first) * size.second);
      if (frame.cursor_visible) {
        EXPECT_TRUE((frame.cursor_x >= 0) && (frame.cursor_x < size.first));
        EXPECT_TRUE((frame.cursor_y >= 0) && (frame.cursor_y < size.second));
      }

      EXPECT_TRUE(ed.doc.view.top_line >= 0);
      EXPECT_TRUE(ed.doc.view.top_line < LineCount(ed.doc.table));
    }

    EXPECT_EQ(LineAt(ed.doc.table, Cur(ed)), Index{120});
  }
}

void ControlBytesNeverReachTheTerminal() {
  TEST_CASE("render: control bytes are pictures, never escape sequences");

  // koi validates UTF-8 and never printability, and 0x1B is a well-formed
  // scalar. Present() hands a cell's text to tb_set_cell as a codepoint, so an
  // unfiltered ESC here is an escape sequence the terminal obeys.
  EXPECT_TRUE(PrintableCluster("\x1b") != std::string_view{"\x1b"});
  EXPECT_TRUE(PrintableCluster("\x07") != std::string_view{"\x07"});
  EXPECT_TRUE(PrintableCluster("\x7f") != std::string_view{"\x7f"});
  EXPECT_EQ(std::string{PrintableCluster("a")}, std::string("a"));
  EXPECT_EQ(std::string{PrintableCluster("é")}, std::string("é"));
  EXPECT_EQ(std::string{PrintableCluster("\xc2\x9b")}, std::string("\xef\xbf\xbd"));  // C1 CSI

  {
    std::string out;
    AppendPrintable(out, "a\x1b]0;t\x07\x1b[2Jb\rc\xc2\x9b" "d");
    EXPECT_TRUE(out.find('\x1b') == std::string::npos);
    EXPECT_TRUE(out.find('\x07') == std::string::npos);
    EXPECT_TRUE(out.find('\r') == std::string::npos);
    EXPECT_TRUE(out.find("\xc2\x9b") == std::string::npos);
    EXPECT_TRUE(out.find('a') != std::string::npos);
    EXPECT_TRUE(out.find('d') != std::string::npos);
  }
  {
    // Tab survives the bulk path: it is meaningful indentation in a rendered
    // line, and a terminal treats it as a move within the line.
    std::string out;
    AppendPrintable(out, "a\tb");
    EXPECT_EQ(out, std::string("a\tb"));
  }
  {
    // The promise above is unconditional, so it has to hold for a caller that
    // feeds the text in chunks too -- cli.cpp does, one style run at a time.
    // 0xC2 ending one chunk and 0x9B starting the next is U+009B, an eight-bit
    // CSI, and a filter that only ever looks inside one call sees two innocent
    // bytes and lets the control through whole.
    const std::string whole = "a\xc2\x9b" "[1mZ";
    const std::string first = whole.substr(0, 2);   // "a" and the lead byte
    const std::string second = whole.substr(2);     // the trailing byte, then text

    std::string once;
    EXPECT_EQ(AppendPrintable(once, whole), std::size_t{0});
    EXPECT_EQ(once, std::string("a\xef\xbf\xbd") + "[1mZ");

    // The protocol: `more` says another chunk follows, the count comes back,
    // and those bytes -- unwritten -- go in front of what is passed next. The
    // result is the same bytes as the single call.
    std::string chunked;
    const std::size_t held = AppendPrintable(chunked, first, /*more=*/true);
    EXPECT_EQ(held, std::size_t{1});
    EXPECT_EQ(chunked, std::string("a"));
    EXPECT_EQ(AppendPrintable(chunked, first.substr(first.size() - held) + second),
              std::size_t{0});
    EXPECT_EQ(chunked, once);
    EXPECT_TRUE(chunked.find("\xc2\x9b") == std::string::npos);

    // And a caller that ignores the protocol still cannot get a C1 out: the
    // 0x9B left alone at the head of the second call is that control all by
    // itself on a terminal in 8-bit mode, so it is substituted as one.
    std::string halves;
    EXPECT_EQ(AppendPrintable(halves, first), std::size_t{0});
    EXPECT_EQ(halves, std::string("a\xef\xbf\xbd"));  // the stump, not dropped
    EXPECT_EQ(AppendPrintable(halves, second), std::size_t{0});
    EXPECT_TRUE(halves.find('\x9b') == std::string::npos);
    EXPECT_EQ(halves, std::string("a\xef\xbf\xbd\xef\xbf\xbd") + "[1mZ");
  }
  {
    // End of stream with a codepoint still half-arrived: the last chunk is
    // passed with `more` false, and the stump becomes U+FFFD rather than being
    // held back for a call that never comes.
    std::string flushed;
    EXPECT_EQ(AppendPrintable(flushed, "x\xe2\x82", /*more=*/true), std::size_t{2});
    EXPECT_EQ(flushed, std::string("x"));
    EXPECT_EQ(AppendPrintable(flushed, "\xe2\x82", /*more=*/false), std::size_t{0});
    EXPECT_EQ(flushed, std::string("x\xef\xbf\xbd"));

    // A continuation byte inside a sequence is not a loose C1 half: 0x82 here
    // belongs to U+20AC, and nothing about this text changes.
    std::string euro;
    EXPECT_EQ(AppendPrintable(euro, "\xe2\x82\xac"), std::size_t{0});
    EXPECT_EQ(euro, std::string("\xe2\x82\xac"));

    // Whole text in one call: nothing is ever withheld, whatever it ends with.
    std::string plain;
    EXPECT_EQ(AppendPrintable(plain, "int main() { return 0; }"), std::size_t{0});
    EXPECT_EQ(plain, std::string("int main() { return 0; }"));
  }

  // End to end, through the real render path.
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table,
                    "harmless\n\x1b]0;PWNED\x07\x1b[2J\x1b]52;c;cHduZWQ=\x07tail\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    Surface surf;
    FitFocusedViewport(ed, 80, 12);
    RenderTo(ed, surf, 80, 12);
    int control_cells = 0;
    for (int y = 0; y < surf.height; ++y) {
      for (int x = 0; x < surf.width; ++x) {
        const std::string& cell = surf.At(x, y).text;
        if (cell.empty()) continue;
        const auto c = static_cast<unsigned char>(cell[0]);
        if ((c < 0x20) || (c == 0x7F)) ++control_cells;
      }
    }
    EXPECT_EQ(control_cells, 0);
  }

  // A control still occupies its column, so the picture is visible rather than
  // silently swallowed -- but tab keeps its zero, because its width is
  // positional and every layout caller computes it from the column instead.
  EXPECT_EQ(GraphemeWidth("\x1b"), 1);
  EXPECT_EQ(GraphemeWidth("\t"), 0);
}

}  // namespace koi
