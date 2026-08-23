#include "editor.h"

#include "crash.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include "commands.h"
#include "query.h"
#include "sha1.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

Index PrimaryCursor(const Document& doc) {
  return CursorOf(doc.table, doc.selections.Primary());
}

Index PrimaryLine(const Document& doc) { return LineAt(doc.table, PrimaryCursor(doc)); }

Index PrimaryColumn(const Document& doc) {
  return ColumnForByte(doc.table, PrimaryCursor(doc), doc.tab_width);
}

}

namespace {

bool AllDigits(std::string_view s) {
  if (s.empty()) return false;
  for (const char c : s) {
    if ((c < '0') || (c > '9')) return false;
  }
  return true;
}

bool ParseIndex(std::string_view s, Index& out) {
  Index value = 0;
  for (const char c : s) {
    if (value > (std::numeric_limits<Index>::max() - 9) / 10) return false;
    value = value * 10 + (c - '0');
  }
  out = value;
  return true;
}

}

Target ParseTarget(std::string_view arg) {
  Target target;
  const std::string whole{arg};

  std::string base = whole;
  std::vector<std::string> bases{whole};
  std::vector<Index> numbers;
  for (int i = 0; i < 2; ++i) {
    const size_t colon = base.rfind(':');
    if ((colon == std::string::npos) || (colon + 1 >= base.size())) break;
    const std::string tail = base.substr(colon + 1);
    Index value = 0;
    if (!AllDigits(tail) || !ParseIndex(tail, value)) break;
    numbers.push_back(value);
    base = base.substr(0, colon);
    bases.push_back(base);
    if (base.empty()) break;
  }

  std::error_code ec;
  size_t chosen = bases.size() - 1;
  for (size_t i = 0; i < bases.size(); ++i) {
    if (!bases[i].empty() && fs::exists(bases[i], ec)) {
      chosen = i;
      break;
    }
  }

  target.path = bases[chosen];
  if (chosen >= 1) {
    target.line = numbers[chosen - 1];
    target.has_line = true;
  }
  if (chosen >= 2) {
    target.column = numbers[chosen - 2];
    target.has_column = true;
    target.line = numbers[chosen - 1];
  }
  return target;
}

void GoToTarget(Document& doc, const Target& target) {
  if (!target.has_line) return;
  const Index last_line = LineCount(doc.table) - 1;
  const Index row = std::clamp<Index>(target.line - 1, 0, last_line);
  const Index line_start = LineStart(doc.table, row);

  Index at = line_start;
  if (target.has_column && (target.column > 1)) {
    const Interval content = LineContentRange(doc.table, row);
    const Index content_end = content.empty() ? line_start : content.back() + 1;
    at = std::clamp<Index>(line_start + target.column - 1, line_start, content_end);
  }
  at = SnapToGraphemeBoundary(doc.table, at);
  doc.selections.Set(MinWidth1(doc.table, Selection{at, at, -1}));
}

fs::path GlobalConfigPath() {
  const char* home = std::getenv("HOME");
  if ((home == nullptr) || (*home == '\0')) return {};
  return fs::path{home} / ".config" / "ronin" / "koi.toml";
}

fs::path LocalConfigPath() { return fs::path{".ronin"} / "koi.toml"; }

std::vector<fs::path> ConfigPaths() {
  std::vector<fs::path> paths;
  std::error_code ec;
  for (const fs::path& candidate : {GlobalConfigPath(), LocalConfigPath()}) {
    if (!candidate.empty() && fs::exists(candidate, ec)) paths.push_back(candidate);
  }
  return paths;
}

fs::path ConfigPath() {
  const fs::path local = LocalConfigPath();
  std::error_code ec;
  if (fs::exists(local, ec)) return local;
  return GlobalConfigPath();
}

namespace {

constexpr Index kMaxIndentAmount = 64;

constexpr Index kIndentScanLines = 20000;
constexpr Index kIndentScanBytes = 2 << 20;

constexpr int kSpace = 0;
constexpr int kTab = 1;

struct IndentTally {
  std::array<std::array<std::int32_t, kMaxIndentAmount + 1>, 2> used{};
  std::array<std::array<std::int32_t, kMaxIndentAmount + 1>, 2> weight{};
  bool any{false};
};

void TallyLine(std::string_view line, bool ignore_single_spaces, IndentTally& tally,
               Index& prev_size, int& prev_type, int& key_type, Index& key_amount) {
  if (!line.empty() && (line.back() == '\r')) line.remove_suffix(1);
  if (line.empty()) return;

  int type;
  std::size_t run = 0;
  if (line.front() == ' ') {
    type = kSpace;
    while ((run < line.size()) && (line[run] == ' ')) ++run;
  } else if (line.front() == '\t') {
    type = kTab;
    while ((run < line.size()) && (line[run] == '\t')) ++run;
  } else {
    prev_size = 0;
    prev_type = -1;
    return;
  }

  const auto indent = static_cast<Index>(run);
  if (ignore_single_spaces && (type == kSpace) && (indent == 1)) return;

  if (type != prev_type) prev_size = 0;
  prev_type = type;

  std::int32_t use = 1;
  std::int32_t weight = 0;
  const Index difference = indent - prev_size;
  prev_size = indent;

  if (difference == 0) {
    use = 0;
    weight = 1;
    if (key_type < 0) return;
  } else {
    const Index amount = (difference < 0) ? -difference : difference;
    if (amount > kMaxIndentAmount) return;
    key_type = type;
    key_amount = amount;
  }

  tally.used[static_cast<size_t>(key_type)][static_cast<size_t>(key_amount)] += use;
  tally.weight[static_cast<size_t>(key_type)][static_cast<size_t>(key_amount)] += weight;
  tally.any = true;
}

void TallyIndents(const PieceTable& table, bool ignore_single_spaces, IndentTally& tally) {
  constexpr Index kChunk = 64 * 1024;
  const Index length = std::min<Index>(DocLength(table), kIndentScanBytes);

  Index prev_size = 0;
  int prev_type = -1;
  int key_type = -1;
  Index key_amount = 0;
  Index lines = 0;

  std::string chunk;
  Index at = 0;
  bool mid_line = false;

  while ((at < length) && (lines < kIndentScanLines)) {
    const Index end = std::min(length, at + kChunk);
    ReadDocRangeInto(table, Interval(at, end), chunk);
    const std::string_view view{chunk};
    const bool last_chunk = (end == length);

    std::size_t pos = 0;
    while (pos < view.size()) {
      const std::size_t nl = view.find('\n', pos);
      const bool terminated = (nl != std::string_view::npos);
      if (!terminated && !last_chunk) break;
      const std::size_t line_end = terminated ? nl : view.size();

      if (mid_line) {
        mid_line = false;
      } else {
        TallyLine(view.substr(pos, line_end - pos), ignore_single_spaces, tally, prev_size,
                  prev_type, key_type, key_amount);
        if (++lines >= kIndentScanLines) break;
      }
      if (!terminated) {
        pos = view.size();
        break;
      }
      pos = nl + 1;
    }

    if (pos == 0) {
      mid_line = true;
      at = end;
    } else {
      at += static_cast<Index>(pos);
    }
  }
}

bool MostUsedIndent(const IndentTally& tally, int& type, Index& amount) {
  std::int32_t best_used = 0;
  std::int32_t best_weight = 0;
  bool found = false;
  for (int t = 0; t < 2; ++t) {
    for (Index n = 1; n <= kMaxIndentAmount; ++n) {
      const std::int32_t used = tally.used[static_cast<size_t>(t)][static_cast<size_t>(n)];
      const std::int32_t weight = tally.weight[static_cast<size_t>(t)][static_cast<size_t>(n)];
      if ((used == 0) && (weight == 0)) continue;
      if ((used > best_used) || ((used == best_used) && (weight > best_weight))) {
        best_used = used;
        best_weight = weight;
        type = t;
        amount = n;
        found = true;
      }
    }
  }
  return found;
}

}

void DetectIndentation(Document& doc) {
  IndentTally tally;
  TallyIndents(doc.table, true, tally);
  if (!tally.any) {
    tally = IndentTally{};
    TallyIndents(doc.table, false, tally);
  }

  int type = kSpace;
  Index amount = 0;
  if (!MostUsedIndent(tally, type, amount)) return;

  if (type == kTab) {
    doc.insert_spaces = false;
    return;
  }
  doc.insert_spaces = true;
  doc.tab_width = std::clamp<Index>(amount, 1, 16);
}

ErrorCtx LoadDocument(const fs::path& path, Document& doc) {
  doc.file = path;
  doc.view_name.clear();
  doc.read_only = false;
  doc.disk_stamp = FileStamp{};
  doc.disk_blob.clear();
  doc.saved_undo_serial = 0;
  // The one call site that wants a missing file to read as an empty document --
  // opening a path that is not there yet is how a new file is started. Said
  // here rather than left to the read below, which reports every failure and
  // cannot tell this one apart on the caller's behalf.
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    ResetToOriginal(doc.table, std::string{});
    doc.selections.Set(Selection{});
    doc.selections.EnsureBlockCursors(doc.table);
    doc.modified = false;
    return Success();
  }

  doc.read_only = (::access(path.c_str(), W_OK) != 0);

  // Taken before the read, and kept only if the read succeeds: a foreign write
  // that lands while we are reading leaves us holding the older stamp, so the
  // next :w warns rather than passing off half of their file as ours.
  FileStamp stamp;
  const bool stamped = StampFile(path.string(), stamp);

  std::error_code read_ec;
  std::string text = ReadWholeFile(path, read_ec);
  if (read_ec) return MakeErrorCtx(static_cast<std::errc>(read_ec.value()));
  if (!IsWellFormedUtf8(text)) {
    return MakeErrorCtx(PieceTableErrorCode::kMalformedUtf8Input);
  }
  doc.disk_blob = BlobOidOf(text);
  ResetToOriginal(doc.table, std::move(text));
  DetectIndentation(doc);
  doc.selections.Set(Selection{});
  doc.selections.EnsureBlockCursors(doc.table);
  doc.modified = false;
  if (stamped) doc.disk_stamp = std::move(stamp);
  return Success();
}

std::string BlobOidOf(std::string_view text) {
  if (text.size() > kMaxBlobBytes) return {};
  return GitBlobOid(text);
}

bool StampFile(const std::string& path, FileStamp& out) {
  std::error_code time_ec;
  const auto mtime = fs::last_write_time(path, time_ec);
  if (time_ec) return false;
  std::error_code size_ec;
  const auto size = fs::file_size(path, size_ec);
  out.path = path;
  out.mtime = mtime;
  // A file whose size cannot be read is still worth stamping by mtime -- it is
  // the same file to every comparison that follows, since both sides of one
  // will have failed the same way.
  out.size = size_ec ? 0 : size;
  return true;
}

bool ExternallyModified(const Document& doc) {
  if (!HasDiskFile(doc)) return false;
  FileStamp now;
  if (!StampFile(doc.file.string(), now)) return false;
  // (mtime, size), and no further. A foreign write that lands in the same mtime
  // tick as ours *and* leaves the byte count exactly where it was is invisible
  // here, and stays so: telling it apart means reading the whole file back on
  // every focus-in and every :w, which is the cost this check exists to spare.
  // That is the same boundary the excerpt views draw (see FileStamp), and the
  // size is what carries it -- without it, koi's own write-then-rename makes
  // the same-tick case the common one rather than the corner it sounds like.
  return !now.SameFile(doc.disk_stamp);
}

namespace {

bool WriteAll(int fd, std::string_view text) {
  // EINTR is the transient a raw write has to survive. Bounded the same way the
  // read side is, and reset by any byte of progress, so a signal storm cannot
  // spin a save forever -- giving up leaves errno EINTR, which the caller
  // reports like any other write failure.
  constexpr int kMaxInterrupts = 64;
  int interrupts = 0;
  std::size_t done = 0;
  while (done < text.size()) {
    const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
    if (n > 0) {
      done += static_cast<std::size_t>(n);
      interrupts = 0;
    } else if ((n < 0) && (errno == EINTR) && (++interrupts <= kMaxInterrupts)) {
      continue;
    } else {
      if (n == 0) errno = EIO;
      return false;
    }
  }
  return true;
}

void SyncDirectoryOf(const fs::path& path) {
  const fs::path dir = path.has_parent_path() ? path.parent_path() : fs::path{"."};
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

// rename() acts on the link, not on what it points at. Writing through a
// symlink therefore has to be aimed at the file the user believes they opened,
// or the replacement lands beside it: the symlink is deleted, the edit ends up
// in a new regular file, and whatever the link pointed into -- usually a
// dotfiles repository -- silently keeps the old contents and reports itself
// clean. Resolved hop by hop rather than with weakly_canonical because only the
// final component matters here and a cycle must not hang the save.
// Empty when the chain did not end -- a loop, or one longer than the kernel
// itself would follow. Giving up has to be an error rather than "write to
// whatever link we stopped on", because that link is still a symlink and
// renaming over it is the very thing this exists to prevent.
std::optional<fs::path> ResolveFinalSymlink(const fs::path& path) {
  constexpr int kMaxHops = 40;  // what Linux allows before it returns ELOOP
  fs::path at = path;
  for (int hop = 0; hop <= kMaxHops; ++hop) {
    std::error_code ec;
    if (!fs::is_symlink(fs::symlink_status(at, ec)) || ec) return at;
    if (hop == kMaxHops) return std::nullopt;
    const fs::path next = fs::read_symlink(at, ec);
    if (ec) return at;
    at = next.is_absolute() ? next : (at.parent_path() / next);
  }
  return std::nullopt;
}

// Keeps the inode, and with it every other name pointing at it. Not atomic --
// it cannot be, because replacing a file atomically means installing a *new*
// inode, which is exactly what detaches the other links. A file with more than
// one name has implicitly asked for this trade. Written before truncating, so a
// short write leaves a longer file rather than a destroyed one.
ErrorCtx WriteInPlace(const fs::path& path, std::string_view text) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return MakeErrorCtx(static_cast<std::errc>(errno ? errno : EIO));
  const auto fail = [fd](int err) {
    ::close(fd);
    return MakeErrorCtx(static_cast<std::errc>(err ? err : EIO));
  };
  if (!WriteAll(fd, text)) return fail(errno);
  if (::ftruncate(fd, static_cast<off_t>(text.size())) != 0) return fail(errno);
  if (::fsync(fd) != 0) return fail(errno);
  if (::close(fd) != 0) return MakeErrorCtx(static_cast<std::errc>(errno ? errno : EIO));
  return Success();
}

// One temp inode per writer. The name used to be a fixed <target>.koi-tmp, so
// two koi processes saving the same file opened the same one: their writes
// interleaved, and whichever renamed first published the other's half-written
// bytes -- or removed the temp the other was about to rename. The pid separates
// processes, the counter separates threads and successive saves within one, and
// O_EXCL is what makes the name a claim rather than a hope. Same directory as
// the target, or the rename crosses a filesystem. The .koi-tmp tail stays
// *last*, so everything that knew the old name by its suffix -- ignore files, a
// `rm *.koi-tmp` habit -- still matches.
fs::path TempPathFor(const fs::path& target, unsigned long long seq) {
  fs::path tmp = target;
  tmp += "." + std::to_string(static_cast<long long>(::getpid())) + "-" +
         std::to_string(seq) + ".koi-tmp";
  return tmp;
}

}

ErrorCtx AtomicWriteFile(const fs::path& path, std::string_view text) {
  if (path.empty()) return MakeErrorCtx(std::errc::invalid_argument);

  // Everything below works on the resolved file, not on the name it was reached
  // by -- including the temp path, which has to be its neighbour or the rename
  // crosses a filesystem and fails with EXDEV.
  const std::optional<fs::path> resolved = ResolveFinalSymlink(path);
  if (!resolved) return MakeErrorCtx(std::errc::too_many_symbolic_link_levels);
  const fs::path& target = *resolved;

  // More than one name for this inode: a rename would leave the others on the
  // old content, which is the same silent divergence as the symlink case one
  // indirection further out. Atomicity is what gets given up instead.
  std::error_code links_ec;
  if (const auto links = fs::hard_link_count(target, links_ec); !links_ec && (links > 1)) {
    return WriteInPlace(target, text);
  }

  std::error_code perm_ec;
  const fs::file_status existing = fs::status(target, perm_ec);
  const bool have_existing = !perm_ec && fs::exists(existing);

  const mode_t create_mode = have_existing ? 0600 : 0666;
  // EEXIST on a name carrying our own pid can only be debris a dead process
  // that once held that pid left behind -- no live process shares it -- so the
  // next counter value is enough, and nothing here ever deletes a file to make
  // room for itself. That deletion is what one shared name forced, and it is
  // how one writer used to unlink the temp another was about to rename.
  static std::atomic<unsigned long long> next_seq{0};
  fs::path tmp;
  int fd = -1;
  for (int attempt = 0; attempt < 8; ++attempt) {
    tmp = TempPathFor(target, next_seq.fetch_add(1, std::memory_order_relaxed));
    fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, create_mode);
    if ((fd >= 0) || (errno != EEXIST)) break;
  }
  if (fd < 0) return MakeErrorCtx(static_cast<std::errc>(errno ? errno : EIO));

  const auto fail = [&](int err) {
    ::close(fd);
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return MakeErrorCtx(static_cast<std::errc>(err ? err : EIO));
  };

  if (have_existing) {
    const auto mode = static_cast<mode_t>(existing.permissions() & fs::perms::mask);
    if (::fchmod(fd, mode) != 0) return fail(errno);
  }
  if (!WriteAll(fd, text)) return fail(errno);
  if (::fsync(fd) != 0) return fail(errno);
  if (::close(fd) != 0) {
    const int err = errno;
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return MakeErrorCtx(static_cast<std::errc>(err ? err : EIO));
  }

  std::error_code ec;
  fs::rename(tmp, target, ec);
  if (ec) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    return ErrorCtx{ec, std::source_location::current()};
  }

  // Debris from when the temp was one shared name: no build creates it any
  // more, so removing it cannot take a live writer's file, and left alone it
  // sits beside the saved file forever. Per-writer temps a crash leaves behind
  // are deliberately *not* swept -- telling a dead writer's temp from a live
  // one means trusting the pid in the name, and pids are reused, so the sweep
  // would eventually delete the file some other koi is about to rename. They
  // are small, rare, and named in plain sight next to the file they belong to.
  fs::path legacy = target;
  legacy += ".koi-tmp";
  std::error_code legacy_ec;
  fs::remove(legacy, legacy_ec);

  SyncDirectoryOf(target);
  return Success();
}

ErrorCtx SaveDocumentAs(Document& doc, const fs::path& path, std::string* wrote) {
  std::string text = ReadDocRange(doc.table, Interval(0, DocLength(doc.table)));
  if (const ErrorCtx err = AtomicWriteFile(path, text); err) return err;
  // The bytes that just became the file's, so this is the one other place the
  // blob is free -- and the record path never hashes a buffer of its own.
  doc.disk_blob = BlobOidOf(text);
  doc.file = path;
  doc.view_name.clear();
  doc.modified = false;
  doc.saved_undo_serial = CurrentUndoSerial(doc.table);
  MarkUndoSavePoint(doc.table);
  // Re-stamped from the file we just wrote, not from the text we wrote into it.
  doc.disk_stamp = FileStamp{};
  std::ignore = StampFile(path.string(), doc.disk_stamp);
  std::error_code recover_ec;
  fs::remove(RecoveryPathFor(path.string()), recover_ec);
  if (wrote != nullptr) *wrote = std::move(text);
  return Success();
}

namespace {

Index AdvanceOf(std::string_view cluster, Index column, Index tab_width) {
  if (cluster == "\t") return tab_width - (column % tab_width);
  return std::max<Index>(1, GraphemeWidth(cluster));
}

}

void LayoutLine(const PieceTable& table, Index line, const WrapMetrics& wrap,
                std::vector<Index>& row_starts, std::string& scratch) {
  const Index line_start = LineStart(table, line);
  row_starts.assign(1, line_start);
  if (!wrap.enabled) return;

  const Index first_width = std::max<Index>(1, wrap.width);
  const Index cont_width = std::max<Index>(1, wrap.width - wrap.indent);
  ReadDocRangeInto(table, LineContentRange(table, line), scratch);

  Index limit = first_width;
  Index column = 0;
  std::size_t row_begin = 0;
  std::size_t space = std::string::npos;
  Index space_column = 0;
  std::size_t i = 0;
  while (i < scratch.size()) {
    const std::size_t next = NextGraphemeInString(scratch, i);
    const std::string_view cluster{scratch.data() + i, next - i};
    const Index advance = AdvanceOf(cluster, column, wrap.tab_width);

    if (((column + advance) > limit) && (column > 0)) {
      std::size_t brk = i;
      if ((space != std::string::npos) && (space > row_begin) &&
          ((column - space_column) <= wrap.max_wrap)) {
        brk = space;
      }
      row_begin = brk;
      row_starts.push_back(line_start + static_cast<Index>(brk));
      limit = cont_width;
      column = 0;
      space = std::string::npos;
      i = brk;
      continue;
    }

    column += advance;
    i = next;
    if ((cluster == " ") || (cluster == "\t")) {
      space = i;
      space_column = column;
    }
  }
}

Index WrappedRows(const PieceTable& table, Index line, const WrapMetrics& wrap) {
  if (!wrap.enabled) return 1;
  std::vector<Index> row_starts;
  std::string scratch;
  LayoutLine(table, line, wrap, row_starts, scratch);
  return static_cast<Index>(row_starts.size());
}

Index RowOfPosition(const std::vector<Index>& row_starts, Index pos) {
  Index row = 0;
  for (std::size_t k = 1; k < row_starts.size(); ++k) {
    if (pos < row_starts[k]) break;
    row = static_cast<Index>(k);
  }
  return row;
}

Index ColumnBetween(const PieceTable& table, Index from, Index pos, Index tab_width) {
  if (pos <= from) return 0;
  const std::string text = ReadDocRange(table, Interval(from, pos));
  Index column = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t next = NextGraphemeInString(text, i);
    column += AdvanceOf(std::string_view{text.data() + i, next - i}, column, tab_width);
    i = next;
  }
  return column;
}

Index ByteForColumnFrom(const PieceTable& table, Index from, Index end, Index column,
                        Index tab_width) {
  if (end <= from) return from;
  if (column <= 0) return from;
  // The callers -- the horizontal clip, the leap, a click -- want the first
  // few hundred columns of a line that can be megabytes long, and reading it
  // whole makes the cost follow the line rather than the pane. Every cluster
  // is worth at least one column, so `column` clusters is the most the walk
  // can step, and four bytes each covers text that is one cluster per column.
  //
  // It is not a bound: a base with a stack of combining marks is one column
  // and arbitrarily many bytes. So the window is a guess that is checked --
  // the walk only trusts an answer it reached with bytes to spare, because a
  // window ending inside a cluster would otherwise return a position that is
  // not a grapheme boundary. Running out doubles the window and tries again,
  // so the total read stays within twice what the answer needed.
  //
  // Bytes to spare is not enough on its own. A window that cuts *mid-encoding*
  // of a joining character leaves an orphan byte the segmenter cannot attach,
  // so the cluster before it ends where the document's does not -- an early
  // stop right there would return a byte inside a real cluster, with spare
  // bytes still in the window. Only the last consumed cluster can be lied
  // about (everything earlier had its full context), so the early answer is
  // trusted only once the table -- which reads the real bytes, uncut --
  // agrees on where that cluster ends.
  const Index span = end - from;
  constexpr Index kBytesPerColumn = 4;
  const Index guess = (column < ((std::numeric_limits<Index>::max() / kBytesPerColumn) - 1))
                          ? ((column + 1) * kBytesPerColumn)
                          : span;
  Index window = std::min(span, std::max<Index>(16, guess));
  std::string text;
  while (true) {
    ReadDocRangeInto(table, Interval(from, from + window), text);
    Index at = 0;
    std::size_t i = 0;
    std::size_t last_start = 0;
    while ((i < text.size()) && (at < column)) {
      const std::size_t next = NextGraphemeInString(text, i);
      last_start = i;
      at += AdvanceOf(std::string_view{text.data() + i, next - i}, at, tab_width);
      i = next;
    }
    if (window >= span) return from + static_cast<Index>(i);
    if (i < text.size()) {
      const Index pos = from + static_cast<Index>(i);
      if ((at == 0) || (NextGraphemeBoundary(table, from + static_cast<Index>(last_start)) == pos)) {
        return pos;
      }
    }
    window = (window >= (span / 2)) ? span : (window * 2);
  }
}

namespace {

Viewport ScrollWrapped(const Document& doc, Viewport view, const WrapMetrics& wrap, Index line,
                       Index text_rows, Index pad) {
  const Index line_count = LineCount(doc.table);
  view.left_column = 0;

  std::vector<Index> row_starts;
  std::string scratch;
  LayoutLine(doc.table, line, wrap, row_starts, scratch);
  const Index cursor_row = RowOfPosition(row_starts, PrimaryCursor(doc));

  view.top_line = std::clamp<Index>(view.top_line, 0, std::max<Index>(0, line_count - 1));
  if ((line < view.top_line) || ((line - view.top_line) > text_rows)) {
    view.top_line = line;
    view.top_row = 0;
  }
  view.top_row =
      std::clamp<Index>(view.top_row, 0, WrappedRows(doc.table, view.top_line, wrap) - 1);

  Index above = cursor_row - view.top_row;
  for (Index l = view.top_line; l < line; ++l) above += WrappedRows(doc.table, l, wrap);

  while ((above < pad) && ((view.top_line > 0) || (view.top_row > 0))) {
    if (view.top_row > 0) {
      --view.top_row;
    } else {
      --view.top_line;
      view.top_row = WrappedRows(doc.table, view.top_line, wrap) - 1;
    }
    ++above;
  }
  while (((above + pad) >= text_rows) && (above > 0)) {
    if ((view.top_row + 1) < WrappedRows(doc.table, view.top_line, wrap)) {
      ++view.top_row;
    } else {
      ++view.top_line;
      view.top_row = 0;
    }
    --above;
  }
  return view;
}

}

Viewport ScrollToCursor(const Document& doc, Viewport view, const WrapMetrics& wrap) {
  const Index line_count = LineCount(doc.table);
  const Index text_rows = std::max<Index>(1, view.rows);
  const Index pad = std::clamp<Index>(view.scrolloff, 0, std::max<Index>(0, (text_rows - 1) / 2));

  const Index line = PrimaryLine(doc);
  if (wrap.enabled) return ScrollWrapped(doc, view, wrap, line, text_rows, pad);
  view.top_row = 0;
  if (line - pad < view.top_line) view.top_line = line - pad;
  if (line + pad >= view.top_line + text_rows) view.top_line = line + pad - text_rows + 1;
  view.top_line = std::clamp<Index>(view.top_line, 0, std::max<Index>(0, line_count - 1));

  const Index columns = std::max<Index>(1, view.columns);
  const Index column = PrimaryColumn(doc);
  if (column < view.left_column) view.left_column = column;
  if (column >= view.left_column + columns) view.left_column = column - columns + 1;
  view.left_column = std::max<Index>(0, view.left_column);
  return view;
}

namespace {

void EnsureBufferList(Editor& ed) {
  if (ed.buffers.empty()) {
    ed.buffers.emplace_back();
    ed.active = 0;
  }
}

}

fs::path CanonicalOf(const fs::path& path) {
  std::error_code ec;
  const fs::path resolved = fs::weakly_canonical(path, ec);
  return ec ? path : resolved;
}

namespace {

void EnsureWindowTree(Editor& ed) {
  if (!ed.windows.empty()) return;
  WindowNode root;
  root.kind = WindowNode::Kind::kLeaf;
  root.buffer = ed.active;
  ed.windows.push_back(std::move(root));
  ed.focused = 0;
}

void CollectLeaves(const std::vector<WindowNode>& nodes, int at, std::vector<int>& out) {
  if ((at < 0) || (at >= std::ssize(nodes))) return;
  const WindowNode& n = nodes[static_cast<std::size_t>(at)];
  if (n.kind == WindowNode::Kind::kLeaf) {
    out.push_back(at);
    return;
  }
  CollectLeaves(nodes, n.first, out);
  CollectLeaves(nodes, n.second, out);
}

void LayoutInto(const std::vector<WindowNode>& nodes, int at, Rect area, std::vector<Rect>& out) {
  if ((at < 0) || (at >= std::ssize(nodes))) return;
  out[static_cast<std::size_t>(at)] = area;
  const WindowNode& n = nodes[static_cast<std::size_t>(at)];
  if (n.kind == WindowNode::Kind::kLeaf) return;
  if (n.kind == WindowNode::Kind::kRow) {
    const int left = SplitAt(area.w, n.ratio);
    LayoutInto(nodes, n.first, Rect{area.x, area.y, left, area.h}, out);
    LayoutInto(nodes, n.second, Rect{area.x + left, area.y, area.w - left, area.h}, out);
    return;
  }
  const int top = SplitAt(area.h, n.ratio);
  LayoutInto(nodes, n.first, Rect{area.x, area.y, area.w, top}, out);
  LayoutInto(nodes, n.second, Rect{area.x, area.y + top, area.w, area.h - top}, out);
}

void StashFocused(Editor& ed) {
  if ((ed.focused < 0) || (ed.focused >= std::ssize(ed.windows))) return;
  WindowNode& node = ed.windows[static_cast<std::size_t>(ed.focused)];
  if (node.kind != WindowNode::Kind::kLeaf) return;
  node.selections = ed.doc.selections;
  node.selections_revision = ed.doc.table.revision;
  node.view = ed.doc.view;
  node.buffer = ed.active;
}

void FocusLeaf(Editor& ed, int leaf) {
  if ((leaf < 0) || (leaf >= std::ssize(ed.windows))) return;
  WindowNode& node = ed.windows[static_cast<std::size_t>(leaf)];
  if (node.kind != WindowNode::Kind::kLeaf) return;

  if (node.buffer >= BufferCount(ed)) RetargetPane(node, ed.active, ed.doc.table);
  if (node.buffer != ed.active) SwitchToBuffer(ed, node.buffer);

  SyncWindowSelections(ed.doc.table, node);
  ed.doc.selections = node.selections;
  ed.doc.view = node.view;

  ed.doc.selections.Normalize(ed.doc.table);
  if (ed.mode != Mode::kInsert) ed.doc.selections.EnsureBlockCursors(ed.doc.table);

  ed.focused = leaf;
}

int NewNode(Editor& ed, WindowNode node) {
  for (std::size_t i = 0; i < ed.windows.size(); ++i) {
    if (ed.windows[i].dead) {
      ed.windows[i] = std::move(node);
      return static_cast<int>(i);
    }
  }
  ed.windows.push_back(std::move(node));
  return static_cast<int>(ed.windows.size()) - 1;
}

int ParentOf(const std::vector<WindowNode>& nodes, int child) {
  for (std::size_t i = 0; i < nodes.size(); ++i) {
    if (nodes[i].dead || (nodes[i].kind == WindowNode::Kind::kLeaf)) continue;
    if ((nodes[i].first == child) || (nodes[i].second == child)) return static_cast<int>(i);
  }
  return -1;
}

}

void SyncWindowSelections(const PieceTable& table, WindowNode& node) {
  if ((node.selections_revision < table.journal_base) ||
      (node.selections_revision >= table.revision)) {
    return;
  }
  const auto from = static_cast<std::size_t>(node.selections_revision - table.journal_base);
  const std::vector<Edit> edits(table.journal.begin() + static_cast<std::ptrdiff_t>(from),
                                table.journal.end());
  node.selections.MapThroughEdits(table, edits);
  node.selections_revision = table.revision;
}

void RetargetPane(WindowNode& node, std::size_t buffer, const PieceTable& table) {
  node.buffer = buffer;
  node.selections = SelectionSet{};
  node.selections_revision = table.revision;
  node.view = Viewport{};
}

std::size_t WindowCount(const Editor& ed) {
  if (ed.windows.empty()) return 1;
  std::vector<int> leaves;
  CollectLeaves(ed.windows, 0, leaves);
  return leaves.empty() ? 1 : leaves.size();
}

std::vector<int> WindowOrder(const Editor& ed) {
  std::vector<int> leaves;
  if (ed.windows.empty()) return leaves;
  CollectLeaves(ed.windows, 0, leaves);
  return leaves;
}

// Whether anything is drawing this buffer right now. The focused buffer
// always is, whether or not a window tree has been built yet; the rest are
// on screen only while a pane points at them.
bool BufferOnScreen(const Editor& ed, std::size_t buffer) {
  if (buffer == ed.active) return true;
  for (const int leaf : WindowOrder(ed)) {
    // The focused leaf draws ed.doc whatever its `buffer` says: that field is
    // rewritten on the way out of a pane, so between a buffer switch and the
    // next stash it still names the document the pane has already left. Render
    // reads it the same way, and a query that did not would call a buffer
    // on screen that nothing is drawing.
    if (leaf == ed.focused) continue;
    if (ed.windows[static_cast<std::size_t>(leaf)].buffer == buffer) return true;
  }
  return false;
}

int SplitAt(int span, double ratio) {
  if (span < 2) return std::max(0, span);
  const int at = static_cast<int>(static_cast<double>(span) * ratio);
  return std::clamp(at, 1, span - 1);
}

double SplitRatio(int span, int first) {
  if (span < 2) return 0.5;
  return (static_cast<double>(first) + 0.5) / static_cast<double>(span);
}

std::vector<Rect> LayoutNodes(const Editor& ed, Rect screen) {
  std::vector<Rect> out(ed.windows.size(), Rect{});
  LayoutInto(ed.windows, 0, screen, out);
  return out;
}

std::vector<Rect> LayoutWindows(const Editor& ed, Rect screen) {
  std::vector<Rect> out;
  if (ed.windows.empty()) {
    out.push_back(screen);
    return out;
  }
  const std::vector<Rect> nodes = LayoutNodes(ed, screen);
  const std::vector<int> leaves = WindowOrder(ed);
  out.reserve(leaves.size());
  for (const int leaf : leaves) out.push_back(nodes[static_cast<std::size_t>(leaf)]);
  return out;
}

void SplitWindow(Editor& ed, bool vertical) {
  EnsureWindowTree(ed);
  StashFocused(ed);
  const int leaf = ed.focused;
  if ((leaf < 0) || (leaf >= std::ssize(ed.windows))) return;

  const Rect area = LayoutNodes(ed, PaneArea(ed))[static_cast<std::size_t>(leaf)];
  const bool fits = vertical ? (area.w >= 2 * kMinPaneWidth) : (area.h >= 2 * kMinPaneHeight);
  if (!fits) {
    ed.status.Warn("not enough room to split this window");
    return;
  }

  WindowNode contents = ed.windows[static_cast<std::size_t>(leaf)];
  contents.dead = false;
  WindowNode copy = contents;
  const int a = NewNode(ed, std::move(contents));
  const int b = NewNode(ed, std::move(copy));

  WindowNode& parent = ed.windows[static_cast<std::size_t>(leaf)];
  parent = WindowNode{};
  parent.kind = vertical ? WindowNode::Kind::kRow : WindowNode::Kind::kColumn;
  parent.first = a;
  parent.second = b;
  parent.ratio = 0.5;

  FocusLeaf(ed, b);
}

void CloseWindow(Editor& ed) {
  if (ed.windows.empty()) return;
  if (WindowCount(ed) <= 1) return;
  const int leaf = ed.focused;
  const int parent = ParentOf(ed.windows, leaf);
  if (parent < 0) return;

  const WindowNode& up = ed.windows[static_cast<std::size_t>(parent)];
  const int sibling = (up.first == leaf) ? up.second : up.first;

  WindowNode promoted = ed.windows[static_cast<std::size_t>(sibling)];
  ed.windows[static_cast<std::size_t>(leaf)].dead = true;
  ed.windows[static_cast<std::size_t>(sibling)].dead = true;
  ed.windows[static_cast<std::size_t>(parent)] = std::move(promoted);

  std::vector<int> leaves;
  CollectLeaves(ed.windows, parent, leaves);
  FocusLeaf(ed, leaves.empty() ? parent : leaves.front());
}

void KeepOnlyFocusedWindow(Editor& ed) {
  if (ed.windows.empty()) return;
  if (WindowCount(ed) <= 1) return;
  if ((ed.focused < 0) || (ed.focused >= std::ssize(ed.windows))) return;
  const WindowNode& focused = ed.windows[static_cast<std::size_t>(ed.focused)];
  if (focused.kind != WindowNode::Kind::kLeaf) return;

  StashFocused(ed);
  WindowNode keep = focused;
  keep.dead = false;
  ed.windows.assign(1, std::move(keep));
  ed.focused = 0;
  FocusLeaf(ed, 0);
}

void FocusWindowAt(Editor& ed, int leaf) {
  if (ed.windows.empty()) return;
  if (leaf == ed.focused) return;
  StashFocused(ed);
  FocusLeaf(ed, leaf);
}

void FocusWindow(Editor& ed, bool forward) {
  EnsureWindowTree(ed);
  StashFocused(ed);
  const std::vector<int> order = WindowOrder(ed);
  if (order.size() <= 1) return;
  std::size_t at = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (order[i] == ed.focused) at = i;
  }
  const std::size_t next =
      forward ? ((at + 1) % order.size()) : ((at + order.size() - 1) % order.size());
  FocusLeaf(ed, order[next]);
}

namespace {

constexpr Rect kNominalScreen{0, 0, 4096, 4096};

int Overlap(int a0, int a1, int b0, int b1) {
  return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

}

int WindowToward(const Editor& ed, WindowDir dir) {
  const std::vector<int> order = WindowOrder(ed);
  if (order.size() <= 1) return -1;
  const std::vector<Rect> areas = LayoutWindows(ed, kNominalScreen);
  if (areas.size() != order.size()) return -1;

  std::size_t from = order.size();
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (order[i] == ed.focused) from = i;
  }
  if (from == order.size()) return -1;
  const Rect& me = areas[from];

  int best = -1;
  int best_gap = 0;
  int best_overlap = 0;
  for (std::size_t i = 0; i < order.size(); ++i) {
    if (i == from) continue;
    const Rect& other = areas[i];
    int gap = 0;
    int overlap = 0;
    switch (dir) {
      case WindowDir::kRight:
        if (other.x < (me.x + me.w)) continue;
        gap = other.x - (me.x + me.w);
        overlap = Overlap(me.y, me.y + me.h, other.y, other.y + other.h);
        break;
      case WindowDir::kLeft:
        if ((other.x + other.w) > me.x) continue;
        gap = me.x - (other.x + other.w);
        overlap = Overlap(me.y, me.y + me.h, other.y, other.y + other.h);
        break;
      case WindowDir::kDown:
        if (other.y < (me.y + me.h)) continue;
        gap = other.y - (me.y + me.h);
        overlap = Overlap(me.x, me.x + me.w, other.x, other.x + other.w);
        break;
      case WindowDir::kUp:
        if ((other.y + other.h) > me.y) continue;
        gap = me.y - (other.y + other.h);
        overlap = Overlap(me.x, me.x + me.w, other.x, other.x + other.w);
        break;
    }
    if (overlap <= 0) continue;

    if ((best < 0) || (gap < best_gap) || ((gap == best_gap) && (overlap > best_overlap))) {
      best = order[i];
      best_gap = gap;
      best_overlap = overlap;
    }
  }
  return best;
}

void JumpWindow(Editor& ed, WindowDir dir) {
  if (ed.windows.empty()) return;
  StashFocused(ed);
  const int target = WindowToward(ed, dir);
  if (target < 0) return;
  FocusLeaf(ed, target);
}

void SwapWindow(Editor& ed, WindowDir dir) {
  if (ed.windows.empty()) return;
  StashFocused(ed);
  const int target = WindowToward(ed, dir);
  if (target < 0) return;

  WindowNode& here = ed.windows[static_cast<std::size_t>(ed.focused)];
  WindowNode& there = ed.windows[static_cast<std::size_t>(target)];
  std::swap(here.buffer, there.buffer);
  std::swap(here.view, there.view);
  std::swap(here.selections, there.selections);
  std::swap(here.selections_revision, there.selections_revision);

  FocusLeaf(ed, target);
}

namespace {

struct SplitAbove {
  int node{-1};
  bool in_first{false};
};

SplitAbove SplitAboveLeaf(const std::vector<WindowNode>& nodes, int leaf, WindowNode::Kind kind) {
  int child = leaf;
  for (std::size_t guard = 0; guard <= nodes.size(); ++guard) {
    const int parent = ParentOf(nodes, child);
    if (parent < 0) break;
    const WindowNode& up = nodes[static_cast<std::size_t>(parent)];
    if (up.kind == kind) return SplitAbove{parent, up.first == child};
    child = parent;
  }
  return SplitAbove{};
}

int ClampDivider(int span, int first, int least) {
  const int lo = std::clamp(std::min(least, span / 2), 1, std::max(1, span - 1));
  const int hi = std::clamp(span - lo, lo, std::max(1, span - 1));
  return std::clamp(first, lo, hi);
}

bool PlaceDivider(WindowNode& node, int span, int first) {
  if (span < 2) return false;
  const int least =
      (node.kind == WindowNode::Kind::kRow) ? kMinPaneWidth : kMinPaneHeight;
  const int want = ClampDivider(span, first, least);
  if (want == SplitAt(span, node.ratio)) return false;
  node.ratio = SplitRatio(span, want);
  return true;
}

}

void TransposeWindow(Editor& ed) {
  if (ed.windows.empty()) return;
  const int parent = ParentOf(ed.windows, ed.focused);
  if (parent < 0) return;
  WindowNode& node = ed.windows[static_cast<std::size_t>(parent)];
  node.kind = (node.kind == WindowNode::Kind::kRow) ? WindowNode::Kind::kColumn
                                                    : WindowNode::Kind::kRow;

  const Rect divided = LayoutNodes(ed, PaneArea(ed))[static_cast<std::size_t>(parent)];
  const int span = (node.kind == WindowNode::Kind::kRow) ? divided.w : divided.h;
  PlaceDivider(node, span, SplitAt(span, node.ratio));
}

ResizeResult ResizePane(Editor& ed, ResizeAxis axis, bool grow, Rect area) {
  if (ed.windows.empty()) return ResizeResult::kNoNeighbour;
  const int leaf = ed.focused;
  if ((leaf < 0) || (leaf >= std::ssize(ed.windows))) return ResizeResult::kNoNeighbour;

  const WindowNode::Kind kind =
      (axis == ResizeAxis::kWidth) ? WindowNode::Kind::kRow : WindowNode::Kind::kColumn;
  const SplitAbove split = SplitAboveLeaf(ed.windows, leaf, kind);
  if (split.node < 0) return ResizeResult::kNoNeighbour;

  const std::vector<Rect> rects = LayoutNodes(ed, area);
  const Rect& divided = rects[static_cast<std::size_t>(split.node)];
  const int span = (axis == ResizeAxis::kWidth) ? divided.w : divided.h;
  if (span < 2) return ResizeResult::kAtLimit;

  WindowNode& node = ed.windows[static_cast<std::size_t>(split.node)];
  const int first = SplitAt(span, node.ratio);
  const int mine = split.in_first ? first : (span - first);
  const int want = grow ? (mine + std::max(1, (mine + 5) / 10))
                        : std::min(mine - 1, ((10 * mine) + 5) / 11);
  return PlaceDivider(node, span, split.in_first ? want : (span - want))
             ? ResizeResult::kMoved
             : ResizeResult::kAtLimit;
}

int DividerAt(const Editor& ed, int x, int y, Rect area) {
  if (ed.windows.empty()) return -1;
  const std::vector<Rect> rects = LayoutNodes(ed, area);

  int found = -1;
  int found_span = 0;
  bool found_row = false;
  for (std::size_t i = 0; i < ed.windows.size(); ++i) {
    const WindowNode& n = ed.windows[i];
    if (n.dead || (n.kind == WindowNode::Kind::kLeaf)) continue;
    const Rect& r = rects[i];
    if ((r.w < 1) || (r.h < 1)) continue;
    if ((x < r.x) || (x >= (r.x + r.w)) || (y < r.y) || (y >= (r.y + r.h))) continue;

    const bool row = (n.kind == WindowNode::Kind::kRow);
    const int span = row ? r.w : r.h;
    if (span < 2) continue;
    const int at = (row ? r.x : r.y) + SplitAt(span, n.ratio) - 1;
    if ((row ? x : y) != at) continue;

    if (found >= 0) {
      if (found_row && !row) continue;
      if ((found_row == row) && (span >= found_span)) continue;
    }
    found = static_cast<int>(i);
    found_span = span;
    found_row = row;
  }
  return found;
}

bool MoveDivider(Editor& ed, int node, int x, int y, Rect area) {
  if ((node < 0) || (node >= std::ssize(ed.windows))) return false;
  WindowNode& n = ed.windows[static_cast<std::size_t>(node)];
  if (n.dead || (n.kind == WindowNode::Kind::kLeaf)) return false;

  const std::vector<Rect> rects = LayoutNodes(ed, area);
  const Rect& r = rects[static_cast<std::size_t>(node)];
  const bool row = (n.kind == WindowNode::Kind::kRow);
  const int first = (row ? (x - r.x) : (y - r.y)) + 1;
  return PlaceDivider(n, row ? r.w : r.h, first);
}

Rect PaneArea(const Editor& ed) {
  const int prompt_rows = ed.prompt_active ? 1 : 0;
  return Rect{0, 0, std::max(0, ed.screen_w), std::max(0, ed.screen_h - prompt_rows)};
}

std::size_t BufferCount(const Editor& ed) {
  return ed.buffers.empty() ? std::size_t{1} : ed.buffers.size();
}

const Document& BufferAt(const Editor& ed, std::size_t i) {
  if (ed.buffers.empty() || (i == ed.active)) return ed.doc;
  return ed.buffers[i];
}

std::size_t FindFileBuffer(const Editor& ed, const fs::path& path) {
  if (path.empty()) return BufferCount(ed);
  const fs::path want = CanonicalOf(path);
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    if (!HasDiskFile(doc)) continue;
    if (CanonicalOf(doc.file) == want) return i;
  }
  return BufferCount(ed);
}

std::size_t FindViewBuffer(const Editor& ed, std::string_view name) {
  if (name.empty()) return BufferCount(ed);
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    if (BufferAt(ed, i).view_name == name) return i;
  }
  return BufferCount(ed);
}

namespace {

void LiveDocumentChanged(Editor& ed) {
  if (ed.live_document_changed != nullptr) ed.live_document_changed(ed);
}

}

void SwitchToBuffer(Editor& ed, std::size_t index) {
  EnsureBufferList(ed);
  if ((index >= ed.buffers.size()) || (index == ed.active)) return;
  ed.buffers[ed.active] = std::move(ed.doc);
  ed.doc = std::move(ed.buffers[index]);
  ed.active = index;
  LiveDocumentChanged(ed);
}

namespace {

bool BufferShownInAnotherWindow(const Editor& ed) {
  for (const int leaf : WindowOrder(ed)) {
    if (leaf == ed.focused) continue;
    if (ed.windows[static_cast<std::size_t>(leaf)].buffer == ed.active) return true;
  }
  return false;
}

void SyncFocusedBuffer(Editor& ed) {
  if ((ed.focused < 0) || (ed.focused >= std::ssize(ed.windows))) return;
  WindowNode& node = ed.windows[static_cast<std::size_t>(ed.focused)];
  if (node.kind != WindowNode::Kind::kLeaf) return;
  node.buffer = ed.active;
}

}

void AddBuffer(Editor& ed, Document doc) {
  EnsureBufferList(ed);

  // The last clause is load-bearing, not belt and braces. This branch replaces
  // the document behind ed.active without retargeting any pane, so every stash
  // still describes the document being dropped. `!BufferShownInAnotherWindow`
  // is what makes that safe: only the focused pane points here, and the focused
  // pane's stash is rewritten on the way out by StashFocused. Drop the clause
  // and a second pane on the placeholder keeps selections and a
  // selections_revision belonging to a buffer that no longer exists -- the
  // exact shape RetargetPane exists to prevent.
  const bool placeholder = !HasDiskFile(ed.doc) && !IsExcerptView(ed.doc) && !ed.doc.modified &&
                           (DocLength(ed.doc.table) == 0) && !CanUndo(ed.doc.table) &&
                           !BufferShownInAnotherWindow(ed);
  if (placeholder) {
    ed.doc = std::move(doc);
    LiveDocumentChanged(ed);
    return;
  }
  ed.buffers[ed.active] = std::move(ed.doc);
  ed.buffers.emplace_back();
  ed.active = ed.buffers.size() - 1;
  ed.doc = std::move(doc);
  SyncFocusedBuffer(ed);
  LiveDocumentChanged(ed);
}

void CloseActiveBuffer(Editor& ed) {
  EnsureBufferList(ed);

  SyncFocusedBuffer(ed);
  if (ed.buffers.size() <= 1) {

    ed.doc = Document{};
    ResetToOriginal(ed.doc.table, "");
    ed.doc.selections.Set(Selection{0, 0, -1});
    ed.active = 0;
    for (WindowNode& node : ed.windows) {
      if (node.kind != WindowNode::Kind::kLeaf) continue;
      RetargetPane(node, 0, ed.doc.table);
    }
    LiveDocumentChanged(ed);
    return;
  }
  const std::size_t removed = ed.active;
  ed.buffers.erase(ed.buffers.begin() + static_cast<std::ptrdiff_t>(removed));

  const std::size_t next = std::min(removed, ed.buffers.size() - 1);

  for (WindowNode& node : ed.windows) {
    if (node.kind != WindowNode::Kind::kLeaf) continue;
    if (node.buffer == removed) {
      RetargetPane(node, next, ed.buffers[next].table);
    } else if (node.buffer > removed) {
      --node.buffer;
    }
  }

  ed.doc = std::move(ed.buffers[next]);
  ed.active = next;
  LiveDocumentChanged(ed);
}

std::vector<std::string> UnsavedBuffers(const Editor& ed) {
  std::vector<std::string> out;
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    if (!doc.modified) continue;
    out.push_back(IsExcerptView(doc)  ? doc.view_name
                  : !HasDiskFile(doc) ? std::string{"[no name]"}
                                      : DisplayPath(doc.file));
  }
  return out;
}

bool AddCursorVertically(Editor& ed, bool below) {
  const PieceTable& table = ed.doc.table;
  const Index last_line = LineCount(table) - 1;
  auto ranges = ed.doc.selections.Ranges();
  const std::size_t primary_at = ed.doc.selections.PrimaryIndex();
  std::vector<Selection> added = ranges;
  std::optional<Selection> primary_copy;

  for (std::size_t r = 0; r < ranges.size(); ++r) {
    const Selection& s = ranges[r];
    const Index cursor = CursorOf(table, s);
    const Index row = LineAt(table, cursor);
    const Index column = ColumnForByte(table, cursor, ed.doc.tab_width);
    const Index step = below ? 1 : -1;

    for (Index target = row + step; (target >= 0) && (target <= last_line); target += step) {
      if (LineContentRange(table, target).empty()) continue;
      const Index at = ByteForColumn(table, target, column, ed.doc.tab_width);
      if (ColumnForByte(table, at, ed.doc.tab_width) != column) continue;
      const Selection fresh = MinWidth1(table, Selection{at, at, -1});
      added.push_back(fresh);
      if (r == primary_at) primary_copy = fresh;
      break;
    }
  }
  const Selection was_primary = ranges[primary_at];
  ed.doc.selections.Replace(table, std::move(added));
  if (primary_copy) {
    const std::vector<Selection>& now = ed.doc.selections.Ranges();
    for (std::size_t i = 0; i < now.size(); ++i) {
      if ((now[i].From() <= primary_copy->From()) && (primary_copy->To() <= now[i].To())) {
        ed.doc.selections.SetPrimary(i);
        break;
      }
    }
  }
  return (ed.doc.selections.Ranges() != ranges) || !(ed.doc.selections.Primary() == was_primary);
}

Index CountOr(const Editor& ed, Index fallback) {
  return (ed.pending_count > 0) ? ed.pending_count : fallback;
}

void ApplyModeInvariants(Editor& ed) {
  const bool collapse = std::exchange(ed.collapse_insert_caret, false);
  if (ed.mode != Mode::kInsert) {
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    return;
  }
  if (!collapse) return;
  SetCursors(
      ed, [&](const Selection& s) { return CursorOf(ed.doc.table, s); }, true);
}

std::vector<std::string>& PromptHistoryOf(Editor& ed) {
  // Three histories, not two: a smart-jump query and a regex look nothing like
  // each other, and up-arrow in one prompt offering the other's last line is a
  // history that is no use in either.
  switch (ed.prompt_kind) {
    case PromptKind::kCommand: return ed.prompt_history;
    case PromptKind::kSmartJump: return ed.jump_history;
    case PromptKind::kSearch:
    case PromptKind::kSelectRegex:
    case PromptKind::kSearchExcerpts: break;
  }
  return ed.search_history;
}

std::string_view PromptSigil(const Editor& ed) {
  switch (ed.prompt_kind) {
    case PromptKind::kSearch: return "/";
    case PromptKind::kSelectRegex: return "select:";
    case PromptKind::kSearchExcerpts: return "search:";
    // The jera rune -- the j -- where the box has no room for a word. Icons
    // off falls back to the word.
    case PromptKind::kSmartJump: return ed.settings.icons ? "ᛃ " : "jump:";
    case PromptKind::kCommand: break;
  }
  return ":";
}

void PromptOpen(Editor& ed, PromptKind kind) {
  ed.prompt_active = true;
  ed.prompt_kind = kind;
  ed.prompt_input.clear();
  ed.prompt_cursor = 0;
  ed.prompt_scroll = 0;
  ed.prompt_history_index = PromptHistoryOf(ed).size();
  ed.status.clear();
  if (kind == PromptKind::kSearch) {
    ed.prompt_return_ranges = ed.doc.selections.Ranges();
    ed.prompt_return_primary = ed.doc.selections.PrimaryIndex();
    ed.prompt_return_view = ed.doc.view;
  }
}

void PromptRestoreSearchOrigin(Editor& ed) {
  if (ed.prompt_return_ranges.empty()) return;
  ed.doc.selections.Replace(ed.doc.table, ed.prompt_return_ranges);
  ed.doc.selections.SetPrimary(ed.prompt_return_primary);
  ed.doc.view = ed.prompt_return_view;
}

void PromptCancel(Editor& ed) {
  if (ed.prompt_active && (ed.prompt_kind == PromptKind::kSearch)) {
    PromptRestoreSearchOrigin(ed);
    ed.prompt_return_ranges.clear();
  }
  ed.prompt_active = false;
  ed.prompt_input.clear();
  ed.prompt_cursor = 0;
  ed.prompt_scroll = 0;
  ed.prompt_history_index = PromptHistoryOf(ed).size();
}

void PromptInsert(Editor& ed, std::string_view text) {
  if (!ed.prompt_active || text.empty()) return;
  ed.prompt_cursor = std::min(ed.prompt_cursor, ed.prompt_input.size());
  ed.prompt_input.insert(ed.prompt_cursor, text);
  ed.prompt_cursor += text.size();
}

void PromptBackspace(Editor& ed) {
  if (!ed.prompt_active || (ed.prompt_cursor == 0)) return;
  const std::size_t prev = PrevGraphemeInString(ed.prompt_input, ed.prompt_cursor);
  ed.prompt_input.erase(prev, ed.prompt_cursor - prev);
  ed.prompt_cursor = prev;
}

void PromptDeleteForward(Editor& ed) {
  if (!ed.prompt_active || (ed.prompt_cursor >= ed.prompt_input.size())) return;
  const std::size_t next = NextGraphemeInString(ed.prompt_input, ed.prompt_cursor);
  ed.prompt_input.erase(ed.prompt_cursor, next - ed.prompt_cursor);
}

void PromptMoveLeft(Editor& ed) {
  if (ed.prompt_active) ed.prompt_cursor = PrevGraphemeInString(ed.prompt_input, ed.prompt_cursor);
}

void PromptMoveRight(Editor& ed) {
  if (ed.prompt_active) ed.prompt_cursor = NextGraphemeInString(ed.prompt_input, ed.prompt_cursor);
}

void PromptHome(Editor& ed) { ed.prompt_cursor = 0; }
void PromptEnd(Editor& ed) { ed.prompt_cursor = ed.prompt_input.size(); }

void PromptHistory(Editor& ed, bool back) {
  const std::vector<std::string>& history = PromptHistoryOf(ed);
  if (!ed.prompt_active || history.empty()) return;
  if (back) {
    if (ed.prompt_history_index == 0) return;
    --ed.prompt_history_index;
  } else {
    if (ed.prompt_history_index >= history.size()) return;
    ++ed.prompt_history_index;
  }
  ed.prompt_input =
      (ed.prompt_history_index < history.size()) ? history[ed.prompt_history_index] : std::string{};
  ed.prompt_cursor = ed.prompt_input.size();
}

std::string DisplayName(const Document& doc) {
  if (IsExcerptView(doc)) return doc.view_name;
  return DisplayPath(doc.file);
}

std::string DisplayPath(const fs::path& path) {
  if (path.empty()) return "[scratch]";
  std::error_code ec;
  const fs::path absolute = fs::weakly_canonical(path, ec);
  const fs::path& shown = ec ? path : absolute;

  const fs::path cwd = fs::current_path(ec);
  if (!ec) {
    const fs::path relative = fs::relative(shown, cwd, ec);
    if (!ec && !relative.empty() && (relative.native().compare(0, 2, "..") != 0)) {
      return relative.string();
    }
  }
  if (const char* home = std::getenv("HOME")) {
    const std::string text = shown.string();
    const std::string prefix{home};
    if (!prefix.empty() && text.starts_with(prefix)) return "~" + text.substr(prefix.size());
  }
  return shown.string();
}

FlatStatus FlattenStatus(std::string_view text, std::size_t target_from,
                         std::size_t target_len) {
  FlatStatus out;
  const std::size_t target_to = target_from + target_len;
  std::string* last = &out.before;
  bool space = false;
  bool any = false;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if ((c == '\n') || (c == '\r') || (c == '\t') || (c == ' ')) {
      space = any;
      continue;
    }
    // A collapsed space stays in the piece before it, so a mark that starts
    // on a word starts its piece on that word.
    std::string& piece = ((target_len == 0) || (i < target_from)) ? out.before
                         : (i < target_to)                        ? out.target
                                                                  : out.after;
    if (space) *last += ' ';
    space = false;
    any = true;
    piece += c;
    last = &piece;
  }
  return out;
}

StatusLine StatusBar(const Editor& ed, bool focused) {
  return StatusBar(ed, ed.doc, ed.doc.selections, ed.active, focused);
}

StatusLine StatusBar(const Editor& ed, const Document& doc, const SelectionSet& sel,
                     std::size_t buffer, bool focused) {
  const Settings& s = ed.settings;
  StatusLine out;
  const auto add = [](std::vector<StatusSpan>& to, std::string text, StatusTone tone) {
    if (!text.empty()) to.push_back(StatusSpan{std::move(text), tone});
  };
  const auto glyph = [&s](const std::string& g) {
    return (s.icons && !g.empty()) ? g : std::string{};
  };

  const bool insert = (ed.mode == Mode::kInsert);
  const std::string& named = insert ? s.mode_insert : s.mode_normal;
  const std::string label =
      (s.icons && !named.empty()) ? named : std::string{insert ? "INS" : "NOR"};
  if (s.mode_indicator == "block") {
    add(out.left, " " + label + " ", StatusTone::kAccent);
    add(out.left, " ", StatusTone::kNormal);
  } else {
    add(out.left, label, StatusTone::kAccent);
    add(out.left, "  ", StatusTone::kNormal);
  }
  if (s.icons) {
    add(out.left, insert ? "ᚲ" : "ᛉ", StatusTone::kDim);
    add(out.left, "  ", StatusTone::kNormal);
  }

  const std::string shown = DisplayName(doc);
  const std::size_t slash = shown.find_last_of('/');
  add(out.left, glyph(s.icon_file), StatusTone::kDim);
  if (!glyph(s.icon_file).empty()) add(out.left, " ", StatusTone::kNormal);
  if (slash == std::string::npos) {
    add(out.left, shown, StatusTone::kStrong);
  } else {
    add(out.left, shown.substr(0, slash + 1), StatusTone::kDim);
    add(out.left, shown.substr(slash + 1), StatusTone::kStrong);
  }

  if (doc.read_only) {
    add(out.left, " ", StatusTone::kNormal);
    add(out.left, glyph(s.icon_readonly).empty() ? "[ro]" : glyph(s.icon_readonly),
        StatusTone::kWarning);
  }
  if (doc.modified) {
    add(out.left, " ", StatusTone::kNormal);
    add(out.left, glyph(s.icon_modified).empty() ? "[+]" : glyph(s.icon_modified),
        StatusTone::kAccent);
  }
  if (IsExcerptView(doc) && doc.excerpts.watched) {
    add(out.left, " ", StatusTone::kNormal);
    add(out.left, "[watch]", StatusTone::kAccent);
  }

  if (const std::size_t count = BufferCount(ed); count > 1) {
    add(out.left, " ", StatusTone::kNormal);
    add(out.left, "[" + std::to_string(buffer + 1) + "/" + std::to_string(count) + "]",
        StatusTone::kDim);
    std::size_t dirty_elsewhere = 0;
    for (std::size_t i = 0; i < count; ++i) {
      if ((i != buffer) && BufferAt(ed, i).modified) ++dirty_elsewhere;
    }
    if (dirty_elsewhere > 0) {
      add(out.left, "+" + std::to_string(dirty_elsewhere), StatusTone::kWarning);
    }
  }

  // A live leap's hint outranks ed.status, which anything at all can take
  // between two keystrokes. Only the focused pane's bar: the hint belongs to
  // the pane the labels are drawn in.
  const std::string_view hint = focused ? LeapHint(ed) : std::string_view{};
  // The smart-jump box hangs the match feedback under itself, and an arrival
  // hangs its branch row off the caret; the bar saying either would say
  // everything twice.
  const bool branch_has_it =
      (ed.prompt_active && (ed.prompt_kind == PromptKind::kSmartJump)) || ed.jump_branch;
  if (focused && (!hint.empty() || (!branch_has_it && !ed.status.empty()))) {
    const StatusLevel level = hint.empty() ? ed.status.level() : StatusLevel::kInfo;
    const StatusTone tone = (level == StatusLevel::kError)     ? StatusTone::kError
                            : (level == StatusLevel::kWarning) ? StatusTone::kWarning
                                                               : StatusTone::kInfo;
    const std::string& mark = (level == StatusLevel::kError)     ? s.icon_error
                              : (level == StatusLevel::kWarning) ? s.icon_warning
                                                                 : s.icon_info;
    const FlatStatus flat = hint.empty() ? FlattenStatus(ed.status) : FlattenStatus(hint);
    out.message_from = out.left.size();
    add(out.left, "   ", StatusTone::kNormal);
    // The destination the message names is its own span, toned kNext, so a
    // path in the feedback cannot be mistaken for the file the bar names.
    std::string head = glyph(mark);
    if (!head.empty() && !flat.empty()) head += " ";
    head += flat.before;
    add(out.left, head, tone);
    add(out.left, flat.target, StatusTone::kNext);
    add(out.left, flat.after, tone);
  }

  const auto dot = [&add, &out, &s]() {
    add(out.right, s.icons ? " \u16eb " : " · ", StatusTone::kDim);
  };

  if (!ed.pending_commands.empty()) {
    const std::vector<int> leaves = ed.windows.empty() ? std::vector<int>{} : WindowOrder(ed);
    const auto visible_somewhere = [&ed, &leaves](const PendingCommand& one) {
      if (leaves.empty()) return ed.doc.view_name == one.view_name;
      for (const int leaf : leaves) {
        if (leaf == ed.focused) {
          if (ed.doc.view_name == one.view_name) return true;
          continue;
        }
        const std::size_t pane = ed.windows[static_cast<std::size_t>(leaf)].buffer;
        if (BufferAt(ed, pane).view_name == one.view_name) return true;
      }
      return false;
    };
    const std::string& here = doc.view_name;
    const PendingCommand* mine = nullptr;
    for (const PendingCommand& one : ed.pending_commands) {
      if (one.done) continue;
      if (one.view_name == here) mine = &one;
    }
    std::size_t strays = 0;
    const PendingCommand* stray = nullptr;
    for (const PendingCommand& one : ed.pending_commands) {
      if (one.done || visible_somewhere(one)) continue;
      ++strays;
      stray = &one;
    }
    const PendingCommand* job = mine;
    std::size_t extra = 0;
    if (mine != nullptr) {
      extra = focused ? strays : 0;
    } else if (focused && (stray != nullptr)) {
      job = stray;
      extra = strays - 1;
    }
    if (job != nullptr) {
      static constexpr std::string_view kFrames[] = {"⠋", "⠙", "⠹", "⠸", "⠼",
                                                     "⠴", "⠦", "⠧", "⠇", "⠏"};
      const auto since = std::chrono::steady_clock::now() - job->started;
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(since).count();
      const auto frame = static_cast<std::size_t>(ms / 100) % std::size(kFrames);
      std::string cmd = job->command;
      if (cmd.size() > 24) {
        std::size_t cut = 23;
        while ((cut > 0) && ((static_cast<unsigned char>(cmd[cut]) & 0xC0) == 0x80)) {
          --cut;
        }
        cmd = cmd.substr(0, cut) + "…";
      }
      // A scan waits for one of a handful of pool workers before it runs, and a
      // seconds counter on a job that has not started counts the wait as work.
      // The spinner still turns -- something is pending either way -- but it
      // says which.
      const bool queued =
          (job->scan != nullptr) && !job->scan->begun.load(std::memory_order_acquire);
      std::string spin = std::string{kFrames[frame]} + " " + cmd + " " +
                         (queued ? std::string{"queued"} : (std::to_string(ms / 1000) + "s"));
      if (extra > 0) spin += " +" + std::to_string(extra);
      add(out.right, spin, StatusTone::kAccent);
      add(out.right, " :from-cancel", StatusTone::kDim);
      dot();
    }
  }

  const std::size_t sels = sel.Size();
  if (sels > 1) {
    add(out.right, std::to_string(sels) + " sel", StatusTone::kAccent);
    dot();
  }

  if (focused && (ed.pending_count > 0)) {
    add(out.right, "x" + std::to_string(ed.pending_count), StatusTone::kAccent);
    dot();
  }
  if (focused && ed.recorder) {
    add(out.right, "REC", StatusTone::kAccent);
    dot();
  }
  const Index cursor = CursorOf(doc.table, sel.Primary());
  add(out.right, std::to_string(LineAt(doc.table, cursor) + 1) + ":" +
                     std::to_string(ColumnForByte(doc.table, cursor, doc.tab_width) + 1),
      StatusTone::kNormal);
  dot();
  add(out.right, std::to_string(LineCount(doc.table)), StatusTone::kDim);
  if (s.icons) add(out.right, " \u16ed", StatusTone::kDim);
  return out;
}

}
