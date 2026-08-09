// Tests for piece_doc.cpp: the piece table itself -- inserts, replaces and
// deletes, undo and redo, the line index, and the mapped original.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

namespace koi {

namespace {

constexpr std::string_view kCharset{"abcdefgabcdefgabcdefgabcdefgabcdefgabcdefgabcdefg"};

constexpr Index kNMaxInsert{100};

constexpr Index kNMaxReplace{100};

constexpr Index kNMaxDelete{100};

constexpr Index kMinRequiredDocLen = 10;

Index BruteLineAt(const std::string& doc, Index pos) {
  return static_cast<Index>(std::count(doc.begin(), doc.begin() + pos, '\n'));
}

std::pair<Index, Index> BruteLineRange(const std::string& doc, Index line) {
  Index start = 0;
  for (Index seen = 0; seen < line; ++seen) {
    const size_t at = doc.find('\n', static_cast<size_t>(start));
    if (at == std::string::npos) return {std::ssize(doc), std::ssize(doc)};
    start = static_cast<Index>(at) + 1;
  }
  const size_t at = doc.find('\n', static_cast<size_t>(start));
  const Index end = (at == std::string::npos) ? std::ssize(doc) : static_cast<Index>(at) + 1;
  return {start, end};
}

ErrorCtx ReplaceFunctor(std::string_view txt, Interval doc_range, PieceTable& table,
                        std::string& brute_force) {
  ErrorCtx err = Replace(txt, doc_range, table);
  if (err) return err;
  const auto pos = static_cast<size_t>(doc_range.front());
  const auto count = std::min<size_t>(static_cast<size_t>(doc_range.size()), brute_force.size() - pos);
  brute_force.replace(pos, count, txt);
  return AssembleDocContents(table) == brute_force
             ? Success()
             : MakeErrorCtx(PieceTableErrorCode::kTestingPieceStringNotEqBruteForceString);
}

using Functor = std::function<ErrorCtx(std::string_view, Interval, PieceTable&, std::string&)>;

void Random(std::string_view name, Index n_max, const Functor& functor, PieceTable& table, Rng& rng,
            bool is_delete = false) {
  std::string brute_force = AssembleDocContents(table);
  const Index n = std::min<Index>(n_max, std::ssize(brute_force));

  for (Index k = 0; k < n; ++k) {
    const Index doc_pos_max = std::ssize(brute_force) - 1;
    if (doc_pos_max < 1) break;
    const Index pos = rng.Pick(0, doc_pos_max);

    Index text_len = std::ssize(kCharset);
    if (is_delete) {
      text_len = std::min<Index>(std::ssize(kCharset), doc_pos_max - pos + 1);
      if (text_len == 0) continue;
    }
    const std::string_view txt = kCharset.substr(0, text_len);
    ExpectOk(functor(txt, {pos, pos + text_len}, table, brute_force), name);
  }
}

void Iota(std::string_view name, Index n_max, const Functor& functor, PieceTable& table) {
  const Index n = std::min<Index>(n_max, std::ssize(OriginalText(table)));
  std::string brute_force = AssembleDocContents(table);
  for (Index pos = 0; pos < n; ++pos) {
    const Index doc_pos_max = std::ssize(brute_force) - 1;
    if (doc_pos_max <= kMinRequiredDocLen) break;
    const Index text_len = std::min<Index>(std::ssize(kCharset), doc_pos_max - pos + 1);
    if (text_len == 0) continue;
    ExpectOk(functor(kCharset.substr(0, text_len), {pos, pos + text_len}, table, brute_force), name);
  }
}

void SequentialAppend(Index n_max, PieceTable& table) {
  std::string brute_force = AssembleDocContents(table);
  const Index doc_pos_max = std::ssize(brute_force);
  constexpr std::string_view charset = "abcdefghijklmnopqrstuvwxyz01  23456789(*&^%$#@!)";
  Index k = 0;
  for (Index doc_pos : std::ranges::iota_view(doc_pos_max, doc_pos_max + n_max)) {
    ExpectOk(InsertFunctor(charset.substr(k, 1), {doc_pos, doc_pos + 1}, table, brute_force),
             "SequentialAppend");
    if (++k == std::ssize(charset)) k = 0;
  }
}

void SequentialDelete(Index n_max, PieceTable& table) {
  std::string brute_force = AssembleDocContents(table);
  const Index doc_pos_max = std::ssize(brute_force) - 1;
  const Index first = std::max<Index>(0, doc_pos_max - n_max);
  for (Index doc_pos : std::ranges::iota_view(first, doc_pos_max) | vw::reverse) {
    ExpectOk(DeleteFunctor("a", {doc_pos, doc_pos + 1}, table, brute_force), "SequentialDelete");
  }
}

void InsertEdgeCase(PieceTable& table) {
  TEST_CASE("insert edge cases");
  std::string brute_force = AssembleDocContents(table);

  EXPECT_TRUE(Insert(kCharset, std::ssize(brute_force) + 9, table));
  EXPECT_TRUE(Insert(kCharset, -5, table));

  ExpectOk(Insert(kCharset, 0, table), "insert at 0");
  brute_force.insert(0, kCharset);
  ExpectSameDoc(table, brute_force, "after insert at 0");

  ExpectOk(Insert(kCharset, std::ssize(brute_force), table), "insert at end");
  brute_force.append(kCharset);
  ExpectSameDoc(table, brute_force, "after insert at end");

  {
    PieceTable empty = table;
    empty.Clear();
    ExpectOk(Insert(kCharset, 0, empty), "insert into empty table");
  }
  {
    PieceTable many = table;
    std::string model = brute_force;
    for (int i = 0; i < 12000; ++i) {
      const Index at = static_cast<Index>((i * 7919) % (model.size() + 1));
      ExpectOk(Insert("#", at, many), "insert into a heavily fragmented table");
      model.insert(static_cast<std::size_t>(at), "#");
    }
    EXPECT_TRUE(PieceCount(many) > 8192);
    ExpectSameDoc(many, model, "after twelve thousand scattered inserts");
  }
}

void ReplaceEdgeCase(PieceTable& table) {
  TEST_CASE("replace edge cases");
  std::string brute_force = AssembleDocContents(table);
  const Index doc_end = std::ssize(brute_force) - 1;

  EXPECT_TRUE(Replace(kCharset, {doc_end + 10, doc_end + 10 + std::ssize(kCharset)}, table));
  EXPECT_TRUE(Replace(kCharset, {-5, -5 + std::ssize(kCharset)}, table));

  ExpectOk(Replace(kCharset.substr(0, 1), {0, 1}, table), "replace at 0");
  brute_force[0] = kCharset[0];
  ExpectSameDoc(table, brute_force, "after replace at 0");

  ExpectOk(Replace(kCharset, {doc_end, doc_end + std::ssize(kCharset)}, table), "replace at end");
  brute_force.back() = kCharset[0];
  brute_force.append(kCharset.substr(1));
  ExpectSameDoc(table, brute_force, "after replace at end");

  EXPECT_TRUE(Replace(std::string_view{}, {0, 100}, table) ==
              std::error_code(static_cast<int>(PieceTableErrorCode::kEmptyInputString),
                              PieceTableErrorCategoryInstance()));
  EXPECT_TRUE(Replace("whatever", {0, 0}, table) ==
              std::error_code(static_cast<int>(PieceTableErrorCode::kMismatchInputStringAndDocRange),
                              PieceTableErrorCategoryInstance()));
  ExpectSameDoc(table, brute_force, "after rejected replaces");
}

void DeleteEdgeCase(PieceTable& table) {
  TEST_CASE("delete edge cases");
  std::string brute_force = AssembleDocContents(table);

  EXPECT_TRUE(Delete({std::ssize(brute_force) + 9, std::ssize(brute_force) + 19}, table));
  EXPECT_TRUE(Delete({-5, -1}, table));

  ExpectOk(Delete({0, 1}, table), "delete at 0");
  brute_force.erase(0, 1);
  ExpectSameDoc(table, brute_force, "after delete at 0");

  const Index doc_end = std::ssize(brute_force) - 1;
  ExpectOk(Delete({doc_end, doc_end + 1}, table), "delete at end");
  brute_force.pop_back();
  ExpectSameDoc(table, brute_force, "after delete at end");

  PieceTable empty = table;
  empty.Clear();
  EXPECT_TRUE(Delete({13, 15}, empty));
}

enum class EditOutcome { kNewUndoStep, kMergedIntoPrevious, kFailed };

EditOutcome ApplyRandomEdit(PieceTable& table, std::string& brute_force, Rng& rng) {
  const Index before = UndoDepth(table);
  const Index doc_pos_max = std::ssize(brute_force) - 1;
  if (doc_pos_max < 2) return EditOutcome::kFailed;

  const Index pos = rng.Pick(0, doc_pos_max - 1);
  const Index len = std::min<Index>(rng.Pick(1, 8), doc_pos_max - pos);
  const std::string_view txt = kCharset.substr(0, len);

  ErrorCtx err;
  switch (rng.Pick(0, 2)) {
    case 0: err = InsertFunctor(txt, {pos, pos + len}, table, brute_force); break;
    case 1: err = ReplaceFunctor(txt, {pos, pos + len}, table, brute_force); break;
    default: err = DeleteFunctor(txt, {pos, pos + len}, table, brute_force); break;
  }
  if (err) return EditOutcome::kFailed;
  return (UndoDepth(table) == before + 1) ? EditOutcome::kNewUndoStep
                                                 : EditOutcome::kMergedIntoPrevious;
}

void UndoRedoEditAfterUndo(PieceTable table) {
  TEST_CASE("edit after undo discards the redo stack");
  ResetHistory(table);

  std::string doc = AssembleDocContents(table);
  const std::string s0 = doc;

  ExpectOk(InsertFunctor("AAAA", {4, 8}, table, doc), "first edit");
  const std::string s1 = doc;
  ExpectOk(InsertFunctor("BBBB", {40, 44}, table, doc), "second edit");

  ExpectOk(Undo(table), "undo back to s1");
  ExpectSameDoc(table, s1, "undo of the second edit");
  doc = s1;
  EXPECT_TRUE(CanRedo(table));

  ExpectOk(InsertFunctor("CCCC", {12, 16}, table, doc), "edit while a redo is pending");
  const std::string s3 = doc;
  EXPECT_TRUE(!CanRedo(table));

  ExpectOk(Undo(table), "undo the edit made after the undo");
  ExpectSameDoc(table, s1, "first undo after the interleaved edit");
  ExpectOk(Undo(table), "undo back to the start");
  ExpectSameDoc(table, s0, "second undo after the interleaved edit");

  ExpectOk(Redo(table), "redo up one");
  ExpectSameDoc(table, s1, "redo after walking to the start");
  ExpectOk(Redo(table), "redo up two");
  ExpectSameDoc(table, s3, "redo back to the interleaved edit");
}

void UndoRedoInterleaved(PieceTable table, Rng& rng) {
  TEST_CASE("interleaved edit / undo / redo");
  ResetHistory(table);

  constexpr Index kNEdits = 32;
  Index edits_made = 0;

  std::string doc = AssembleDocContents(table);
  std::vector<std::string> history{doc};
  size_t cursor = 0;

  for (Index step = 0; step < 300; ++step) {
    const Index choice = rng.Pick(0, 9);

    if (choice <= 5 && edits_made < kNEdits) {
      const EditOutcome outcome = ApplyRandomEdit(table, doc, rng);
      if (outcome == EditOutcome::kFailed) continue;
      ++edits_made;
      history.resize(cursor + 1);
      if (outcome == EditOutcome::kNewUndoStep) {
        history.push_back(doc);
        ++cursor;
      } else {
        history[cursor] = doc;
      }
      EXPECT_TRUE(!CanRedo(table));

    } else if (choice <= 7) {
      if (cursor == 0 || !CanUndo(table)) continue;
      ExpectOk(Undo(table), "undo during interleaved walk");
      --cursor;
      doc = history[cursor];
      ExpectSameDoc(table, doc, "undo during interleaved walk");

    } else {
      if (cursor + 1 >= history.size() || !CanRedo(table)) continue;
      ExpectOk(Redo(table), "redo during interleaved walk");
      ++cursor;
      doc = history[cursor];
      ExpectSameDoc(table, doc, "redo during interleaved walk");
    }
  }
}

void UndoRedoNoChange(PieceTable table, Rng& rng) {
  TEST_CASE("undo / redo without interleaved edits");
  ResetHistory(table);

  std::string doc = AssembleDocContents(table);
  std::vector<std::string> history{doc};

  constexpr Index kNEdits = 32;
  for (Index i = 0; i < kNEdits; ++i) {
    const EditOutcome outcome = ApplyRandomEdit(table, doc, rng);
    if (outcome == EditOutcome::kFailed) continue;
    if (outcome == EditOutcome::kNewUndoStep) {
      history.push_back(doc);
    } else {
      history.back() = doc;
    }
  }
  EXPECT_TRUE(history.size() > 1);

  for (size_t i = history.size() - 1; i > 0; --i) {
    ExpectOk(Undo(table), "full rewind");
    ExpectSameDoc(table, history[i - 1], "full rewind");
  }
  for (size_t i = 1; i < history.size(); ++i) {
    ExpectOk(Redo(table), "full replay");
    ExpectSameDoc(table, history[i], "full replay");
  }
}

}  // namespace

void LineIndexStatic() {
  TEST_CASE("line index");
  const std::string doc = "alpha\nbravo\n\ncharlie\ndelta";
  PieceTable table = MakeTable(doc);

  EXPECT_EQ(LineCount(table), Index{5});
  EXPECT_EQ(LineAt(table, 0), Index{0});
  EXPECT_EQ(LineAt(table, 5), Index{0});
  EXPECT_EQ(LineAt(table, 6), Index{1});
  EXPECT_EQ(LineAt(table, 12), Index{2});
  EXPECT_EQ(LineAt(table, 13), Index{3});
  EXPECT_EQ(LineAt(table, std::ssize(doc)), Index{4});

  const auto range_is = [&](Index line, Index lo, Index hi) {
    const Interval r = LineRange(table, line);
    EXPECT_EQ(r.front(), lo);
    EXPECT_EQ(r.back() + 1, hi);
  };
  range_is(0, 0, 6);
  range_is(1, 6, 12);
  range_is(2, 12, 13);
  range_is(3, 13, 21);
  range_is(4, 21, 26);

  EXPECT_TRUE(LineRange(table, -1).empty());
  EXPECT_TRUE(LineRange(table, 5).empty());
  EXPECT_EQ(GetNlinesInDocRange({0, std::ssize(doc)}, table), Index{4});

  PieceTable empty;
  ResetToOriginal(empty, "");
  EXPECT_EQ(LineCount(empty), Index{1});
  EXPECT_EQ(LineAt(empty, 0), Index{0});
}

void LineIndexUnderEditing(Rng& rng) {
  TEST_CASE("line index survives editing");
  std::string brute_force;
  for (int i = 0; i < 400; ++i) brute_force += "line " + std::to_string(i) + " of text\n";
  PieceTable table = MakeTable(brute_force);

  const std::string_view payloads[] = {"x", "one\ntwo", "\n", "\n\n\n", "no newlines here"};

  for (int step = 0; step < 250; ++step) {
    const Index len = std::ssize(brute_force);
    if (len < 64) break;
    const Index at = rng.Pick(0, len - 1);

    switch (rng.Pick(0, 3)) {
      case 0: {
        const std::string_view txt = payloads[rng.Pick(0, std::ssize(payloads) - 1)];
        ExpectOk(InsertFunctor(txt, {at, at + std::ssize(txt)}, table, brute_force), "line insert");
        break;
      }
      case 1: {
        const Index end = std::min(len, at + rng.Pick(1, 20));
        if (end <= at) continue;
        ExpectOk(DeleteFunctor("", {at, end}, table, brute_force), "line delete");
        break;
      }
      case 2: {
        const Index end = std::min(len, at + rng.Pick(1, 12));
        if (end <= at) continue;
        const std::string_view txt = payloads[rng.Pick(0, std::ssize(payloads) - 1)];
        ExpectOk(ReplaceFunctor(txt, {at, end}, table, brute_force), "line replace");
        break;
      }
      default:
        if (!!CanUndo(table)) {
          ExpectOk(Undo(table), "line undo");
          brute_force = AssembleDocContents(table);
        }
        break;
    }

    EXPECT_EQ(LineCount(table), BruteLineAt(brute_force, std::ssize(brute_force)) + 1);
    for (Index line = 0; line < LineCount(table); ++line) {
      const auto [want_lo, want_hi] = BruteLineRange(brute_force, line);
      const Interval want{want_lo, want_hi};
      const Interval got = LineRange(table, line);
      if ((got.begin() != want.begin()) || (got.end() != want.end())) {
        ++common::g_test_failures;
        std::cerr << "FAIL [" << common::g_test_case << "] line " << line << " range of size "
                  << std::ssize(got) << " differs from want [" << want_lo << ',' << want_hi
                  << ") at step " << step << std::endl;
        break;
      }
      ++common::g_test_checks;
    }
    const Index probe = rng.Pick(0, std::ssize(brute_force));
    EXPECT_EQ(LineAt(table, probe), BruteLineAt(brute_force, probe));
  }
}

void ReplaceChangesLength(Rng& rng) {
  TEST_CASE("replace with a different length");
  std::string brute_force = "the quick brown fox jumps over the lazy dog\n";
  for (int i = 0; i < 60; ++i) brute_force += brute_force.substr(0, 44);
  PieceTable table = MakeTable(brute_force);

  ExpectOk(ReplaceFunctor("LONGER-REPLACEMENT", {4, 9}, table, brute_force), "replace grows");
  ExpectOk(ReplaceFunctor("x", {20, 40}, table, brute_force), "replace shrinks");
  ExpectOk(ReplaceFunctor(kEAcute, {0, 1}, table, brute_force), "replace ASCII with e-acute");
  ExpectSameDoc(table, brute_force, "after length-changing replaces");

  const std::string before = AssembleDocContents(table);
  ExpectOk(ReplaceFunctor("SOMETHING MUCH LONGER THAN WHAT IT REPLACES", {50, 55}, table,
                          brute_force),
           "replace for undo");
  ExpectOk(Undo(table), "undo a length-changing replace");
  ExpectSameDoc(table, before, "undo of a length-changing replace");
  ExpectOk(Redo(table), "redo a length-changing replace");
  ExpectSameDoc(table, brute_force, "redo of a length-changing replace");

  for (int step = 0; step < 200; ++step) {
    const Index len = std::ssize(brute_force);
    if (len < 64) break;
    const Index at = SnapToGraphemeBoundary(table, rng.Pick(0, len - 2));
    Index end = SnapToGraphemeBoundary(table, std::min(len, at + rng.Pick(1, 15)));
    if (end <= at) end = NextGraphemeBoundary(table, at);
    if (end <= at) break;
    const std::string txt(static_cast<size_t>(rng.Pick(1, 25)), 'z');
    ExpectOk(ReplaceFunctor(txt, {at, end}, table, brute_force), "random length-changing replace");
  }
  ExpectSameDoc(table, brute_force, "after random length-changing replaces");
}

namespace {

Point BrutePoint(const std::string& doc, Index pos) {
  pos = std::clamp<Index>(pos, 0, std::ssize(doc));
  Index row = 0, line_start = 0;
  for (Index i = 0; i < pos; ++i) {
    if (doc[i] == '\n') {
      ++row;
      line_start = i + 1;
    }
  }
  return Point{row, pos - line_start};
}

void ExpectEditDescribes(const std::string& before, const std::string& after, const Edit& e,
                         std::string_view ctx) {
  const bool in_range = (e.start_byte >= 0) && (e.start_byte <= std::ssize(before)) &&
                        (e.start_byte <= std::ssize(after)) &&
                        (e.old_end_byte <= std::ssize(before)) && (e.new_end_byte <= std::ssize(after));
  ++common::g_test_checks;
  if (!in_range) {
    ++common::g_test_failures;
    std::cerr << "FAIL [" << common::g_test_case << "] edit out of range: " << ctx << " start="
              << e.start_byte << " old_end=" << e.old_end_byte << " new_end=" << e.new_end_byte
              << " before=" << before.size() << " after=" << after.size() << std::endl;
    return;
  }
  EXPECT_EQ(before.substr(0, static_cast<size_t>(e.start_byte)),
            after.substr(0, static_cast<size_t>(e.start_byte)));
  EXPECT_EQ(before.substr(static_cast<size_t>(e.old_end_byte)),
            after.substr(static_cast<size_t>(e.new_end_byte)));
  EXPECT_EQ(std::ssize(after) - std::ssize(before), e.Delta());
  EXPECT_TRUE(e.start_point == BrutePoint(before, e.start_byte));
  EXPECT_TRUE(e.old_end_point == BrutePoint(before, e.old_end_byte));
  EXPECT_TRUE(e.new_end_point == BrutePoint(after, e.new_end_byte));
}

}  // namespace

void EditDescriptors(Rng& rng) {
  TEST_CASE("edit descriptors");
  std::string doc;
  for (int i = 0; i < 120; ++i) doc += "row " + std::to_string(i) + " some text here\n";
  PieceTable table = MakeTable(doc);

  const std::string_view payloads[] = {"a", "with\nnewline", "\n\n", "plain text", kCJK, kFamily};

  for (int step = 0; step < 300; ++step) {
    const std::string before = AssembleDocContents(table);
    if (std::ssize(before) < 64) break;
    const Index at = SnapToGraphemeBoundary(table, rng.Pick(0, std::ssize(before) - 2));
    Edit edit;
    ErrorCtx err;
    const char* what = "";

    switch (rng.Pick(0, 2)) {
      case 0: {
        const std::string_view txt = payloads[rng.Pick(0, std::ssize(payloads) - 1)];
        err = Insert(txt, at, table, &edit);
        what = "insert";
        break;
      }
      case 1: {
        const Index end = SnapToGraphemeBoundary(table, std::min<Index>(std::ssize(before), at + rng.Pick(1, 18)));
        if (end <= at) continue;
        err = Delete(Interval(at, end), table, &edit);
        what = "delete";
        break;
      }
      default: {
        const Index end = SnapToGraphemeBoundary(table, std::min<Index>(std::ssize(before), at + rng.Pick(1, 12)));
        if (end <= at) continue;
        const std::string_view txt = payloads[rng.Pick(0, std::ssize(payloads) - 1)];
        err = Replace(txt, Interval(at, end), table, &edit);
        what = "replace";
        break;
      }
    }
    if (err) continue;
    ExpectEditDescribes(before, AssembleDocContents(table), edit, what);
  }

  for (int step = 0; step < 60; ++step) {
    const std::string before = AssembleDocContents(table);
    std::vector<Edit> edits;
    if (!CanUndo(table)) break;
    ExpectOk(Undo(table, &edits), "undo reports edits");
    const std::string after = AssembleDocContents(table);
    EXPECT_TRUE(!edits.empty());
    if (edits.size() == 1) ExpectEditDescribes(before, after, edits[0], "undo");

    std::vector<Edit> redo_edits;
    ExpectOk(Redo(table, &redo_edits), "redo reports edits");
    EXPECT_EQ(AssembleDocContents(table), before);
    if (redo_edits.size() == 1) ExpectEditDescribes(after, before, redo_edits[0], "redo");
    ExpectOk(Undo(table, &edits), "undo again");
  }
}

void RunTests(PieceTable& table, Rng& rng) {
  TEST_CASE("bootstrap");
  Random("Bootstrap", kNMaxInsert, InsertFunctor, table, rng);

  TEST_CASE("insert");
  { PieceTable t = table; Random("Insert", kNMaxInsert, InsertFunctor, t, rng); }
  { PieceTable t = table; Iota("Insert", kNMaxInsert, InsertFunctor, t); }
  { PieceTable t = table; InsertEdgeCase(t); }
  { PieceTable t = table; SequentialAppend(kNMaxInsert, t); }

  TEST_CASE("replace");
  { PieceTable t = table; Random("Replace", kNMaxReplace, ReplaceFunctor, t, rng); }
  { PieceTable t = table; Iota("Replace", kNMaxReplace, ReplaceFunctor, t); }
  { PieceTable t = table; ReplaceEdgeCase(t); }

  TEST_CASE("delete");
  { PieceTable t = table; Random("Delete", kNMaxDelete, DeleteFunctor, t, rng, true); }
  { PieceTable t = table; DeleteEdgeCase(t); }
  { PieceTable t = table; SequentialDelete(kNMaxDelete, t); }

  UndoRedoNoChange(table, rng);
  UndoRedoEditAfterUndo(table);
  UndoRedoInterleaved(table, rng);
}

void LineIndexMemo(Rng& rng) {
  TEST_CASE("line index: the lookup memo never changes an answer");

  const auto check_all = [](const PieceTable& table, const std::string& brute,
                            const std::vector<Index>& order) {
    for (const Index line : order) {
      const auto [want_start, want_end] = BruteLineRange(brute, line);
      EXPECT_EQ(LineStart(table, line), want_start);
      const Interval got = LineRange(table, line);
      EXPECT_EQ(got.empty() ? want_start : got.front(), want_start);
      EXPECT_EQ(got.empty() ? want_start : (got.back() + 1), want_end);
      EXPECT_EQ(LineAt(table, want_start), BruteLineAt(brute, want_start));
    }
  };

  std::string brute;
  for (Index i = 0; i < 400; ++i) {
    brute += std::string(static_cast<size_t>(rng.Pick(0, 12)), 'x');
    brute += '\n';
  }
  PieceTable table;
  ResetToOriginal(table, brute);

  const Index lines = LineCount(table);
  std::vector<Index> forward;
  for (Index i = 0; i < lines; ++i) forward.push_back(i);
  std::vector<Index> backward{forward.rbegin(), forward.rend()};
  std::vector<Index> shuffled = forward;
  std::shuffle(shuffled.begin(), shuffled.end(), rng.gen);

  check_all(table, brute, forward);
  check_all(table, brute, backward);
  check_all(table, brute, shuffled);

  for (Index round = 0; round < 40; ++round) {
    const Index len = std::ssize(brute);
    std::ignore = LineStart(table, rng.Pick(0, LineCount(table) - 1));

    if ((round % 2) == 0) {
      const Index at = rng.Pick(0, len);
      const std::string text = ((round % 4) == 0) ? "zz\nz" : "qq";
      EXPECT_TRUE(!Insert(text, at, table));
      brute.insert(static_cast<size_t>(at), text);
    } else if (len > 4) {
      const Index at = rng.Pick(0, len - 3);
      const Index n = rng.Pick(1, 3);
      EXPECT_TRUE(!Delete(Interval(at, at + n), table));
      brute.erase(static_cast<size_t>(at), static_cast<size_t>(n));
    }

    std::vector<Index> order;
    for (Index i = 0; i < LineCount(table); ++i) order.push_back(i);
    std::shuffle(order.begin(), order.end(), rng.gen);
    check_all(table, brute, order);
  }

  for (Index i = 0; i < 20; ++i) std::ignore = Undo(table);
  {
    const std::string now = ReadDocRange(table, {0, DocLength(table)});
    std::vector<Index> order;
    for (Index l = 0; l < LineCount(table); ++l) order.push_back(l);
    check_all(table, now, order);
  }
}

void MappedOriginal() {
  TEST_CASE("mapped original: borrowed until the first edit");

  static constexpr std::string_view kText = "alpha\nbravo\ncharlie\ndelta\n";
  const auto text_of = [](const PieceTable& t) {
    return ReadDocRange(t, {0, DocLength(t)});
  };

  {
    PieceTable table;
    ResetToMapped(table, kText, nullptr);
    EXPECT_TRUE(table.original_mapped != nullptr);
    EXPECT_TRUE(table.original.empty());
    EXPECT_EQ(OriginalText(table), kText);
    EXPECT_EQ(text_of(table), std::string{kText});
    EXPECT_EQ(LineCount(table), Index{5});
    EXPECT_EQ(LineStart(table, 2), Index{12});
    EXPECT_EQ(LineAt(table, 12), Index{2});
  }

  {
    PieceTable table;
    ResetToMapped(table, kText, nullptr);
    EXPECT_TRUE(!Insert("X", 0, table));
    EXPECT_TRUE(table.original_mapped == nullptr);
    EXPECT_EQ(table.original, std::string{kText});
    EXPECT_EQ(text_of(table), std::string("Xalpha\nbravo\ncharlie\ndelta\n"));
    EXPECT_TRUE(!Undo(table));
    EXPECT_EQ(text_of(table), std::string{kText});
  }

  {
    PieceTable table;
    ResetToMapped(table, kText, nullptr);
    EXPECT_TRUE(!Delete(Interval(0, 6), table));
    EXPECT_TRUE(table.original_mapped == nullptr);
    EXPECT_EQ(text_of(table), std::string("bravo\ncharlie\ndelta\n"));
  }

  {
    auto owner = std::make_shared<std::vector<char>>(kText.begin(), kText.end());
    const std::string_view view{owner->data(), owner->size()};
    PieceTable table;
    ResetToMapped(table, view, std::move(owner));
    EXPECT_TRUE(table.original_mapped != nullptr);

    const Index at = DocLength(table) - 3;
    char before = 0;
    char here = 0;
    EXPECT_TRUE(BytePairAt(table, at, before, here));
    EXPECT_EQ(here, 't');

    EXPECT_TRUE(!Insert("x", at, table));
    EXPECT_TRUE(table.original_mapped == nullptr);
    EXPECT_EQ(text_of(table), std::string("alpha\nbravo\ncharlie\ndelxta\n"));
    EXPECT_TRUE(!Undo(table));
    EXPECT_EQ(text_of(table), std::string{kText});
  }

  {
    PieceTable table;
    ResetToMapped(table, std::string_view{}, nullptr);
    EXPECT_EQ(text_of(table), std::string{});
    EXPECT_EQ(LineCount(table), Index{1});
  }

  {
    std::string big;
    big.reserve(3u << 20);
    for (Index i = 0; i < 60000; ++i) big += "a line of some length here\n";
    PieceTable table;
    ResetToOriginal(table, big);
    EXPECT_TRUE(PieceCount(table) > 1);
    EXPECT_EQ(LineCount(table), Index{60001});
    for (const Index line : {Index{0}, Index{1}, Index{30000}, Index{59999}}) {
      const auto [want_start, want_end] = BruteLineRange(big, line);
      EXPECT_EQ(LineStart(table, line), want_start);
      EXPECT_EQ(LineAt(table, want_start), BruteLineAt(big, want_start));
      std::ignore = want_end;
    }
  }
}

void PieceLookupBoundaries() {
  TEST_CASE("piece table: lookups exactly on a piece boundary");

  {
    PieceTable t = MakeTable("abc");
    ExpectOk(Insert("XY", 3, t), "append at doc end");
    ExpectSameDoc(t, "abcXY", "append at doc end");
    ExpectOk(Undo(t), "undo the append");
    ExpectSameDoc(t, "abc", "undo the append");
    ExpectOk(Redo(t), "redo the append");
    ExpectSameDoc(t, "abcXY", "redo the append");
  }

  for (const bool cold : {false, true}) {
    PieceTable t = MakeTable("abcdef");
    ExpectOk(Insert("--", 3, t), "split into pieces");
    ExpectSameDoc(t, "abc--def", "split into pieces");
    ExpectOk(Insert("Z", 3, t), "insert on the piece seam");
    ExpectSameDoc(t, "abcZ--def", "insert on the piece seam");
    ExpectOk(Undo(t), "undo the seam insert");
    ExpectSameDoc(t, "abc--def", "undo the seam insert");
  }

  for (const bool cold : {false, true}) {
    PieceTable t = MakeTable("abcdef");
    ExpectOk(Insert("--", 3, t), "split into pieces");
    const Index len = DocLength(t);
    ExpectOk(Insert("!", len, t), "append past the last piece");
    ExpectSameDoc(t, "abc--def!", "append past the last piece");
    ExpectOk(Undo(t), "undo the append");
    ExpectSameDoc(t, "abc--def", "undo the append");
  }

  for (const bool cold : {false, true}) {
    PieceTable t = MakeTable("abcdef");
    ExpectOk(Insert("--", 3, t), "split into pieces");
    ExpectOk(Delete(Interval(5, DocLength(t)), t), "delete to the end");
    ExpectSameDoc(t, "abc--", "delete to the end");
    ExpectOk(Undo(t), "undo the tail delete");
    ExpectSameDoc(t, "abc--def", "undo the tail delete");
  }

  {
    PieceTable seed = MakeTable("the quick brown fox jumps over the lazy dog");
    for (const Index at : {30, 20, 4, 12, 38, 8}) ExpectOk(Insert("*", at, seed), "make pieces");
    const std::string base = AssembleDocContents(seed);

    for (Index at = 0; at <= std::ssize(base); ++at) {
      std::string want = base;
      want.insert(static_cast<size_t>(at), "@");

      std::string warm_doc;
      for (const bool cold : {false, true}) {
        PieceTable t = seed;
            ExpectOk(Insert("@", at, t), "sweep insert");
        const std::string got = AssembleDocContents(t);
        ExpectSameDoc(t, want, cold ? "sweep insert (cold)" : "sweep insert (warm)");
        if (cold) {
          ++common::g_test_checks;
          if (got != warm_doc) {
            ++common::g_test_failures;
            std::cerr << "FAIL [" << common::g_test_case
                      << "] recency changed the answer at offset " << at << ": cold \"" << got
                      << "\" vs warm \"" << warm_doc << '"' << std::endl;
          }
        } else {
          warm_doc = got;
        }
      }
    }
  }
}

}  // namespace koi
