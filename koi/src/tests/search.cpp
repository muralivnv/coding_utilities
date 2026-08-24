// Tests for search.cpp: matching text, the highlights a match leaves, the
// per-document cache, and the scans that run behind an editor search.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

Index HitStart(const std::vector<Interval>& hits, std::size_t i) {
  return (i < hits.size()) ? hits[i].front() : Index{-1};
}

Index HitLast(const std::vector<Interval>& hits, std::size_t i) {
  return (i < hits.size()) ? hits[i].back() : Index{-2};
}

Index RangeFrom(const SelectionSet& sel, std::size_t i) {
  return (i < sel.Ranges().size()) ? sel.Ranges()[i].From() : Index{-1};
}

}  // namespace

void RegexSearch() {
  TEST_CASE("search");


  {
    std::vector<Interval> hits;
    std::string error;
    EXPECT_TRUE(FindInText("al", "alpha alfalfa", hits, error));
    EXPECT_EQ(hits.size(), std::size_t{3});

    if (hits.size() == 3) {
      EXPECT_EQ(HitStart(hits, 0), Index{0});
      EXPECT_EQ(HitStart(hits, 1), Index{6});
      EXPECT_EQ(HitStart(hits, 2), Index{9});
      EXPECT_EQ(HitLast(hits, 0) + 1, Index{2});
    }

    hits.clear();
    EXPECT_TRUE(FindInText("lta", "délta", hits, error));
    EXPECT_EQ(hits.size(), std::size_t{1});
    if (!hits.empty()) EXPECT_EQ(HitStart(hits, 0), Index{3});

    hits.clear();
    EXPECT_TRUE(FindInText("d.l", "délta", hits, error));
    EXPECT_EQ(hits.size(), std::size_t{1});
    if (!hits.empty()) EXPECT_EQ(HitLast(hits, 0) + 1, Index{4});

    hits.clear();
    EXPECT_TRUE(FindInText("x*", "abc", hits, error));
    EXPECT_TRUE(hits.empty());

    hits.clear();
    error.clear();
    EXPECT_FALSE(FindInText("alpha(", "alpha", hits, error));
    EXPECT_TRUE(!error.empty());
    EXPECT_TRUE(error.find('\n') == std::string::npos);
    EXPECT_TRUE(error.front() != '.');

    hits.clear();
    EXPECT_TRUE(FindInText("alpha", "alpha", hits, error));
    EXPECT_EQ(hits.size(), std::size_t{1});
  }

  {
    // gai's compile failure echoes the pattern back before it names the reason,
    // so a pattern containing the "Error: " field marker used to take the
    // status line for itself: `Error: (` reported `(`, which tells the person
    // typing nothing about what is wrong with what they typed. The reason is
    // the last line-anchored field of gai's message, so it wins from behind.
    constexpr std::string_view kReal = "missing closing parenthesis";
    std::vector<Interval> hits;
    std::string error;

    // The plain case that always worked, held down so the anchor cannot move
    // off the reason altogether.
    error.clear();
    EXPECT_FALSE(FindInText("alpha(", "alpha", hits, error));
    EXPECT_TRUE(error.find(kReal) != std::string::npos);

    // The hijack itself.
    error.clear();
    EXPECT_FALSE(FindInText("Error: (", "alpha", hits, error));
    EXPECT_TRUE(error.find(kReal) != std::string::npos);
    EXPECT_TRUE(error != "(");

    // Dots after the forged marker: the old leading-dot strip -- there to drop
    // one artefact byte gai leaves in front of PCRE2's message -- ate the
    // pattern's own dots once the pattern fragment had become the message.
    error.clear();
    EXPECT_FALSE(FindInText("Error: ..(", "alpha", hits, error));
    EXPECT_TRUE(error.find(kReal) != std::string::npos);

    // A pattern can carry a newline, so it can forge the line-anchored marker
    // in full. gai's own still comes last: it is the final field of the format
    // string and PCRE2's message has no newline to hide another behind.
    error.clear();
    EXPECT_FALSE(FindInText("x\nError: y(", "alpha", hits, error));
    EXPECT_TRUE(error.find(kReal) != std::string::npos);

    // Whatever it picked, the status line gets one line and no artefact dot.
    EXPECT_TRUE(error.find('\n') == std::string::npos);
    EXPECT_TRUE(!error.empty() && (error.front() != '.'));
  }

  {
    // The compile cache promises FindInText either a compiled regex or a
    // reason, never neither -- FindInText dereferences one on the strength of
    // the other being absent. Walk the cache through every transition it has:
    // a fresh failure, the same failure taken as a cache hit, a success
    // replacing a primed failure, and a failure replacing a primed success.
    std::vector<Interval> hits;
    std::string error;

    const auto reason_only = [&](std::string_view pattern) {
      hits.clear();
      error.clear();
      EXPECT_FALSE(FindInText(pattern, "alpha beta", hits, error));
      EXPECT_TRUE(!error.empty());
      EXPECT_TRUE(hits.empty());
    };
    const auto matches_only = [&](std::string_view pattern, std::size_t count) {
      hits.clear();
      error.clear();
      EXPECT_TRUE(FindInText(pattern, "alpha beta", hits, error));
      EXPECT_TRUE(error.empty());
      EXPECT_EQ(hits.size(), count);
    };

    reason_only("alpha(");
    reason_only("alpha(");
    matches_only("alpha", std::size_t{1});
    matches_only("alpha", std::size_t{1});
    reason_only("[");
    matches_only("beta", std::size_t{1});
    reason_only("(?P<");
    reason_only("alpha(");
    matches_only("a", std::size_t{3});
  }

  {
    Document doc;
    ResetToOriginal(doc.table, "alpha\nbeta alpha\ngamma\n");
    std::vector<Interval> hits;
    std::string error;
    EXPECT_TRUE(FindInDocument(doc.table, "alpha", Interval(0, DocLength(doc.table)), hits, error));
    EXPECT_EQ(hits.size(), std::size_t{2});
    EXPECT_EQ(HitStart(hits, 0), Index{0});
    EXPECT_EQ(HitStart(hits, 1), Index{11});

    hits.clear();
    EXPECT_TRUE(FindInDocument(doc.table, "^alpha", Interval(0, DocLength(doc.table)), hits, error));
    EXPECT_EQ(hits.size(), std::size_t{1});

    hits.clear();
    EXPECT_TRUE(FindInDocument(doc.table, "alpha", Interval(0, 3), hits, error));
    EXPECT_TRUE(hits.empty());
  }

  {
    Document doc;
    ResetToOriginal(doc.table,
                    "alpha beta\n"
                    "gamma\n"
                    "\n"
                    "beta alpha beta\n"
                    "  alpha}\n"
                    "zeta\n"
                    "alpha\n");
    const Index length = DocLength(doc.table);

    const auto reference = [&](std::string_view pattern, Index from) {
      std::vector<Interval> hits;
      std::string error;
      std::string out = "none";
      if (!FindInDocument(doc.table, pattern, Interval(0, length), hits, error)) return out;
      if (hits.empty()) return out;
      const Interval* landed = &hits.front();
      for (const Interval& hit : hits) {
        if (hit.front() >= from) {
          landed = &hit;
          break;
        }
      }
      return std::to_string(landed->front()) + ".." + std::to_string(landed->back() + 1);
    };
    const auto early_exit = [&](std::string_view pattern, Index from) {
      std::optional<Interval> found;
      std::string error;
      if (!FindFirstInDocument(doc.table, pattern, from, found, error)) return std::string{"none"};
      if (!found) return std::string{"none"};
      return std::to_string(found->front()) + ".." + std::to_string(found->back() + 1);
    };

    for (const std::string_view pattern :
         {"alpha", "^alpha", "beta$", "\\}$", "alpha|zeta", "^", "zeta", "nowhere", "^$", "a+",
          "\\balpha\\b", "gamma"}) {
      for (const Index from :
           {Index{0}, Index{1}, Index{11}, Index{17}, Index{18}, Index{40}, length - 1, length}) {
        EXPECT_EQ(early_exit(pattern, from), reference(pattern, from));
      }
    }

    std::optional<Interval> found;
    std::string error;
    EXPECT_FALSE(FindFirstInDocument(doc.table, "alpha(", 0, found, error));
    EXPECT_TRUE(!found.has_value());
    EXPECT_TRUE(!error.empty());

    error.clear();
    EXPECT_TRUE(FindFirstInDocument(doc.table, "", 0, found, error));
    EXPECT_TRUE(!found.has_value());

    EXPECT_EQ(early_exit("gamma", length), std::string{"11..16"});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\nbeta alpha\ngamma alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    RunSearch(ed, "alpha");
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{5});
    EXPECT_EQ(Cur(ed), Index{4});

    RunCommands(ed, {"search_next"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{11});
    RunCommands(ed, {"search_next"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{23});
    ed.status.clear();
    RunCommands(ed, {"search_next"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_TRUE(ed.status.find("[1/3]") != std::string::npos);
    EXPECT_TRUE(ed.status.find("wrapped") != std::string::npos);

    RunCommands(ed, {"search_prev"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{23});
    RunCommands(ed, {"search_prev"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{11});

    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    RunSearch(ed, "alpha");
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    ed.pending_count = 2;
    RunCommands(ed, {"search_next"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{23});
    EXPECT_EQ(ed.pending_count, Index{0});

    ed.pending_count = 2;
    RunCommands(ed, {"search_next"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{11});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aXbXc\n");
    ed.search_pattern = "X";
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{2, 2, -1}));
    RunCommands(ed, {"search_next"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{3});
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{2, 2, -1}));
    RunCommands(ed, {"search_prev"});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{1});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "one\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("needle ", ed.doc.table, ed.doc.selections);
    ed.mode = Mode::kNormal;
    ApplyModeInvariants(ed);
    RunSearch(ed, "needle");
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{6});
    EXPECT_TRUE(ed.status.find("no match") == std::string::npos);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{2, 2, -1}));

    RunCommands(ed, {"search_next"});
    EXPECT_TRUE(ed.status.find("no search pattern") != std::string::npos);

    RunSearch(ed, "zzz");
    EXPECT_TRUE(ed.status.find("no match") != std::string::npos);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{2});
    EXPECT_EQ(ed.search_pattern, std::string{});

    RunSearch(ed, "alpha(");
    EXPECT_TRUE(ed.status.find("bad pattern") != std::string::npos);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{2});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha beta alpha\ngamma alpha\n");
    ed.doc.selections.Set(Selection{0, 16, -1});
    SelectRegex(ed, "alpha");
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{2});
    EXPECT_EQ(RangeFrom(ed.doc.selections, 0), Index{0});
    EXPECT_EQ(RangeFrom(ed.doc.selections, 1), Index{11});
    EXPECT_EQ(ed.search_pattern, std::string{"alpha"});

    ed.doc.selections.Set(Selection{0, 16, -1});
    SelectRegex(ed, "zzz");
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_TRUE(ed.status.find("no match in selection") != std::string::npos);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha beta\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;

    Key slash;
    EXPECT_TRUE(ParseKey("/", slash));
    HandleKeyInput(ed, maps, slash, pending);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kSearch);
    EXPECT_EQ(PromptSigil(ed), std::string_view{"/"});

    for (const char c : std::string{"beta"}) {
      Key k;
      EXPECT_TRUE(ParseKey(std::string_view{&c, 1}, k));
      HandleKeyInput(ed, maps, k, pending);
    }
    EXPECT_EQ(ed.prompt_input, std::string{"beta"});
    EXPECT_EQ(ActiveSearchPattern(ed), std::string_view{"beta"});

    Key ret;
    EXPECT_TRUE(ParseKey("ret", ret));
    HandleKeyInput(ed, maps, ret, pending);
    EXPECT_TRUE(!ed.prompt_active);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{6});
    EXPECT_EQ(ed.search_history.size(), std::size_t{1});
    EXPECT_TRUE(ed.prompt_history.empty());

    EXPECT_EQ(ActiveSearchPattern(ed), std::string_view{"beta"});

    Key s;
    EXPECT_TRUE(ParseKey("s", s));
    HandleKeyInput(ed, maps, s, pending);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kSelectRegex);
    EXPECT_EQ(PromptSigil(ed), std::string_view{"select:"});
    PromptCancel(ed);

    Key colon;
    EXPECT_TRUE(ParseKey(":", colon));
    HandleKeyInput(ed, maps, colon, pending);
    EXPECT_TRUE(ed.prompt_kind == PromptKind::kCommand);
    EXPECT_EQ(PromptSigil(ed), std::string_view{":"});
    PromptCancel(ed);
  }

  TEST_CASE("search: / previews by jumping to the first match, and escape comes back");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha beta\ngamma\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;
    const auto type = [&](std::string_view text) {
      for (const char c : text) {
        Key k;
        EXPECT_TRUE(ParseKey(std::string_view{&c, 1}, k));
        HandleKeyInput(ed, maps, k, pending);
      }
    };
    Key slash, esc, ret, backspace;
    EXPECT_TRUE(ParseKey("/", slash));
    EXPECT_TRUE(ParseKey("esc", esc));
    EXPECT_TRUE(ParseKey("ret", ret));
    EXPECT_TRUE(ParseKey("backspace", backspace));

    HandleKeyInput(ed, maps, slash, pending);
    type("gam");
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{11});
    type("[");
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});
    HandleKeyInput(ed, maps, backspace, pending);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{11});
    HandleKeyInput(ed, maps, esc, pending);
    EXPECT_TRUE(!ed.prompt_active);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{0});

    // A backspace with nothing left to delete deletes nothing: esc is the only
    // way out, here as in every other prompt.
    HandleKeyInput(ed, maps, slash, pending);
    HandleKeyInput(ed, maps, backspace, pending);
    EXPECT_TRUE(ed.prompt_active);
    EXPECT_TRUE(ed.prompt_input.empty());
    HandleKeyInput(ed, maps, esc, pending);
    EXPECT_TRUE(!ed.prompt_active);

    HandleKeyInput(ed, maps, slash, pending);
    type("gamma");
    HandleKeyInput(ed, maps, ret, pending);
    EXPECT_TRUE(!ed.prompt_active);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{11});
  }

  TEST_CASE("multi-cursor: C skips empty lines; A-C stacks upward; A-, drops the primary");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "add(a)\n\nadd(b)\nadd(c)\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;

    Key shift_c, alt_c, alt_comma;
    EXPECT_TRUE(ParseKey("C", shift_c));
    EXPECT_TRUE(ParseKey("A-C", alt_c));
    EXPECT_TRUE(ParseKey("A-,", alt_comma));

    HandleKeyInput(ed, maps, shift_c, pending);
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{2});
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Ranges()[1].From()), Index{2});
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().From()), Index{2});

    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{15, 15, -1}));
    HandleKeyInput(ed, maps, alt_c, pending);
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{2});
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Ranges()[0].From()), Index{2});
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().From()), Index{2});

    HandleKeyInput(ed, maps, alt_comma, pending);
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(LineAt(ed.doc.table, ed.doc.selections.Primary().From()), Index{3});
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "alpha\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.search_pattern = "alpha";
    PromptOpen(ed, PromptKind::kSearch);
    PromptSubmit(ed);
    EXPECT_TRUE(!ed.prompt_active);
    EXPECT_EQ(ed.search_pattern, std::string{"alpha"});
    EXPECT_TRUE(ed.search_history.empty());
  }
}

void SearchHighlightDismissal() {
  TEST_CASE("search: esc dismisses the highlight, not the pattern");

  Editor ed;
  ResetToOriginal(ed.doc.table, "alpha one\nbeta alpha\n");
  ed.doc.selections.Set(Selection{0, 1, -1});

  EXPECT_TRUE(ActiveSearchPattern(ed).empty());
  RunSearch(ed, "alpha");
  EXPECT_TRUE(ed.search_highlight);
  EXPECT_EQ(ActiveSearchPattern(ed), std::string_view{"alpha"});

  RunCommands(ed, {"clear_search_highlight"});
  EXPECT_TRUE(!ed.search_highlight);
  EXPECT_TRUE(ActiveSearchPattern(ed).empty());
  EXPECT_EQ(ed.search_pattern, std::string{"alpha"});

  SearchStep(ed, true);
  EXPECT_TRUE(ed.search_highlight);
  EXPECT_EQ(ActiveSearchPattern(ed), std::string_view{"alpha"});

  RunCommands(ed, {"clear_search_highlight"});
  PromptOpen(ed, PromptKind::kSearch);
  PromptInsert(ed, "bet");
  EXPECT_EQ(ActiveSearchPattern(ed), std::string_view{"bet"});
  EXPECT_TRUE(!SearchIsConfinedToSelections(ed));
  PromptCancel(ed);

  const KeyMaps maps = DefaultKeyMaps();
  const std::vector<std::string>* bound = nullptr;
  const std::vector<Key> esc{Key{0, NamedKey::kEsc, 0}};
  EXPECT_TRUE(maps.normal.Find(esc, &bound) == KeyMap::Lookup::kMatched);
  EXPECT_TRUE(bound != nullptr);
  if (bound != nullptr) {
    EXPECT_EQ(bound->size(), 2u);
    EXPECT_EQ(bound->front(), std::string{"clear_search_highlight"});
  }
}

void SelectRegexIsInteractive() {
  TEST_CASE("search: `s` previews inside the selection only");

  Editor ed;
  ResetToOriginal(ed.doc.table, "alpha one alpha\nbeta alpha beta\n");

  ed.doc.selections.Set(Selection{0, 16, -1});
  PromptOpen(ed, PromptKind::kSelectRegex);
  PromptInsert(ed, "alpha");

  EXPECT_EQ(ActiveSearchPattern(ed), std::string_view{"alpha"});
  EXPECT_TRUE(SearchIsConfinedToSelections(ed));

  PromptSubmit(ed);
  EXPECT_EQ(ed.doc.selections.Ranges().size(), 2u);
  for (const Selection& sel : ed.doc.selections.Ranges()) {
    EXPECT_TRUE(sel.To() <= 16);
  }
  EXPECT_TRUE(ed.search_highlight);
}

void SearchCacheIsPerDocument() {
  TEST_CASE("search cache does not survive a file switch");
  const Scratch scratch{"koi-search-cache-switch"};
  const std::filesystem::path a =
      scratch.Write("a.txt", "alpha one\nfiller\nfiller\nfiller\nalpha two at the very end\n");
  const std::filesystem::path b = scratch.Write("b.txt", "tiny\n");

  Editor ed;
  EXPECT_TRUE(OpenTarget(ed, a.string()));
  RunSearch(ed, "alpha");
  EXPECT_EQ(ed.doc.search_cache.size(), std::size_t{2});
  EXPECT_EQ(ed.doc.search_cache_revision, ed.doc.table.revision);

  EXPECT_TRUE(OpenTarget(ed, b.string()));

  EXPECT_TRUE(ed.doc.search_cache.empty());
  EXPECT_EQ(ed.doc.search_cache_revision, Index{-1});

  ed.status.clear();
  SearchStep(ed, true);
  const Selection s = ed.doc.selections.Primary();
  EXPECT_TRUE(s.head <= DocLength(ed.doc.table));
  EXPECT_TRUE(s.anchor <= DocLength(ed.doc.table));
  EXPECT_TRUE(ed.status.find("no match") != std::string::npos);

  EXPECT_TRUE(OpenTarget(ed, a.string()));
  SearchStep(ed, true);
  EXPECT_EQ(ed.doc.search_cache.size(), std::size_t{2});
  EXPECT_TRUE(ed.doc.selections.Primary().head <= DocLength(ed.doc.table));
}

// PCRE2 matches on code point boundaries, which inside a combining sequence, a
// ZWJ run or a regional indicator pair is not a grapheme boundary -- and Apply
// refuses every edit whose ends are not on one. The search commands used to
// hand those offsets straight to SelectionSet::Set, the one mutator that does
// not snap, so the editor reported a match, highlighted it, and then ignored
// every editing key with no message. EditorInvariants has always checked for
// this; nothing had ever pointed it at search.
void SearchLandsOnGraphemeBoundaries() {
  const auto whole_doc = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, Interval(0, DocLength(ed.doc.table)));
  };

  TEST_CASE("search: a match ending inside a cluster selects the cluster and stays editable");
  {
    Editor ed;
    // "xéy" in NFD -- x, e, U+0301, y. The pattern is ASCII and the match still
    // lands mid-cluster: "e" ends at offset 2, inside the "é".
    ResetToOriginal(ed.doc.table, "xe\xcc\x81y\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    RunSearch(ed, "e");
    EXPECT_TRUE(ed.status.find("[1/1]") != std::string::npos);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{1});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunCommands(ed, {"delete_selection"});
    EXPECT_EQ(whole_doc(ed), std::string{"xy\n"});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  {
    struct Case {
      std::string_view what;
      std::string_view text;     // one cluster, held between "a" and "b\n"
      std::string_view pattern;  // matches inside that cluster, never at its start
    };
    const Case cases[] = {
        {"search: a decomposed cluster is widened, not dropped", "a" "e\xcc\x81" "b\n", "\xcc\x81"},
        {"search: a flag is widened, not dropped",
         "a" "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8" "b\n", "\xf0\x9f\x87\xb8"},
        {"search: a zwj sequence is widened, not dropped",
         "a" "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7" "b\n",
         "\xf0\x9f\x91\xa9"},
    };

    for (const Case& c : cases) {
      TEST_CASE(c.what);
      Editor ed;
      ResetToOriginal(ed.doc.table, std::string{c.text});
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

      RunSearch(ed, c.pattern);
      EXPECT_TRUE(ed.status.find("[1/1]") != std::string::npos);
      EXPECT_EQ(EditorInvariants(ed), std::string{});

      // Exactly the cluster the match sits in: everything between the "a" and
      // the "b", however many code points that is.
      EXPECT_EQ(ed.doc.selections.Primary().From(), Index{1});
      EXPECT_EQ(ed.doc.selections.Primary().To(), Index{DocLength(ed.doc.table) - 2});

      // The point of the finding: the edit has to actually happen.
      RunCommands(ed, {"delete_selection"});
      EXPECT_EQ(whole_doc(ed), std::string{"ab\n"});
      EXPECT_EQ(EditorInvariants(ed), std::string{});
    }
  }

  TEST_CASE("search: the / preview and select_regex snap the same way");
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "xe\xcc\x81y\n");
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    const KeyMaps maps = DefaultKeyMaps();
    std::vector<Key> pending;

    Key slash, e;
    EXPECT_TRUE(ParseKey("/", slash));
    EXPECT_TRUE(ParseKey("e", e));
    HandleKeyInput(ed, maps, slash, pending);
    HandleKeyInput(ed, maps, e, pending);
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{1});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
    PromptCancel(ed);
  }

  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "xe\xcc\x81y\n");
    ed.doc.selections.Set(Selection{0, DocLength(ed.doc.table), -1});
    SelectRegex(ed, "e");
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{1});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{4});
    EXPECT_EQ(EditorInvariants(ed), std::string{});

    RunCommands(ed, {"delete_selection"});
    EXPECT_EQ(whole_doc(ed), std::string{"xy\n"});
  }
}

void ChunkedScanSearch() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-scan"};

  const auto fresh = [&scratch] {
    Editor ed;
    ed.theme = BuiltinTheme();
    ed.settings.excerpt_context = 0;
    ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";
    return ed;
  };
  const auto hit_lines = [](const Editor& ed) {
    std::vector<Index> lines;
    for (const ExcerptRef& ref : ed.doc.excerpts.refs) lines.push_back(ref.line);
    std::ranges::sort(lines);
    return lines;
  };

  TEST_CASE("scan: exact line numbers across many chunk boundaries and at an unterminated EOF");
  {
    std::string big;
    big.reserve(1000000);
    big += "needle first\n";
    for (int i = 2; i < 40000; ++i) {
      big += (i == 20000) ? "the needle mid\n" : ("filler line number " + std::to_string(i) + "\n");
    }
    big += "needle last";
    scratch.Write("big.txt", big);
    Editor ed = fresh();
    SearchExcerpts(ed, "needle");
    PumpUntilIdle(ed);
    EXPECT_TRUE(IsExcerptView(ed.doc));
    EXPECT_TRUE(hit_lines(ed) == (std::vector<Index>{1, 20000, 40000}));
    scratch.Write("big.txt", "quiet\n");
  }

  TEST_CASE("scan: a line longer than the chunk grows the buffer and still matches");
  {
    std::string monster(std::size_t{1} << 20, 'x');
    monster.insert(700 * 1024, "needle");
    monster += "\nneedle after\n";
    scratch.Write("monster.txt", monster);
    Editor ed = fresh();
    SearchExcerpts(ed, "needle");
    PumpUntilIdle(ed);
    EXPECT_TRUE(hit_lines(ed) == (std::vector<Index>{1, 2}));
    scratch.Write("monster.txt", "quiet\n");
  }

  TEST_CASE("scan: past the line ceiling the prefix matches and the tail is skipped, never grown");
  {
    std::string huge(std::size_t{9} * 1024 * 1024, 'y');
    huge.insert(1024, "needle");
    const std::string beyond = "beyond-the-ceiling-marker";
    huge.insert(huge.size() - 10, beyond);
    huge += "\nneedle tail\n";
    scratch.Write("huge.txt", huge);
    Editor ed = fresh();
    SearchExcerpts(ed, "needle");
    PumpUntilIdle(ed);
    EXPECT_TRUE(hit_lines(ed) == (std::vector<Index>{1, 2}));
    Editor eb = fresh();
    SearchExcerpts(eb, beyond);
    PumpUntilIdle(eb);
    EXPECT_FALSE(IsExcerptView(eb.doc));
  }
}

void ScanResultsAreCapped() {
  TEST_CASE("search: a scan stops at a ceiling, and says that it did");
  const Scratch scratch{"koi-scan-cap"};

  // One ExcerptRef per matching line, each copying the whole path. The
  // fork/exec `:from` path already capped at kMaxCommandHits; the scan path did
  // not, so a broad pattern over a large tree grew without a ceiling on the
  // worker thread. 25k matches is comfortably past the 20k cap.
  {
    std::string many;
    many.reserve(25000 * 32);
    for (int i = 0; i < 25000; ++i) {
      many += "line ";
      many += std::to_string(i);
      many += " has a widget in it\n";
    }
    scratch.Write("many.txt", many);
  }

  Editor ed;
  ed.theme = BuiltinTheme();
  ed.settings.excerpt_context = 0;
  ed.settings.file_filter = "find " + scratch.dir.string() + " -type f -printf '%p\\n'";
  SearchExcerpts(ed, "widget");
  PumpUntilIdle(ed);

  EXPECT_TRUE(IsExcerptView(ed.doc));
  EXPECT_EQ(ed.doc.excerpts.refs.size(), std::size_t{20000});
  // Silently stopping short is worse than being slow: a missing match reads as
  // proof there is none.
  EXPECT_TRUE(ed.status.find("first 20000") != std::string::npos);

  // A search that fits is untouched, and says nothing.
  {
    const Scratch small{"koi-scan-cap-small"};
    Editor few;
    few.theme = BuiltinTheme();
    few.settings.excerpt_context = 0;
    few.settings.file_filter = "find " + small.dir.string() + " -type f -printf '%p\\n'";
    small.Write("few.txt", "widget one\nplain\nwidget two\n");
    SearchExcerpts(few, "widget");
    PumpUntilIdle(few);
    EXPECT_EQ(few.doc.excerpts.refs.size(), std::size_t{2});
    EXPECT_TRUE(few.status.find("first") == std::string::npos);
  }
}

void SearchSaysWhenItFailedInsteadOfFindingNothing() {
  TEST_CASE("search: a byte that is not UTF-8 does not hide the match beside it");

  // Documents can no longer hold ill-formed bytes -- every way in gates on
  // IsWellFormedUtf8 (editor.cpp's open, koi.cpp's stdin, shell.cpp's command
  // output, navigate.cpp's excerpt bodies), so there is no in-editor repro left
  // to build. FindInText is where it still matters: render.cpp highlights
  // whatever bytes a line holds and the shell paths hand it arbitrary strings.
  //
  // The bug was two-sided. FindInText compiles with UTF on, and without
  // PCRE2_MATCH_INVALID_UTF the interpreter validates the whole subject and
  // refused the line on the first bad byte while the JIT matched it anyway --
  // so whether "needle" was found came down to whether pcre2_jit_compile had
  // happened to succeed on the machine.
  {
    const std::string subject = "caf\xE9 needle here";
    std::vector<Interval> hits;
    std::string error;
    EXPECT_TRUE(FindInText("needle", subject, hits, error));
    EXPECT_EQ(error, std::string{});
    EXPECT_EQ(hits.size(), std::size_t{1});
    EXPECT_TRUE(!hits.empty() && (hits[0].front() == static_cast<Index>(subject.find("needle"))));
    EXPECT_TRUE(!hits.empty() && (hits[0].back() + 1 == static_cast<Index>(subject.find("needle") + 6)));
  }
  {
    // Bad byte after the match, and glued to it.
    for (const std::string& subject : {std::string("needle here caf\xE9"),
                                       std::string("\x80") + "needle here",
                                       std::string("\xE2\x82 needle \xC0 here")}) {
      std::vector<Interval> hits;
      std::string error;
      EXPECT_TRUE(FindInText("needle", subject, hits, error));
      EXPECT_EQ(error, std::string{});
      EXPECT_EQ(hits.size(), std::size_t{1});
    }
  }
  {
    // Genuinely absent is still a clean nothing, not an error.
    std::vector<Interval> hits;
    std::string error;
    EXPECT_TRUE(FindInText("needle", "caf\xE9 nothing here", hits, error));
    EXPECT_EQ(error, std::string{});
    EXPECT_EQ(hits.size(), std::size_t{0});
  }
  {
    // The interpreter leg, reached straight through gai: FindInText always asks
    // for a JIT, and on a box where pcre2_jit_compile succeeds the checks above
    // only ever exercise the JIT. Compiling with jit=false is the only way in
    // -- once a pattern is JIT-compiled, pcre2_match dispatches to the JIT
    // itself. This is the leg koi runs on no-W^X kernels, under seccomp and in
    // the TSan build, which turns the JIT off outright.
    gai::Pcre2Regex interpreted = gai::Regex(gai::Compile("needle", false, true));
    EXPECT_FALSE(interpreted.re.jitted);
    std::vector<gai::MatchSpan> spans;
    std::string error;
    EXPECT_EQ(gai::FindSpans(interpreted, "caf\xE9 needle here", spans, &error), std::size_t{1});
    EXPECT_EQ(error, std::string{});
    EXPECT_EQ(gai::FindSpans(interpreted, "caf\xE9 nothing here", spans, &error), std::size_t{0});
    EXPECT_EQ(error, std::string{});
  }

  TEST_CASE("search: a match-time failure comes back as an error, not an empty result");

  // (*LIMIT_MATCH=1) over a backtracking pattern is the one deterministic,
  // fast match-time failure, and both engines count it the same way. Before
  // the fix FindSpans folded PCRE2_ERROR_MATCHLIMIT in with "no match" and the
  // search reported nothing at all, with nothing said about why.
  const std::string kExploding = "(*LIMIT_MATCH=1)a*ab";
  const std::string kSubject = std::string(8, 'a') + "b";
  {
    std::vector<Interval> hits;
    std::string error;
    EXPECT_FALSE(FindInText(kExploding, kSubject, hits, error));
    EXPECT_FALSE(error.empty());
    EXPECT_TRUE(error.find("match limit") != std::string::npos);
  }
  {
    // And it travels out through the document-level entry points.
    PieceTable table = MakeTable(kSubject + "\n");
    std::vector<Interval> hits;
    std::string error;
    EXPECT_FALSE(FindInDocument(table, kExploding, Interval(0, DocLength(table)), hits, error));
    EXPECT_FALSE(error.empty());

    std::optional<Interval> landed;
    std::string first_error;
    EXPECT_FALSE(FindFirstInDocument(table, kExploding, 0, landed, first_error));
    EXPECT_FALSE(landed.has_value());
    EXPECT_FALSE(first_error.empty());
  }
  {
    // All the way to the status line, through the wiring that was already there.
    Editor ed;
    ResetToOriginal(ed.doc.table, kSubject + "\n");
    ed.doc.selections.Set(Selection{});
    RunSearch(ed, kExploding);
    EXPECT_TRUE(ed.status.find("match limit") != std::string::npos);
    EXPECT_TRUE(ed.status.find("no match") == std::string::npos);
  }

  TEST_CASE("search: error is left alone when nothing went wrong");
  {
    std::vector<Interval> hits;
    std::string error = "sentinel";
    EXPECT_TRUE(FindInText("needle", "a needle here", hits, error));
    EXPECT_EQ(error, std::string("sentinel"));
    hits.clear();
    EXPECT_TRUE(FindInText("needle", "nothing here", hits, error));
    EXPECT_EQ(error, std::string("sentinel"));
    EXPECT_TRUE(hits.empty());
  }
}

void RunawaySearchesGiveUpInsteadOfFreezingTheEditor() {
  TEST_CASE("search: a catastrophic pattern fails fast instead of costing 35 ms a line");

  // `(x+x+)+$` over a run of x's is the textbook exponential backtracker, and
  // pcre2's out-of-the-box budget of ten million match steps is not a budget
  // anyone waits out: it terminates, at ~35 ms per line, as a constant rather
  // than as a failure. These loops run the pattern once per line of the
  // document -- and the search prompt's live preview runs the whole sweep
  // again on every keystroke -- so a 60k-line file was ~236 seconds of frozen
  // editor per key, ending only at SIGKILL. gai now caps the steps one subject
  // may spend, which turns the runaway into a match-time error on the status
  // line instead.
  //
  // Twenty x's is the length that tells the new cap from pcre2's default: it
  // costs over a hundred thousand steps and far short of ten million, so it is
  // an error under the cap and a slow, silent no-match without it. One such
  // line is the whole test, deliberately -- a document big enough to be
  // dramatic would hang a suite run with the cap removed rather than fail it.
  const std::string kRunaway = "(x+x+)+$";
  const std::string kBadLine = std::string(20, 'x') + "z";
  {
    PieceTable table = MakeTable(kBadLine + "\n");
    std::vector<Interval> hits;
    std::string error;
    EXPECT_FALSE(FindInDocument(table, kRunaway, Interval(0, DocLength(table)), hits, error));
    EXPECT_TRUE(error.find("match limit") != std::string::npos);

    std::optional<Interval> landed;
    std::string first_error;
    EXPECT_FALSE(FindFirstInDocument(table, kRunaway, 0, landed, first_error));
    EXPECT_FALSE(landed.has_value());
    EXPECT_TRUE(first_error.find("match limit") != std::string::npos);
  }
  {
    // And out to the status line, which is where a person meets it.
    Editor ed;
    ResetToOriginal(ed.doc.table, kBadLine + "\n");
    ed.doc.selections.Set(Selection{});
    RunSearch(ed, kRunaway);
    EXPECT_TRUE(ed.status.find("bad pattern") != std::string::npos);
    EXPECT_TRUE(ed.status.find("match limit") != std::string::npos);
  }

  TEST_CASE("search: a slow pattern over many lines gives up on a clock");
  {
    // The step cap bounds one line; it says nothing about a document. Twelve
    // x's is well under the cap -- it takes about eighteen to reach it -- and
    // costs ~0.04 ms a line, so this sweep is around a second of work, six
    // times the 150 ms the loops are allowed. The only thing that can stop it
    // is the wall clock, which is what makes this a test of the budget and not
    // of the cap.
    std::string text;
    for (int i = 0; i < 24000; ++i) text += std::string(12, 'x') + "z\n";
    PieceTable table = MakeTable(text);
    std::vector<Interval> hits;
    std::string error;
    EXPECT_FALSE(FindInDocument(table, kRunaway, Interval(0, DocLength(table)), hits, error));
    EXPECT_TRUE(error.find("gave up") != std::string::npos);

    // One budget spans both sweeps of the wrapping search, so wrapping cannot
    // spend the promise twice.
    std::optional<Interval> landed;
    std::string first_error;
    EXPECT_FALSE(FindFirstInDocument(table, kRunaway, DocLength(table) / 2, landed, first_error));
    EXPECT_FALSE(landed.has_value());
    EXPECT_TRUE(first_error.find("gave up") != std::string::npos);
  }

  TEST_CASE("search: ordinary patterns notice neither guard");
  {
    PieceTable table = MakeTable("alpha beta\ngamma alpha\ndelta\n");
    const Interval whole(0, DocLength(table));
    struct Case {
      const char* pattern;
      std::size_t hits;
    };
    for (const Case& c : {Case{"alpha", 2}, Case{"^alpha", 1}, Case{"\\w+", 5}, Case{"zeta", 0}}) {
      std::vector<Interval> hits;
      std::string error = "sentinel";
      EXPECT_TRUE(FindInDocument(table, c.pattern, whole, hits, error));
      EXPECT_EQ(hits.size(), c.hits);
      EXPECT_EQ(error, std::string("sentinel"));
    }

    std::optional<Interval> landed;
    std::string error = "sentinel";
    EXPECT_TRUE(FindFirstInDocument(table, "alpha", 5, landed, error));
    EXPECT_TRUE(landed.has_value() && (landed->front() == Index{17}));
    EXPECT_EQ(error, std::string("sentinel"));
  }
}

}  // namespace koi
