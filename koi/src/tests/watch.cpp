// Tests for the file watcher and the event-driven disk check it feeds: what a
// directory watch reports, what it filters out, what it does when the kernel
// says no, and what CheckDiskChange does when it is asked over and over instead
// of once per focus-in.
//
// Every wait here is on the watcher's own fd with a generous ceiling. inotify
// delivery is asynchronous and a fixed sleep is a race dressed up as a test:
// too short and it fails on a loaded machine, too long and the suite pays for
// it on every run.
//
// Declared in tests.h, run from main.cpp.

#include "tests.h"

#include <poll.h>
#include <sys/resource.h>

// Declarations only, and the same option macros koi.cpp compiles termbox with,
// so the TB_ codes below are the ones the event loop actually sees.
#define TB_OPT_ATTR_W 64
#define TB_OPT_EGC
#include <termbox2.h>

#include "watch.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

// Loose on purpose. What this separates is "arrived" from "never arrives", and
// the two are apart by orders of magnitude -- inotify delivery is a wakeup on
// the same machine, not a round trip.
constexpr int kDeliveryCeilingMs = 4000;
// Zero, and not a margin. There is nothing in flight to wait for: the kernel
// queues an event inside the syscall that caused it, so by the time write() or
// rename() has returned the fd is readable if it is ever going to be. A window
// here would slow every negative assertion down without making one of them
// stronger -- and six of them were most of this file's runtime.
constexpr int kQuietMs = 0;

int MsLeft(std::chrono::steady_clock::time_point deadline) {
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                        deadline - std::chrono::steady_clock::now())
                        .count();
  return static_cast<int>(std::clamp<long long>(left, 0, kDeliveryCeilingMs));
}

// POLLIN specifically, not "poll said something". POLLERR, POLLHUP and POLLNVAL
// all come back in revents unasked for, and a test that read them as "an event
// arrived" would pass on a watcher whose fd was closed under it.
bool Readable(const FileWatcher& watcher, int timeout_ms) {
  struct pollfd fd{.fd = watcher.Fd(), .events = POLLIN, .revents = 0};
  return (::poll(&fd, 1, timeout_ms) > 0) && ((fd.revents & POLLIN) != 0);
}

// Drains until something koi cares about turns up, or the ceiling runs out.
bool DrainUntilReported(FileWatcher& watcher) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{kDeliveryCeilingMs};
  for (;;) {
    if (watcher.Drain()) return true;
    const int left = MsLeft(deadline);
    if ((left == 0) || !Readable(watcher, left)) return false;
  }
}

// Drains until the fd is quiet, and says whether anything in all of that was
// koi's. Drains rather than drains once, which is what makes a negative
// assertion mean something: events arrive in batches, and one drain that came
// up empty only says the batch it read was empty.
//
// Bounded by the same ceiling as everything else, because the loop is otherwise
// unbounded -- an fd that stays readable without ever yielding an event, which
// is what a broken one looks like, would hang the suite rather than fail it.
bool DrainedAnythingWithin(FileWatcher& watcher, int quiet_ms = kQuietMs) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{kDeliveryCeilingMs};
  bool reported = false;
  for (;;) {
    if (watcher.Drain()) reported = true;
    const int left = MsLeft(deadline);
    if ((left == 0) || !Readable(watcher, std::min(quiet_ms, left))) return reported;
  }
}

// The negative assertion, and the reason it is not a sleep. Something did move
// in a watched directory, so an event is on its way; waiting for the fd to
// become readable proves it arrived, and the drain coming up empty afterwards
// is then the filter working rather than the event being late.
bool DeliveredButFilteredOut(FileWatcher& watcher) {
  if (!Readable(watcher, kDeliveryCeilingMs)) return false;
  return !DrainedAnythingWithin(watcher);
}

// A watch the kernel took away arrives as IN_IGNORED, which may trail the
// delete it belongs to by a batch.
bool WaitForRearmRequest(FileWatcher& watcher) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{kDeliveryCeilingMs};
  for (;;) {
    std::ignore = watcher.Drain();
    if (watcher.NeedsRearm()) return true;
    const int left = MsLeft(deadline);
    if ((left == 0) || !Readable(watcher, left)) return watcher.NeedsRearm();
  }
}

// fs.inotify.max_queued_events, which is what the overflow case has to get
// past. 16384 is the default and what this runs on; read rather than assumed so
// a machine that raised it makes the burst bigger instead of making the case
// pass without ever overflowing.
int QueuedEventCap() {
  std::ifstream in{"/proc/sys/fs/inotify/max_queued_events"};
  int cap = 0;
  if (in >> cap && (cap > 0)) return cap;
  return 16384;
}

// A rename into place, which is how koi's own AtomicWriteFile, every formatter
// worth the name and most coding agents write a file. The inode the buffer's
// path pointed at is gone afterwards -- which is exactly what a watch on the
// file would have gone on following, and why the watch is on the directory.
void RenameIntoPlace(const fs::path& target, std::string_view contents) {
  std::error_code ec;
  const bool existed = fs::exists(target, ec);
  const fs::file_time_type before = existed ? fs::last_write_time(target, ec) : fs::file_time_type{};
  const bool knew_before = existed && !ec;

  // Checked, both of them. A fixture that failed to write or failed to rename
  // is otherwise indistinguishable from a watcher that missed the event, and
  // the failure gets reported against whatever assertion came next instead of
  // against the line that actually went wrong.
  const fs::path temp = target.parent_path() / (target.filename().string() + ".tmp-write");
  {
    std::ofstream out{temp, std::ios::binary | std::ios::trunc};
    out << contents;
    out.close();
    EXPECT_TRUE(out.good());
  }
  std::error_code rename_ec;
  fs::rename(temp, target, rename_ec);
  EXPECT_TRUE(!rename_ec);

  // The same guard WriteFixtureFile carries, and for the same reason: a rewrite
  // of equal length inside one mtime tick carries the stamp koi already holds,
  // so on a coarse clock the fixture would be saying "this changed" while the
  // editor is correctly answering "no it did not".
  if (!knew_before) return;
  std::error_code now_ec;
  const auto now = fs::last_write_time(target, now_ec);
  if (!now_ec && (now <= before)) {
    fs::last_write_time(target, before + std::chrono::seconds{1}, now_ec);
  }
}

std::string TextOf(const Editor& ed) {
  return ReadDocRange(ed.doc.table, {0, DocLength(ed.doc.table)});
}

}  // namespace

void FileWatcherReportsWhatMovedUnderIt() {
  const Scratch scratch{"koi-watch-reports"};

  TEST_CASE("watcher: a file rewritten in place is reported");
  {
    const fs::path file = scratch.Write("inplace.txt", "before\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    EXPECT_TRUE(watcher.Fd() >= 0);
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    // Nothing has happened yet, so there is nothing to report.
    EXPECT_FALSE(watcher.Drain());

    WriteFixtureFile(file, "after\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
    // And the fd is empty again once drained.
    EXPECT_FALSE(watcher.Drain());
  }

  TEST_CASE("watcher: a file replaced by rename-into-place is reported");
  {
    // The case a watch on the file's own inode gets wrong: the rename swaps the
    // inode out, and an inode watch would follow the orphan and report nothing
    // ever again. Repeated, because "fires once then goes deaf" is the failure
    // shape and one rename would not catch it.
    const fs::path file = scratch.Write("renamed.txt", "before\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    for (int round = 0; round < 3; ++round) {
      RenameIntoPlace(file, "round " + std::to_string(round) + "\n");
      EXPECT_TRUE(DrainUntilReported(watcher));
    }
  }

  TEST_CASE("watcher: unrelated churn in a watched directory is not reported");
  {
    const fs::path file = scratch.Write("mine.txt", "mine\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    // What a build does next to an open buffer: object files, a temp, a
    // directory. All of it lands on the same watch and none of it is koi's.
    for (int i = 0; i < 8; ++i) {
      WriteFixtureFile(scratch.dir / ("junk-" + std::to_string(i) + ".o"), "not text\n");
    }
    std::error_code ec;
    fs::create_directories(scratch.dir / "build-output", ec);
    fs::remove(scratch.dir / "junk-0.o", ec);
    EXPECT_TRUE(DeliveredButFilteredOut(watcher));

    // The filter is not simply broken-off: the watched name still reports.
    WriteFixtureFile(file, "mine, changed\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: one drain empties the queue however many reads it takes");
  {
    // 800 files is 1600 events, some six times what one read of Drain's buffer
    // holds. A Drain that read once would hand the rest straight back to poll,
    // and the editor drains once per turn -- so a build's worth of churn would
    // take as many turns to get through as it took reads, each of them waking
    // the loop for events already on the floor.
    std::error_code ec;
    const fs::path dir = scratch.dir / "burst";
    fs::create_directories(dir, ec);
    EXPECT_TRUE(!ec);
    const fs::path file = dir / "mine.txt";
    WriteFixtureFile(file, "mine\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    for (int i = 0; i < 800; ++i) {
      WriteFixtureFile(dir / ("junk-" + std::to_string(i) + ".o"), "not text\n");
    }
    EXPECT_TRUE(Readable(watcher, kDeliveryCeilingMs));
    // None of it is koi's -- and the fd is empty after the one call, which is
    // the part the read loop exists for.
    EXPECT_FALSE(watcher.Drain());
    EXPECT_FALSE(Readable(watcher, 0));

    // Still armed and still filtering after a burst that size.
    WriteFixtureFile(file, "mine, changed\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a name whose prefix matches a watched one is not reported");
  {
    const fs::path file = scratch.Write("prefix.txt", "mine\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    // The kernel NUL-pads names out to a multiple of its alignment, so a
    // shorter name written into the tail of a longer one's slot is exactly the
    // read bug this guards: prefix.txt.bak must not read as prefix.txt.
    WriteFixtureFile(scratch.dir / "prefix.txt.bak", "theirs\n");
    WriteFixtureFile(scratch.dir / "prefix.tx", "theirs\n");
    EXPECT_TRUE(DeliveredButFilteredOut(watcher));
  }

  TEST_CASE("watcher: a file that is not there yet is reported when it appears, and when it goes");
  {
    // `koi notes/todo.md` on a path something else is about to generate, and
    // the checkout that takes it away again. Neither is a write, so neither
    // IN_CLOSE_WRITE nor IN_MOVED_TO has anything to say about them: it is
    // IN_CREATE and IN_DELETE or it is silence.
    std::error_code ec;
    const fs::path dir = scratch.dir / "appears";
    fs::create_directories(dir, ec);
    EXPECT_TRUE(!ec);
    const fs::path file = dir / "todo.md";
    const fs::path source = scratch.Write("todo-source.md", "todo\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    // The directory is there, so the watch is real even though the file is not.
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    EXPECT_FALSE(watcher.Drain());

    // A hard link rather than a write, so what arrives is the bare IN_CREATE:
    // opening the file for writing would bring an IN_CLOSE_WRITE along and the
    // assertion below would be passing on that instead.
    fs::create_hard_link(source, file, ec);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(DrainUntilReported(watcher));

    EXPECT_TRUE(fs::remove(file, ec));
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a watched file moved out of its directory is reported");
  {
    // The other half of the swap the whole design is shaped around, and `mv
    // notes.md ../` on its own: the buffer is the last copy now. The
    // destination is not watched, so IN_MOVED_TO cannot be what carries this --
    // only the source directory's IN_MOVED_FROM can.
    std::error_code ec;
    const fs::path dir = scratch.dir / "leaves";
    const fs::path away = scratch.dir / "leaves-elsewhere";
    fs::create_directories(dir, ec);
    fs::create_directories(away, ec);
    EXPECT_TRUE(!ec);
    const fs::path file = dir / "notes.md";
    WriteFixtureFile(file, "notes\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    fs::rename(file, away / "notes.md", ec);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a permission change on the watched file is reported");
  {
    // chmod, not touch: a bare mtime bump is IN_MODIFY, which is out of the
    // mask on purpose. What this stands for is the write bit going away under
    // an open buffer -- not a byte moved, and the next `:w` is going to fail.
    const fs::path file = scratch.Write("perms.txt", "mine\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    std::error_code ec;
    fs::permissions(file, fs::perms::owner_read, fs::perm_options::replace, ec);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(DrainUntilReported(watcher));

    // Put back, or the fixture leaves a file its own cleanup has to fight.
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    EXPECT_TRUE(!ec);
  }

  TEST_CASE("watcher: something done to the watched directory itself arrives without a name");
  {
    // A nameless event: the directory's own attributes changed, and there is no
    // basename in it for the filter to compare. Let through rather than
    // compared, because comparing gives the empty string, which no watched name
    // ever is -- and the events that arrive this way are the ones that say the
    // watch itself is in trouble.
    std::error_code ec;
    const fs::path dir = scratch.dir / "nameless";
    fs::create_directories(dir, ec);
    EXPECT_TRUE(!ec);
    const fs::path file = dir / "f.txt";
    WriteFixtureFile(file, "one\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    fs::permissions(dir, fs::perms::owner_all | fs::perms::group_read,
                    fs::perm_options::replace, ec);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(DrainUntilReported(watcher));
    // And the watch is untouched by it, which is what separates this nameless
    // event from the nameless ones that mean the directory is gone.
    EXPECT_FALSE(watcher.NeedsRearm());
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
  }

  TEST_CASE("watcher: a buffer through a symlink is watched where the writes land");
  {
    // Link and target in separate directories, which is the arrangement that
    // makes the second watch mean anything: one directory is where the bytes
    // land and the other is where the name lives, and with both in one place a
    // watcher that only ever added the first would look identical.
    std::error_code ec;
    const fs::path target_dir = scratch.dir / "link-target";
    const fs::path link_dir = scratch.dir / "link-here";
    fs::create_directories(target_dir, ec);
    fs::create_directories(link_dir, ec);
    EXPECT_TRUE(!ec);
    const fs::path real = target_dir / "real.txt";
    WriteFixtureFile(real, "real\n");
    const fs::path other = target_dir / "other.txt";
    WriteFixtureFile(other, "other\n");
    const fs::path link = link_dir / "linked.txt";
    // Asserted, not skipped past. This is Linux; a filesystem that refuses a
    // symlink is a broken fixture, and the `if (!ec)` this used to be ran no
    // checks at all when it happened.
    fs::create_symlink(real, link, ec);
    EXPECT_TRUE(!ec);

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({link.string()});
    // Two directories for the one buffer.
    EXPECT_EQ(watcher.WatchCount(), std::size_t{2});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{2});
    EXPECT_FALSE(watcher.Drain());

    // Writers write the target, not the link, so that is the name the watcher
    // has to be filtering on.
    RenameIntoPlace(real, "real, rewritten\n");
    EXPECT_TRUE(DrainUntilReported(watcher));

    // And re-pointing the link is a change to what the buffer's path means that
    // never touches the target's directory: the only watch that can see it is
    // the one on the link's own.
    EXPECT_TRUE(fs::remove(link, ec));
    fs::create_symlink(other, link, ec);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: koi's own save is spent by the token it leaves, and only once");
  {
    // A :w renames into place, which reaches the watch as an event on a watched
    // basename and would otherwise arm a debounce, a stat sweep over every
    // on-screen buffer and a repaint -- all of which find nothing, because the
    // save re-stamps the buffer after the rename.
    //
    // The file is not there beforehand on purpose: RenameIntoPlace pushes the
    // mtime forward for a target it already knew, and that arrives as a second
    // event -- IN_ATTRIB on the same watched name -- which no token covers and
    // which would make the muting look intermittent rather than absent.
    const fs::path file = scratch.dir / "koi-writes-this.txt";
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    watcher.ExpectSelfWrite(file.string());
    RenameIntoPlace(file, "koi wrote this\n");
    EXPECT_TRUE(DeliveredButFilteredOut(watcher));

    // One token, one event. The same file written again with nothing said about
    // it is somebody else's write and is reported -- a token is a mute for the
    // save it belongs to, not for the name.
    RenameIntoPlace(file, "somebody else wrote this\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a token is spent by its own file and no other");
  {
    // Not there yet, for the reason the case above gives.
    const fs::path mine = scratch.dir / "saved.txt";
    const fs::path theirs = scratch.Write("not-saved.txt", "theirs\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({mine.string(), theirs.string()});
    EXPECT_FALSE(watcher.Drain());

    // Both watched, both in one directory, so both arrive on the same
    // descriptor and the basename is the only thing telling them apart.
    watcher.ExpectSelfWrite(mine.string());
    RenameIntoPlace(theirs, "theirs, rewritten\n");
    EXPECT_TRUE(DrainUntilReported(watcher));

    // And the token is still there for the file it was left for.
    RenameIntoPlace(mine, "koi wrote this\n");
    EXPECT_TRUE(DeliveredButFilteredOut(watcher));
  }

  TEST_CASE("watcher: a token past its window mutes nothing");
  {
    // The window is what keeps a token nothing consumed -- a save under a
    // directory no watch covers, an event the kernel dropped on overflow --
    // from sitting there to eat a real write later. Set to nothing here rather
    // than waited out, the way the retry delay is.
    const fs::path file = scratch.Write("stale-token.txt", "before\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.SetSelfWriteWindow(std::chrono::milliseconds{0});
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    watcher.ExpectSelfWrite(file.string());
    RenameIntoPlace(file, "after\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a token left on a watcher that never started is not one at all");
  {
    // Nothing is watching, so nothing will report the write and there is
    // nothing to mute. The token must not survive into the watcher's next life.
    const fs::path file = scratch.Write("late-start.txt", "before\n");
    FileWatcher watcher;
    watcher.ExpectSelfWrite(file.string());
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    RenameIntoPlace(file, "somebody else\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }
}

void FileWatcherArmingAndFailure() {
  const Scratch scratch{"koi-watch-arm"};

  TEST_CASE("watcher: re-arming the same set keeps the watches it already had");
  {
    const fs::path a = scratch.Write("a.txt", "a\n");
    const fs::path b = scratch.Write("b.txt", "b\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({a.string(), b.string()});
    // One directory, two names: the watch is per directory, which is the whole
    // point of the design and also what keeps the watch count off the file
    // count.
    EXPECT_EQ(watcher.WatchCount(), std::size_t{1});
    const std::vector<int> held = watcher.WatchDescriptors();
    EXPECT_EQ(held.size(), std::size_t{1});

    watcher.Arm({a.string(), b.string()});
    // Same descriptors, so nothing was torn down and put back -- a re-add would
    // have lost whatever was queued against the old watch.
    EXPECT_TRUE(watcher.WatchDescriptors() == held);

    WriteFixtureFile(b, "b, changed\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a path dropped from the set stops being reported");
  {
    const fs::path kept = scratch.Write("kept.txt", "kept\n");
    const fs::path dropped = scratch.Write("dropped.txt", "dropped\n");
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({kept.string(), dropped.string()});
    watcher.Arm({kept.string()});
    EXPECT_EQ(watcher.WatchCount(), std::size_t{1});
    EXPECT_FALSE(watcher.Drain());

    // The directory is still watched, so the event still arrives -- it is the
    // name filter that has to have let go of it.
    WriteFixtureFile(dropped, "dropped, changed\n");
    EXPECT_TRUE(DeliveredButFilteredOut(watcher));

    WriteFixtureFile(kept, "kept, changed\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a whole directory dropped from the set gives its watch back");
  {
    const fs::path here = scratch.Write("here.txt", "here\n");
    std::error_code ec;
    const fs::path elsewhere_dir = scratch.dir / "elsewhere";
    fs::create_directories(elsewhere_dir, ec);
    const fs::path elsewhere = elsewhere_dir / "there.txt";
    WriteFixtureFile(elsewhere, "there\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({here.string(), elsewhere.string()});
    EXPECT_EQ(watcher.WatchCount(), std::size_t{2});
    watcher.Arm({here.string()});
    EXPECT_EQ(watcher.WatchCount(), std::size_t{1});
    // inotify_rm_watch queues an IN_IGNORED for the watch it just dropped.
    // Draining it here is also the assertion that it is neither reported nor
    // taken as a watch to put back: the descriptor was forgotten before the
    // event arrived, which is the ordering Arm has to get right.
    EXPECT_FALSE(DrainedAnythingWithin(watcher));
    EXPECT_FALSE(watcher.NeedsRearm());

    WriteFixtureFile(elsewhere, "there, changed\n");
    // No watch on that directory at all now, so nothing should even arrive.
    EXPECT_FALSE(Readable(watcher, kQuietMs));
    EXPECT_FALSE(watcher.Drain());
  }

  TEST_CASE("watcher: a directory that is taken away and put back is watched again");
  {
    std::error_code ec;
    const fs::path dir = scratch.dir / "vanishes";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "f.txt";
    WriteFixtureFile(file, "one\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});

    // What a `git checkout` across a directory that only exists on one branch
    // looks like from here.
    fs::remove_all(dir, ec);
    EXPECT_TRUE(WaitForRearmRequest(watcher));
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{0});

    fs::create_directories(dir, ec);
    WriteFixtureFile(file, "two\n");
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.NeedsRearm());
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    std::ignore = DrainedAnythingWithin(watcher);

    WriteFixtureFile(file, "three\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: the watched directory removed under it gives the descriptor up");
  {
    // `rm -rf` on the directory a buffer lives in. There is no name left to
    // report -- the watch itself is what went -- and holding the descriptor
    // afterwards is worse than useless: the kernel hands the number out again,
    // and the next watch to get it would be answering for this one.
    std::error_code ec;
    const fs::path dir = scratch.dir / "removed";
    fs::create_directories(dir, ec);
    EXPECT_TRUE(!ec);
    const fs::path file = dir / "f.txt";

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    EXPECT_FALSE(watcher.Drain());

    // Empty when it goes, deliberately: with a file in it the IN_DELETE for
    // that name would carry the report and the directory's own event would
    // never have to be read at all.
    EXPECT_TRUE(fs::remove(dir, ec));
    EXPECT_TRUE(DrainUntilReported(watcher));
    EXPECT_TRUE(watcher.NeedsRearm());
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{0});
  }

  TEST_CASE("watcher: a queue the kernel overflowed is read as everything at once");
  {
    // The kernel's queue is bounded. Past the bound every event is dropped and
    // one IN_Q_OVERFLOW stands in for all of them -- including, since it queues
    // like anything else, the IN_IGNORED that would have said a watch died. So
    // an overflow has to read as "something happened" *and* as "the watches are
    // no longer to be trusted", which is the re-arm.
    std::error_code ec;
    const fs::path dir = scratch.dir / "overflows";
    fs::create_directories(dir, ec);
    EXPECT_TRUE(!ec);
    const fs::path file = dir / "mine.txt";
    WriteFixtureFile(file, "mine\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.Drain());

    // Renames of a pair of names nothing is watching. A rename is two events
    // for one syscall, and -- unlike a chmod loop, which the kernel folds into
    // the single identical event already at the tail -- no two of them in a row
    // are identical, so they all queue.
    const fs::path ping = dir / "ping";
    const fs::path pong = dir / "pong";
    WriteFixtureFile(ping, "x\n");
    // Sized so that one event per rename is already past the cap: what is
    // under test here is the overflow, and it must not quietly stop happening
    // because some other bit of the mask went missing.
    const int rounds = (QueuedEventCap() / 2) + 200;
    for (int i = 0; i < rounds; ++i) {
      fs::rename(ping, pong, ec);
      fs::rename(pong, ping, ec);
    }
    EXPECT_TRUE(!ec);

    // One drain. Not one of those names is koi's, so a report here is the
    // overflow branch and nothing else -- and so is the re-arm, because no
    // watch was added, dropped or invalidated in any of it.
    EXPECT_TRUE(watcher.Drain());
    EXPECT_TRUE(watcher.NeedsRearm());

    // And the watch is still live on the other side of it.
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.NeedsRearm());
    WriteFixtureFile(file, "mine, changed\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a watch the kernel will not give is quiet, not fatal");
  {
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    // A directory that is not there (ENOENT) and a "directory" that is really a
    // file (ENOTDIR, via IN_ONLYDIR). Both are ordinary -- a buffer on a path
    // under a directory that does not exist yet is a normal thing to have open
    // -- so neither is worth a word on the status line.
    const fs::path plain = scratch.Write("not-a-directory", "x\n");
    watcher.Arm({(scratch.dir / "no-such-dir" / "f.txt").string(),
                 (plain / "f.txt").string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{0});
    EXPECT_FALSE(watcher.Drain());
    EXPECT_TRUE(watcher.TakeProblem().empty());

    // And a good path armed alongside them still works.
    const fs::path good = scratch.Write("good.txt", "good\n");
    watcher.Arm({(scratch.dir / "no-such-dir" / "f.txt").string(), good.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    WriteFixtureFile(good, "good, changed\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a watched directory renamed away is let go of, not followed");
  {
    std::error_code ec;
    const fs::path dir = scratch.dir / "moves";
    fs::create_directories(dir, ec);
    const fs::path file = dir / "f.txt";
    WriteFixtureFile(file, "one\n");

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});

    // The kernel keeps the watch across this and sends no IN_IGNORED: the watch
    // rides the inode to its new path. A watcher that waited for IN_IGNORED
    // would stay keyed by a path it no longer hears anything about, and would
    // report every matching name written under the new one instead.
    const fs::path moved = scratch.dir / "moves.bak";
    fs::rename(dir, moved, ec);
    EXPECT_TRUE(WaitForRearmRequest(watcher));
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{0});
    // The IN_IGNORED the rm_watch above queues, out of the way -- and nothing
    // of koi's in it.
    EXPECT_FALSE(DrainedAnythingWithin(watcher));

    // Where the directory went is not koi's business any more.
    WriteFixtureFile(moved / "f.txt", "two\n");
    EXPECT_FALSE(Readable(watcher, kQuietMs));
    EXPECT_FALSE(watcher.Drain());

    // And the path koi actually named, recreated, is watched again.
    fs::create_directories(dir, ec);
    watcher.Arm({file.string()});
    EXPECT_FALSE(watcher.NeedsRearm());
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    WriteFixtureFile(file, "three\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: a directory the kernel refused is asked for again later");
  {
    std::error_code ec;
    const fs::path dir = scratch.dir / "appears-later";
    const fs::path file = dir / "f.txt";

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    // `koi notes/todo.md` before notes/ exists. Nothing about the buffer set
    // changes when the directory finally turns up, and no event can arrive on a
    // watch that was never added, so the deadline is the only thing that brings
    // Arm back here.
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{0});
    // Not straight away, though: at the delay the editor runs with, a retry
    // that keeps failing must not turn every loop turn into an Arm.
    EXPECT_FALSE(watcher.NeedsRearm());

    watcher.SetRetryDelay(std::chrono::milliseconds{0});
    watcher.Arm({file.string()});
    EXPECT_TRUE(watcher.NeedsRearm());

    fs::create_directories(dir, ec);
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchDescriptors().size(), std::size_t{1});
    // Armed, so there is nothing left outstanding to retry.
    EXPECT_FALSE(watcher.NeedsRearm());

    WriteFixtureFile(file, "here now\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
  }

  TEST_CASE("watcher: the one line is one per watcher, not one per process");
  {
    // A real inotify_init1 failure, made cheap: RLIMIT_NOFILE down to where the
    // next descriptor cannot exist. Nothing but the Start calls happens inside
    // the window, and the limit is back up before anything is asserted.
    struct rlimit was{};
    if (::getrlimit(RLIMIT_NOFILE, &was) == 0) {
      struct rlimit tight = was;
      // Descriptor numbers have to come out below the limit, and stdin holds
      // the only one this leaves.
      tight.rlim_cur = 1;
      FileWatcher watcher;
      bool tightened = false;
      bool started = true;
      std::string first;
      std::string again;
      std::string same_watcher;
      std::string after_stop;
      if (::setrlimit(RLIMIT_NOFILE, &tight) == 0) {
        tightened = true;
        started = watcher.Start();
        first = watcher.TakeProblem();
        again = watcher.TakeProblem();
        std::ignore = watcher.Start();
        same_watcher = watcher.TakeProblem();
        watcher.Stop();
        std::ignore = watcher.Start();
        after_stop = watcher.TakeProblem();
        std::ignore = ::setrlimit(RLIMIT_NOFILE, &was);
      }
      if (tightened) {
        // setrlimit took, so there is no descriptor left for inotify_init1 to
        // come back with. A Start that succeeded here would mean the failure
        // path was never entered and everything below was asserted about
        // nothing at all.
        EXPECT_FALSE(started);
      }
      if (tightened && !started) {
        EXPECT_FALSE(first.empty());
        // Taken means taken: the status bar has it and the watcher does not.
        EXPECT_TRUE(again.empty());
        // And it is said once however many times the same watcher fails --
        // otherwise a failing Arm would say it once per directory per re-arm.
        EXPECT_TRUE(same_watcher.empty());
        // But the latch belongs to the watcher, not to the run: `:config-
        // reload` stands a new one up, and its failures are news again.
        EXPECT_FALSE(after_stop.empty());
      }
    }
  }

  TEST_CASE("watcher: one that never started is inert rather than dangerous");
  {
    // What a watcher with no descriptor owes the editor -- the state a failed
    // inotify_init1 leaves, and the identical state Stop leaves, which is why
    // Stop is what sets it up here. The failure itself is forced for real in
    // the case above, through RLIMIT_NOFILE.
    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    const fs::path file = scratch.Write("inert.txt", "x\n");
    watcher.Arm({file.string()});
    watcher.Stop();

    EXPECT_EQ(watcher.Fd(), -1);
    EXPECT_EQ(watcher.WatchCount(), std::size_t{0});
    EXPECT_FALSE(watcher.NeedsRearm());
    // Arming and draining a dead watcher must be nothing at all: this is the
    // path the editor takes on every turn when auto-reload is off, and -1 is
    // what tells the loop to fall back to its old blocking wait.
    watcher.Arm({file.string()});
    EXPECT_EQ(watcher.WatchCount(), std::size_t{0});
    EXPECT_FALSE(watcher.Drain());
    watcher.Arm({});
    EXPECT_FALSE(watcher.Drain());
  }

  TEST_CASE("watch signature: file-backed buffers only, and never the no-signature value");
  {
    Editor ed;
    // Nothing behind the buffer, so nothing is mixed in -- and the result still
    // has to be a signature, because kNoWatchSignature means "re-arm anyway".
    const std::size_t none = WatchSignature(ed);
    EXPECT_TRUE(none != kNoWatchSignature);
    EXPECT_EQ(WatchSignature(ed), none);

    ed.doc.file = fs::path{"/w/alpha.txt"};
    const std::size_t one = WatchSignature(ed);
    EXPECT_TRUE(one != none);
    EXPECT_TRUE(one != kNoWatchSignature);
    // Asked twice on an unchanged buffer set, which is the only reason the
    // signature exists: same answer, so no Arm.
    EXPECT_EQ(WatchSignature(ed), one);
  }

  TEST_CASE("watch signature: id, path, count and order all move it");
  {
    Editor ed;
    // Slot 0 is the active one and is read from ed.doc, so buffers[0] is a
    // placeholder the hash never sees.
    ed.buffers.emplace_back();
    ed.buffers.emplace_back();
    ed.active = 0;
    ed.doc.file = fs::path{"/w/alpha.txt"};
    ed.buffers[1].file = fs::path{"/w/beta.txt"};
    const std::size_t two = WatchSignature(ed);

    // A path renamed under a buffer that is otherwise the same buffer.
    ed.buffers[1].file = fs::path{"/w/beta-renamed.txt"};
    EXPECT_TRUE(WatchSignature(ed) != two);
    ed.buffers[1].file = fs::path{"/w/beta.txt"};
    EXPECT_EQ(WatchSignature(ed), two);

    // Same path, different document: a closed buffer reopened is not the one
    // that was armed.
    const Index was = ed.buffers[1].id;
    ed.buffers[1].id = was + 1;
    EXPECT_TRUE(WatchSignature(ed) != two);
    ed.buffers[1].id = was;
    EXPECT_EQ(WatchSignature(ed), two);

    // One more buffer, then the same one gone again.
    ed.buffers.emplace_back();
    ed.buffers[2].file = fs::path{"/w/gamma.txt"};
    const std::size_t three = WatchSignature(ed);
    EXPECT_TRUE(three != two);
    ed.buffers.pop_back();
    EXPECT_EQ(WatchSignature(ed), two);

    // A buffer with no file on disk is not in the hash at all.
    ed.buffers.emplace_back();
    ed.buffers[2].file.clear();
    EXPECT_EQ(WatchSignature(ed), two);

    // Order-sensitive, and pinned as such: FNV-1a folds each buffer into the
    // running hash, so the same two buffers in the other order hash apart.
    // Nothing depends on that -- Arm takes a set -- but a re-Arm too many is
    // the harmless side to be wrong on, and this is what the hash does today.
    ed.buffers[2].file = fs::path{"/w/gamma.txt"};
    const Index id_1 = ed.buffers[1].id;
    const Index id_2 = ed.buffers[2].id;
    const fs::path path_1 = ed.buffers[1].file;
    const fs::path path_2 = ed.buffers[2].file;
    ed.buffers[1].id = id_2;
    ed.buffers[1].file = path_2;
    ed.buffers[2].id = id_1;
    ed.buffers[2].file = path_1;
    EXPECT_TRUE(WatchSignature(ed) != three);
  }
}

void EventDrivenDiskCheckDoesNotRepeatItself() {
  const Scratch scratch{"koi-watch-recheck"};

  TEST_CASE("event-driven check: a clean buffer reloads, a modified one is held");
  {
    const fs::path file = scratch.Write("both.txt", "one\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    // Clean: taken, and taken by the same call the watcher drives.
    RenameIntoPlace(file, "two\n");
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"two\n"});
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);

    // Modified: held, and the text on disk never reaches the buffer.
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("mine ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    RenameIntoPlace(file, "three\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"mine two\n"});
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_TRUE(ed.status.find(":w! to overwrite") != std::string::npos);
  }

  TEST_CASE("event-driven check: the held warning is said once per disk state");
  {
    const fs::path file = scratch.Write("held.txt", "theirs\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("mine ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;

    RenameIntoPlace(file, "theirs, one\n");
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);

    // Driven by the watcher this runs on every write anything in the directory
    // makes, so the same sentence must not come back on a state the user has
    // already been told about -- there is no way to dismiss it and it sits on
    // top of whatever they were actually being told.
    for (int again = 0; again < 5; ++again) {
      ed.status.clear();
      CheckDiskChange(ed);
      EXPECT_TRUE(ed.status.empty());
    }
    EXPECT_TRUE(ed.doc.modified);
    EXPECT_EQ(TextOf(ed), std::string{"mine theirs\n"});

    // A different disk state is different news, and is said again.
    RenameIntoPlace(file, "theirs, two\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());

    // And once the buffer is saved the slate is clean: the next foreign write
    // is news again rather than a repeat of something settled. Asserted on the
    // two fields and not only on the sentence that follows, because the save
    // moves the file's stamp too -- the sentence would come back whether or not
    // the save had cleared what the buffer was last told.
    EXPECT_FALSE(ed.doc.warned_stamp.SameFile(FileStamp{}));
    EXPECT_TRUE(ed.self_writes.empty());
    RunTypableCommand(ed, "w!");
    // What the loop hands the watcher so its own rename does not buy a sweep.
    // The handoff itself lives in the loop and is not reachable from here; this
    // is the half of it that is.
    EXPECT_EQ(ed.self_writes.size(), std::size_t{1});
    EXPECT_EQ(fs::path{ed.self_writes.front()}, file);
    ed.self_writes.clear();
    EXPECT_TRUE(ed.doc.warned_stamp.SameFile(FileStamp{}));
    EXPECT_FALSE(ed.doc.warned_reload_failed);
    EXPECT_FALSE(ed.doc.modified);
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("more ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    RenameIntoPlace(file, "theirs, three\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
  }

  TEST_CASE("event-driven check: a reload that cannot be done is reported once");
  {
    // A file koi cannot take: not UTF-8. The reload fails, so the stamp stays
    // where it was and the buffer reads as changed on every sweep after --
    // which is why `failed` is suppressed the same way `held` is.
    const fs::path file = scratch.Write("broken.txt", "fine\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    WriteFixtureFile(file, std::string{"\xff\xfe bad\n", 7});
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("cannot reload") != std::string::npos);
    EXPECT_EQ(TextOf(ed), std::string{"fine\n"});

    for (int again = 0; again < 5; ++again) {
      ed.status.clear();
      CheckDiskChange(ed);
      EXPECT_TRUE(ed.status.empty());
    }

    // Typing into it turns the news from "cannot reload" into the advice a held
    // buffer gets, which is different advice and worth saying even though the
    // file has not moved.
    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("mine ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find(":w! to overwrite") != std::string::npos);

    // And a file that becomes readable again is taken, not left stuck behind
    // the suppression.
    ed.doc.modified = false;
    WriteFixtureFile(file, "fine again\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"fine again\n"});
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);
    // The reload's own reset, and the reason it is a reset and not a re-stamp:
    // buffer and file agree again, so nothing the buffer was told about the old
    // state is still true. Left as it was, the stamp above would go on
    // suppressing the next thing this file has to say.
    EXPECT_TRUE(ed.doc.warned_stamp.SameFile(FileStamp{}));
    EXPECT_FALSE(ed.doc.warned_reload_failed);
  }

  TEST_CASE("event-driven check: a file that goes away is said so, once");
  {
    // The buffer is the last copy of the file now, which is the one thing the
    // user has to be told before they quit: ExternallyModified reads a missing
    // file as an unchanged one, so this used to be silence.
    const fs::path file = scratch.Write("vanishes.txt", "here\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    std::error_code rm_ec;
    EXPECT_TRUE(fs::remove(file, rm_ec));
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("is gone from disk") != std::string::npos);
    EXPECT_EQ(TextOf(ed), std::string{"here\n"});

    // Driven by the watcher, so the same nothing must not be reported over and
    // over while the rest of the directory moves.
    for (int again = 0; again < 5; ++again) {
      ed.status.clear();
      CheckDiskChange(ed);
      EXPECT_TRUE(ed.status.empty());
    }

    // The key is the stamp a failed stat leaves, which nothing on disk equals,
    // so a file written back at that path is news rather than a repeat. Longer
    // than what was there, so the (mtime, size) pair cannot land on the old one
    // through a coarse clock.
    WriteFixtureFile(file, "put back again\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"put back again\n"});
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);
    EXPECT_FALSE(ed.doc.warned_reload_failed);
  }

  TEST_CASE("event-driven check: a buffer loaded over a warned one starts with a clean slate");
  {
    // `:e other.md` into a buffer that has already been told its file changed
    // on disk. The two fields belong to the document, not to the file, so a
    // load that left them where they were would either suppress the first real
    // warning about the new file or report a failure the new file never had.
    const fs::path first = scratch.Write("slate-one.txt", "one\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(first, ed.doc));
    ed.doc.warned_stamp = ed.doc.disk_stamp;
    ed.doc.warned_reload_failed = true;
    EXPECT_FALSE(ed.doc.warned_stamp.SameFile(FileStamp{}));

    const fs::path second = scratch.Write("slate-two.txt", "two\n");
    EXPECT_TRUE(!LoadDocument(second, ed.doc));
    EXPECT_TRUE(ed.doc.warned_stamp.SameFile(FileStamp{}));
    EXPECT_FALSE(ed.doc.warned_reload_failed);

    // And the new file's first foreign write is said, which is the same thing
    // from the outside.
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));
    ed.doc.modified = true;
    RenameIntoPlace(second, "two, theirs\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("changed on disk") != std::string::npos);
  }

  TEST_CASE("event-driven check: a buffer undone back to its save point is held");
  {
    // Clean by the save-point test, and its text is on the redo branch. A
    // reload's whole-document Apply overwrites that branch and the text is
    // gone for good, so `modified` alone is not the question to ask.
    const fs::path file = scratch.Write("redoable.txt", "base\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    ed.mode = Mode::kInsert;
    std::ignore = InsertAtCursorsKeeping("mine ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    RunTypableCommand(ed, "w!");
    EXPECT_FALSE(ed.doc.modified);
    const std::string at_save = TextOf(ed);

    BreakUndoCoalescing(ed.doc.table);
    std::ignore = InsertAtCursorsKeeping("more ", ed.doc.table, ed.doc.selections);
    ed.doc.modified = true;
    const std::string with_more = TextOf(ed);
    EXPECT_TRUE(with_more != at_save);

    RunCommands(ed, {"undo"});
    EXPECT_EQ(TextOf(ed), at_save);
    EXPECT_FALSE(ed.doc.modified);
    EXPECT_TRUE(CanRedo(ed.doc.table));

    RenameIntoPlace(file, "theirs, all of it\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), at_save);
    EXPECT_TRUE(ed.status.find(":w! to overwrite") != std::string::npos);

    // And the text is still there to come back to, which is the whole point.
    EXPECT_TRUE(CanRedo(ed.doc.table));
    RunCommands(ed, {"redo"});
    EXPECT_EQ(TextOf(ed), with_more);
  }

  TEST_CASE("event-driven check: only the buffers the report names are marked as told");
  {
    // More held buffers than the message will name. The ones folded into "and
    // N more" have not been told anything, so they must not go quiet -- the
    // next sweep names the next three, and so on until everything is said.
    constexpr int kPanes = 6;
    Editor ed;
    ed.screen_h = 200;

    std::vector<fs::path> files;
    for (int i = 0; i < kPanes; ++i) {
      files.push_back(scratch.Write("many-" + std::to_string(i) + ".txt", "one\n"));
    }
    EXPECT_TRUE(OpenTarget(ed, files[0].string()));
    for (int i = 1; i < kPanes; ++i) {
      SplitWindow(ed, false);
      EXPECT_TRUE(OpenTarget(ed, files[i].string()));
    }
    EXPECT_EQ(BufferCount(ed), std::size_t{kPanes});

    const auto doc_for = [&ed](const fs::path& path) -> Document* {
      for (std::size_t i = 0; i < BufferCount(ed); ++i) {
        Document& doc = (ed.buffers.empty() || (i == ed.active)) ? ed.doc : ed.buffers[i];
        if (doc.file == path) return &doc;
      }
      return nullptr;
    };

    for (int i = 0; i < kPanes; ++i) {
      Document* doc = doc_for(files[i]);
      EXPECT_TRUE(doc != nullptr);
      if (doc == nullptr) continue;
      EXPECT_TRUE(BufferOnScreen(ed, static_cast<std::size_t>(i)));
      doc->modified = true;
      RenameIntoPlace(files[i], "theirs, " + std::to_string(i) + "\n");
    }

    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("and 3 more") != std::string::npos);

    // Named and marked have to be the same set, in both directions.
    std::vector<fs::path> first_batch;
    std::vector<fs::path> rest;
    for (int i = 0; i < kPanes; ++i) {
      Document* doc = doc_for(files[i]);
      if (doc == nullptr) continue;
      FileStamp now;
      EXPECT_TRUE(StampFile(files[i].string(), now));
      const bool marked = now.SameFile(doc->warned_stamp);
      const bool named = ed.status.find(files[i].filename().string()) != std::string::npos;
      EXPECT_EQ(marked, named);
      (marked ? first_batch : rest).push_back(files[i]);
    }
    EXPECT_EQ(first_batch.size(), std::size_t{3});
    EXPECT_EQ(rest.size(), std::size_t{3});

    // Next sweep: the told ones are quiet and the rest fit, so everything that
    // changed has now been named once.
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("and 1 more") == std::string::npos);
    for (const fs::path& file : first_batch) {
      EXPECT_TRUE(ed.status.find(file.filename().string()) == std::string::npos);
    }
    for (const fs::path& file : rest) {
      EXPECT_TRUE(ed.status.find(file.filename().string()) != std::string::npos);
      Document* doc = doc_for(file);
      FileStamp now;
      EXPECT_TRUE(doc != nullptr && StampFile(file.string(), now));
      if (doc != nullptr) EXPECT_TRUE(now.SameFile(doc->warned_stamp));
    }

    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());
  }

  TEST_CASE("event-driven check: a suppressed reload is not attempted at all");
  {
    // The suppression has to gate the work, not just the sentence: a file koi
    // cannot read costs a whole-file read, a UTF-8 scan and a hash, and the
    // watcher asks again on every write anywhere in that directory.
    const fs::path file = scratch.Write("stuck.txt", "fine\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    WriteFixtureFile(file, std::string{"\xff\xfe bad\n", 7});
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.find("cannot reload") != std::string::npos);
    EXPECT_TRUE(ed.doc.warned_reload_failed);
    FileStamp bad;
    EXPECT_TRUE(StampFile(file.string(), bad));
    EXPECT_TRUE(bad.SameFile(ed.doc.warned_stamp));

    // Readable bytes behind the stamp koi was told about: same size, mtime put
    // back. If the sweep still reads the file it will reload happily and say
    // so, and that is the only way to tell a skipped read from a skipped
    // message without instrumenting the reload.
    WriteFixtureFile(file, "goodok\n");
    std::error_code time_ec;
    fs::last_write_time(file, bad.mtime, time_ec);
    EXPECT_FALSE(static_cast<bool>(time_ec));
    FileStamp posing;
    EXPECT_TRUE(StampFile(file.string(), posing));
    EXPECT_TRUE(posing.SameFile(bad));

    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());
    EXPECT_EQ(TextOf(ed), std::string{"fine\n"});

    // Not stuck forever: a stamp that moves is a new question and gets asked.
    WriteFixtureFile(file, "good and longer\n");
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"good and longer\n"});
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);
  }

  TEST_CASE("event-driven check: the watcher wakes on exactly what the check then takes");
  {
    // The two halves together, which is all the editor's loop does with them:
    // arm on the open buffers, wait for the fd, sweep once.
    const fs::path file = scratch.Write("end-to-end.txt", "before\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{0, 0, -1}));

    FileWatcher watcher;
    EXPECT_TRUE(watcher.Start());
    watcher.Arm({ed.doc.file.string()});
    EXPECT_FALSE(watcher.Drain());

    RenameIntoPlace(file, "after\n");
    EXPECT_TRUE(DrainUntilReported(watcher));
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"after\n"});
    EXPECT_TRUE(ed.status.find("reloaded") != std::string::npos);

    // The sweep took the new stamp, so the next wake finds nothing to say --
    // no warning the user cannot get rid of, however often the loop asks.
    ed.status.clear();
    CheckDiskChange(ed);
    EXPECT_TRUE(ed.status.empty());
  }

  TEST_CASE("event-driven check: a reload puts the cursor back on its column");
  {
    // A reload replaces the whole document, so the cursor is put back by
    // position rather than carried: line alone drops it to column 0, which for
    // a file a formatter rewrote is the cursor walking off mid-word.
    const fs::path file = scratch.Write("column.txt", "alpha\nbravo beans\ncharlie\n");
    Editor ed;
    EXPECT_TRUE(!LoadDocument(file, ed.doc));
    const Index line_two = LineStart(ed.doc.table, 1);
    ed.doc.selections.Set(MinWidth1(ed.doc.table, Selection{line_two + 6, line_two + 6, -1}));

    RenameIntoPlace(file, "alpha\nbravo BEANS\ncharlie\n");
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"alpha\nbravo BEANS\ncharlie\n"});

    Index at = CursorOf(ed.doc.table, ed.doc.selections.Primary());
    Index row = LineAt(ed.doc.table, at);
    EXPECT_EQ(row, Index{1});
    EXPECT_EQ(at - LineStart(ed.doc.table, row), Index{6});

    // And a line the rewrite made too short for that column clamps to its end
    // rather than running into the next line or off the document.
    RenameIntoPlace(file, "alpha\nbr\ncharlie\n");
    CheckDiskChange(ed);
    EXPECT_EQ(TextOf(ed), std::string{"alpha\nbr\ncharlie\n"});
    at = CursorOf(ed.doc.table, ed.doc.selections.Primary());
    row = LineAt(ed.doc.table, at);
    EXPECT_EQ(row, Index{1});
    EXPECT_EQ(at - LineStart(ed.doc.table, row), Index{2});
  }

  TEST_CASE("debounce: the first event of a burst arms it and the rest do not push it back");
  {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const auto ms_after = [t0](std::chrono::steady_clock::time_point at) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(at - t0).count();
    };

    // A turn where the fd held nothing arms nothing.
    EXPECT_FALSE(NextDiskDeadline(std::nullopt, false, t0, 50).has_value());

    const auto armed = NextDiskDeadline(std::nullopt, true, t0, 50);
    EXPECT_TRUE(armed.has_value());
    EXPECT_EQ(ms_after(*armed), std::int64_t{50});

    // The rest of the burst, and then a writer that keeps going for far longer
    // than the window. The deadline has to stay where the first event put it --
    // pushing it back on every event is what would let the writer defer the
    // reload for as long as it ran.
    auto held = armed;
    for (int i = 1; i <= 500; ++i) {
      held = NextDiskDeadline(held, true, t0 + std::chrono::milliseconds{i}, 50);
    }
    EXPECT_TRUE(held.has_value());
    EXPECT_EQ(ms_after(*held), std::int64_t{50});

    // And a quiet turn leaves an armed deadline alone rather than clearing it.
    const auto quiet = NextDiskDeadline(armed, false, t0 + std::chrono::milliseconds{10}, 50);
    EXPECT_TRUE(quiet.has_value());
    EXPECT_EQ(ms_after(*quiet), std::int64_t{50});
  }

  TEST_CASE("debounce: the window is clamped at both ends");
  {
    using Clock = std::chrono::steady_clock;
    const auto t0 = Clock::now();
    const auto ms_after = [t0](std::chrono::steady_clock::time_point at) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(at - t0).count();
    };

    const auto huge = NextDiskDeadline(std::nullopt, true, t0, 999999);
    EXPECT_TRUE(huge.has_value());
    EXPECT_EQ(ms_after(*huge), kMaxDebounceMs);

    // Zero is a legal setting and means "sweep on the next turn", not "never".
    const auto zero = NextDiskDeadline(std::nullopt, true, t0, 0);
    EXPECT_TRUE(zero.has_value());
    EXPECT_EQ(ms_after(*zero), std::int64_t{0});

    // A negative one cannot come from the config, but it must not arm a
    // deadline in the past that the wait would then treat as a spin.
    const auto negative = NextDiskDeadline(std::nullopt, true, t0, -5000);
    EXPECT_TRUE(negative.has_value());
    EXPECT_EQ(ms_after(*negative), std::int64_t{0});
  }

  TEST_CASE("termbox read: a part-arrived event is not a failure and the real errors still are");
  {
    EXPECT_TRUE(IncompleteTermboxRead(TB_ERR_NO_EVENT));
    // A half-arrived escape sequence.
    EXPECT_TRUE(IncompleteTermboxRead(TB_ERR_NEED_MORE));
    // A utf-8 sequence cut by the read that carried it. Read as fatal, this one
    // quits the editor over a keystroke that arrived in two pieces.
    EXPECT_TRUE(IncompleteTermboxRead(TB_ERR));

    EXPECT_FALSE(IncompleteTermboxRead(TB_OK));
    // The codes that mean the terminal is gone or unusable, which the loop has
    // to keep treating as the end of the run.
    for (const int fatal : {TB_ERR_INIT_ALREADY, TB_ERR_INIT_OPEN, TB_ERR_MEM, TB_ERR_NO_TERM,
                            TB_ERR_NOT_INIT, TB_ERR_OUT_OF_BOUNDS, TB_ERR_READ,
                            TB_ERR_RESIZE_IOCTL, TB_ERR_RESIZE_PIPE, TB_ERR_RESIZE_SIGACTION,
                            TB_ERR_POLL, TB_ERR_TCGETATTR, TB_ERR_TCSETATTR,
                            TB_ERR_UNSUPPORTED_TERM, TB_ERR_RESIZE_WRITE, TB_ERR_RESIZE_POLL,
                            TB_ERR_RESIZE_READ, TB_ERR_RESIZE_SSCANF, TB_ERR_CAP_COLLISION}) {
      EXPECT_FALSE(IncompleteTermboxRead(fatal));
    }
  }
}

}  // namespace koi
