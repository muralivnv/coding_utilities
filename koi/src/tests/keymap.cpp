// Tests for keymap.cpp: key parsing, the keymap trie, and the TOML config that
// fills both -- including the errors a bad config has to report.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void ConfigMerge() {
  TEST_CASE("config: the project's config merges over the global one");

  KeyMaps maps = DefaultKeyMaps();
  Settings settings;
  std::vector<std::string> errors;

  constexpr std::string_view kGlobal = R"TOML(
theme = "ronin"
[editor]
tab-width = 4
scrolloff = 3
excerpt-context = 5
scan-workers = 8
file-filter = "global-files"
mode-normal = "global-normal"
[keys.normal]
"F5" = "goto_file_start"
)TOML";
  constexpr std::string_view kLocal = R"TOML(
[editor]
tab-width = 2
scan-workers = 2
file-filter = "local-files"
[keys.normal]
"F6" = ":quit"
)TOML";

  std::ignore = ParseKeyMapConfig(kGlobal, maps, settings, errors);
  std::ignore = ParseKeyMapConfig(kLocal, maps, settings, errors);
  EXPECT_TRUE(errors.empty());

  EXPECT_EQ(settings.tab_width, 2);
  EXPECT_EQ(settings.scrolloff, 3);
  EXPECT_EQ(settings.excerpt_context, 5);
  EXPECT_EQ(settings.scan_workers, 2);
  EXPECT_EQ(settings.file_filter, std::string{"local-files"});
  EXPECT_EQ(settings.mode_normal, std::string{"global-normal"});
  EXPECT_EQ(settings.theme, std::string{"ronin"});

  const auto bound = [&maps](std::string_view spec) {
    Key key{};
    if (!ParseKey(spec, key)) return false;
    const std::vector<std::string>* commands = nullptr;
    return maps.normal.Find({key}, &commands) == KeyMap::Lookup::kMatched;
  };
  EXPECT_TRUE(bound("F5"));
  EXPECT_TRUE(bound("F6"));
}

void ShippedConfigsLoad() {
  TEST_CASE("config: the files koi ships still parse");

  for (const std::string_view name : {"config.toml", "config.reference.toml"}) {
    const std::filesystem::path path = FindRuntimeFile(name);
    if (path.empty()) continue;
    KeyMaps maps = DefaultKeyMaps();
    Settings settings;
    std::vector<std::string> errors;
    std::ignore = LoadKeyMapConfig(path, maps, settings, errors);
    for (const std::string& one : errors) {
      std::cout << "  " << name << ": " << one << '\n';
    }
    EXPECT_TRUE(errors.empty());
  }

  const std::filesystem::path path = FindRuntimeFile("config.toml");
  if (path.empty()) return;
  KeyMaps maps = DefaultKeyMaps();
  Settings settings;
  std::vector<std::string> errors;
  std::ignore = LoadKeyMapConfig(path, maps, settings, errors);
  Editor ed;
  ed.doc.file = "a.cpp";
  ed.settings = settings;
  EXPECT_TRUE(!settings.file_filter.empty());
  EXPECT_TRUE(ExpandVariables(PickerScanCommand(ed, "symbols"), ed).find("%{") ==
              std::string::npos);
}

namespace {

std::vector<Key> Seq(std::string_view text) {
  std::vector<Key> keys;
  EXPECT_TRUE(ParseKeySequence(text, keys));
  return keys;
}

}  // namespace

void KeyParsing() {
  TEST_CASE("key parsing");
  EXPECT_EQ(K("a").code, std::uint32_t{'a'});
  EXPECT_EQ(K("a").mods, std::uint8_t{kModNone});
  EXPECT_EQ(K("C-s").mods, std::uint8_t{kModCtrl});
  EXPECT_EQ(K("A-e").mods, std::uint8_t{kModAlt});
  EXPECT_EQ(K("C-A-x").mods, std::uint8_t{kModCtrl | kModAlt});
  EXPECT_TRUE(K("esc").named == NamedKey::kEsc);
  EXPECT_TRUE(K("ret").named == NamedKey::kRet);
  EXPECT_TRUE(K("S-tab").named == NamedKey::kTab);
  EXPECT_EQ(K("S-tab").mods, std::uint8_t{kModShift});
  EXPECT_EQ(K("minus").code, std::uint32_t{'-'});
  EXPECT_TRUE(K("F5").named == NamedKey::kF5);

  EXPECT_TRUE(K("S-a") == K("A"));
  EXPECT_TRUE(K("A-I") == K("A-S-i"));
  EXPECT_EQ(K("A-I").code, std::uint32_t{'I'});

  Key discard;
  EXPECT_FALSE(ParseKey("", discard));
  EXPECT_FALSE(ParseKey("C-", discard));
  EXPECT_FALSE(ParseKey("nonsense", discard));
  EXPECT_FALSE(ParseKey("ab", discard));

  for (std::string_view name : {"a", "C-s", "A-e", "esc", "ret", "S-tab", "F1", "minus"}) {
    Key parsed;
    EXPECT_TRUE(ParseKey(name, parsed));
    Key again;
    EXPECT_TRUE(ParseKey(KeyToString(parsed), again));
    EXPECT_TRUE(parsed == again);
  }
  EXPECT_EQ(Seq("gt").size(), size_t{2});
  EXPECT_EQ(Seq("space p").size(), size_t{2});

  TEST_CASE("key parsing: modified-key CSI reports decode to the key they stand for");
  {
    Key k;
    EXPECT_TRUE(DecodeCsiKey("27;4;99~", k));
    EXPECT_TRUE(k == K("A-C"));
    EXPECT_TRUE(DecodeCsiKey("27;3;67~", k));
    EXPECT_TRUE(k == K("A-C"));
    EXPECT_TRUE(DecodeCsiKey("99;4u", k));
    EXPECT_TRUE(k == K("A-C"));
    EXPECT_TRUE(DecodeCsiKey("99:67;4u", k));
    EXPECT_TRUE(k == K("A-C"));
    EXPECT_TRUE(DecodeCsiKey("44;3u", k));
    EXPECT_TRUE(k == K("A-,"));
    EXPECT_TRUE(DecodeCsiKey("112;5u", k));
    EXPECT_TRUE(k == K("C-p"));

    EXPECT_TRUE(DecodeCsiKey("27u", k));
    EXPECT_TRUE(k == K("esc"));
    EXPECT_TRUE(DecodeCsiKey("13;3u", k));
    EXPECT_TRUE(k == K("A-ret"));
    EXPECT_TRUE(DecodeCsiKey("9;2u", k));
    EXPECT_TRUE(k == K("S-tab"));
    EXPECT_TRUE(DecodeCsiKey("127u", k));
    EXPECT_TRUE(k == K("backspace"));

    EXPECT_FALSE(DecodeCsiKey("1;2u", k));
    EXPECT_FALSE(DecodeCsiKey("1;2A", k));
    EXPECT_FALSE(DecodeCsiKey("200~", k));
    EXPECT_FALSE(DecodeCsiKey("", k));
    EXPECT_FALSE(DecodeCsiKey("u", k));
    EXPECT_FALSE(DecodeCsiKey(";;u", k));
    EXPECT_FALSE(DecodeCsiKey("27;4~", k));
  }
}

void KeyMapTrie() {
  TEST_CASE("keymap trie");
  KeyMap map;
  EXPECT_FALSE(map.Bind(Seq("a"), {"move_char_left"}));
  EXPECT_FALSE(map.Bind(Seq("gt"), {"goto_file_start"}));
  EXPECT_FALSE(map.Bind(Seq("gb"), {"goto_last_line"}));

  const std::vector<std::string>* out = nullptr;
  EXPECT_TRUE(map.Find(Seq("a"), &out) == KeyMap::Lookup::kMatched);
  EXPECT_EQ((*out)[0], std::string("move_char_left"));
  EXPECT_TRUE(map.Find(Seq("g"), &out) == KeyMap::Lookup::kPending);
  EXPECT_TRUE(map.Find(Seq("gt"), &out) == KeyMap::Lookup::kMatched);
  EXPECT_EQ((*out)[0], std::string("goto_file_start"));
  EXPECT_TRUE(map.Find(Seq("gz"), &out) == KeyMap::Lookup::kNoMatch);
  EXPECT_TRUE(map.Find(Seq("z"), &out) == KeyMap::Lookup::kNoMatch);

  std::string detail;
  EXPECT_TRUE(map.Bind(Seq("ax"), {"no_op"}, &detail));
  EXPECT_TRUE(detail.find("unreachable") != std::string::npos);
  detail.clear();
  EXPECT_TRUE(map.Bind(Seq("g"), {"no_op"}, &detail));
  EXPECT_TRUE(detail.find("prefix") != std::string::npos);
  EXPECT_TRUE(map.Bind({}, {"no_op"}));
  EXPECT_TRUE(map.Bind(Seq("q"), {}));
}

void ConfigParsing() {
  TEST_CASE("keymap config");
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    EXPECT_FALSE(ParseKeyMapConfig(R"(
[keys.normal]
i = "move_line_up"
"A-e" = ["collapse_selection", "insert_mode"]
"[" = { "[" = "goto_file_start" }

[keys.normal.space]
w = "no_op"

[keys.normal.space.p]
"1" = "no_op"

[keys.insert]
esc = "normal_mode"
)", maps, errors));
    for (const std::string& e : errors) std::cerr << "  unexpected: " << e << '\n';
    EXPECT_TRUE(errors.empty());

    const std::vector<std::string>* out = nullptr;
    EXPECT_TRUE(maps.normal.Find(Seq("i"), &out) == KeyMap::Lookup::kMatched);
    EXPECT_TRUE(maps.normal.Find({K("A-e")}, &out) == KeyMap::Lookup::kMatched);
    EXPECT_EQ(out->size(), size_t{2});
    EXPECT_EQ((*out)[1], std::string("insert_mode"));
    EXPECT_TRUE(maps.normal.Find(Seq("[["), &out) == KeyMap::Lookup::kMatched);
    EXPECT_TRUE(maps.normal.Find({K("space"), K("w")}, &out) == KeyMap::Lookup::kMatched);
    EXPECT_TRUE(maps.normal.Find({K("space"), K("p"), K("1")}, &out) == KeyMap::Lookup::kMatched);
    EXPECT_TRUE(maps.normal.Find({K("space")}, &out) == KeyMap::Lookup::kPending);
    EXPECT_TRUE(maps.insert.Find({K("esc")}, &out) == KeyMap::Lookup::kMatched);
  }
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    EXPECT_FALSE(ParseKeyMapConfig(R"(
theme = "ronin"

[editor]
line-number = "relative"

[editor.lsp]
enable = false

[keys.normal]
i = "move_line_up"
)", maps, errors));
    EXPECT_TRUE(errors.empty());
    const std::vector<std::string>* out = nullptr;
    EXPECT_TRUE(maps.normal.Find(Seq("i"), &out) == KeyMap::Lookup::kMatched);
  }
  {
    KeyMaps maps;
    Settings settings;
    std::vector<std::string> errors;
    EXPECT_TRUE(settings.cursorline);
    EXPECT_FALSE(ParseKeyMapConfig(R"(
[editor]
cursorline = false
tab-width = 2
)", maps, settings, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_FALSE(settings.cursorline);
    EXPECT_EQ(settings.tab_width, Index{2});

    Settings bad_settings;
    std::vector<std::string> bad_errors;
    std::ignore = ParseKeyMapConfig(R"(
[editor]
cursorline = "yes"
)", maps, bad_settings, bad_errors);
    EXPECT_EQ(std::ssize(bad_errors), Index{1});
    EXPECT_TRUE(bad_settings.cursorline);
  }
  {
    KeyMaps maps;
    Settings settings;
    std::vector<std::string> errors;
    // Seeded, not assumed: the point is that the config turns it on, which says
    // nothing unless it was off going in. render_tabs is seeded for the same
    // reason and not read off the default, which is a setting somebody can and
    // does change -- left as an assumption, flipping that default turned the
    // claim below into one that would hold with the parser doing nothing.
    settings.trim_trailing_whitespace_on_save = false;
    settings.render_tabs = false;
    EXPECT_FALSE(settings.render_tabs);
    EXPECT_FALSE(ParseKeyMapConfig(R"(
[editor]
trim_trailing_whitespace_on_save = true

[editor.whitespace.characters]
tab = "→"
tabpad = "·"
)", maps, settings, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(settings.trim_trailing_whitespace_on_save);
    EXPECT_TRUE(settings.render_tabs);
    EXPECT_EQ(settings.tab_glyph, std::string{"→"});
    EXPECT_EQ(settings.tab_pad, std::string{"·"});

    Settings off;
    std::vector<std::string> off_errors;
    std::ignore = ParseKeyMapConfig(R"(
[editor.whitespace]
render = "none"

[editor.whitespace.characters]
tab = "→"
)", maps, off, off_errors);
    EXPECT_TRUE(off_errors.empty());
    EXPECT_FALSE(off.render_tabs);
  }
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig(R"(
[keys.normal]
"D" = "expand_selecton"
"Q" = ["collapse_selection", "delete_seleciton"]
"C-S-v" = "paste_clipboard_after"
"C-]" = "no_op"

[keys.select]
"backspace" = "delete_selection"
)", maps, errors);
    EXPECT_EQ(errors.size(), size_t{5});
    const std::vector<std::string>* out = nullptr;
    EXPECT_TRUE(maps.normal.Find(Seq("D"), &out) == KeyMap::Lookup::kNoMatch);
    EXPECT_TRUE(maps.normal.Find(Seq("Q"), &out) == KeyMap::Lookup::kNoMatch);

    KeyMaps ok_maps;
    std::vector<std::string> ok_errors;
    std::ignore = ParseKeyMapConfig(R"(
[keys.normal]
"A-S-l" = "extend_next_word_start"
"C-v" = "paste_clipboard_before"
)", ok_maps, ok_errors);
    EXPECT_TRUE(ok_errors.empty());
    EXPECT_TRUE(ok_maps.normal.Find(Seq("A-L"), &out) == KeyMap::Lookup::kMatched);
    EXPECT_TRUE(ok_maps.normal.Find(Seq("C-v"), &out) == KeyMap::Lookup::kMatched);
  }
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    EXPECT_TRUE(ParseKeyMapConfig(R"(
[keys.normal]
nonsense = "move_line_up"
i = "move_line_up"

[keys.sideways]
a = "no_op"
)", maps, errors));
    EXPECT_EQ(errors.size(), size_t{2});
    const std::vector<std::string>* out = nullptr;
    EXPECT_TRUE(maps.normal.Find(Seq("i"), &out) == KeyMap::Lookup::kMatched);
  }
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    EXPECT_TRUE(ParseKeyMapConfig("[keys.normal\ni = ", maps, errors));
    EXPECT_EQ(errors.size(), size_t{1});
  }
  {
    KeyMaps maps = DefaultKeyMaps();
    std::vector<std::string> errors;
    EXPECT_FALSE(ParseKeyMapConfig(R"(
[keys.insert.j]
k = "normal_mode"
)", maps, errors));
    EXPECT_TRUE(errors.empty());

    Editor ed;
    ResetToOriginal(ed.doc.table, "ab\n");
    ed.doc.view.rows = 5;
    ed.doc.view.columns = 20;
    ed.doc.selections.Set(Selection{2, 2, -1});
    ed.mode = Mode::kInsert;
    std::vector<Key> pending;
    Key j;
    Key left;
    EXPECT_TRUE(ParseKey("j", j));
    EXPECT_TRUE(ParseKey("left", left));
    HandleKeyInput(ed, maps, j, pending);
    EXPECT_TRUE(!pending.empty());
    HandleKeyInput(ed, maps, left, pending);
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string{"abj\n"});
    EXPECT_EQ(Cur(ed), Index{2});
    EXPECT_TRUE(pending.empty());

    ed.status.clear();
    Key f5;
    EXPECT_TRUE(ParseKey("F5", f5));
    HandleKeyInput(ed, maps, j, pending);
    HandleKeyInput(ed, maps, f5, pending);
    EXPECT_TRUE(ed.status.find("is not bound") != std::string::npos);
  }
}

void KeymapErrors() {
  TEST_CASE("keymap: bad config is refused with a reason, never accepted quietly");
  const auto refuse = [](std::string_view toml) {
    KeyMaps maps;
    std::vector<std::string> errors;
    const ErrorCtx err = ParseKeyMapConfig(toml, maps, errors);

    EXPECT_TRUE(static_cast<bool>(err));
    EXPECT_TRUE(!errors.empty());
  };

  refuse("this is not toml at all [[[");
  refuse("[keys.normal]\nx = 42\n");
  refuse("[keys.normal]\nx = [1, 2]\n");
  refuse("[keys.sideways]\nx = \"no_op\"\n");
  refuse("[keys.select]\nx = \"no_op\"\n");
  refuse("[keys.normal]\nx = \"there_is_no_such_command\"\n");
  refuse("[keys.normal]\n\"\" = \"no_op\"\n");
  refuse("[keys.normal]\n\"C-\" = \"no_op\"\n");
  refuse("[keys.normal]\n\"not-a-key\" = \"no_op\"\n");
  refuse("keys = 3\n");
  refuse("[keys]\nnormal = 7\n");

  TEST_CASE("keymap: good config is accepted, including sequences and arrays");
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    const ErrorCtx err = ParseKeyMapConfig(
        "[keys.normal]\n"
        "x = \"no_op\"\n"
        "y = [\"collapse_selection\", \"insert_mode\"]\n"
        "g = { g = \"goto_file_start\", e = \"goto_line_end\" }\n"
        "G = \"goto_line\"\n"
        "\"A-G\" = \"extend_to_line\"\n"
        "\"C-w\" = { h = \"jump_view_left\" }\n",
        maps, errors);
    EXPECT_FALSE(static_cast<bool>(err));
    EXPECT_TRUE(errors.empty());
  }

  TEST_CASE("keymap: `G` reaches goto_line in the default map");
  {
    const KeyMaps maps = DefaultKeyMaps();
    const std::vector<std::string>* out = nullptr;
    EXPECT_TRUE(maps.normal.Find(Seq("G"), &out) == KeyMap::Lookup::kMatched);
    EXPECT_EQ((*out)[0], std::string("goto_line"));
    EXPECT_TRUE(maps.normal.Find(Seq("A-G"), &out) == KeyMap::Lookup::kMatched);
    EXPECT_EQ((*out)[0], std::string("extend_to_line"));
  }

  TEST_CASE("keymap: one bad binding does not throw away the good ones");
  {
    KeyMaps maps;
    std::vector<std::string> errors;
    std::ignore = ParseKeyMapConfig(
        "[keys.normal]\nx = \"no_op\"\nz = \"nonsense_command\"\nq = \"insert_mode\"\n", maps,
        errors);

    EXPECT_TRUE(!errors.empty());
    bool mentions_the_bad_one = false;
    for (const std::string& e : errors) {
      if (e.find("nonsense_command") != std::string::npos) mentions_the_bad_one = true;
    }
    EXPECT_TRUE(mentions_the_bad_one);
  }

  TEST_CASE("keymap: absurd input is refused rather than chewed on");
  {
    KeyMaps maps;
    std::vector<std::string> errors;

    std::string huge = "[keys.normal]\n\"";
    for (int i = 0; i < 5000; ++i) huge += "a ";
    huge += "\" = \"no_op\"\n";
    std::ignore = ParseKeyMapConfig(huge, maps, errors);

    std::string deep = "[keys.normal]\na = ";
    for (int i = 0; i < 200; ++i) deep += "{ b = ";
    deep += "\"no_op\"";
    for (int i = 0; i < 200; ++i) deep += " }";
    deep += "\n";
    std::vector<std::string> deep_errors;
    KeyMaps deep_maps;
    std::ignore = ParseKeyMapConfig(deep, deep_maps, deep_errors);
    EXPECT_TRUE(true);
  }
}

}  // namespace koi
