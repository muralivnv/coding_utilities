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
