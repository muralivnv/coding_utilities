// Tests for shell.cpp: quoting, expansion, and running a command from the
// editor.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void ShellIntegration() {
  TEST_CASE("shell quoting and expansion");
  EXPECT_EQ(ShellQuote("plain"), std::string("plain"));
  EXPECT_EQ(ShellQuote("/path/to/file.cpp"), std::string("/path/to/file.cpp"));
  EXPECT_EQ(ShellQuote(""), std::string("''"));
  EXPECT_EQ(ShellQuote("two words"), std::string("'two words'"));
  EXPECT_EQ(ShellQuote("$(rm -rf /)"), std::string("'$(rm -rf /)'"));
  EXPECT_EQ(ShellQuote("a`b`"), std::string("'a`b`'"));
  EXPECT_EQ(ShellQuote("a;b"), std::string("'a;b'"));
  EXPECT_EQ(ShellQuote("it's"), std::string("'it'\"'\"'s'"));
  EXPECT_EQ(ShellQuote("x' ; rm -rf / ; '"), std::string("'x'\"'\"' ; rm -rf / ; '\"'\"''"));

  TEST_CASE("shell variable expansion");
  Editor ed;
  ResetToOriginal(ed.doc.table, "first line\nsecond line here\nthird\n");
  ed.doc.file = "/tmp/some dir/file.cpp";
  ed.doc.view.rows = 10;
  ed.doc.view.columns = 40;
  ed.doc.selections.Set(Selection{LineStart(ed.doc.table, 1), LineStart(ed.doc.table, 1) + 6, -1});

  EXPECT_EQ(ExpandVariables("%{buffer_name}", ed), std::string("'/tmp/some dir/file.cpp'"));
  EXPECT_EQ(ExpandVariables("%{basename}", ed), std::string("file.cpp"));
  EXPECT_EQ(ExpandVariables("%{extension}", ed), std::string("cpp"));
  EXPECT_EQ(ExpandVariables("%{cursor_line}", ed), std::string("2"));
  EXPECT_EQ(ExpandVariables("%{linenumber}", ed), std::string("2"));
  EXPECT_EQ(ExpandVariables("%{selection}", ed), std::string("second"));
  EXPECT_EQ(ExpandVariables("navc record-edit %{buffer_name} --line %{cursor_line} --col %{cursor_column}", ed),
            std::string("navc record-edit '/tmp/some dir/file.cpp' --line 2 --col 6"));
  EXPECT_EQ(ExpandVariables("x %{nope} y", ed), std::string("x %{nope} y"));
  EXPECT_EQ(ExpandVariables("100% done", ed), std::string("100% done"));
  EXPECT_EQ(ExpandVariables("%{unterminated", ed), std::string("%{unterminated"));

  ResetToOriginal(ed.doc.table, "$(touch /tmp/koi-pwned)\n");
  ed.doc.selections.Set(Selection{0, 23, -1});
  const std::string cmd = ExpandVariables("echo %{selection}", ed);
  EXPECT_TRUE(cmd.find("'$(touch /tmp/koi-pwned)'") != std::string::npos);

  TEST_CASE("shell command modes");
  Editor sh;
  ResetToOriginal(sh.doc.table, "one\ntwo\nthree\n");
  sh.doc.view.rows = 10;
  sh.doc.view.columns = 40;

  sh.doc.selections.Set(Selection{0, 3, -1});
  RunShellCommand(sh, "tr a-z A-Z", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("ONE\ntwo\nthree\n"));
  RunCommands(sh, {"undo"});
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("one\ntwo\nthree\n"));

  sh.doc.selections.Set(Selection{0, 3, -1});
  RunShellCommand(sh, "cat", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("one\ntwo\nthree\n"));

  sh.doc.selections.Set(Selection{0, 3, -1});
  RunShellCommand(sh, "printf X", ShellMode::kInsertOutput);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("Xone\ntwo\nthree\n"));
  RunCommands(sh, {"undo"});
  sh.doc.selections.Set(Selection{0, 3, -1});
  RunShellCommand(sh, "printf X", ShellMode::kAppendOutput);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("oneX\ntwo\nthree\n"));
  RunCommands(sh, {"undo"});

  const std::string before = AssembleDocContents(sh.doc.table);
  sh.doc.selections.Set(Selection{0, 3, -1});
  sh.status.clear();
  RunShellCommand(sh, "exit 3", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), before);
  EXPECT_TRUE(sh.status.find("exited 3") != std::string::npos);

  sh.doc.selections.Set(Selection{0, 3, -1});
  sh.status.clear();
  RunShellCommand(sh, "printf '\\xff\\xfe'", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), before);
  EXPECT_TRUE(sh.status.find("invalid UTF-8") != std::string::npos);

  sh.doc.selections.Set(Selection{0, 3, -1});
  RunShellCommand(sh, "cat > /dev/null", ShellMode::kPipeTo);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), before);

  sh.status.clear();
  RunShellCommand(sh, "   ", ShellMode::kPipeTo);
  EXPECT_TRUE(sh.status.find("empty shell command") != std::string::npos);

  ResetToOriginal(sh.doc.table, "aa\nbb\n");
  sh.doc.selections.Replace(sh.doc.table, {{0, 2, -1}, {3, 5, -1}});
  RunShellCommand(sh, "tr -d '\\n' | tr a-z A-Z", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("AABB\nAABB\n"));
  RunCommands(sh, {"undo"});
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("aa\nbb\n"));

  // A pipe over several selections is all-or-nothing. Output starting with a
  // combining mark is the case that used to split it: replacing the second
  // selection moved the first one's end inside the grapheme it had just made,
  // the edit on the first was refused, and the half-edited document was left
  // with the pre-edit selection set still on it.
  ResetToOriginal(sh.doc.table, "ab\n");
  sh.doc.selections.Replace(sh.doc.table, {{0, 1, -1}, {1, 2, -1}});
  sh.status.clear();
  RunShellCommand(sh, "printf '\\xcc\\x81x'", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("\xcc\x81x\xcc\x81x\n"));
  EXPECT_EQ(std::string{sh.status}, std::string{});
  EXPECT_EQ(EditorInvariants(sh), std::string{});
  EXPECT_TRUE(sh.doc.modified);
  RunCommands(sh, {"undo"});
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("ab\n"));

  // The refusal path is the other half of the contract: when Apply does say no,
  // it says so before writing, so the document is exactly as it was.
  ResetToOriginal(sh.doc.table, "ab\n");
  sh.doc.selections.Replace(sh.doc.table, {{0, 1, -1}, {1, 2, -1}});
  sh.status.clear();
  RunShellCommand(sh, "printf '\\xff'", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("ab\n"));
  EXPECT_EQ(EditorInvariants(sh), std::string{});

  TEST_CASE("a replace over several selections is all or nothing");
  // The shell pipe was not the only loop that edited one selection at a time
  // and returned on the first refusal. Every one of these replaces a range per
  // selection, and text beginning with a combining mark turns the *previous*
  // selection's end into a position inside a grapheme -- which Apply then
  // refuses, after the earlier iterations have already been written. Touching
  // selections are what make the two positions the same byte.
  {
    // Registers, via replace_with_yanked.
    Editor rwy;
    ResetToOriginal(rwy.doc.table, "ab\n");
    rwy.doc.selections.Replace(rwy.doc.table, {{0, 1, -1}, {1, 2, -1}});
    rwy.registers.assign(1, std::string("\xcc\x81y"));
    rwy.status.clear();
    RunCommands(rwy, {"replace_with_yanked"});
    EXPECT_EQ(AssembleDocContents(rwy.doc.table), std::string("\xcc\x81y\xcc\x81y\n"));
    EXPECT_EQ(std::string{rwy.status}, std::string{});
    EXPECT_EQ(EditorInvariants(rwy), std::string{});
    RunCommands(rwy, {"undo"});
    EXPECT_EQ(AssembleDocContents(rwy.doc.table), std::string("ab\n"));

    // A grapheme off the keyboard, via `r`. This one is on a default binding.
    Editor rec;
    ResetToOriginal(rec.doc.table, "ab\n");
    rec.doc.selections.Replace(rec.doc.table, {{0, 1, -1}, {1, 2, -1}});
    rec.status.clear();
    RunCommands(rec, {"replace"});
    ApplyPendingChar(rec, "\xcc\x81");
    EXPECT_EQ(AssembleDocContents(rec.doc.table), std::string("\xcc\x81\xcc\x81\n"));
    EXPECT_EQ(std::string{rec.status}, std::string{});
    EXPECT_EQ(EditorInvariants(rec), std::string{});
    RunCommands(rec, {"undo"});
    EXPECT_EQ(AssembleDocContents(rec.doc.table), std::string("ab\n"));

    // Document text, via rotate. The mark has to already be a cluster of its
    // own for a selection to start on it, so it sits at the start of the file.
    Editor rot;
    ResetToOriginal(rot.doc.table, "\xcc\x81x\n");
    rot.doc.selections.Replace(rot.doc.table, {{0, 2, -1}, {2, 3, -1}});
    rot.status.clear();
    RunCommands(rot, {"rotate_selection_contents_forward"});
    EXPECT_EQ(AssembleDocContents(rot.doc.table), std::string("x\xcc\x81\n"));
    EXPECT_EQ(std::string{rot.status}, std::string{});
    EXPECT_EQ(EditorInvariants(rot), std::string{});
    RunCommands(rot, {"undo"});
    EXPECT_EQ(AssembleDocContents(rot.doc.table), std::string("\xcc\x81x\n"));
  }

  TEST_CASE("replace_with_yanked, reached only by keystrokes");
  // The same defect with nothing set up behind the editor's back: the register
  // is filled by yanking a lone combining mark off the first line, and the
  // touching selections come from a select-regex split.
  {
    Editor kb;
    ResetToOriginal(kb.doc.table, "\xcc\x81\nab\n");
    kb.doc.view.rows = 10;
    kb.doc.view.columns = 40;
    kb.doc.selections.Set(Selection{0, 0, -1});
    RunCommands(kb, {"extend_line"});  // x
    RunCommands(kb, {"yank"});         // y
    EXPECT_EQ(kb.registers.size(), size_t{1});
    EXPECT_EQ(kb.registers.front(), std::string("\xcc\x81\n"));
    RunCommands(kb, {"move_line_down"});  // j
    RunCommands(kb, {"extend_line"});     // x
    PromptOpen(kb, PromptKind::kSelectRegex);
    PromptInsert(kb, "[ab]");  // s [ab] <CR>
    PromptSubmit(kb);
    EXPECT_EQ(kb.doc.selections.Ranges().size(), size_t{2});
    kb.status.clear();
    RunCommands(kb, {"replace_with_yanked"});
    EXPECT_EQ(AssembleDocContents(kb.doc.table),
              std::string("\xcc\x81\n\xcc\x81\n\xcc\x81\n\n"));
    EXPECT_EQ(std::string{kb.status}, std::string{});
    EXPECT_EQ(EditorInvariants(kb), std::string{});
  }

  ResetToOriginal(sh.doc.table, "hello world\n");
  sh.doc.selections.Set(Selection{0, 5, -1});
  RunShellCommand(sh, "printf X", ShellMode::kPipe);
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string("X world\n"));
  EXPECT_EQ(sh.doc.selections.Primary().From(), Index{0});
  EXPECT_EQ(sh.doc.selections.Primary().To(), Index{1});
  RunCommands(sh, {"delete_selection"});
  EXPECT_EQ(AssembleDocContents(sh.doc.table), std::string(" world\n"));
}

}  // namespace koi
