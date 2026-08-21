// Tests for the window and pane layer: editor.cpp holds the tree, render.cpp
// draws it, and commands.cpp splits and closes it -- so splits, resizes, mouse
// hits and pane invariants are tested here rather than under any one of them.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

std::string g_live_document;

void WatchLiveDocument(Editor& ed) {
  ed.live_document_changed = [](Editor& e) { g_live_document = e.doc.file.string(); };
  g_live_document = ed.doc.file.string();
}

}  // namespace

void AdversarialWindows(Rng& rng) {
  TEST_CASE("adversarial: closing a buffer keeps every window pointing at the right file");
  {
    const Scratch scratch{"koi-adv-buffers"};
    const std::filesystem::path a = scratch.Write("pa.txt", "aaa\n");
    const std::filesystem::path b = scratch.Write("pb.txt", "bbb\n");
    const std::filesystem::path c = scratch.Write("pc.txt", "ccc\n");

    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    EXPECT_TRUE(OpenTarget(ed, c.string()));
    SplitWindow(ed, true);

    RunTypableCommand(ed, "bp");
    RunTypableCommand(ed, "bp");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"pa.txt"});
    RunTypableCommand(ed, "bc!");
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunCommands(ed, {"jump_view_next"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"pc.txt"});

    RunTypableCommand(ed, "bc!");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    EXPECT_TRUE(!ed.doc.file.empty() || (DocLength(ed.doc.table) == 0));
    RunTypableCommand(ed, "bc!");
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("adversarial: splitting stops at the floor rather than at exhaustion");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    ApplyModeInvariants(ed);

    for (int i = 0; i < 4000; ++i) SplitWindow(ed, (i % 2) == 0);
    EXPECT_TRUE(WindowCount(ed) > std::size_t{1});
    EXPECT_TRUE(ed.status.find("not enough room") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    for (const Rect& r : LayoutWindows(ed, Rect{0, 0, ed.screen_w, ed.screen_h})) {
      EXPECT_TRUE(r.w >= kMinPaneWidth);
      EXPECT_TRUE(r.h >= kMinPaneHeight);
    }
    RunCommands(ed, {"wonly"});
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("windows: wonly keeps the pane it was typed in, whatever the tree looks like");
  {
    Surface frame;
    constexpr int kW = 120;
    constexpr int kH = 30;
    for (int panes = 2; panes <= 5; ++panes) {
      for (int start = 0; start < panes; ++start) {
        Editor ed;
        ResetToOriginal(ed.doc.table, "l0\nl1\nl2\nl3\nl4\nl5\n");
        ApplyModeInvariants(ed);
        ed.screen_w = kW;
        ed.screen_h = kH;
        const auto draw = [&] {
          FitFocusedViewport(ed, kW, kH);
          RenderTo(ed, frame, kW, kH);
        };
        draw();
        for (int i = 1; i < panes; ++i) {
          SplitWindow(ed, true);
          draw();
        }
        for (int i = 0; i < panes; ++i) {
          const std::vector<int> order = WindowOrder(ed);
          while (ed.focused != order[static_cast<std::size_t>(i)]) {
            RunCommands(ed, {"jump_view_next"});
          }
          const Index at = LineStart(ed.doc.table, i);
          ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
          draw();
        }
        const std::vector<int> order = WindowOrder(ed);
        while (ed.focused != order[static_cast<std::size_t>(start)]) {
          RunCommands(ed, {"jump_view_next"});
          draw();
        }
        RunTypableCommand(ed, "wonly");
        draw();
        EXPECT_EQ(WindowCount(ed), std::size_t{1});
        EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary())),
                  static_cast<Index>(start));
        EXPECT_EQ(EditorInvariants(ed), std::string{});
      }
    }
  }

  TEST_CASE("adversarial: a deep tree does not run out of stack");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    ApplyModeInvariants(ed);

    constexpr int kLeaves = 4001;
    ed.windows.assign(static_cast<std::size_t>((2 * kLeaves) - 1), WindowNode{});
    for (int k = 0; (k + 1) < kLeaves; ++k) {
      WindowNode& node = ed.windows[static_cast<std::size_t>(2 * k)];
      node.kind = ((k % 2) == 0) ? WindowNode::Kind::kRow : WindowNode::Kind::kColumn;
      node.first = (2 * k) + 1;
      node.second = (2 * k) + 2;
      node.ratio = 0.5;
    }
    ed.focused = (2 * kLeaves) - 2;

    EXPECT_EQ(WindowCount(ed), std::size_t{kLeaves});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    RunCommands(ed, {"wonly"});
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("adversarial: everything at once, with the invariants checked each step");
  {
    const Scratch scratch{"koi-adv-fuzz"};
    std::vector<std::string> files;
    for (int i = 0; i < 3; ++i) {
      files.push_back(
          scratch.Write("f" + std::to_string(i) + ".txt", "alpha\nbravo\ncharlie\ndelta\n")
              .string());
    }

    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, files[0]));
    std::vector<std::string> history;

    static constexpr std::array<std::string_view, 16> kSteps{
        "hsplit",           "vsplit",          "wclose",           "transpose_view",
        "jump_view_left",   "jump_view_right", "jump_view_up",     "jump_view_down",
        "swap_view_left",   "swap_view_right", "swap_view_up",     "swap_view_down",
        "jump_view_next",   "undo",            "move_line_down",   "goto_file_start"};

    for (int step = 0; step < 3000; ++step) {
      const Index pick = rng.Pick(0, 21);
      std::string what;
      if (pick < static_cast<Index>(kSteps.size())) {
        what = std::string{kSteps[static_cast<std::size_t>(pick)]};
        RunCommands(ed, {what});
      } else if (pick == 16) {
        what = "open";
        std::ignore = OpenTarget(ed, files[static_cast<std::size_t>(rng.Pick(0, 2))]);
      } else if (pick == 17) {
        what = "bc!";
        RunTypableCommand(ed, "bc!");
      } else if (pick == 18) {
        what = "bn";
        RunTypableCommand(ed, "bn");
      } else if (pick == 19) {
        what = "new";
        RunTypableCommand(ed, "new");
      } else if (pick == 20) {
        what = "type";
        TypeInto(ed, static_cast<char>('a' + rng.Pick(0, 25)));
      } else {
        what = "wonly";
        RunTypableCommand(ed, "wonly");
      }

      history.push_back(what);
      if (history.size() > 8) history.erase(history.begin());
      const std::string broke = EditorInvariants(ed);
      if (!broke.empty()) {
        std::cerr << "      after step " << step << " (" << what << "): " << broke << "\n";
        std::cerr << "      leading up to it:";
        for (const std::string& h : history) std::cerr << " " << h;
        std::cerr << "\n";
      }
      EXPECT_EQ(broke, std::string{});
      if (!broke.empty()) break;
    }
  }
}

namespace {

std::string PaneRow(const Surface& frame, const Rect& area, int row) {
  std::string out;
  for (int x = area.x; x < (area.x + area.w); ++x) {
    if (!frame.Holds(x, area.y + row)) continue;
    const std::string& text = frame.At(x, area.y + row).text;
    out += text.empty() ? " " : text;
  }
  return out;
}

int ColourDrift(const Surface& got, const Rect& area, const Surface& want, int rows,
                int from = 0) {
  int wrong = 0;
  for (int y = from; y < rows; ++y) {
    for (int x = 0; x < area.w; ++x) {
      if (!got.Holds(area.x + x, area.y + y) || !want.Holds(x, y)) continue;
      if (got.At(area.x + x, area.y + y).fg != want.At(x, y).fg) ++wrong;
      if (got.At(area.x + x, area.y + y).bg != want.At(x, y).bg) ++wrong;
    }
  }
  return wrong;
}

Surface DrawAlone(const std::filesystem::path& path, int w, int h) {
  Editor solo;
  solo.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(solo, path.string()));
  Surface frame;
  FitFocusedViewport(solo, w, h);
  RenderTo(solo, frame, w, h);
  return frame;
}

int NeighbourOf(const std::vector<Rect>& areas, std::size_t from, WindowDir dir) {
  const auto span = [](int a0, int a1, int b0, int b1) {
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
  };
  const Rect& me = areas[from];
  int best = -1;
  int best_gap = 0;
  int best_overlap = 0;
  for (std::size_t i = 0; i < areas.size(); ++i) {
    if (i == from) continue;
    const Rect& other = areas[i];
    int gap = 0;
    int overlap = 0;
    switch (dir) {
      case WindowDir::kRight:
        if (other.x < (me.x + me.w)) continue;
        gap = other.x - (me.x + me.w);
        overlap = span(me.y, me.y + me.h, other.y, other.y + other.h);
        break;
      case WindowDir::kLeft:
        if ((other.x + other.w) > me.x) continue;
        gap = me.x - (other.x + other.w);
        overlap = span(me.y, me.y + me.h, other.y, other.y + other.h);
        break;
      case WindowDir::kDown:
        if (other.y < (me.y + me.h)) continue;
        gap = other.y - (me.y + me.h);
        overlap = span(me.x, me.x + me.w, other.x, other.x + other.w);
        break;
      case WindowDir::kUp:
        if ((other.y + other.h) > me.y) continue;
        gap = me.y - (other.y + other.h);
        overlap = span(me.x, me.x + me.w, other.x, other.x + other.w);
        break;
    }
    if (overlap <= 0) continue;
    if ((best < 0) || (gap < best_gap) || ((gap == best_gap) && (overlap > best_overlap))) {
      best = static_cast<int>(i);
      best_gap = gap;
      best_overlap = overlap;
    }
  }
  return best;
}

}  // namespace

void SplitsAndHighlighting() {
  const Scratch scratch{"koi-split-highlight"};
  const std::filesystem::path cpp = scratch.Write(
      "a.cpp", "int alpha() {\n  // one\n  return 1;\n}\nint beta() {\n  return 2;\n}\n");
  const std::filesystem::path py = scratch.Write(
      "b.py", "def gamma():\n    # one\n    return 3\n\ndef delta():\n    return 4\n");

  constexpr int kW = 60;
  constexpr int kPane = 8;
  constexpr int kH = 2 * kPane;

  const Surface alone_cpp = DrawAlone(cpp, kW, kPane);
  const Surface alone_py = DrawAlone(py, kW, kPane);

  TEST_CASE("windows: each half of a split is painted with its own grammar");

  for (const bool open_below : {true, false}) {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, cpp.string()));
    SplitWindow(ed, false);
    if (!open_below) JumpWindow(ed, WindowDir::kUp);
    EXPECT_TRUE(OpenTarget(ed, py.string()));

    Surface frame;
    FitFocusedViewport(ed, kW, kH);
    RenderTo(ed, frame, kW, kH);

    const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, kW, kH});
    EXPECT_EQ(areas.size(), std::size_t{2});
    if (areas.size() != 2) continue;

    const Rect& has_py = open_below ? areas[1] : areas[0];
    const Rect& has_cpp = open_below ? areas[0] : areas[1];
    EXPECT_TRUE(PaneRow(frame, has_py, 0).find("def gamma") != std::string::npos);
    EXPECT_TRUE(PaneRow(frame, has_cpp, 0).find("int alpha") != std::string::npos);

    EXPECT_EQ(ColourDrift(frame, PaneContent(ed, has_cpp, kW), alone_cpp, kPane - 1, 1), 0);
    EXPECT_EQ(ColourDrift(frame, PaneContent(ed, has_py, kW), alone_py, kPane - 1, 1), 0);
  }

  TEST_CASE("windows: stepping to a buffer in another language repaints it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, cpp.string()));
    EXPECT_TRUE(OpenTarget(ed, py.string()));

    RunTypableCommand(ed, "bn");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.cpp"});
    Surface frame;
    FitFocusedViewport(ed, kW, kPane);
    RenderTo(ed, frame, kW, kPane);
    EXPECT_EQ(ColourDrift(frame, Rect{0, 0, kW, kPane}, alone_cpp, kPane - 1), 0);
  }

  TEST_CASE("windows: a theme change reaches the buffers that are not in front");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, cpp.string()));
    EXPECT_TRUE(OpenTarget(ed, py.string()));

    std::ignore = ApplyTheme(ed, ed.settings.theme);
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      const Document& doc = BufferAt(ed, i);
      if (doc.syntax == nullptr) continue;
      EXPECT_EQ(doc.capture_styles.size(), doc.syntax->CaptureNames().size());
    }
  }

  TEST_CASE("windows: installing a theme by hand still resolves every buffer");
  {

    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, cpp.string()));
    EXPECT_TRUE(OpenTarget(ed, py.string()));
    for (std::size_t i = 0; i < ed.buffers.size(); ++i) {
      if (i != ed.active) ed.buffers[i].capture_styles.clear();
    }
    ed.doc.capture_styles.clear();

    ed.theme = BuiltinTheme();
    RefreshCaptureStyles(ed);
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      const Document& doc = BufferAt(ed, i);
      if (doc.syntax == nullptr) continue;
      EXPECT_EQ(doc.capture_styles.size(), doc.syntax->CaptureNames().size());
    }

    RunTypableCommand(ed, "bn");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.cpp"});
    Surface frame;
    FitFocusedViewport(ed, kW, kPane);
    RenderTo(ed, frame, kW, kPane);
    EXPECT_EQ(ColourDrift(frame, Rect{0, 0, kW, kPane}, alone_cpp, kPane - 1), 0);
  }

  TEST_CASE("windows: the editor's message goes on the window that ran the command");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, cpp.string()));
    SplitWindow(ed, false);
    JumpWindow(ed, WindowDir::kDown);
    EXPECT_TRUE(OpenTarget(ed, py.string()));
    ed.status = "probe";

    Surface frame;
    FitFocusedViewport(ed, kW, kH);
    RenderTo(ed, frame, kW, kH);
    const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, kW, kH});
    EXPECT_EQ(areas.size(), std::size_t{2});
    if (areas.size() == 2) {

      const std::string above = PaneRow(frame, areas[0], areas[0].h - 1);
      const std::string below = PaneRow(frame, areas[1], areas[1].h - 1);
      EXPECT_TRUE(above.find("probe") == std::string::npos);
      EXPECT_TRUE(below.find("probe") != std::string::npos);
      EXPECT_TRUE(above.find("a.cpp") != std::string::npos);
    }
  }

  TEST_CASE("windows: opening a file into a split scratch buffer leaves the other half");
  {

    Editor ed;
    ed.theme = BuiltinTheme();
    ResetToOriginal(ed.doc.table, "");
    ed.doc.selections.Set(Selection{0, 0, -1});
    SplitWindow(ed, false);
    JumpWindow(ed, WindowDir::kDown);
    EXPECT_TRUE(OpenTarget(ed, py.string()));
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    const std::vector<int> order = WindowOrder(ed);
    EXPECT_EQ(order.size(), std::size_t{2});
    if (order.size() == 2) {
      EXPECT_EQ(ed.focused, order[1]);
      EXPECT_EQ(ed.doc.file.filename().string(), std::string{"b.py"});
      EXPECT_TRUE(BufferAt(ed, ed.windows[static_cast<std::size_t>(order[0])].buffer)
                      .file.empty());
    }

    Surface frame;
    FitFocusedViewport(ed, kW, kH);
    RenderTo(ed, frame, kW, kH);
    const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, kW, kH});
    if (areas.size() == 2) {
      EXPECT_TRUE(PaneRow(frame, areas[0], 0).find("def gamma") == std::string::npos);
      EXPECT_TRUE(PaneRow(frame, areas[1], 0).find("def gamma") != std::string::npos);
    }

    EXPECT_EQ(BufferCount(ed), std::size_t{2});
  }

  TEST_CASE("windows: an editor that never split still shows its own message");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, cpp.string()));
    ed.status = "probe";
    Surface frame;
    FitFocusedViewport(ed, kW, kPane);
    RenderTo(ed, frame, kW, kPane);
    EXPECT_TRUE(frame.Row(kPane - 1).find("probe") != std::string::npos);
  }
}

void LiveViewsAndPanesFuzz(Rng& rng) {
  TEST_CASE("adversarial: live views, resizable panes and excerpt saves at once");
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-live-fuzz"};
  std::vector<std::string> files;
  for (int i = 0; i < 4; ++i) {
    files.push_back(
        scratch.Write("z" + std::to_string(i) + ".txt", "a1\na2\na3\na4\na5\na6\na7\na8\n")
            .string());
  }

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.excerpt_context = 1;
  ed.screen_w = 100;
  ed.screen_h = 30;
  std::string db_error;
  ed.project = ProjectStore::Open(scratch.dir / "fuzz.db", db_error);
  EXPECT_TRUE(ed.project != nullptr);
  EXPECT_TRUE(OpenTarget(ed, files[0]));

  static constexpr std::array<std::string_view, 4> kResizes{"expand_width", "shrink_width",
                                                            "expand_height", "shrink_height"};
  std::vector<std::string> history;
  for (int step = 0; step < 900; ++step) {
    std::string what;
    switch (rng.Pick(0, 23)) {
      case 18:
        // Rebuilds a live view in place, which is what pushes an epoch
        // boundary. Undo and redo then cross it, which is the intersection
        // findings 9 and 13 lived in and which nothing drove before.
        what = "excerpt-context";
        RunTypableCommand(ed, "set-excerpt-context " + std::to_string(rng.Pick(0, 3)));
        break;
      case 19:
        what = "wonly";
        RunTypableCommand(ed, "wonly");
        break;
      case 20:
        what = "pipe";
        RunTypableCommand(ed, (rng.Pick(0, 1) == 0) ? "| tr a-z A-Z" : "! echo koi");
        break;
      case 21:
        what = "select-all";
        RunCommands(ed, {(rng.Pick(0, 1) == 0) ? "select_all" : "collapse_selection"});
        break;
      case 22:
        what = "delete";
        RunCommands(ed, {"delete_selection"});
        break;
      case 0:
        what = "pin";
        RunTypableCommand(ed, "pin " + std::to_string(rng.Pick(1, 4)));
        break;
      case 1:
        what = "clear-pin";
        RunTypableCommand(ed, "clear-pin " + std::to_string(rng.Pick(1, 4)));
        break;
      case 2:
        what = "pins-excerpt";
        RunTypableCommand(ed, "pins-excerpt");
        break;
      case 3:
        // Moves a pin behind the view's back, which is what leaves a live pins
        // view stale for case 9 to rebuild.
        what = "record-edit";
        RecordEditHere(ed);
        break;
      case 4:
        what = "open";
        std::ignore = OpenTarget(ed, files[static_cast<std::size_t>(rng.Pick(0, 3))]);
        break;
      case 5:
        what = "split";
        SplitWindow(ed, rng.Pick(0, 1) == 0);
        break;
      case 6:
        what = "wclose";
        CloseWindow(ed);
        break;
      case 7:
        what = "resize";
        RunCommands(ed, {std::string{kResizes[static_cast<std::size_t>(rng.Pick(0, 3))]}});
        break;
      case 8: {
        what = "drag";
        const int x = static_cast<int>(rng.Pick(-5, 105));
        const int y = static_cast<int>(rng.Pick(-5, 35));
        if (const int node = DividerAt(ed, x, y, PaneArea(ed)); node >= 0) {
          MoveDivider(ed, node, static_cast<int>(rng.Pick(-5, 105)),
                      static_cast<int>(rng.Pick(-5, 35)), PaneArea(ed));
        }
        break;
      }
      case 9:
        what = "refresh-live";
        RefreshLiveExcerptViews(ed);
        break;
      case 10:
        what = "refresh-focus";
        MaybeRefreshExcerptView(ed);
        break;
      case 11:
        what = "jump_view";
        RunCommands(ed, {"jump_view_next"});
        break;
      case 12:
        what = "bn";
        RunTypableCommand(ed, "bn");
        break;
      case 13:
        what = "type";
        TypeInto(ed, static_cast<char>('a' + rng.Pick(0, 25)));
        break;
      case 14:
        what = "undo";
        RunCommands(ed, {"undo"});
        break;
      case 15:
        what = "redo";
        RunCommands(ed, {"redo"});
        break;
      case 16:
        what = "write";
        RunTypableCommand(ed, "w");
        break;
      case 17:
        what = "bc!";
        RunTypableCommand(ed, "bc!");
        break;
      default:
        what = "draw";
        RefreshLiveExcerptViews(ed);
        FitFocusedViewport(ed, ed.screen_w, ed.screen_h);
        Surface frame;
        RenderTo(ed, frame, ed.screen_w, ed.screen_h);
        for (int y = 0; y < frame.height; ++y) {
          for (int x = 0; x < frame.width; ++x) {
            if (!frame.At(x, y).text.empty()) continue;
            ++common::g_test_failures;
            std::cerr << "FAIL [" << common::g_test_case << "] unpainted cell after step " << step
                      << "\n";
            y = frame.height;
            break;
          }
        }
        break;
    }

    history.push_back(what);
    if (history.size() > 8) history.erase(history.begin());
    const std::string broke = EditorInvariants(ed);
    if (!broke.empty()) {
      std::cerr << "      after step " << step << " (" << what << "): " << broke << "\n";
      std::cerr << "      leading up to it:";
      for (const std::string& one : history) std::cerr << " " << one;
      std::cerr << "\n";
    }
    EXPECT_EQ(broke, std::string{});
    if (!broke.empty()) break;
  }
}

void AdversarialWindowContents(Rng& rng) {
  TEST_CASE("adversarial: every pane holds what the operations say it should");
  const Scratch scratch{"koi-window-model"};
  std::vector<std::string> files;
  std::vector<std::string> marks;
  int nth = 0;
  for (const std::string_view name : {"one.cpp", "two.py", "three.rs", "four.json"}) {
    const std::string mark(3, static_cast<char>('A' + nth++));
    marks.push_back(mark);
    files.push_back(scratch.Write(name, mark + "\n" + mark + "\n" + mark + "\n").string());
  }
  const auto mark_of = [&](const std::string& path) {
    for (std::size_t f = 0; f < files.size(); ++f) {
      if (files[f] == path) return marks[f];
    }
    return std::string{};
  };

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, files[0]));

  std::vector<std::string> panes{marks[0]};
  std::size_t focus = 0;

  constexpr Rect kNominal{0, 0, 4096, 4096};
  ed.screen_w = kNominal.w;
  ed.screen_h = kNominal.h;
  static constexpr std::array<WindowDir, 4> kDirs{WindowDir::kLeft, WindowDir::kRight,
                                                  WindowDir::kUp, WindowDir::kDown};
  static constexpr std::array<std::string_view, 4> kJumps{"jump_view_left", "jump_view_right",
                                                          "jump_view_up", "jump_view_down"};
  static constexpr std::array<std::string_view, 4> kSwaps{"swap_view_left", "swap_view_right",
                                                          "swap_view_up", "swap_view_down"};

  for (int step = 0; step < 4000; ++step) {
    const std::vector<Rect> before = LayoutWindows(ed, kNominal);
    const bool aligned = (before.size() == panes.size());
    const Index pick = rng.Pick(0, 13);
    std::string what;

    if (pick <= 1) {
      what = (pick == 0) ? "hsplit" : "vsplit";
      const std::size_t was = WindowCount(ed);
      RunCommands(ed, {what});
      if (WindowCount(ed) > was) {
        panes.insert(panes.begin() + static_cast<std::ptrdiff_t>(focus) + 1, panes[focus]);
        ++focus;
      }
    } else if (pick == 2) {
      what = "jump_view_next";
      RunCommands(ed, {what});
      if (panes.size() > 1) focus = (focus + 1) % panes.size();
    } else if (pick == 3) {
      what = "jump_view_previous";
      RunCommands(ed, {what});
      if (panes.size() > 1) focus = (focus + panes.size() - 1) % panes.size();
    } else if (pick <= 7) {
      const std::size_t d = static_cast<std::size_t>(pick - 4);
      what = std::string{kJumps[d]};
      const int to = aligned ? NeighbourOf(before, focus, kDirs[d]) : -1;
      RunCommands(ed, {what});
      if (to >= 0) focus = static_cast<std::size_t>(to);
    } else if (pick <= 11) {
      const std::size_t d = static_cast<std::size_t>(pick - 8);
      what = std::string{kSwaps[d]};
      const int to = aligned ? NeighbourOf(before, focus, kDirs[d]) : -1;
      RunCommands(ed, {what});

      if (to >= 0) {
        std::swap(panes[focus], panes[static_cast<std::size_t>(to)]);
        focus = static_cast<std::size_t>(to);
      }
    } else {
      what = "open";
      const std::size_t f = static_cast<std::size_t>(rng.Pick(0, 3));
      if (OpenTarget(ed, files[f])) panes[focus] = marks[f];
    }

    const std::string broke = EditorInvariants(ed);
    if (!broke.empty()) {
      std::cerr << "      after step " << step << " (" << what << "): " << broke << "\n";
      EXPECT_EQ(broke, std::string{});
      break;
    }

    const std::vector<int> order = WindowOrder(ed);
    std::vector<std::string> got;
    std::size_t saw = 0;
    if (order.empty()) got.push_back(mark_of(ed.doc.file.string()));
    for (std::size_t i = 0; i < order.size(); ++i) {

      const std::size_t buffer = (order[i] == ed.focused)
                                     ? ed.active
                                     : ed.windows[static_cast<std::size_t>(order[i])].buffer;
      got.push_back(buffer < BufferCount(ed) ? mark_of(BufferAt(ed, buffer).file.string())
                                             : std::string{});
      if (order[i] == ed.focused) saw = i;
    }

    const auto joined = [](const std::vector<std::string>& of) {
      std::string out;
      for (const std::string& one : of) out += one + " ";
      return out;
    };
    if ((got != panes) || (saw != focus)) {
      std::cerr << "      after step " << step << " (" << what << ")\n";
    }
    EXPECT_EQ(joined(got), joined(panes));
    EXPECT_EQ(saw, focus);
    if ((got != panes) || (saw != focus)) break;
  }
}

void RenderLeavesWindowsAlone(Rng& rng) {
  TEST_CASE("adversarial: drawing a split does not move the editor");
  const Scratch scratch{"koi-render-observer"};
  std::vector<std::string> files;
  for (const std::string_view name : {"r0.cpp", "r1.py", "r2.rs"}) {
    files.push_back(scratch.Write(name, "alpha\nbravo\ncharlie\ndelta\necho\nfoxtrot\n").string());
  }

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.scrolloff = 4;

  WatchLiveDocument(ed);
  EXPECT_TRUE(OpenTarget(ed, files[0]));

  static constexpr std::array<std::string_view, 10> kSteps{
      "hsplit",         "vsplit",          "wclose",       "transpose_view",
      "jump_view_left", "jump_view_right", "jump_view_up", "jump_view_down",
      "jump_view_next", "swap_view_down"};

  for (int step = 0; step < 1200; ++step) {
    const Index pick = rng.Pick(0, 12);
    if (pick < static_cast<Index>(kSteps.size())) {
      RunCommands(ed, {std::string{kSteps[static_cast<std::size_t>(pick)]}});
    } else if (pick == 10) {
      std::ignore = OpenTarget(ed, files[static_cast<std::size_t>(rng.Pick(0, 2))]);
    } else {
      RunTypableCommand(ed, "bn");
    }

    const int w = static_cast<int>(rng.Pick(4, 90));
    const int h = static_cast<int>(rng.Pick(2, 40));
    FitFocusedViewport(ed, w, h);

    const int was_focused = ed.focused;
    const std::size_t was_active = ed.active;
    const std::string was_path = ed.doc.file.string();
    const std::vector<Selection> was_sel = ed.doc.selections.Ranges();
    const Index was_top = ed.doc.view.top_line;

    const auto shown = [](const Editor& e) {
      std::string out;
      for (const int leaf : WindowOrder(e)) {
        const std::size_t buffer = (leaf == e.focused)
                                       ? e.active
                                       : e.windows[static_cast<std::size_t>(leaf)].buffer;
        out += std::to_string(buffer) + " ";
      }
      return out;
    };
    const std::string was_buffers = shown(ed);

    Surface frame;
    RenderTo(ed, frame, w, h);

    EXPECT_EQ(ed.focused, was_focused);
    EXPECT_EQ(ed.active, was_active);
    EXPECT_EQ(ed.doc.file.string(), was_path);
    EXPECT_EQ(ed.doc.selections.Ranges().size(), was_sel.size());
    EXPECT_EQ(ed.doc.view.top_line, was_top);
    EXPECT_EQ(shown(ed), was_buffers);
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    EXPECT_EQ(g_live_document, ed.doc.file.string());

    FitFocusedViewport(ed, w, h);
    EXPECT_EQ(ed.doc.view.scrolloff, ed.settings.scrolloff);
  }
}

void ClickingChoosesTheWindowUnderIt() {
  TEST_CASE("windows: a click lands in the window drawn under it");
  const Scratch scratch{"koi-window-click"};
  const std::filesystem::path cpp =
      scratch.Write("c.cpp", "int alpha() {\n  return 1;\n}\n");
  const std::filesystem::path py = scratch.Write("d.py", "def gamma():\n    return 3\n");

  constexpr int kW = 60;
  constexpr int kH = 24;

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, cpp.string()));
  SplitWindow(ed, false);

  const std::vector<int> order = WindowOrder(ed);
  const std::vector<Rect> areas = LayoutWindows(ed, Rect{0, 0, kW, kH});
  EXPECT_EQ(order.size(), std::size_t{2});
  if (order.size() != 2) return;
  EXPECT_EQ(ed.focused, order[1]);

  for (std::size_t i = 0; i < areas.size(); ++i) {
    for (int y = areas[i].y; y < (areas[i].y + areas[i].h); ++y) {
      Rect got{};
      EXPECT_EQ(WindowAtPoint(ed, 10, y, kW, kH, got), order[i]);
    }
  }

  Rect area{};
  const int under = WindowAtPoint(ed, 10, areas[1].y + 2, kW, kH, area);
  FocusWindowAt(ed, under);
  EXPECT_EQ(ed.focused, order[1]);

  EXPECT_TRUE(OpenTarget(ed, py.string()));
  Surface frame;
  FitFocusedViewport(ed, kW, kH);
  RenderTo(ed, frame, kW, kH);
  EXPECT_TRUE(PaneRow(frame, areas[0], 0).find("int alpha") != std::string::npos);
  EXPECT_TRUE(PaneRow(frame, areas[1], 0).find("def gamma") != std::string::npos);
  EXPECT_TRUE(PaneRow(frame, areas[0], 0).find("def gamma") == std::string::npos);
}

void Windows() {
  TEST_CASE("windows: an editor that never split has one window filling the screen");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
    const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
    EXPECT_EQ(laid.size(), std::size_t{1});
    if (laid.size() == 1) EXPECT_TRUE(laid[0] == (Rect{0, 0, 80, 24}));

    CloseWindow(ed);
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
  }

  TEST_CASE("windows: a horizontal split divides the width and covers it exactly");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    SplitWindow(ed, true);
    EXPECT_EQ(WindowCount(ed), std::size_t{2});
    const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
    EXPECT_EQ(laid.size(), std::size_t{2});
    if (laid.size() == 2) {
      EXPECT_EQ(laid[0].h, 24);
      EXPECT_EQ(laid[1].h, 24);
      EXPECT_EQ(laid[0].x, 0);

      EXPECT_EQ(laid[1].x, laid[0].x + laid[0].w);
      EXPECT_EQ(laid[0].w + laid[1].w, 80);
    }
  }

  TEST_CASE("windows: a vertical split divides the height");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    SplitWindow(ed, false);
    const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
    EXPECT_EQ(laid.size(), std::size_t{2});
    if (laid.size() == 2) {
      EXPECT_EQ(laid[0].w, 80);
      EXPECT_EQ(laid[1].w, 80);
      EXPECT_EQ(laid[1].y, laid[0].y + laid[0].h);
      EXPECT_EQ(laid[0].h + laid[1].h, 24);
    }
  }

  TEST_CASE("windows: nested splits still tile the screen exactly");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    SplitWindow(ed, true);
    SplitWindow(ed, false);
    FocusWindow(ed, true);
    SplitWindow(ed, false);
    EXPECT_EQ(WindowCount(ed), std::size_t{4});

    const Rect screen{0, 0, 100, 40};
    const std::vector<Rect> laid = LayoutWindows(ed, screen);
    EXPECT_EQ(laid.size(), std::size_t{4});

    std::vector<int> covered(static_cast<std::size_t>(screen.w * screen.h), 0);
    for (const Rect& r : laid) {
      EXPECT_TRUE(r.w >= 1);
      EXPECT_TRUE(r.h >= 1);
      for (int y = r.y; y < (r.y + r.h); ++y) {
        for (int x = r.x; x < (r.x + r.w); ++x) {
          EXPECT_TRUE((x >= 0) && (x < screen.w) && (y >= 0) && (y < screen.h));
          if ((x >= 0) && (x < screen.w) && (y >= 0) && (y < screen.h)) {
            ++covered[static_cast<std::size_t>((y * screen.w) + x)];
          }
        }
      }
    }
    bool exactly_once = true;
    for (const int n : covered) {
      if (n != 1) exactly_once = false;
    }
    EXPECT_TRUE(exactly_once);
  }

  TEST_CASE("windows: a tiny screen still gives every window a cell to draw in");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "x\n");
    SplitWindow(ed, true);
    SplitWindow(ed, true);

    long area = 0;
    int drawable = 0;
    for (const Rect& r : LayoutWindows(ed, Rect{0, 0, 3, 1})) {
      EXPECT_TRUE(r.w >= 0);
      EXPECT_TRUE(r.h >= 0);
      area += static_cast<long>(r.w) * r.h;
      if ((r.w > 0) && (r.h > 0)) ++drawable;
    }
    EXPECT_EQ(area, 3L);
    EXPECT_TRUE(drawable >= 1);
  }

  TEST_CASE("windows: two windows on one buffer keep their own cursor and scroll");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\ndelta\necho\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);

    SplitWindow(ed, true);

    EXPECT_EQ(Cur(ed), Index{0});

    RunCommands(ed, {"move_line_down", "move_line_down"});
    const Index moved = Cur(ed);
    EXPECT_TRUE(moved > 0);

    FocusWindow(ed, true);

    EXPECT_EQ(Cur(ed), Index{0});
    FocusWindow(ed, true);
    EXPECT_EQ(Cur(ed), moved);

    EXPECT_EQ(BufferCount(ed), std::size_t{1});
  }

  TEST_CASE("windows: rendering a split touches no focus, buffer or host state");
  {
    const Scratch scratch{"koi-render-readonly"};
    const std::filesystem::path a = scratch.Write("a.txt", "alpha-contents\n");
    const std::filesystem::path b = scratch.Write("b.txt", "bravo-contents\n");

    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    SplitWindow(ed, false);
    RunTypableCommand(ed, "bp");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});

    static int changed_calls;
    changed_calls = 0;
    ed.live_document_changed = [](Editor&) { ++changed_calls; };

    const int focused_before = ed.focused;
    const std::size_t active_before = ed.active;
    Surface frame;
    FitFocusedViewport(ed, 80, 24);
    RenderTo(ed, frame, 80, 24);

    EXPECT_EQ(changed_calls, 0);
    EXPECT_EQ(ed.focused, focused_before);
    EXPECT_EQ(ed.active, active_before);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"a.txt"});

    bool saw_a = false;
    bool saw_b = false;
    for (int y = 0; y < frame.height; ++y) {
      const std::string row = frame.Row(y);
      saw_a = saw_a || (row.find("alpha-contents") != std::string::npos);
      saw_b = saw_b || (row.find("bravo-contents") != std::string::npos);
    }
    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);
  }

  TEST_CASE("windows: an edit in one window shifts the other window's cursor with the text");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\ncharlie\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);

    SplitWindow(ed, true);
    RunCommands(ed, {"move_line_down", "move_line_down"});
    const Index parked = Cur(ed);
    const std::string under = ReadDocRange(ed.doc.table, Interval(parked, parked + 1));

    FocusWindow(ed, true);
    RunCommands(ed, {"goto_file_start"});
    TypeInto(ed, 'x');
    TypeInto(ed, 'y');
    TypeInto(ed, 'z');

    FocusWindow(ed, true);
    EXPECT_EQ(Cur(ed), parked + 3);
    EXPECT_EQ(ReadDocRange(ed.doc.table, Interval(Cur(ed), Cur(ed) + 1)), under);
  }

  TEST_CASE("windows: an edit in one window is in the other, because it is one buffer");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbravo\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    SplitWindow(ed, false);

    TypeInto(ed, 'Z');
    const std::string after = AssembleDocContents(ed.doc.table);
    EXPECT_TRUE(after.starts_with("Z"));

    FocusWindow(ed, true);

    EXPECT_EQ(AssembleDocContents(ed.doc.table), after);

    RunCommands(ed, {"undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha\nbravo\n"));
    FocusWindow(ed, true);
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("alpha\nbravo\n"));
  }

  TEST_CASE("windows: closing gives the space back and keeps the survivor whole");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    SplitWindow(ed, true);
    SplitWindow(ed, false);
    EXPECT_EQ(WindowCount(ed), std::size_t{3});
    CloseWindow(ed);
    EXPECT_EQ(WindowCount(ed), std::size_t{2});
    CloseWindow(ed);
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
    const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
    EXPECT_EQ(laid.size(), std::size_t{1});
    if (laid.size() == 1) EXPECT_TRUE(laid[0] == (Rect{0, 0, 80, 24}));
    CloseWindow(ed);
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
  }

  TEST_CASE("windows: the commands are wired to the tree");
  {
    const Scratch scratch{"koi-window-cmds"};
    const std::filesystem::path a = scratch.Write("wa.txt", "alpha\nbravo\ncharlie\n");
    const std::filesystem::path b = scratch.Write("wb.txt", "delta\n");

    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    RunCommands(ed, {"hsplit"});
    EXPECT_EQ(WindowCount(ed), std::size_t{2});
    RunCommands(ed, {"vsplit"});
    EXPECT_EQ(WindowCount(ed), std::size_t{3});
    RunCommands(ed, {"wclose"});
    EXPECT_EQ(WindowCount(ed), std::size_t{2});

    RunTypableCommand(ed, "vs");
    EXPECT_EQ(WindowCount(ed), std::size_t{3});
    RunTypableCommand(ed, "sp");
    EXPECT_EQ(WindowCount(ed), std::size_t{4});

    EXPECT_TRUE(OpenTarget(ed, b.string()));
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"wb.txt"});
    RunTypableCommand(ed, "wonly");
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"wb.txt"});
    ed.status.clear();
    RunTypableCommand(ed, "only");
    EXPECT_TRUE(ed.status.find("unknown command") != std::string::npos);

    for (int i = 0; i < 6; ++i) RunCommands(ed, {(i % 2) == 0 ? "hsplit" : "vsplit"});
    EXPECT_TRUE(WindowCount(ed) > 1);
    RunCommands(ed, {"wonly"});
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
  }

  TEST_CASE("windows: vsplit puts panes side by side, hsplit stacks them -- as helix");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\n");
    RunCommands(ed, {"vsplit"});
    std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 120, 50});
    EXPECT_EQ(laid.size(), std::size_t{2});
    EXPECT_EQ(laid[0].y, laid[1].y);
    EXPECT_TRUE(laid[0].x != laid[1].x);
    RunCommands(ed, {"wclose"});
    RunCommands(ed, {"hsplit"});
    laid = LayoutWindows(ed, Rect{0, 0, 120, 50});
    EXPECT_EQ(laid.size(), std::size_t{2});
    EXPECT_EQ(laid[0].x, laid[1].x);
    EXPECT_TRUE(laid[0].y != laid[1].y);
    RunCommands(ed, {"vsplit"});
    RunTypableCommand(ed, "wonly");
    EXPECT_TRUE(ed.status.find("closed 2 window(s)") != std::string::npos);
  }

  TEST_CASE(":bco! reports how many buffers it closed");
  {
    const Scratch scratch{"koi-bco-report"};
    const std::filesystem::path a = scratch.Write("ra.txt", "alpha\n");
    const std::filesystem::path b = scratch.Write("rb.txt", "bravo\n");
    const std::filesystem::path c = scratch.Write("rc.txt", "charlie\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    EXPECT_TRUE(OpenTarget(ed, c.string()));
    RunTypableCommand(ed, "bco!");
    EXPECT_EQ(BufferCount(ed), std::size_t{1});
    EXPECT_TRUE(ed.status.find("closed 2 buffer(s)") != std::string::npos);
  }

  TEST_CASE("pins say what happened: set, replace, clear, already empty");
  {
    const Scratch scratch{"koi-pin-report"};
    const std::filesystem::path a = scratch.Write("pf.txt", "one\ntwo\nthree\n");
    Editor ed;
    ed.theme = BuiltinTheme();
    std::string db_error;
    ed.project = ProjectStore::Open(scratch.dir / "pins.db", db_error);
    EXPECT_TRUE(ed.project != nullptr);
    EXPECT_TRUE(OpenTarget(ed, a.string()));

    RunTypableCommand(ed, "pin 3");
    EXPECT_TRUE(ed.status.find("pinned ") != std::string::npos);
    EXPECT_TRUE(ed.status.find(":1 to 3") != std::string::npos);
    RunCommands(ed, {"move_line_down"});
    RunTypableCommand(ed, "pin 3");
    EXPECT_TRUE(ed.status.find(":2 to 3") != std::string::npos);
    EXPECT_TRUE(ed.status.find("replaced the old pin") != std::string::npos);
    RunTypableCommand(ed, "clear-pin 3");
    EXPECT_EQ(ed.status.text(), std::string{"cleared pin 3"});
    RunTypableCommand(ed, "clear-pin 3");
    EXPECT_TRUE(ed.status.find("pin 3 was already empty") != std::string::npos);
  }

  TEST_CASE("windows: a split shows the buffer the focused window was showing");
  {
    const Scratch scratch{"koi-window-buffers"};
    const std::filesystem::path a = scratch.Write("qa.txt", "alpha\n");
    const std::filesystem::path b = scratch.Write("qb.txt", "bravo\n");
    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    EXPECT_EQ(BufferCount(ed), std::size_t{2});

    RunCommands(ed, {"hsplit"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"qb.txt"});

    RunTypableCommand(ed, "bp");
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"qa.txt"});
    RunCommands(ed, {"jump_view_next"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"qb.txt"});
    RunCommands(ed, {"jump_view_next"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"qa.txt"});
  }

  TEST_CASE("windows: jumping by direction lands where a person would point");
  {

    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    SplitWindow(ed, true);
    SplitWindow(ed, false);
    JumpWindow(ed, WindowDir::kUp);
    EXPECT_EQ(WindowCount(ed), std::size_t{3});

    const std::vector<int> order = WindowOrder(ed);
    EXPECT_EQ(order.size(), std::size_t{3});
    const int a = order[0];
    const int b = order[1];
    const int c = order[2];
    EXPECT_EQ(ed.focused, b);

    EXPECT_EQ(WindowToward(ed, WindowDir::kLeft), a);
    EXPECT_EQ(WindowToward(ed, WindowDir::kDown), c);
    EXPECT_EQ(WindowToward(ed, WindowDir::kUp), -1);
    EXPECT_EQ(WindowToward(ed, WindowDir::kRight), -1);

    RunCommands(ed, {"jump_view_down"});
    EXPECT_EQ(ed.focused, c);

    EXPECT_EQ(WindowToward(ed, WindowDir::kLeft), a);
    EXPECT_EQ(WindowToward(ed, WindowDir::kUp), b);
    EXPECT_EQ(WindowToward(ed, WindowDir::kDown), -1);

    RunCommands(ed, {"jump_view_left"});
    EXPECT_EQ(ed.focused, a);

    const int from_a = WindowToward(ed, WindowDir::kRight);
    EXPECT_TRUE((from_a == b) || (from_a == c));
    EXPECT_EQ(WindowToward(ed, WindowDir::kLeft), -1);

    const int stood = ed.focused;
    RunCommands(ed, {"jump_view_left"});
    EXPECT_EQ(ed.focused, stood);
  }

  TEST_CASE("windows: swapping moves the buffer and takes the cursor with it");
  {
    const Scratch scratch{"koi-window-swap"};
    const std::filesystem::path a = scratch.Write("sa.txt", "alpha\n");
    const std::filesystem::path b = scratch.Write("sb.txt", "bravo\n");

    Editor ed;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, b.string()));

    const std::vector<int> order = WindowOrder(ed);
    EXPECT_EQ(order.size(), std::size_t{2});
    EXPECT_EQ(ed.focused, order[1]);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"sb.txt"});

    RunCommands(ed, {"swap_view_left"});

    EXPECT_EQ(ed.focused, order[0]);
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"sb.txt"});

    RunCommands(ed, {"jump_view_right"});
    EXPECT_EQ(ed.doc.file.filename().string(), std::string{"sa.txt"});

    RunCommands(ed, {"jump_view_left"});
    const std::string before = ed.doc.file.filename().string();
    RunCommands(ed, {"swap_view_left"});
    EXPECT_EQ(ed.doc.file.filename().string(), before);
    EXPECT_EQ(WindowCount(ed), std::size_t{2});
  }

  TEST_CASE("windows: transpose turns the split the other way");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    SplitWindow(ed, true);
    {
      const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
      EXPECT_EQ(laid.size(), std::size_t{2});
      if (laid.size() == 2) EXPECT_EQ(laid[1].x, laid[0].x + laid[0].w);
    }
    RunCommands(ed, {"transpose_view"});
    {
      const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
      EXPECT_EQ(laid.size(), std::size_t{2});
      if (laid.size() == 2) EXPECT_EQ(laid[1].y, laid[0].y + laid[0].h);
    }

    RunCommands(ed, {"transpose_view"});
    {
      const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 80, 24});
      if (laid.size() == 2) EXPECT_EQ(laid[1].x, laid[0].x + laid[0].w);
    }

    RunCommands(ed, {"wonly"});
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
    RunCommands(ed, {"transpose_view"});
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
  }

  TEST_CASE("windows: direction commands survive an arbitrary tree");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    Rng rng{0xB0A7ULL};
    for (int i = 0; i < 60; ++i) {
      switch (rng.Pick(0, 7)) {
        case 0: SplitWindow(ed, true); break;
        case 1: SplitWindow(ed, false); break;
        case 2: RunCommands(ed, {"jump_view_left"}); break;
        case 3: RunCommands(ed, {"jump_view_right"}); break;
        case 4: RunCommands(ed, {"jump_view_up"}); break;
        case 5: RunCommands(ed, {"jump_view_down"}); break;
        case 6: RunCommands(ed, {"transpose_view"}); break;
        default: RunCommands(ed, {"swap_view_right"}); break;
      }

      const std::vector<int> order = WindowOrder(ed);
      const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 100, 40});
      EXPECT_EQ(laid.size(), order.size());
      long area = 0;
      for (const Rect& r : laid) area += static_cast<long>(r.w) * r.h;
      EXPECT_EQ(area, 100L * 40L);
      EXPECT_TRUE(std::ranges::find(order, ed.focused) != order.end());
    }
  }

  TEST_CASE("windows: splitting and closing many times leaves no wreckage");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\ntwo\nthree\n");
    for (int round = 0; round < 40; ++round) {
      SplitWindow(ed, (round % 2) == 0);
      SplitWindow(ed, (round % 3) == 0);
      FocusWindow(ed, true);
      CloseWindow(ed);
    }

    const std::size_t count = WindowCount(ed);
    EXPECT_TRUE(count >= 1);
    const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, 120, 50});
    EXPECT_EQ(laid.size(), count);
    long area = 0;
    for (const Rect& r : laid) {
      EXPECT_TRUE(r.w >= 0);
      EXPECT_TRUE(r.h >= 0);
      area += static_cast<long>(r.w) * r.h;
    }
    EXPECT_EQ(area, 120L * 50L);
    while (WindowCount(ed) > 1) CloseWindow(ed);
    EXPECT_EQ(WindowCount(ed), std::size_t{1});
  }
}

namespace {

Rect FocusedPane(const Editor& ed, Rect screen) {
  const std::vector<int> order = WindowOrder(ed);
  const std::vector<Rect> laid = LayoutWindows(ed, screen);
  for (std::size_t i = 0; (i < order.size()) && (i < laid.size()); ++i) {
    if (order[i] == ed.focused) return laid[i];
  }
  return laid.empty() ? Rect{} : laid.front();
}

Editor OnScreen(int width, int height, std::string_view text = "one\ntwo\nthree\nfour\nfive\n") {
  Editor ed;
  ResetToOriginal(ed.doc.table, std::string{text});
  ed.doc.selections.Set(Selection{0, 0, -1});
  ed.doc.selections.EnsureBlockCursors(ed.doc.table);
  ed.screen_w = width;
  ed.screen_h = height;
  return ed;
}

}  // namespace

void ResizablePanes(Rng& rng) {
  TEST_CASE("resize: a ratio put back through the layout lands on the same cell");
  {
    for (int span = 2; span <= 200; ++span) {
      bool exact = true;
      bool in_range = true;
      for (int first = 1; first < span; ++first) {
        if (SplitAt(span, SplitRatio(span, first)) != first) exact = false;
        const double ratio = SplitRatio(span, first);
        if (!((ratio > 0.0) && (ratio < 1.0))) in_range = false;
      }
      EXPECT_TRUE(exact);
      EXPECT_TRUE(in_range);
    }
    EXPECT_EQ(SplitAt(0, 0.5), 0);
    EXPECT_EQ(SplitAt(1, 0.5), 1);
    EXPECT_EQ(SplitAt(10, 0.0), 1);
    EXPECT_EQ(SplitAt(10, 1.0), 9);
    EXPECT_EQ(SplitAt(10, -5.0), 1);
    EXPECT_EQ(SplitAt(10, 5.0), 9);
  }

  TEST_CASE("resize: the window chord binds all four of them");
  {
    const KeyMaps maps = DefaultKeyMaps();
    const auto chord = [&maps](std::string_view second) {
      Key prefix{};
      Key key{};
      if (!ParseKey("e", prefix) || !ParseKey(second, key)) return std::string{"<unparsed>"};
      const std::vector<std::string>* commands = nullptr;
      if (maps.normal.Find({prefix, key}, &commands) != KeyMap::Lookup::kMatched) {
        return std::string{"<unbound>"};
      }
      if ((commands == nullptr) || (commands->size() != 1)) return std::string{"<not one>"};
      return commands->front();
    };
    EXPECT_EQ(chord(">"), std::string{"expand_width"});
    EXPECT_EQ(chord("<"), std::string{"shrink_width"});
    EXPECT_EQ(chord("+"), std::string{"expand_height"});
    EXPECT_EQ(chord("-"), std::string{"shrink_height"});
  }

  TEST_CASE("resize: expand_width takes a tenth of this pane, and shrink gives it back");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, true);
    const Rect screen{0, 0, 80, 24};
    EXPECT_EQ(FocusedPane(ed, screen).w, 40);

    RunCommands(ed, {"expand_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, 44);
    EXPECT_EQ(LayoutWindows(ed, screen)[0].w, 36);

    RunCommands(ed, {"shrink_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, 40);
    EXPECT_EQ(LayoutWindows(ed, screen)[0].w, 40);

    RunCommands(ed, {"jump_view_left"});
    RunCommands(ed, {"expand_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, 44);
    EXPECT_EQ(LayoutWindows(ed, screen)[1].w, 36);
    RunCommands(ed, {"shrink_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, 40);

    long area = 0;
    for (const Rect& r : LayoutWindows(ed, screen)) area += static_cast<long>(r.w) * r.h;
    EXPECT_EQ(area, 80L * 24L);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("resize: expand_height moves the divider between stacked panes");
  {
    Editor ed = OnScreen(80, 40);
    SplitWindow(ed, false);
    const Rect screen{0, 0, 80, 40};
    EXPECT_EQ(FocusedPane(ed, screen).h, 20);

    RunCommands(ed, {"expand_height"});
    EXPECT_EQ(FocusedPane(ed, screen).h, 22);
    EXPECT_EQ(LayoutWindows(ed, screen)[0].h, 18);
    RunCommands(ed, {"shrink_height"});
    EXPECT_EQ(FocusedPane(ed, screen).h, 20);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("resize: growing then shrinking leaves the pane exactly as it was");
  {
    constexpr int kWide = 200;
    Editor ed = OnScreen(kWide, 24);
    SplitWindow(ed, true);
    const Rect screen{0, 0, kWide, 24};

    const auto grew = [](int from) { return from + std::max(1, (from + 5) / 10); };
    const auto shrank = [](int from) { return std::min(from - 1, ((10 * from) + 5) / 11); };

    int checked = 0;
    bool exact = true;
    bool placed = true;
    for (int left = kMinPaneWidth; left <= (kWide - kMinPaneWidth); ++left) {
      MoveDivider(ed, 0, left - 1, 3, screen);
      const int was = FocusedPane(ed, screen).w;
      if (was != (kWide - left)) placed = false;
      RunCommands(ed, {"expand_width"});
      if (FocusedPane(ed, screen).w != grew(was)) continue;
      RunCommands(ed, {"shrink_width"});
      if (FocusedPane(ed, screen).w != was) exact = false;
      ++checked;
    }
    EXPECT_TRUE(placed);
    EXPECT_TRUE(exact);
    EXPECT_TRUE(checked > 150);

    int checked_back = 0;
    bool close_back = true;
    for (int left = kMinPaneWidth; left <= (kWide - kMinPaneWidth); ++left) {
      MoveDivider(ed, 0, left - 1, 3, screen);
      const int was = FocusedPane(ed, screen).w;
      RunCommands(ed, {"shrink_width"});
      if (FocusedPane(ed, screen).w != shrank(was)) continue;
      RunCommands(ed, {"expand_width"});
      if (std::abs(FocusedPane(ed, screen).w - was) > 1) close_back = false;
      ++checked_back;
    }
    EXPECT_TRUE(close_back);
    EXPECT_TRUE(checked_back > 150);

    MoveDivider(ed, 0, 99, 3, screen);
    const int settled = FocusedPane(ed, screen).w;
    for (int i = 0; i < 40; ++i) RunCommands(ed, {"expand_width", "shrink_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, settled);
    for (int i = 0; i < 40; ++i) RunCommands(ed, {"shrink_width", "expand_width"});
    EXPECT_TRUE(std::abs(FocusedPane(ed, screen).w - settled) <= 1);
    ed.status.clear();
    const Rect before_height = FocusedPane(ed, screen);
    for (int i = 0; i < 5; ++i) RunCommands(ed, {"shrink_height", "expand_height"});
    EXPECT_TRUE(FocusedPane(ed, screen) == before_height);
    EXPECT_TRUE(ed.status.find("no window above or below") != std::string::npos);
  }

  TEST_CASE("resize: a count repeats the step");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, true);
    const Rect screen{0, 0, 80, 24};

    Editor stepped = ed;
    for (int i = 0; i < 3; ++i) RunCommands(stepped, {"expand_width"});

    ed.pending_count = 3;
    RunCommands(ed, {"expand_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, FocusedPane(stepped, screen).w);
    EXPECT_TRUE(FocusedPane(ed, screen).w > 40);
  }

  TEST_CASE("resize: a pane with no neighbour on that axis says so and changes nothing");
  {
    Editor ed = OnScreen(80, 24);
    const Rect screen{0, 0, 80, 24};
    RunCommands(ed, {"expand_width"});
    EXPECT_TRUE(ed.status.find("no window beside") != std::string::npos);
    EXPECT_EQ(FocusedPane(ed, screen).w, 80);

    SplitWindow(ed, true);
    ed.status.clear();
    RunCommands(ed, {"expand_height"});
    EXPECT_TRUE(ed.status.find("no window above or below") != std::string::npos);
    EXPECT_EQ(FocusedPane(ed, screen).h, 24);
    EXPECT_EQ(FocusedPane(ed, screen).w, 40);
  }

  TEST_CASE("resize: shrinking stops at a pane you can still read");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, true);
    const Rect screen{0, 0, 80, 24};
    for (int i = 0; i < 200; ++i) RunCommands(ed, {"shrink_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, kMinPaneWidth);
    EXPECT_TRUE(ed.status.find("as narrow as it goes") != std::string::npos);

    for (int i = 0; i < 200; ++i) RunCommands(ed, {"expand_width"});
    EXPECT_EQ(FocusedPane(ed, screen).w, 80 - kMinPaneWidth);
    EXPECT_TRUE(ed.status.find("as wide as it goes") != std::string::npos);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("resize: stacked panes keep a text row and a status line each");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, false);
    const Rect screen{0, 0, 80, 24};
    for (int i = 0; i < 200; ++i) RunCommands(ed, {"shrink_height"});
    EXPECT_EQ(FocusedPane(ed, screen).h, kMinPaneHeight);
    for (int i = 0; i < 200; ++i) RunCommands(ed, {"expand_height"});
    EXPECT_EQ(FocusedPane(ed, screen).h, 24 - kMinPaneHeight);
  }

  TEST_CASE("resize: a nested pane resizes against the split that actually borders it");
  {
    Editor ed = OnScreen(100, 40);
    SplitWindow(ed, true);
    RunCommands(ed, {"jump_view_left"});
    SplitWindow(ed, false);
    RunCommands(ed, {"jump_view_up"});
    const Rect screen{0, 0, 100, 40};
    EXPECT_EQ(WindowCount(ed), std::size_t{3});

    const Rect before = FocusedPane(ed, screen);
    EXPECT_EQ(before.w, 50);
    EXPECT_EQ(before.h, 20);

    RunCommands(ed, {"expand_width"});
    const Rect wider = FocusedPane(ed, screen);
    EXPECT_EQ(wider.w, 55);
    EXPECT_EQ(wider.h, 20);
    const std::vector<Rect> laid = LayoutWindows(ed, screen);
    EXPECT_EQ(laid.size(), std::size_t{3});
    EXPECT_EQ(laid[0].w, 55);
    EXPECT_EQ(laid[1].w, 55);
    EXPECT_EQ(laid[2].w, 45);
    EXPECT_EQ(laid[2].h, 40);

    RunCommands(ed, {"expand_height"});
    const Rect taller = FocusedPane(ed, screen);
    EXPECT_EQ(taller.w, 55);
    EXPECT_EQ(taller.h, 22);
    EXPECT_EQ(LayoutWindows(ed, screen)[2].h, 40);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("resize: the prompt row is not counted as pane height");
  {
    Editor ed = OnScreen(80, 25);
    SplitWindow(ed, false);
    ed.prompt_active = true;
    EXPECT_EQ(PaneArea(ed).h, 24);
    RunCommands(ed, {"expand_height"});
    EXPECT_EQ(FocusedPane(ed, Rect{0, 0, 80, 24}).h, 13);
    ed.prompt_active = false;
  }

  TEST_CASE("resize: a screen too small to divide is left alone rather than mangled");
  {
    for (const std::pair<int, int>& size :
         {std::pair{0, 0}, std::pair{1, 1}, std::pair{2, 1}, std::pair{3, 2}, std::pair{4, 4}}) {
      Editor ed = OnScreen(size.first, size.second);
      SplitWindow(ed, true);
      SplitWindow(ed, false);
      for (const std::string& cmd :
           {std::string{"expand_width"}, std::string{"shrink_width"},
            std::string{"expand_height"}, std::string{"shrink_height"}}) {
        for (int i = 0; i < 5; ++i) RunCommands(ed, {cmd});
        EXPECT_EQ(EditorInvariants(ed), std::string{});
      }
      long area = 0;
      for (const Rect& r : LayoutWindows(ed, Rect{0, 0, size.first, size.second})) {
        EXPECT_TRUE(r.w >= 0);
        EXPECT_TRUE(r.h >= 0);
        area += static_cast<long>(r.w) * r.h;
      }
      EXPECT_EQ(area, static_cast<long>(size.first) * size.second);
    }
  }

  TEST_CASE("resize: a resized split keeps its proportions when the terminal changes size");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, true);
    RunCommands(ed, {"expand_width"});
    EXPECT_EQ(FocusedPane(ed, Rect{0, 0, 80, 24}).w, 44);
    const int wide = FocusedPane(ed, Rect{0, 0, 160, 24}).w;
    EXPECT_TRUE((wide >= 86) && (wide <= 90));
    EXPECT_EQ(FocusedPane(ed, Rect{0, 0, 80, 24}).w, 44);
  }

  TEST_CASE("resize: resizing moves no cursor, focus or buffer");
  {
    const Scratch scratch{"koi-resize-quiet"};
    const std::filesystem::path a = scratch.Write("ra.txt", "alpha\nbravo\ncharlie\n");
    const std::filesystem::path b = scratch.Write("rb.txt", "delta\necho\nfoxtrot\n");

    Editor ed;
    ed.screen_w = 80;
    ed.screen_h = 24;
    EXPECT_TRUE(OpenTarget(ed, a.string()));
    SplitWindow(ed, true);
    EXPECT_TRUE(OpenTarget(ed, b.string()));
    RunCommands(ed, {"move_line_down"});

    const int focused = ed.focused;
    const std::size_t active = ed.active;
    const Index cursor = Cur(ed);
    const std::filesystem::path path = ed.doc.file;
    const Index revision = ed.doc.table.revision;

    RunCommands(ed, {"expand_width", "expand_width", "shrink_width"});
    EXPECT_EQ(ed.focused, focused);
    EXPECT_EQ(ed.active, active);
    EXPECT_EQ(Cur(ed), cursor);
    EXPECT_EQ(ed.doc.file.string(), path.string());
    EXPECT_EQ(ed.doc.table.revision, revision);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("resize: the pane's viewport is refitted to the width it just gained");
  {
    Editor ed = OnScreen(80, 24, "alpha bravo charlie delta echo foxtrot golf hotel india\n");
    SplitWindow(ed, true);
    FitFocusedViewport(ed, 80, 24);
    const Index before = ed.doc.view.columns;
    RunCommands(ed, {"expand_width"});
    FitFocusedViewport(ed, 80, 24);
    EXPECT_EQ(ed.doc.view.columns, before + 4);
    EXPECT_EQ(ed.doc.view.rows, Index{23});
  }

  TEST_CASE("resize: a drawn frame still covers the screen at any divider position");
  {
    Editor ed = OnScreen(60, 16, "alpha bravo\ncharlie delta\necho foxtrot\ngolf\n");
    SplitWindow(ed, true);
    SplitWindow(ed, false);
    for (int i = 0; i < 12; ++i) {
      RunCommands(ed, {((i % 3) == 0) ? "shrink_width" : "expand_width"});
      RunCommands(ed, {((i % 2) == 0) ? "expand_height" : "shrink_height"});
      Surface frame;
      FitFocusedViewport(ed, 60, 16);
      RenderTo(ed, frame, 60, 16);
      bool painted = true;
      for (int y = 0; y < frame.height; ++y) {
        for (int x = 0; x < frame.width; ++x) {
          if (frame.At(x, y).text.empty()) painted = false;
        }
      }
      EXPECT_TRUE(painted);
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
  }

  TEST_CASE("mouse: the divider the hit test finds is the rule that was drawn");
  {
    Editor ed = OnScreen(60, 12, "alpha bravo\ncharlie delta\n");
    SplitWindow(ed, true);
    const Rect screen{0, 0, 60, 12};
    Surface frame;
    FitFocusedViewport(ed, 60, 12);
    RenderTo(ed, frame, 60, 12);

    const int rule = LayoutWindows(ed, screen)[0].w - 1;
    EXPECT_EQ(rule, 29);
    for (int y = 0; y < 12; ++y) {
      EXPECT_EQ(frame.At(rule, y).text, std::string{"│"});
      EXPECT_EQ(DividerAt(ed, rule, y, screen), 0);
    }
    EXPECT_EQ(DividerAt(ed, rule - 1, 3, screen), -1);
    EXPECT_EQ(DividerAt(ed, rule + 1, 3, screen), -1);
    EXPECT_EQ(DividerAt(ed, rule, 99, screen), -1);
    EXPECT_EQ(DividerAt(ed, 999, 3, screen), -1);
  }

  TEST_CASE("mouse: every rule the draw puts down is a divider, and nothing else is");
  {
    Editor ed = OnScreen(80, 24, "abc def\nghi jkl\nmno pqr\nstu vwx\n");
    SplitWindow(ed, true);
    SplitWindow(ed, false);
    RunCommands(ed, {"jump_view_left"});
    SplitWindow(ed, false);
    RunCommands(ed, {"expand_height", "expand_height", "shrink_width"});
    RunCommands(ed, {"jump_view_right"});
    RunCommands(ed, {"shrink_height", "expand_width"});
    EXPECT_EQ(WindowCount(ed), std::size_t{4});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    Surface frame;
    FitFocusedViewport(ed, 80, 24);
    RenderTo(ed, frame, 80, 24);
    const Rect screen{0, 0, 80, 24};

    int rules = 0;
    bool agrees = true;
    for (int y = 0; y < 24; ++y) {
      for (int x = 0; x < 80; ++x) {
        const int node = DividerAt(ed, x, y, screen);
        const bool row_divider =
            (node >= 0) &&
            (ed.windows[static_cast<std::size_t>(node)].kind == WindowNode::Kind::kRow);
        const bool painted = (frame.At(x, y).text == "│");
        if (painted) ++rules;
        if (painted != row_divider) agrees = false;
      }
    }
    EXPECT_TRUE(agrees);
    EXPECT_TRUE(rules >= 24);

    for (int y = 0; y < 24; ++y) {
      for (int x = 0; x < 80; ++x) {
        const int node = DividerAt(ed, x, y, screen);
        if (node < 0) continue;
        if (ed.windows[static_cast<std::size_t>(node)].kind != WindowNode::Kind::kColumn) continue;
        Rect area{};
        EXPECT_TRUE(WindowAtPoint(ed, x, y, 80, 24, area) >= 0);
        EXPECT_EQ(y, area.y + area.h - 1);
      }
    }
  }

  TEST_CASE("mouse: dragging a vertical divider puts it exactly under the pointer");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, true);
    const Rect screen{0, 0, 80, 24};
    const int node = DividerAt(ed, 39, 5, screen);
    EXPECT_EQ(node, 0);

    EXPECT_TRUE(MoveDivider(ed, node, 59, 5, screen));
    EXPECT_EQ(LayoutWindows(ed, screen)[0].w, 60);
    EXPECT_EQ(LayoutWindows(ed, screen)[1].w, 20);
    EXPECT_EQ(DividerAt(ed, 59, 5, screen), node);

    for (int x = -20; x < 100; ++x) {
      MoveDivider(ed, node, x, 5, screen);
      const int left = LayoutWindows(ed, screen)[0].w;
      EXPECT_EQ(left, std::clamp(x + 1, kMinPaneWidth, 80 - kMinPaneWidth));
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("mouse: a stacked split is grabbed by the status line between the panes");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, false);
    const Rect screen{0, 0, 80, 24};
    const int handle = LayoutWindows(ed, screen)[0].h - 1;
    EXPECT_EQ(handle, 11);
    EXPECT_EQ(DividerAt(ed, 40, handle, screen), 0);
    EXPECT_EQ(DividerAt(ed, 40, handle - 1, screen), -1);
    EXPECT_EQ(DividerAt(ed, 40, handle + 1, screen), -1);
    EXPECT_EQ(DividerAt(ed, 40, 23, screen), -1);

    EXPECT_TRUE(MoveDivider(ed, 0, 40, 5, screen));
    EXPECT_EQ(LayoutWindows(ed, screen)[0].h, 6);
    EXPECT_EQ(LayoutWindows(ed, screen)[1].h, 18);
    for (int y = -10; y < 40; ++y) {
      MoveDivider(ed, 0, 40, y, screen);
      const int top = LayoutWindows(ed, screen)[0].h;
      EXPECT_EQ(top, std::clamp(y + 1, kMinPaneHeight, 24 - kMinPaneHeight));
    }
  }

  TEST_CASE("mouse: where a rule crosses a status line, the rule is what is grabbed");
  {
    Editor ed = OnScreen(100, 40);
    SplitWindow(ed, true);
    RunCommands(ed, {"jump_view_left"});
    SplitWindow(ed, false);
    const Rect screen{0, 0, 100, 40};
    const std::vector<Rect> laid = LayoutWindows(ed, screen);
    EXPECT_EQ(laid.size(), std::size_t{3});

    const int rule = laid[0].w - 1;
    const int handle = laid[0].h - 1;
    const int outer = DividerAt(ed, rule, 3, screen);
    const int inner = DividerAt(ed, rule - 5, handle, screen);
    EXPECT_TRUE(outer >= 0);
    EXPECT_TRUE(inner >= 0);
    EXPECT_TRUE(outer != inner);
    EXPECT_EQ(DividerAt(ed, rule, handle, screen), outer);

    EXPECT_TRUE(MoveDivider(ed, outer, 39, handle, screen));
    EXPECT_EQ(LayoutWindows(ed, screen)[0].w, 40);
    EXPECT_EQ(LayoutWindows(ed, screen)[0].h, 20);
  }

  TEST_CASE("mouse: a drag on a divider moves nothing but the divider");
  {
    Editor ed = OnScreen(80, 24);
    SplitWindow(ed, true);
    const Rect screen{0, 0, 80, 24};
    const int focused = ed.focused;
    const Index cursor = Cur(ed);
    const std::size_t buffers = BufferCount(ed);

    for (int x : {30, 20, 50, 44}) MoveDivider(ed, 0, x, 7, screen);
    EXPECT_EQ(ed.focused, focused);
    EXPECT_EQ(Cur(ed), cursor);
    EXPECT_EQ(BufferCount(ed), buffers);
    EXPECT_EQ(LayoutWindows(ed, screen)[0].w, 45);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("mouse: a divider that cannot move reports so, and an unsplit editor has none");
  {
    Editor ed = OnScreen(80, 24);
    const Rect screen{0, 0, 80, 24};
    EXPECT_EQ(DividerAt(ed, 40, 5, screen), -1);
    EXPECT_FALSE(MoveDivider(ed, 0, 40, 5, screen));
    EXPECT_FALSE(MoveDivider(ed, -1, 40, 5, screen));
    EXPECT_FALSE(MoveDivider(ed, 99, 40, 5, screen));

    SplitWindow(ed, true);
    EXPECT_FALSE(MoveDivider(ed, ed.focused, 20, 5, screen));
    EXPECT_TRUE(MoveDivider(ed, 0, 20, 5, screen));
    EXPECT_FALSE(MoveDivider(ed, 0, 20, 5, screen));
    EXPECT_TRUE(MoveDivider(ed, 0, 0, 5, screen));
    EXPECT_FALSE(MoveDivider(ed, 0, 0, 5, screen));
    EXPECT_FALSE(MoveDivider(ed, 0, -50, 5, screen));
  }

  TEST_CASE("mouse: the prompt row shifts the panes the hit test searches");
  {
    Editor ed = OnScreen(80, 25);
    SplitWindow(ed, false);
    Surface frame;
    ed.prompt_active = true;
    ed.prompt_input = "x";
    FitFocusedViewport(ed, 80, 25);
    RenderTo(ed, frame, 80, 25);

    EXPECT_EQ(DividerAtPoint(ed, 40, 11, 80, 25), 0);
    EXPECT_EQ(DividerAtPoint(ed, 40, 12, 80, 25), -1);
    EXPECT_TRUE(DragDivider(ed, 0, 40, 5, 80, 25));
    EXPECT_EQ(LayoutWindows(ed, PaneArea(ed))[0].h, 6);
    ed.prompt_active = false;
    ed.prompt_input.clear();
  }

  TEST_CASE("resize: any sequence of splits, drags and resizes keeps the layout whole");
  {
    Editor ed = OnScreen(96, 32, "alpha\nbravo\ncharlie\ndelta\necho\n");
    static constexpr std::array<std::string_view, 8> kResizes{
        "expand_width",  "shrink_width",   "expand_height", "shrink_height",
        "jump_view_left", "jump_view_right", "jump_view_up",  "jump_view_down"};

    for (int step = 0; step < 4000; ++step) {
      const Index pick = rng.Pick(0, 12);
      if (pick < static_cast<Index>(kResizes.size())) {
        RunCommands(ed, {std::string{kResizes[static_cast<std::size_t>(pick)]}});
      } else if (pick == 8) {
        SplitWindow(ed, rng.Pick(0, 1) == 0);
      } else if (pick == 9) {
        CloseWindow(ed);
      } else if (pick == 10) {
        RunCommands(ed, {"transpose_view"});
      } else {
        const int x = static_cast<int>(rng.Pick(-10, 110));
        const int y = static_cast<int>(rng.Pick(-10, 40));
        const int node = DividerAt(ed, x, y, PaneArea(ed));
        if (node >= 0) {
          MoveDivider(ed, node, static_cast<int>(rng.Pick(-10, 110)),
                      static_cast<int>(rng.Pick(-10, 40)), PaneArea(ed));
        }
      }

      const std::string broke = EditorInvariants(ed);
      if (!broke.empty()) {
        std::cerr << "      after step " << step << ": " << broke << "\n";
        EXPECT_EQ(broke, std::string{});
        break;
      }

      const std::vector<Rect> laid = LayoutWindows(ed, PaneArea(ed));
      if (laid.size() == 2) {
        for (const Rect& r : laid) {
          EXPECT_TRUE(r.w >= kMinPaneWidth);
          EXPECT_TRUE(r.h >= kMinPaneHeight);
        }
      }
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void SwappingPanesKeepsTheirCursors() {
  TEST_CASE("windows: swapping two panes moves neither pane's cursor");
  const Scratch scratch{"koi-swap-panes"};
  const std::filesystem::path a = scratch.Write("a.txt", NumberedLines(200));
  const std::filesystem::path b = scratch.Write("b.txt", NumberedLines(200));

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.screen_w = 100;
  ed.screen_h = 40;
  EXPECT_TRUE(OpenTarget(ed, a.string()));

  const auto draw = [&] {
    Surface frame;
    FitFocusedViewport(ed, ed.screen_w, ed.screen_h);
    RenderTo(ed, frame, ed.screen_w, ed.screen_h);
  };
  const auto put_on_line = [&](Index line) {
    const Index at = LineStart(ed.doc.table, line);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
  };
  const auto panes = [&] {
    std::vector<std::string> out;
    for (const int leaf : WindowOrder(ed)) {
      const WindowNode& node = ed.windows[static_cast<std::size_t>(leaf)];
      const Document& doc = (leaf == ed.focused) ? ed.doc : BufferAt(ed, node.buffer);
      const SelectionSet& sel = (leaf == ed.focused) ? ed.doc.selections : node.selections;
      out.push_back(doc.file.filename().string() + ":" +
                    std::to_string(LineAt(doc.table, CursorOf(doc.table, sel.Primary()))));
    }
    return out;
  };

  put_on_line(101);
  RunCommands(ed, {"vsplit"});
  EXPECT_TRUE(OpenTarget(ed, b.string()));

  for (int i = 0; i < 40; ++i) {
    put_on_line(0);
    RunCommands(ed, {"add_newline_above"});
    draw();
  }
  put_on_line(61);
  draw();

  const std::vector<std::string> before = panes();
  EXPECT_EQ(before.size(), std::size_t{2});
  RunCommands(ed, {"swap_view_left"});
  draw();
  const std::vector<std::string> after = panes();

  EXPECT_EQ(after.size(), before.size());
  if ((before.size() == 2) && (after.size() == 2)) {
    EXPECT_EQ(after[0], before[1]);
    EXPECT_EQ(after[1], before[0]);
  }

  RunCommands(ed, {"swap_view_right"});
  draw();
  const std::vector<std::string> back = panes();
  EXPECT_EQ(back.size(), before.size());
  if ((back.size() == 2) && (before.size() == 2)) {
    EXPECT_EQ(back[0], before[0]);
    EXPECT_EQ(back[1], before[1]);
  }
  EXPECT_EQ(EditorInvariants(ed), std::string{});

  TEST_CASE("windows: closing a buffer leaves the panes that showed it somewhere real");
  {
    Editor two;
    two.theme = BuiltinTheme();
    two.screen_w = 100;
    two.screen_h = 40;
    EXPECT_TRUE(OpenTarget(two, a.string()));
    RunCommands(two, {"vsplit"});
    EXPECT_TRUE(OpenTarget(two, b.string()));
    const Index at = LineStart(two.doc.table, 150);
    two.doc.selections.Set(MinWidth1(two.doc.table, Selection{at, at, -1}));
    RunCommands(two, {"jump_view_next"});

    RunCommands(two, {"jump_view_next"});
    EXPECT_EQ(two.doc.file.filename().string(), std::string{"b.txt"});
    RunTypableCommand(two, "bc!");
    EXPECT_EQ(EditorInvariants(two), std::string{});
    for (std::size_t i = 0; i < WindowCount(two); ++i) {
      RunCommands(two, {"jump_view_next"});
      EXPECT_EQ(two.doc.file.filename().string(), std::string{"a.txt"});
      const Index length = DocLength(two.doc.table);
      for (const Selection& s : two.doc.selections.Ranges()) {
        EXPECT_TRUE((s.anchor >= 0) && (s.anchor <= length));
        EXPECT_TRUE((s.head >= 0) && (s.head <= length));
      }
    }
  }
}

}  // namespace koi
