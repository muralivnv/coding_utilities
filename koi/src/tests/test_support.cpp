// The definitions behind test_support.h.
//
// They live here rather than inline in the header because every one of the 31
// test files includes it: as inline definitions they were parsed, instantiated
// and emitted once per file, which is most of what an otherwise empty test file
// spent its time on.

#include "test_support.h"

namespace koi {

// A temp path of this process's own. Fixture names used to be fixed, so two
// runs of the suite shared every directory and each one's cleanup deleted the
// other's files mid-test -- which surfaces as unrelated cases failing all over
// the suite, or as a std::filesystem throw out of a destructor, rather than as
// anything that points at a collision.
std::filesystem::path TempFixture(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("koi-" + std::to_string(static_cast<long long>(::getpid())) + "-" + std::string{name});
}

// Fixture cleanup, without the throwing overloads. A directory that could not
// be removed is not worth losing every later test case over, and a
// filesystem_error escaping mid-suite reports no test name at all.
void RemoveAllQuietly(const std::filesystem::path& p) {
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
}

// Writes a fixture file, and leaves it carrying a stamp it did not carry
// before.
//
// A fixture that rewrites a file means "this changed under koi", and koi
// decides whether a file changed by the stamp it carries. Two writes inside one
// mtime tick carry the same stamp -- a whole second of them on ext3, HFS+ and
// many NFS mounts, a clock tick of them on Linux for any inode nobody stat()ed
// in between -- so on some machines, on some runs, the rewrite is invisible and
// the case tests nothing. Nothing sleeps here: the new stamp is simply moved
// past the one it replaced. The same-stamp case has its own test, which forces
// the collision on purpose.
void WriteFixtureFile(const std::filesystem::path& path, std::string_view contents) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const bool existed = fs::exists(path, ec);
  const fs::file_time_type before = existed ? fs::last_write_time(path, ec) : fs::file_time_type{};
  const bool knew_before = existed && !ec;
  {
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out << contents;
  }
  if (!knew_before) return;
  std::error_code now_ec;
  const auto now = fs::last_write_time(path, now_ec);
  if (!now_ec && (now <= before)) {
    fs::last_write_time(path, before + std::chrono::seconds{1}, now_ec);
  }
}

std::string AssembleDocContents(const PieceTable& table) {
  return ReadDocRange(table, Interval(0, DocLength(table)));
}

void ExpectSameDoc(const PieceTable& table, const std::string& want, std::string_view context) {
  const std::string got = AssembleDocContents(table);
  ++common::g_test_checks;
  if (got == want) return;

  ++common::g_test_failures;
  size_t at = 0;
  while (at < got.size() && at < want.size() && got[at] == want[at]) ++at;
  const size_t from = (at > 24) ? at - 24 : 0;
  std::cerr << "FAIL [" << common::g_test_case << "] document mismatch: " << context
            << "\n      first difference at byte " << at << " (got " << got.size() << " bytes, want "
            << want.size() << ")\n      got:  \"" << got.substr(from, 56) << "\"\n      want: \""
            << want.substr(from, 56) << "\"" << std::endl;
}

void ExpectOk(const ErrorCtx& err, std::string_view context) {
  ++common::g_test_checks;
  if (!err) return;
  ++common::g_test_failures;
  std::cerr << "FAIL [" << common::g_test_case << "] " << context << '\n'
            << FormatErrorCtxDebug(err) << std::endl;
}

PieceTable MakeTable(std::string text) {
  PieceTable table;
  ResetToOriginal(table, std::move(text));
  return table;
}

ErrorCtx InsertFunctor(std::string_view txt, Interval doc_range, PieceTable& table,
                       std::string& brute_force) {
  ErrorCtx err = Insert(txt, doc_range.front(), table);
  if (err) return err;
  brute_force.insert(static_cast<size_t>(doc_range.front()), txt);
  return AssembleDocContents(table) == brute_force
             ? Success()
             : MakeErrorCtx(PieceTableErrorCode::kTestingPieceStringNotEqBruteForceString);
}

ErrorCtx DeleteFunctor([[maybe_unused]] std::string_view txt, Interval doc_range, PieceTable& table,
                       std::string& brute_force) {
  ErrorCtx err = Delete(doc_range, table);
  if (err) return err;
  brute_force.erase(static_cast<size_t>(doc_range.front()),
                    static_cast<size_t>(doc_range.size()));
  return AssembleDocContents(table) == brute_force
             ? Success()
             : MakeErrorCtx(PieceTableErrorCode::kTestingPieceStringNotEqBruteForceString);
}

Index Cur(const PieceTable& table, const SelectionSet& sel) {
  return CursorOf(table, sel.Primary());
}

Index Cur(const Editor& ed) { return CursorOf(ed.doc.table, ed.doc.selections.Primary()); }

Index RangeTo(const SelectionSet& sel, std::size_t i) {
  return (i < sel.Ranges().size()) ? sel.Ranges()[i].To() : Index{-2};
}

Key K(std::string_view text) {
  Key key;
  EXPECT_TRUE(ParseKey(text, key));
  return key;
}

void TypeInto(Editor& ed, char c) {
  ed.mode = Mode::kInsert;
  std::vector<Key> pending{Key{.code = static_cast<std::uint32_t>(c)}};
  FlushPendingAsText(ed, pending);
  ed.mode = Mode::kNormal;

  ApplyModeInvariants(ed);
}

// What the undo chain must be true of at rest, checked for every buffer below
// and directly by the history tests -- so the property and fuzz runs that
// already call EditorInvariants inherit it.
//
// revisions[0] is the floor: it is a *state*, not a step. Nothing parents it,
// undo stops on it rather than walking out of it, and its `forward` is empty --
// TrimHistory clears it on whatever it re-roots the chain onto for exactly that
// reason. An edit folded in there is one undo steps straight past: it is in the
// document and there is no way left to take it back.
std::string UndoChainInvariants(const PieceTable& table) {
  const Index count = std::ssize(table.revisions);
  if (count == 0) {
    // Never loaded. `current` indexes nothing and must say so.
    return (table.current == 0) ? std::string{} : "no revisions but current names one";
  }
  if ((table.current < 0) || (table.current >= count)) return "current is not a revision";
  if (table.revisions[0].parent != -1) return "the history base has a parent";
  if (!table.revisions[0].forward.empty()) return "an edit was folded into the history base";
  for (Index i = 1; i < count; ++i) {
    const Revision& rev = table.revisions[static_cast<std::size_t>(i)];
    // Parents point strictly backwards and children strictly forwards: both
    // walks (undo, redo, and TrimHistory's chain) then have to terminate.
    if ((rev.parent < 0) || (rev.parent >= i)) return "a revision's parent is not below it";
    if (rev.last_child >= count) return "a revision's redo branch is not a revision";
    if ((rev.last_child >= 0) && (rev.last_child <= i)) return "a redo branch points backwards";
  }
  if ((table.revisions[0].last_child >= count) || (table.revisions[0].last_child == 0)) {
    return "the base's redo branch is not a revision above it";
  }
  return {};
}

std::string EditorInvariants(const Editor& ed) {
  const std::size_t buffers = BufferCount(ed);
  if (buffers == 0) return "no buffers";
  if (ed.active >= buffers) return "active buffer out of range";

  for (std::size_t b = 0; b < buffers; ++b) {
    if (std::string broke = UndoChainInvariants(BufferAt(ed, b).table); !broke.empty()) {
      return broke;
    }
  }

  // A refused edit on top of an already-written one in the same undo group.
  // Checked for every buffer, not just the active one: the command that did it
  // has usually moved on by the time anything looks.
  for (std::size_t b = 0; b < buffers; ++b) {
    if (BufferAt(ed, b).table.partial_group_edits != 0) return "an undo group was half applied";
  }

  const std::vector<int> order = WindowOrder(ed);
  const std::size_t windows = WindowCount(ed);
  if (windows == 0) return "no windows";

  if (!ed.windows.empty()) {
    if (order.empty()) return "tree has no leaves";
    if (order.size() != windows) return "leaf count disagrees with WindowCount";
    if (std::ranges::find(order, ed.focused) == order.end()) return "focus is not a live leaf";

    std::vector<int> seen(ed.windows.size(), 0);
    std::vector<int> stack{0};
    while (!stack.empty()) {
      const int at = stack.back();
      stack.pop_back();
      if ((at < 0) || (at >= std::ssize(ed.windows))) return "child index out of range";
      const WindowNode& n = ed.windows[static_cast<std::size_t>(at)];
      if (n.dead) return "a dead node is still in the tree";
      if (++seen[static_cast<std::size_t>(at)] > 1) return "node reachable twice (cycle or share)";
      if (n.kind == WindowNode::Kind::kLeaf) {
        if (n.buffer >= buffers) return "window names a buffer that is not there";
        continue;
      }
      if ((n.first < 0) || (n.second < 0)) return "internal node missing a child";
      if ((n.first == at) || (n.second == at)) return "node is its own child";
      if (!(n.ratio > 0.0) || !(n.ratio < 1.0)) return "split ratio is not a fraction";
      stack.push_back(n.first);
      stack.push_back(n.second);
    }
  }

  constexpr int kW = 100;
  constexpr int kH = 40;
  const std::vector<Rect> laid = LayoutWindows(ed, Rect{0, 0, kW, kH});
  if (laid.size() != windows) return "layout returned the wrong number of rectangles";
  long area = 0;
  for (const Rect& r : laid) {
    if ((r.w < 0) || (r.h < 0)) return "negative rectangle";
    if ((r.x < 0) || (r.y < 0)) return "rectangle outside the screen";
    if (((r.x + r.w) > kW) || ((r.y + r.h) > kH)) return "rectangle past the screen";
    area += static_cast<long>(r.w) * r.h;
  }
  if (area != (static_cast<long>(kW) * kH)) return "layout does not tile the screen";

  for (std::size_t i = 0; i < buffers; ++i) {
    if (const std::string_view broke = pt::Validate(BufferAt(ed, i).table.tree); !broke.empty()) {
      return std::string{"the piece tree is malformed: "} + std::string{broke};
    }
  }

  for (const PendingCommand& job : ed.pending_commands) {
    if (job.view_name.empty()) return "a pending command has no view";
    if (job.scan != nullptr) {
      if (job.pid != -1) return "a scan job names a child";
      if (job.fd != -1) return "a scan job holds a pipe";
    } else if (job.done) {
      if (job.pid != -1) return "a finished command still names its child";
      if (job.fd != -1) return "a finished command still holds its pipe";
    } else {
      if (job.pid <= 0) return "a pending command has no child";
      if (job.fd < 0) return "a pending command has no pipe";
    }
  }

  for (std::size_t i = 0; i < buffers; ++i) {
    const PieceTable& t = BufferAt(ed, i).table;
    if (std::ssize(t.journal) != (t.revision - t.journal_base)) {
      return "the journal does not line up with the revision counter";
    }
  }

  for (const int leaf : order) {
    if (leaf == ed.focused) continue;
    const WindowNode& node = ed.windows[static_cast<std::size_t>(leaf)];
    if (node.selections_revision > BufferAt(ed, node.buffer).table.revision) {
      return "a window's selection stamp is not a revision of its buffer";
    }
  }

  for (std::size_t i = 0; i < buffers; ++i) {
    const Document& doc = BufferAt(ed, i);
    if (IsExcerptView(doc)) {
      if (doc.capture_styles.size() != doc.excerpts.capture_names.size()) {
        return "an excerpt view's colour table is not its own capture table";
      }
      const auto block_broken = [](const ExcerptBlock& block) -> const char* {
        if (block.path.empty()) return "an excerpt block names no file";
        if (block.first < 1) return "an excerpt block starts before line 1";
        if (block.last < block.first - 1) return "an excerpt block's span is inverted";
        if (!block.no_body && (std::ssize(block.original) < (block.last - block.first))) {
          return "an excerpt block's original cannot cover its span";
        }
        return nullptr;
      };
      const auto view_broken = [&block_broken](const ExcerptView& view) -> const char* {
        std::size_t headers = 0;
        for (const ExcerptBlock& block : view.blocks) headers += 1 + block.prior_headers.size();
        for (const DroppedExcerpt& gone : view.dropped) {
          headers += 1 + gone.block.prior_headers.size();
        }
        if (view.header_index.size() != headers) {
          return "an excerpt view's header index does not cover its blocks";
        }
        if (!std::ranges::is_sorted(view.header_index)) {
          return "an excerpt view's header index is not sorted";
        }
        for (const ExcerptBlock& block : view.blocks) {
          if (const char* broke = block_broken(block)) return broke;
        }
        for (const DroppedExcerpt& gone : view.dropped) {
          if (const char* broke = block_broken(gone.block)) return broke;
        }
        return nullptr;
      };
      if (const char* broke = view_broken(doc.excerpts)) return broke;
      const ExcerptEpochs& epochs = doc.excerpt_epochs;
      if (epochs.store.empty()) {
        if (!epochs.boundaries.empty()) return "excerpt epochs have boundaries but no store";
        if (epochs.active != 0) return "excerpt epochs are active outside an empty store";
      } else {
        if (epochs.store.size() != epochs.boundaries.size() + 1) {
          return "excerpt epochs' store does not pair with its boundaries";
        }
        if (epochs.active >= epochs.store.size()) return "the active excerpt epoch is out of range";
        if (!std::ranges::is_sorted(epochs.boundaries)) {
          return "excerpt epoch boundaries are out of order";
        }
        // `active` is a claim about the text: the model in `doc.excerpts`
        // describes the newest rebuild whose edit is still applied. That claim
        // is what DropUnreachableEpochs resizes on, so it has to be true at
        // every step, not only just after AlignExcerptModel has run.
        std::size_t desired = 0;
        for (std::size_t e = epochs.boundaries.size(); e > 0; --e) {
          if (SerialApplied(doc.table, epochs.boundaries[e - 1])) {
            desired = e;
            break;
          }
        }
        if (desired != epochs.active) return "the active excerpt epoch is not the applied one";
        for (std::size_t e = 0; e < epochs.store.size(); ++e) {
          if (e == epochs.active) continue;
          if (const char* broke = view_broken(epochs.store[e])) return broke;
        }
      }
      continue;
    }
    if (doc.syntax == nullptr) {
      if (!doc.capture_styles.empty()) return "a buffer with no grammar has colours for one";
      continue;
    }
    if (doc.capture_styles.empty()) continue;
    if (doc.capture_styles.size() != doc.syntax->CaptureNames().size()) {
      return "a buffer's colour table is another grammar's";
    }
  }

  const Index length = DocLength(ed.doc.table);
  for (const Selection& sel : ed.doc.selections.Ranges()) {
    if ((sel.anchor < 0) || (sel.head < 0)) return "negative selection";
    if ((sel.anchor > length) || (sel.head > length)) return "selection past the end";
  }

  const auto& ranges = ed.doc.selections.Ranges();
  if (ranges.empty()) return "no selections at all";
  if (ed.doc.selections.PrimaryIndex() >= ranges.size()) return "primary is not a selection";
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    const Selection& s = ranges[i];

    for (const Index at : {s.From(), s.To()}) {
      if ((at > 0) && (at < length) && !IsGraphemeBoundary(ed.doc.table, at)) {
        return "a selection edge is inside a grapheme";
      }
    }
    if (i > 0) {
      const Selection& prev = ranges[i - 1];
      if (s.From() < prev.From()) return "selections are out of document order";
      if (s.From() < prev.To()) return "selections overlap";
    }

    if ((ed.mode != Mode::kInsert) && s.IsEmpty() && (s.From() < length)) {
      return "a bare caret survived into normal mode";
    }
  }
  return {};
}

std::string NumberedLines(int count) {
  std::string out;
  for (int i = 1; i <= count; ++i) {
    out += "line-" + std::to_string(i) + "\n";
  }
  return out;
}

void PumpUntilIdle(Editor& ed) {
  for (int i = 0; (i < 20000) && PumpCommandJobs(ed); ++i) usleep(500);
}

// Runs a body on a thread of its own and waits for it there.
//
// Everything the scanner caches -- which grammars loaded, which queries
// compiled, which languages have a query at all -- is thread_local and sticky
// for the life of the thread, deliberately. A case that hands the scanner a
// deliberately broken or deliberately pathological query would leave those
// entries behind for every later case on the same thread, and the entry is
// keyed by language, so "cpp definitions" would stay broken for the rest of the
// run. On its own thread the poisoning dies with the thread. Nothing runs
// concurrently -- the caller is inside join() throughout -- so the harness's
// plain counters are as safe here as anywhere.
void OnAThreadOfItsOwn(const std::function<void()>& body) {
  std::thread worker{[&body] { body(); }};
  worker.join();
}

// The query half of a degenerate fixture, shared by the two cases below.
//
// The shape is the symbol scan's, because the defect is: patterns of the form
// "an identifier, then later a number", over a call whose last argument is the
// only number in it. Every identifier starts a match that cannot finish until
// the argument list ends, so the states in flight are the identifiers times the
// patterns, and that cost is paid again for every call in the file. Nothing koi
// ships looks remotely like this -- measured, every source in this repository
// and six thousand third-party ones peak in the low tens of simultaneous states
// -- which is why the query has to come from a runtime root of the case's own.
std::string PilingUpQuery(int patterns, std::string_view first, std::string_view second) {
  std::string out;
  for (int p = 0; p < patterns; ++p) {
    out += "(argument_list (identifier) @" + std::string{first} + " (number_literal) @" +
           std::string{second} + ")\n";
  }
  return out;
}

// The other half: `calls` calls of `arguments` identifiers each, ending in the
// one number. `spread` gives each call a function of its own, which is what a
// text-object lookup wants to find; false puts them all in one body, which
// keeps the overview's own O(calls x functions) attribution out of the timing.
std::string WideCalls(int arguments, int calls, bool spread) {
  std::string out = spread ? "" : "void only() {\n";
  for (int c = 0; c < calls; ++c) {
    out += spread ? ("void d" + std::to_string(c) + "() { g(") : "  g(";
    for (int i = 0; i < arguments; ++i) out += "x" + std::to_string(i) + ", ";
    out += spread ? "7); }\n" : "7);\n";
  }
  if (!spread) out += "}\n";
  return out;
}

Rng::Rng(unsigned long long s)
    : gen{s}, seed{s} {}

Index Rng::Pick(Index lo, Index hi) {
  return std::uniform_int_distribution<Index>(lo, hi)(gen);
}

Scratch::Scratch(std::string_view name)
    : dir{TempFixture(name)} {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
}

Scratch::~Scratch() {
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

std::filesystem::path Scratch::Write(std::string_view name, std::string_view contents) const {
  const std::filesystem::path path = dir / name;
  WriteFixtureFile(path, contents);
  return path;
}

FakeQueryDir::FakeQueryDir(std::string_view language) {
  const char* home = std::getenv("HOME");
  if ((home == nullptr) || (*home == '\0')) return;
  dir = std::filesystem::path{home} / ".config" / "ronin" / "koi" / "queries" / language;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
}

FakeQueryDir::~FakeQueryDir() { Forget(); }

bool FakeQueryDir::Ready() const { return !dir.empty() && std::filesystem::exists(dir); }

void FakeQueryDir::Write(std::string_view file, std::string_view source) const {
  if (!dir.empty()) WriteFixtureFile(dir / file, source);
}

void FakeQueryDir::Forget() const {
  if (dir.empty()) return;
  RemoveAllQuietly(dir);
}

void Presser::operator()(Editor& ed, std::string_view key) {
  pending.clear();
  HandleKeyInput(ed, maps, K(key), pending);
}

}  // namespace koi
