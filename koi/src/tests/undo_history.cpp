// Tests for undo and redo as the editor drives them: cursor notes, grouped
// edits, the trimmed history base, and the per-file budget. The piece table's
// own undo chain is tested in piece_doc.cpp.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

void JournalIsIndexableByRevision() {
  TEST_CASE("journal: a revision names exactly one edit");
  const Scratch scratch{"koi-journal"};
  const std::filesystem::path file = scratch.Write(
      "j.cpp", "int a = 1;\nint b = 2;\nint c = 3;\nint d = 4;\nint e = 5;\n");

  const auto lines_up = [](const Editor& ed) {
    const PieceTable& t = ed.doc.table;
    return std::ssize(t.journal) == (t.revision - t.journal_base);
  };

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, file.string()));
  EXPECT_TRUE(lines_up(ed));

  ed.doc.selections.Set(Selection{0, 0, -1});
  TypeInto(ed, 'x');
  EXPECT_TRUE(lines_up(ed));

  ed.doc.selections.Set(Selection{0, 0, -1});
  AddCursorVertically(ed, true);
  AddCursorVertically(ed, true);
  EXPECT_EQ(ed.doc.selections.Size(), std::size_t{3});
  TypeInto(ed, 'y');
  EXPECT_TRUE(lines_up(ed));

  for (int i = 0; i < 6; ++i) {
    TypeInto(ed, static_cast<char>('a' + i));
    EXPECT_TRUE(lines_up(ed));
  }

  RunCommands(ed, {"undo"});
  EXPECT_TRUE(lines_up(ed));
  RunCommands(ed, {"redo"});
  EXPECT_TRUE(lines_up(ed));

  ed.doc.selections.Set(Selection{0, 0, -1});
  AddCursorVertically(ed, true);
  AddCursorVertically(ed, true);
  RunCommands(ed, {"delete_selection"});
  EXPECT_TRUE(lines_up(ed));

  EXPECT_TRUE(ReloadDocument(ed));
  EXPECT_TRUE(lines_up(ed));
  EXPECT_EQ(EditorInvariants(ed), std::string{});
}

void JournalIsBounded() {
  TEST_CASE("journal: a long session does not grow it without end");
  {
    PieceTable table;
    ResetToOriginal(table, "aaa\nbbb\nccc\n");
    const Index base_at_load = table.journal_base;
    const std::size_t edits = kMaxJournalEntries + 4096;
    bool all_applied = true;
    for (std::size_t i = 0; i < edits; ++i) {
      if (Insert("z", 0, table)) all_applied = false;
    }
    EXPECT_TRUE(all_applied);
    EXPECT_TRUE(table.journal.size() <= kMaxJournalEntries);
    EXPECT_TRUE(table.journal_base > base_at_load);
    EXPECT_EQ(std::ssize(table.journal), table.revision - table.journal_base);

    for (int i = 0; (i < 8) && CanUndo(table); ++i) ExpectOk(Undo(table), "undo past the cap");
    EXPECT_TRUE(table.journal.size() <= kMaxJournalEntries);
    EXPECT_EQ(std::ssize(table.journal), table.revision - table.journal_base);
    for (int i = 0; (i < 8) && CanRedo(table); ++i) ExpectOk(Redo(table), "redo past the cap");
    EXPECT_TRUE(table.journal.size() <= kMaxJournalEntries);
    EXPECT_EQ(std::ssize(table.journal), table.revision - table.journal_base);
  }

  TEST_CASE("journal: a pane stashed before the base still comes back in range");
  {
    const Scratch scratch{"koi-journal-trim"};
    const std::filesystem::path file = scratch.Write("t.txt", NumberedLines(200));

    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, file.string()));
    SplitWindow(ed, true);
    const Index at = LineStart(ed.doc.table, 150);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
    RunCommands(ed, {"jump_view_next"});

    PieceTable& table = ed.doc.table;
    table.journal.clear();
    table.journal_base = table.revision;
    ed.doc.selections.Set(MinWidth1(table, Selection{0, 0, -1}));
    RunCommands(ed, {"extend_to_file_end", "delete_selection"});

    RunCommands(ed, {"jump_view_next"});
    const Index length = DocLength(ed.doc.table);
    for (const Selection& s : ed.doc.selections.Ranges()) {
      EXPECT_TRUE((s.anchor >= 0) && (s.anchor <= length));
      EXPECT_TRUE((s.head >= 0) && (s.head <= length));
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

void UndoRestoresTheCursorOfTheStepItUndoes() {
  TEST_CASE("undo: the cursor goes back to where the undone step started");
  const Scratch scratch{"koi-undo-cursor"};
  std::string many;
  for (int i = 0; i < 12; ++i) many += "line" + std::to_string(i) + "\n";
  const std::filesystem::path file = scratch.Write("u.txt", many);

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, file.string()));

  const auto cursor_line = [&] {
    return LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary()));
  };
  const auto put_on_line = [&](Index line) {
    const Index at = LineStart(ed.doc.table, line);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
  };

  put_on_line(0);
  TypeInto(ed, 'Z');
  EXPECT_EQ(cursor_line(), Index{0});

  put_on_line(8);
  ed.registers.clear();
  ed.registers.push_back("PASTED\n");
  RunCommands(ed, {"paste_after"});
  RunCommands(ed, {"undo"});
  EXPECT_EQ(cursor_line(), Index{8});

  put_on_line(4);
  RunCommands(ed, {"open_below"});
  RunCommands(ed, {"undo"});
  EXPECT_EQ(cursor_line(), Index{4});

  put_on_line(10);
  TypeInto(ed, 'Q');
  RunCommands(ed, {"undo"});
  EXPECT_EQ(cursor_line(), Index{10});

  put_on_line(2);
  RunCommands(ed, {"goto_file_start"});
  put_on_line(7);
  RunCommands(ed, {"paste_after"});
  RunCommands(ed, {"undo"});
  EXPECT_EQ(cursor_line(), Index{7});

  EXPECT_EQ(EditorInvariants(ed), std::string{});
}

// Redo has to hand back the cursors the step recorded, the same way undo does.
// UndoOrRedo used to gate the recorded state behind `if (undo)`, so the
// cursors_after that IndentBy and EditThroughCursors go out of their way to
// record were discarded and the edit-span fallback ran in their place. Apply
// folds a grouped transaction into one revision's forward list, so that
// fallback produced one selection per *change*, not per cursor: redoing an
// indent over N lines left N cursors, and redoing a keystroke put the caret
// back as a forward span over the text it had typed.
void RedoRestoresTheCursorsItRecorded() {
  TEST_CASE("redo: the cursors come back as recorded, not one per edit");

  // The reported trace: one selection over three lines, indent, undo, redo.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aaa\nbbb\nccc\n");
    ed.doc.tab_width = 4;
    ed.doc.selections.Set(Selection{0, 12, -1});
    RunCommands(ed, {"indent"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("    aaa\n    bbb\n    ccc\n"));
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{4});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{24});

    RunCommands(ed, {"undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("aaa\nbbb\nccc\n"));

    RunCommands(ed, {"redo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("    aaa\n    bbb\n    ccc\n"));
    // One cursor, spanning what it spanned before the undo -- not one per
    // indented line.
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{4});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{24});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  // Insert-mode carets. Typing at three carets leaves three *empty* carets sat
  // after what each one typed; redo has to give those back as carets. The
  // fallback gave a forward span over the inserted byte instead, which draws
  // the caret a grapheme short and swallows the next character typed.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "aaa\nbbb\nccc\n");
    ed.doc.selections.Replace(ed.doc.table, std::vector<Selection>{
                                                Selection{0, 0, -1},
                                                Selection{4, 4, -1},
                                                Selection{8, 8, -1},
                                            });
    RunCommands(ed, {"insert_mode"});
    ExpectOk(InsertAtCursors("X", ed.doc.table, ed.doc.selections), "typing at three carets");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("Xaaa\nXbbb\nXccc\n"));

    const std::vector<Selection> typed = ed.doc.selections.Ranges();
    EXPECT_EQ(typed.size(), std::size_t{3});
    for (const Selection& s : typed) EXPECT_TRUE(s.IsEmpty());

    RunCommands(ed, {"undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("aaa\nbbb\nccc\n"));

    RunCommands(ed, {"redo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("Xaaa\nXbbb\nXccc\n"));
    const std::vector<Selection> back = ed.doc.selections.Ranges();
    EXPECT_EQ(back.size(), typed.size());
    if (back.size() == typed.size()) {
      for (std::size_t i = 0; i < back.size(); ++i) {
        // Zero-width carets, at the same offsets typing left them at.
        EXPECT_TRUE(back[i].IsEmpty());
        EXPECT_EQ(back[i].From(), typed[i].From());
        EXPECT_EQ(back[i].To(), typed[i].To());
      }
    }
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  // The control: a revision that recorded no cursors_after at all -- an edit
  // put straight through Apply, as the Insert/Delete/Replace wrappers do. Redo
  // has nothing to restore, so the edit-span fallback still has to run and
  // still has to land somewhere sensible.
  {
    Editor ed;
    ResetToOriginal(ed.doc.table, "hello world\n");
    ed.doc.selections.Set(Selection{0, 0, -1});
    const Change c{6, 11, std::string_view{"there"}};
    ExpectOk(Apply(ed.doc.table, std::span{&c, 1}, CursorState{}, CursorState{}),
             "an edit that notes no cursors");
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("hello there\n"));

    RunCommands(ed, {"undo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("hello world\n"));

    RunCommands(ed, {"redo"});
    EXPECT_EQ(AssembleDocContents(ed.doc.table), std::string("hello there\n"));
    EXPECT_EQ(ed.doc.selections.Size(), std::size_t{1});
    EXPECT_EQ(ed.doc.selections.Primary().From(), Index{6});
    EXPECT_EQ(ed.doc.selections.Primary().To(), Index{11});
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

// The note NoteCursorsBefore leaves is a one-shot: the transaction it was taken
// for spends it, and no later edit may inherit it. It used to live until
// ResetHistory, so every edit that passes no cursors of its own -- the
// Insert/Delete/Replace wrappers, which a dozen commands go through -- was
// stamped with the snapshot of some earlier, unrelated command, and undo then
// restored a selection the user was never at.
void ACursorNoteIsSpentByTheEditItWasTakenFor() {
  TEST_CASE("history: a noted cursor snapshot is consumed by exactly one revision");

  const auto note_of = [](Index anchor, Index head) {
    CursorState state;
    state.spans.push_back(CursorSpan{anchor, head});
    return state;
  };

  // The noted path still round-trips: what was noted is exactly what undo hands
  // back, even though the transaction itself passed no cursors.
  {
    PieceTable table;
    ResetToOriginal(table, std::string(64, 'a') + "\n");
    NoteCursorsBefore(table, note_of(7, 11));
    const Change c{7, 11, std::string_view{"XY"}};
    ExpectOk(Apply(table, std::span{&c, 1}, CursorState{}, CursorState{}), "noted edit");

    CursorState got;
    ExpectOk(Undo(table, &got), "undo of the noted edit");
    EXPECT_EQ(got.spans.size(), std::size_t{1});
    if (got.spans.size() == 1) {
      EXPECT_EQ(got.spans[0].anchor, Index{7});
      EXPECT_EQ(got.spans[0].head, Index{11});
    }
  }

  // The reported trace: an edit through the selections (which both notes and
  // passes its cursors), then a wrapper edit that notes nothing. The wrapper's
  // revision must not be stamped with the selection-path snapshot -- here that
  // snapshot spans a document the delete has since emptied.
  {
    PieceTable table;
    ResetToOriginal(table, std::string(2000, 'x'));
    const CursorState select_all = note_of(0, 2000);
    NoteCursorsBefore(table, select_all);
    const Change del{0, 2000, std::string_view{}};
    ExpectOk(Apply(table, std::span{&del, 1}, select_all, CursorState{}), "delete everything");
    EXPECT_EQ(DocLength(table), Index{0});

    ExpectOk(Insert("hello", 0, table), "wrapper insert after the selection-path edit");

    CursorState got;
    ExpectOk(Undo(table, &got), "undo of the wrapper edit");
    EXPECT_EQ(DocLength(table), Index{0});
    EXPECT_TRUE(got.spans.empty());
    for (const CursorSpan& s : got.spans) {
      EXPECT_TRUE((s.anchor >= 0) && (s.anchor <= DocLength(table)));
      EXPECT_TRUE((s.head >= 0) && (s.head <= DocLength(table)));
    }
  }

  // A grouped command folds several transactions into one revision: the note
  // belongs to that revision and must survive to it, not be dropped by the
  // first fold.
  {
    PieceTable table;
    ResetToOriginal(table, "alpha bravo charlie\n");
    const std::string original = ReadDocRange(table, {0, DocLength(table)});
    NoteCursorsBefore(table, note_of(3, 5));
    {
      UndoGroup group(table);
      const Change c1{0, 0, std::string_view{"A"}};
      ExpectOk(Apply(table, std::span{&c1, 1}, CursorState{}, CursorState{}), "group step one");
      const Change c2{7, 7, std::string_view{"B"}};
      ExpectOk(Apply(table, std::span{&c2, 1}, CursorState{}, CursorState{}), "group step two");
    }
    EXPECT_EQ(UndoDepth(table), Index{1});

    CursorState got;
    ExpectOk(Undo(table, &got), "undo of the grouped edit");
    ExpectSameDoc(table, original, "undo of the grouped edit");
    EXPECT_EQ(got.spans.size(), std::size_t{1});
    if (got.spans.size() == 1) {
      EXPECT_EQ(got.spans[0].anchor, Index{3});
      EXPECT_EQ(got.spans[0].head, Index{5});
    }
  }

  // A transaction that folds into the previous revision (coalesced typing)
  // spends its note too -- the revision it folded into already carries the one
  // taken for it, so carrying this one forward would stamp the *next* revision.
  {
    PieceTable table;
    ResetToOriginal(table, "abc\n");
    const Change c1{0, 0, std::string_view{"x"}};
    ExpectOk(Apply(table, std::span{&c1, 1}, CursorState{}, CursorState{}), "first keystroke");
    NoteCursorsBefore(table, note_of(2, 3));
    const Change c2{1, 1, std::string_view{"y"}};
    ExpectOk(Apply(table, std::span{&c2, 1}, CursorState{}, CursorState{}), "second keystroke");
    // Not adjacent to the run above, so this one starts a revision of its own.
    const Change c3{0, 0, std::string_view{"z"}};
    ExpectOk(Apply(table, std::span{&c3, 1}, CursorState{}, CursorState{}), "unrelated edit");

    CursorState got;
    ExpectOk(Undo(table, &got), "undo of the unrelated edit");
    EXPECT_TRUE(got.spans.empty());
  }
}

void UndoDoesNotRestoreASelectionFromAnEarlierCommand() {
  TEST_CASE("undo: a wrapper-driven edit does not inherit an earlier command's selection");
  const Scratch scratch{"koi-undo-stale-note"};
  std::string many;
  for (int i = 0; i < 12; ++i) many += "line" + std::to_string(i) + " of the file\n";
  const std::filesystem::path file = scratch.Write("s.txt", many);

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, file.string()));

  // An edit over the whole file, which leaves a whole-file cursor snapshot
  // behind it.
  RunCommands(ed, {"select_all"});
  RunCommands(ed, {"switch_case"});
  const Index length = DocLength(ed.doc.table);
  EXPECT_TRUE(length > 0);

  // Park a caret on line 5 directly: running a command here would take a fresh
  // snapshot and hide what this is testing.
  const Index at = LineStart(ed.doc.table, 5);
  ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));

  // Auto-pairs types through the Insert wrapper, which notes no cursors.
  EXPECT_TRUE(ed.settings.auto_pairs);
  TypeInto(ed, '(');
  RunCommands(ed, {"undo"});

  const Selection& primary = ed.doc.selections.Primary();
  EXPECT_TRUE((primary.To() - primary.From()) < (length / 2));
  EXPECT_EQ(LineAt(ed.doc.table, CursorOf(ed.doc.table, primary)), Index{5});
  EXPECT_EQ(DocLength(ed.doc.table), length);
  EXPECT_EQ(EditorInvariants(ed), std::string{});
}

// The other half of the one-shot rule, on the taking side. RunCommands noted the
// cursors before the batch whether or not the batch went on to edit, and a batch
// that only moves -- every motion binding, the scroll wheel -- left that note
// behind for whatever edited next. The next edit is usually from another path
// entirely (insert-mode typing, a pending-char resolution), and undo then put
// the user back where they had merely passed through on the way here. A note is
// taken for one edit: per command, so it describes the cursors as they stand
// when that command runs, and withdrawn when the run made no edit to spend it.
void ABatchThatEditsNothingLeavesNoCursorNote() {
  const Scratch scratch{"koi-note-motion-batch"};
  std::string many;
  for (int i = 0; i < 20; ++i) many += "line" + std::to_string(i) + " of the file\n";
  const std::filesystem::path file = scratch.Write("s.txt", many);

  const auto line_of = [](const Editor& ed) {
    return LineAt(ed.doc.table, CursorOf(ed.doc.table, ed.doc.selections.Primary()));
  };
  const auto park = [](Editor& ed, Index line) {
    const Index at = LineStart(ed.doc.table, line);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{at, at, -1}));
  };

  TEST_CASE("undo: a batch that only moves leaves no cursor note behind it");
  {
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, file.string()));

    // The batch under test: two motions, no edit. It starts on line 0.
    RunCommands(ed, {"move_visual_line_down", "move_visual_line_down"});
    EXPECT_EQ(line_of(ed), Index{2});
    EXPECT_TRUE(ed.doc.table.pending_before.spans.empty());

    // The user carries on elsewhere and *then* edits, through a path that
    // passes no cursors of its own -- the auto-pair Insert wrapper.
    park(ed, 8);
    EXPECT_TRUE(ed.settings.auto_pairs);
    TypeInto(ed, '(');
    RunCommands(ed, {"undo"});

    // Where they were when the edit happened. The stale note said line 0.
    EXPECT_EQ(line_of(ed), Index{8});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}), many);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("undo: a batch that does edit still restores the cursors it edited from");
  {
    // The control. A batch that moves *and then* edits still stamps its
    // revision -- and stamps it with where the cursors stood when the editing
    // command ran, not with where the batch happened to begin.
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, file.string()));
    park(ed, 3);

    // open_below edits through the Insert wrapper, so the batch's note is the
    // only thing that can say where it edited from.
    RunCommands(ed, {"move_visual_line_down", "open_below"});
    EXPECT_TRUE(DocLength(ed.doc.table) > std::ssize(many));
    RunCommands(ed, {"normal_mode"});
    RunCommands(ed, {"undo"});

    EXPECT_EQ(line_of(ed), Index{4});
    EXPECT_EQ(ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)}), many);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

// A fold must never land on the base. `extending_group` sent every later
// transaction of an open group into revisions[current] without asking
// ShouldCoalesce, whose oldest rule is that the base is not folded into: it is
// parentless, undo stops below it, and TrimHistory clears its `forward`. A trim
// firing mid-group re-roots the chain onto the step the group is standing on --
// it drops what is underneath us and that step *becomes* the base -- and the
// next step of the group then folded into a revision undo walks straight past.
// The text went in and could never be taken back.
void AGroupedEditIsNeverFoldedIntoTheHistoryBase() {
  TEST_CASE("history: a step of an open group is not folded into the base a trim left us on");

  const auto note_of = [](Index anchor, Index head) {
    CursorState state;
    state.spans.push_back(CursorSpan{anchor, head});
    return state;
  };

  PieceTable table;
  ResetToOriginal(table, "");
  // A long chain under the stock budget so nothing trims on the way up, then
  // back down to the first step: the 39 above become the redo branch, and that
  // is what lets the trim below drop the one step underneath us -- leaving us
  // standing on a base that was, a moment ago, an ordinary step.
  for (int i = 0; i < 40; ++i) {
    BreakUndoCoalescing(table);
    ExpectOk(Insert(std::string(48, static_cast<char>('a' + (i % 26))), DocLength(table), table),
             "building the chain");
  }
  while (UndoDepth(table) > 1) ExpectOk(Undo(table), "walking back down");
  EXPECT_EQ(table.current, Index{1});
  const std::string base_text = ReadDocRange(table, {0, DocLength(table)});
  EXPECT_EQ(std::ssize(base_text), Index{48});

  // Undo turned coalescing off and the fold has a 700 ms window; both are put
  // back by hand so a slow machine building the chain above cannot decide the
  // shape of what is tested here. The first step of the group has to fold into
  // the step we are standing on -- that is what leaves the trim something to
  // re-root onto.
  table.revisions[1].stamp_ms = table.revisions.back().stamp_ms;
  table.allow_coalesce = true;
  table.history_budget_bytes = 0;

  const std::string first_text = base_text + "one";
  const std::string second_text = first_text + "two";
  {
    UndoGroup group(table);
    const Change c1{DocLength(table), DocLength(table), std::string_view{"one"}};
    ExpectOk(Apply(table, std::span{&c1, 1}, note_of(1, 2), note_of(3, 3)),
             "first step of the group");
    ExpectSameDoc(table, first_text, "the first step of the group");
    // It folded, and the trim at the end of that same Apply then dropped
    // everything below us. We are on the base, mid-group, with the group's
    // first step baked into it -- all of that is by design.
    EXPECT_EQ(table.current, Index{0});
    EXPECT_FALSE(CanUndo(table));
    EXPECT_TRUE(std::ssize(table.revisions) < 41);

    const Change c2{DocLength(table), DocLength(table), std::string_view{"two"}};
    ExpectOk(Apply(table, std::span{&c2, 1}, note_of(5, 6), note_of(7, 7)),
             "second step of the group");
  }
  ExpectSameDoc(table, second_text, "the second step of the group");
  EXPECT_EQ(UndoChainInvariants(table), std::string{});

  // The point of all of it: the second step is undoable. Folded into the base
  // it was not -- undo skipped it and its text stayed in for good.
  EXPECT_TRUE(CanUndo(table));
  CursorState got;
  ExpectOk(Undo(table, &got), "undo of the second step");
  ExpectSameDoc(table, first_text, "undo of the second step");
  EXPECT_EQ(got.spans.size(), std::size_t{1});
  if (got.spans.size() == 1) {
    EXPECT_EQ(got.spans[0].anchor, Index{5});
    EXPECT_EQ(got.spans[0].head, Index{6});
  }
  // As far back as the document goes: the trim baked the first step into the
  // base, and a base has nothing below it.
  EXPECT_EQ(table.current, Index{0});
  EXPECT_FALSE(CanUndo(table));
  EXPECT_EQ(UndoChainInvariants(table), std::string{});

  // And forward again, cursors and all.
  EXPECT_TRUE(CanRedo(table));
  CursorState back;
  ExpectOk(Redo(table, &back), "redo of the second step");
  ExpectSameDoc(table, second_text, "redo of the second step");
  EXPECT_EQ(back.spans.size(), std::size_t{1});
  if (back.spans.size() == 1) {
    EXPECT_EQ(back.spans[0].anchor, Index{7});
    EXPECT_EQ(back.spans[0].head, Index{7});
  }
  EXPECT_EQ(UndoChainInvariants(table), std::string{});
}

// Both of the above in one story, because they meet: the note a motion-only
// batch left behind is exactly what a fold into the base would have made
// permanent. Motions, edits through a path that notes nothing, a trim that
// renumbers every revision under us, more edits -- then the whole walk back and
// forward again, document and caret checked at every stop.
void AMotionBatchAnEditATrimAndAnEditAllWalkBack() {
  TEST_CASE("undo: motions, wrapper edits and a history trim walk back and forward exactly");
  const Scratch scratch{"koi-undo-trim-walk"};
  std::string body;
  for (int i = 0; i < 40; ++i) body += "line" + std::to_string(i) + " of the file\n";
  const std::filesystem::path file = scratch.Write("s.txt", body);

  Editor ed;
  ed.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(ed, file.string()));

  const auto text_of = [](const Editor& e) {
    return ReadDocRange(e.doc.table, {0, DocLength(e.doc.table)});
  };
  const auto line_of = [](const Editor& e) {
    return LineAt(e.doc.table, CursorOf(e.doc.table, e.doc.selections.Primary()));
  };

  // The document and the caret as they stood on either side of each edit.
  struct Stop {
    std::string before_text;
    Index before_line;
    std::string after_text;
    Index after_line;
  };
  std::vector<Stop> stops;

  const auto move_then_edit = [&](char c) {
    // A batch that only moves. It must leave nothing behind for the edit.
    RunCommands(ed, {"move_visual_line_down"});
    EXPECT_TRUE(ed.doc.table.pending_before.spans.empty());
    Stop stop;
    stop.before_text = text_of(ed);
    stop.before_line = line_of(ed);
    BreakUndoCoalescing(ed.doc.table);
    TypeInto(ed, c);  // the auto-pair wrapper: no cursors of its own
    stop.after_text = text_of(ed);
    stop.after_line = line_of(ed);
    stops.push_back(std::move(stop));
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  };

  for (int i = 0; i < 24; ++i) move_then_edit('(');

  // The trim: it drops the oldest steps and renumbers every one that is left,
  // which is the state the base-fold defect needed.
  const Index before_trim = std::ssize(ed.doc.table.revisions);
  ed.doc.table.history_budget_bytes = 0;
  TrimHistory(ed.doc.table);
  ed.doc.table.history_budget_bytes = 64 * 1024 * 1024;
  const Index after_trim = std::ssize(ed.doc.table.revisions);
  EXPECT_TRUE(after_trim < before_trim);
  EXPECT_TRUE(after_trim > 1);
  EXPECT_EQ(EditorInvariants(ed), std::string{});

  for (int i = 0; i < 3; ++i) move_then_edit('[');

  // Back down, one step at a time, as far as the trimmed history goes.
  std::size_t walked = 0;
  while (CanUndo(ed.doc.table)) {
    RunCommands(ed, {"undo"});
    ++walked;
    EXPECT_TRUE(walked <= stops.size());
    if (walked > stops.size()) break;
    const Stop& stop = stops[stops.size() - walked];
    EXPECT_EQ(text_of(ed), stop.before_text);
    EXPECT_EQ(line_of(ed), stop.before_line);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
  // The trim kept a real stretch of history, and the last edits are all in it.
  EXPECT_TRUE(walked >= 4);

  // And forward again over exactly the same ground.
  while (CanRedo(ed.doc.table) && (walked > 0)) {
    const Stop& stop = stops[stops.size() - walked];
    RunCommands(ed, {"redo"});
    --walked;
    EXPECT_EQ(text_of(ed), stop.after_text);
    EXPECT_EQ(line_of(ed), stop.after_line);
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
  EXPECT_EQ(walked, std::size_t{0});
  EXPECT_EQ(text_of(ed), stops.back().after_text);
}

void HistoryBudgetIsPerFile() {
  TEST_CASE("history: a second buffer does not shorten how far this one undoes");
  const Scratch scratch{"koi-history-budget"};
  std::string body;
  for (int i = 0; i < 200; ++i) body += "line " + std::to_string(i) + " of the file\n";
  const std::filesystem::path a = scratch.Write("a.txt", body);
  const std::filesystem::path b = scratch.Write("b.txt", body);

  const auto undo_depth = [](Editor& ed) {
    int n = 0;
    while (CanUndo(ed.doc.table) && (n < 100000)) {
      RunCommands(ed, {"undo"});
      ++n;
    }
    return n;
  };
  const auto edit_a_lot = [](Editor& ed, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
      for (int c = 0; c < 8; ++c) AddCursorVertically(ed, true);

      const std::size_t was = ed.doc.table.modified.size();
      TypeInto(ed, static_cast<char>('a' + (i % 26)));
      EXPECT_TRUE(ed.doc.table.modified.size() >= was);
      EXPECT_TRUE(pt::Validate(ed.doc.table.tree).empty());
    }
  };

  Editor alone;
  alone.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(alone, a.string()));

  alone.doc.table.history_budget_bytes = 64 * 1024;
  edit_a_lot(alone, 60);
  const int depth_alone = undo_depth(alone);

  Editor shared;
  shared.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(shared, a.string()));
  shared.doc.table.history_budget_bytes = 64 * 1024;
  EXPECT_TRUE(OpenTarget(shared, b.string()));
  shared.doc.table.history_budget_bytes = 64 * 1024;

  for (int i = 0; i < 60; ++i) {
    RunTypableCommand(shared, "bn");
    edit_a_lot(shared, 1);
    RunTypableCommand(shared, "bn");
    edit_a_lot(shared, 1);
  }

  while (shared.doc.file.filename().string() != "a.txt") RunTypableCommand(shared, "bn");
  const int depth_shared = undo_depth(shared);

  EXPECT_EQ(depth_shared, depth_alone);
  EXPECT_TRUE(depth_alone > 1);

  Editor huge;
  huge.theme = BuiltinTheme();
  EXPECT_TRUE(OpenTarget(huge, a.string()));
  huge.doc.table.history_budget_bytes = 1024;
  huge.doc.selections.Set(MinWidth1(huge.doc.table, Selection{0, 0, -1}));
  for (int c = 0; c < 400; ++c) AddCursorVertically(huge, true);
  TypeInto(huge, 'Z');
  const std::string after_edit = ReadDocRange(huge.doc.table, {0, DocLength(huge.doc.table)});

  EXPECT_TRUE(CanUndo(huge.doc.table));
  RunCommands(huge, {"undo"});
  const std::string after_undo = ReadDocRange(huge.doc.table, {0, DocLength(huge.doc.table)});
  EXPECT_TRUE(after_edit != after_undo);
}

void UndoingPastATrimmedHistoryBaseKeepsTheBufferDirty() {
  namespace fs = std::filesystem;
  const Scratch scratch{"koi-trimmed-undo-serial"};
  std::string pristine;
  for (int i = 0; i < 120; ++i) pristine += "line " + std::to_string(i) + "\n";
  const fs::path path = scratch.Write("subject.txt", pristine);

  const auto text_of = [](const Editor& ed) {
    return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
  };
  // One revision per call, each touching enough lines to spend history bytes.
  const auto edit_wide = [](Editor& ed, int rounds) {
    for (int i = 0; i < rounds; ++i) {
      ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
      for (int c = 0; c < 16; ++c) AddCursorVertically(ed, true);
      BreakUndoCoalescing(ed.doc.table);
      TypeInto(ed, static_cast<char>('a' + (i % 26)));
    }
  };

  TEST_CASE("undo down to a trimmed history base leaves the buffer modified");
  {
    // CurrentUndoSerial answered 0 for `current == 0`, which is only the
    // as-loaded state until TrimHistory re-roots the chain -- after that the
    // base is some edit far from disk, and reporting its serial as 0 matched
    // saved_undo_serial and called the buffer clean. Quitting then asked
    // nothing and the crash journal declined the document: the edits between
    // the base and the file on disk went out silently.
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    const std::string on_disk = text_of(ed);
    EXPECT_EQ(ed.doc.saved_undo_serial, Index{0});

    ed.doc.table.history_budget_bytes = 1024;
    edit_wide(ed, 60);
    EXPECT_TRUE(ed.doc.modified);
    // Trimming really fired: the parentless base is an edit, not the load.
    EXPECT_TRUE(std::ssize(ed.doc.table.revisions) < 60);
    EXPECT_TRUE(ed.doc.table.revisions[0].serial != 0);

    while (CanUndo(ed.doc.table)) RunCommands(ed, {"undo"});
    EXPECT_EQ(ed.doc.table.current, Index{0});
    EXPECT_TRUE(text_of(ed) != on_disk);
    EXPECT_TRUE(CurrentUndoSerial(ed.doc.table) != 0);
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_FALSE(UnsavedBuffers(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }

  TEST_CASE("undo down to an untrimmed base still reports the buffer clean");
  {
    // The other half: with history intact the floor *is* the loaded document,
    // and undoing back to it must still drop the modified flag.
    Editor ed;
    ed.theme = BuiltinTheme();
    EXPECT_TRUE(OpenTarget(ed, path.string()));
    const std::string on_disk = text_of(ed);
    for (int i = 0; i < 6; ++i) {
      BreakUndoCoalescing(ed.doc.table);
      TypeInto(ed, static_cast<char>('a' + i));
    }
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(ed.doc.table.revisions[0].serial, Index{0});

    while (CanUndo(ed.doc.table)) RunCommands(ed, {"undo"});
    EXPECT_EQ(ed.doc.table.current, Index{0});
    EXPECT_EQ(text_of(ed), on_disk);
    EXPECT_EQ(CurrentUndoSerial(ed.doc.table), Index{0});
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(UnsavedBuffers(ed).empty());
    EXPECT_EQ(EditorInvariants(ed), std::string{});
  }
}

// NoteCursorsAfter carried the same trim-base blind spot CurrentUndoSerial did:
// it gated on `current <= 0` and so dropped every stamp taken while standing on
// revision 0. That is only ever right while revision 0 is the document as
// loaded; once TrimHistory re-roots the chain the base is a real edit the user
// can be standing on, and the stamp meant for it went nowhere. "Is there a
// revision to stamp" is the range test the rest of the file uses -- `current <
// 0 || current >= revisions.size()` -- which still covers the one table with no
// revisions at all, the one that was never loaded.
void CursorNotesLandOnATrimmedHistoryBase() {
  const auto note_of = [](Index anchor, Index head) {
    CursorState state;
    state.spans.push_back(CursorSpan{anchor, head});
    return state;
  };
  const auto type_step = [](PieceTable& t, char c) {
    BreakUndoCoalescing(t);
    ExpectOk(Insert(std::string(48, c), DocLength(t), t), "typing a step");
  };

  TEST_CASE("history: a cursor note lands on the base a trim left us standing on");
  {
    PieceTable table;
    ResetToOriginal(table, "");
    // Built under the stock budget so nothing trims on the way up: the store is
    // one long chain, and undoing back down to the first step makes everything
    // above it the redo branch. The trim then has 40 steps to spend and exactly
    // one below us to drop, which lands us on the re-rooted base.
    for (int i = 0; i < 40; ++i) type_step(table, static_cast<char>('a' + (i % 26)));
    while (UndoDepth(table) > 1) ExpectOk(Undo(table), "walking back down");
    EXPECT_EQ(table.current, Index{1});

    table.history_budget_bytes = 0;
    EXPECT_TRUE(HistoryBytes(table) > table.history_budget_bytes);
    TrimHistory(table);

    // Standing on the base, and the base is an edit rather than the load.
    EXPECT_EQ(table.current, Index{0});
    EXPECT_TRUE(std::ssize(table.revisions) > 1);
    EXPECT_TRUE(table.revisions[0].serial != 0);
    EXPECT_TRUE(CurrentUndoSerial(table) != 0);
    EXPECT_FALSE(CanUndo(table));
    EXPECT_TRUE(CanRedo(table));

    // The stamp the command takes for the step it just finished. `<= 0` threw
    // it away here and left whatever the base was carrying from before.
    NoteCursorsAfter(table, note_of(3, 7));
    EXPECT_EQ(table.revisions[0].cursors_after.spans.size(), std::size_t{1});
    if (!table.revisions[0].cursors_after.spans.empty()) {
      EXPECT_EQ(table.revisions[0].cursors_after.spans[0].anchor, Index{3});
      EXPECT_EQ(table.revisions[0].cursors_after.spans[0].head, Index{7});
    }
    // Writing it changed nothing else: the redo branch above the base is still
    // walkable and still hands back its own recorded cursors.
    CursorState got;
    ExpectOk(Redo(table, &got), "redo off the trimmed base");
    EXPECT_EQ(table.current, Index{1});
    EXPECT_TRUE(got.spans.empty());
  }

  TEST_CASE("history: a cursor note on a table with no history is a no-op");
  {
    // The only table whose `current` names nothing: never loaded, so the store
    // is empty and the range test -- not the sign of `current` -- is what has
    // to catch it.
    PieceTable table;
    EXPECT_TRUE(table.revisions.empty());
    EXPECT_EQ(table.current, Index{0});
    NoteCursorsAfter(table, note_of(1, 2));
    EXPECT_TRUE(table.revisions.empty());
    EXPECT_FALSE(CanUndo(table));
    EXPECT_FALSE(CanRedo(table));
  }

  TEST_CASE("history: stamping the as-loaded base does not leak into redo");
  {
    // Nothing redoes *into* revision 0 -- it is no revision's last_child -- so
    // stamping it must stay invisible: the redone step's own cursors_after is
    // what comes back, not the note left on the base.
    PieceTable table;
    ResetToOriginal(table, "abc\n");
    NoteCursorsAfter(table, note_of(99, 99));
    const Change c{0, 0, std::string_view{"X"}};
    ExpectOk(Apply(table, std::span{&c, 1}, CursorState{}, note_of(1, 1)), "edit off the base");
    ExpectOk(Undo(table), "back to the as-loaded base");
    EXPECT_EQ(table.current, Index{0});

    CursorState got;
    ExpectOk(Redo(table, &got), "redo of the edit");
    EXPECT_EQ(got.spans.size(), std::size_t{1});
    if (got.spans.size() == 1) {
      EXPECT_EQ(got.spans[0].anchor, Index{1});
      EXPECT_EQ(got.spans[0].head, Index{1});
    }
  }
}

void AbandonedUndoBranchesAreReclaimed() {
  // Editing after an undo repoints the parent's last_child at the new step and
  // leaves the old branch in `revisions`, holding live tree nodes that no key
  // can reach. Rebuilding the store is the only thing that frees one, and
  // TrimHistory used to decide whether to rebuild from the length of the *live
  // chain*: with the chain oscillating below kMinRevisionsKept the rebuild was
  // never reached, so an undo-then-retype rhythm grew the store without bound
  // while HistoryBytes sat arbitrarily far over budget.
  const auto type_char = [](PieceTable& t, char c) {
    const std::string s(1, c);
    BreakUndoCoalescing(t);
    ExpectOk(Insert(s, DocLength(t), t), "typing a character");
  };

  TEST_CASE("history: an undo-then-retype rhythm converges instead of growing");
  {
    PieceTable table;
    ResetToOriginal(table, "");
    table.history_budget_bytes = 4096;

    // Five steps up, five steps back: the live chain never reaches the floor,
    // so every cycle abandons a five-step branch that only a rebuild frees.
    const auto cycle = [&](int rounds) {
      for (int i = 0; i < rounds; ++i) {
        for (int k = 0; k < 5; ++k) type_char(table, 'x');
        for (int k = 0; k < 5; ++k) ExpectOk(Undo(table), "undoing the burst");
      }
    };

    cycle(20);
    const Index early_revisions = std::ssize(table.revisions);
    const Index early_bytes = HistoryBytes(table);
    cycle(180);
    const Index late_revisions = std::ssize(table.revisions);
    const Index late_bytes = HistoryBytes(table);

    // Nine times the work must not cost more history than the first stretch
    // did: the store converges rather than climbing with the cycle count.
    EXPECT_TRUE(late_revisions <= early_revisions);
    EXPECT_TRUE(late_bytes <= early_bytes);
    // ... and what it converges to is the neighbourhood of the floor and the
    // budget, not some multiple of them.
    EXPECT_TRUE(late_revisions <= 2 * kMinRevisionsKept);
    EXPECT_TRUE(late_bytes <= 4 * table.history_budget_bytes);
    // The rhythm ends where it started, with the whole burst redoable.
    EXPECT_EQ(DocLength(table), Index{0});
    EXPECT_TRUE(CanRedo(table));
  }

  TEST_CASE("history: reclaiming abandoned branches leaves undo and redo alone");
  {
    // The same script under a budget that trims and one that never does. No
    // step of the live chain is ever droppable here -- the chain stays under
    // kMinRevisionsKept throughout -- so the two histories must be walkable
    // into exactly the same sequence of documents.
    const auto script = [&](PieceTable& t) {
      for (int c = 0; c < 8; ++c) {
        for (int k = 0; k < 3; ++k) type_char(t, static_cast<char>('a' + c));
        for (int k = 0; k < 3; ++k) ExpectOk(Undo(t), "undoing the burst");
      }
      for (char c : std::string_view{"ABCD"}) type_char(t, c);
      ExpectOk(Undo(t), "stepping back off the tip");
      ExpectOk(Undo(t), "stepping back off the tip");
    };
    // Redo to the top, then undo to the bottom, recording every document seen.
    // Neither key touches TrimHistory, so the walk reads history without
    // changing it.
    const auto walk = [](PieceTable& t) {
      std::vector<std::string> seen{AssembleDocContents(t)};
      while (CanRedo(t)) {
        ExpectOk(Redo(t), "walking up");
        seen.push_back(AssembleDocContents(t));
      }
      while (CanUndo(t)) {
        ExpectOk(Undo(t), "walking down");
        seen.push_back(AssembleDocContents(t));
      }
      return seen;
    };

    PieceTable tight;
    ResetToOriginal(tight, "");
    tight.history_budget_bytes = 512;
    script(tight);

    PieceTable roomy;
    ResetToOriginal(roomy, "");
    script(roomy);

    // The trimming table really did reclaim: the roomy one is still carrying
    // every abandoned branch.
    EXPECT_TRUE(std::ssize(tight.revisions) < std::ssize(roomy.revisions));
    // Reclaiming is bounded by the same floor that protects steps: the store
    // is never rebuilt below kMinRevisionsKept entries, so what is left is the
    // chain plus at most one floor's worth of not-yet-collected branches.
    EXPECT_TRUE(std::ssize(tight.revisions) <= 2 * kMinRevisionsKept);
    EXPECT_EQ(AssembleDocContents(tight), AssembleDocContents(roomy));
    EXPECT_EQ(UndoDepth(tight), UndoDepth(roomy));
    EXPECT_EQ(CanRedo(tight), CanRedo(roomy));

    const std::vector<std::string> tight_seen = walk(tight);
    const std::vector<std::string> roomy_seen = walk(roomy);
    EXPECT_EQ(tight_seen.size(), roomy_seen.size());
    for (std::size_t i = 0; i < std::min(tight_seen.size(), roomy_seen.size()); ++i) {
      EXPECT_EQ(tight_seen[i], roomy_seen[i]);
    }
    EXPECT_TRUE(tight_seen.size() > 6);
  }
}

}  // namespace koi
