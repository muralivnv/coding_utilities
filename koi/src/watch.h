// The "somebody else just wrote one of our files" oracle.
//
// One inotify fd, owned and read by the main thread, added to the wait the
// event loop already blocks in. There is no thread here and nothing polls: when
// nothing changes koi sits in the kernel exactly as it did before this file
// existed, and the fd costs one read() per loop turn to find empty.
//
// The watcher is advisory and only advisory. It answers "go and look", never
// "this is what changed" -- every decision about what actually moved and what
// to do about it stays in CheckDiskChange, which re-stats regardless. A
// spurious wake therefore costs a stat sweep and nothing else, and a missed
// event degrades to the focus-in check koi had before, never to a buffer
// holding text its file no longer has.
#ifndef KOI_WATCH_H_
#define KOI_WATCH_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace koi {

struct Editor;

class FileWatcher {
 public:
  FileWatcher() = default;
  ~FileWatcher();
  FileWatcher(const FileWatcher&) = delete;
  FileWatcher& operator=(const FileWatcher&) = delete;

  // Idempotent. False means inotify said no -- out of instances, out of file
  // descriptors, or not there at all -- and the caller carries on with the
  // watcher switched off. TakeProblem() has the one line worth saying about it.
  bool Start();

  // Drops every watch by closing the fd, which is what the kernel documents as
  // the way to do it. Safe to call on a watcher that never started.
  void Stop();

  // The directories behind `paths`, watched; anything else, dropped. Cheap when
  // the set is unchanged: an already-held directory keeps its watch descriptor
  // and only its name set is rewritten.
  void Arm(const std::vector<std::string>& paths);

  // For the loop's poll() set. -1 when the watcher is off or never started, and
  // that is the signal to fall back to the plain blocking wait.
  int Fd() const { return fd_; }

  // Reads until EAGAIN. True means something koi named has moved and the caller
  // should go and look; false means the fd held nothing, or nothing that
  // matters. Never blocks, never allocates, and touches no editor state -- a
  // false is what lets the caller go straight back to sleep without drawing.
  bool Drain();

  // A watch the kernel took away -- IN_IGNORED, or a rename of the watched
  // directory -- leaves a hole the next Arm fills. Without this a `git
  // checkout` that removes and recreates a directory would blind the watcher
  // for that directory until the buffer set happened to change.
  //
  // True as well once the retry deadline has passed, which is the only thing
  // that ever asks again for a directory the kernel refused outright.
  bool NeedsRearm() const;

  // How long Arm waits before trying a directory the kernel would not give.
  // The default is what the editor runs with; tests shrink it so the retry is
  // observable without waiting for it.
  void SetRetryDelay(std::chrono::milliseconds delay) { retry_delay_ = delay; }

  // koi is about to write this path itself. AtomicWriteFile renames into place,
  // which reaches the watch as an event on a watched basename and is not
  // otherwise distinguishable from somebody else's write -- so every :w bought
  // a debounce, a stat sweep over every on-screen buffer and a repaint, all of
  // them finding nothing because the save re-stamps after the rename. The token
  // this leaves is spent by the one event the save produces.
  //
  // One token, one event, and never more than a mute: a token changes nothing
  // about which watches exist or which names are watched. One that nothing
  // consumes -- a save under a directory no watch covers, an event the kernel
  // dropped on overflow -- dies of old age rather than waiting around to eat a
  // real write minutes later.
  void ExpectSelfWrite(const std::string& path);

  // How long a self-write token stays live. Tests set it to nothing to prove an
  // expired token mutes nothing.
  void SetSelfWriteWindow(std::chrono::milliseconds window) { self_write_window_ = window; }

  // The one line the status bar gets, moved out. Empty afterwards and empty
  // for the rest of this watcher's life: one that cannot add watches would
  // otherwise say so once per armed directory per re-arm. Stop() ends that
  // life, so a watcher restarted around a failure the user has acted on is
  // free to speak again.
  std::string TakeProblem();

  std::size_t WatchCount() const { return dirs_.size(); }

  // The watch descriptors held right now, sorted. Diagnostics only -- the tests
  // read it to tell "re-armed the same set" apart from "tore every watch down
  // and put it back", which is otherwise unobservable from outside.
  std::vector<int> WatchDescriptors() const;

 private:
  // Transparent, so a basename can be looked up as a string_view straight over
  // the byte the kernel handed back. The unwatched-file case is the hot one --
  // every build artefact written next to an open buffer comes through here --
  // and it has to cost a hash and a compare, not a std::string.
  struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  using NameSet = std::unordered_set<std::string, NameHash, std::equal_to<>>;

  struct Dir {
    // -1 for a directory koi wants but the kernel would not give it: gone, not
    // a directory, or past max_user_watches. Kept in the map so the next Arm
    // tries again instead of forgetting the directory ever mattered.
    int wd{-1};
    NameSet names;
  };

  // koi's own save, waiting for the event it will cause. Matched on (directory,
  // basename) because that is all an inotify event carries.
  struct SelfWrite {
    std::string dir;
    std::string name;
    std::chrono::steady_clock::time_point at;
  };

  void Note(const std::string& what);

  // Drops `dir` from what `wd` reports to, and gives the descriptor back to the
  // kernel only once nothing is left holding it. Two spellings of one directory
  // -- a bind mount, a filesystem mounted twice -- share the single descriptor
  // inotify hands back per inode, and letting go of one of them must not take
  // the other's live watch with it.
  void DropOwner(int wd, Dir* dir);

  // A live token for this descriptor and basename, consumed. False leaves the
  // event to be reported the ordinary way. Expired tokens are dropped on the
  // way past.
  bool TakeSelfWrite(int wd, std::string_view name);

  int fd_{-1};
  // Keyed by canonical directory path, so two spellings of one directory -- a
  // symlinked parent and the real one -- cannot end up as two entries sharing
  // the single watch descriptor inotify hands back for one inode.
  //
  // unordered_map is node-based, so the Dir* below stay good across every
  // insert; only an erase invalidates one, and Arm drops both sides together.
  std::unordered_map<std::string, Dir> dirs_;
  // One descriptor, every entry holding it. Canonicalising collapses the
  // spellings a symlink makes, but not the ones a bind mount or a second mount
  // of the same filesystem make: those are genuinely different paths to one
  // inode, and inotify keys on the inode, so both entries come back with the
  // same descriptor. A single owner per descriptor meant the loser of that race
  // was never matched against again -- its files' rewrites arrived on a
  // descriptor whose only owner was the other spelling and were filtered out as
  // somebody else's -- and dropping either entry gave the shared watch back
  // underneath the survivor.
  std::unordered_map<int, std::vector<Dir*>> by_wd_;
  std::string problem_;
  bool problem_spent_{false};
  bool rearm_{false};
  // Armed while a directory koi wants has no watch on it: not there at open,
  // not there for longer than a checkout takes, or past max_user_watches. Arm's
  // other triggers are the buffer set changing and a watch the kernel took
  // away, and neither of those fires for a directory that merely appears.
  std::chrono::milliseconds retry_delay_{1000};
  std::optional<std::chrono::steady_clock::time_point> retry_at_;
  // Saves waiting for their own event, oldest first. Empty on every turn koi
  // did not just write, which is nearly all of them.
  std::vector<SelfWrite> self_writes_;
  std::chrono::milliseconds self_write_window_{2000};
};

// The parts of the event loop's auto-reload turn that are arithmetic rather
// than i/o. Here rather than in koi.cpp because nothing links koi.cpp: a
// predicate that quits the editor when it is wrong belongs where a test can
// reach it.

// The ceiling the config already holds the debounce to, repeated because a
// clock that jumped backwards is the one way a window can come out silly.
inline constexpr std::int64_t kMaxDebounceMs = 2000;

// The debounce as arithmetic instead of as a wait. `pending` is the deadline in
// hand, `drained` is whether the watcher just said something of koi's moved.
//
// Set on the first event of a burst and deliberately not pushed back by the
// ones behind it. Extending on every event is the usual shape of a debounce and
// it starves here: a writer touching watched files faster than the window -- a
// big checkout, an agent rewriting a tree -- would move the deadline ahead of
// the clock for as long as it ran, and the reload this feature exists for would
// never happen. A fixed window still coalesces the burst and bounds the wait at
// debounce-ms.
std::optional<std::chrono::steady_clock::time_point> NextDiskDeadline(
    std::optional<std::chrono::steady_clock::time_point> pending, bool drained,
    std::chrono::steady_clock::time_point now, std::int64_t debounce_ms);

// Cannot come out of WatchSignature, so it means "re-arm whatever happens".
inline constexpr std::size_t kNoWatchSignature = 0;

// FNV-1a over the id and the path of every file-backed buffer, in buffer order.
// Compared once per loop turn so that Arm -- the only part of this that
// canonicalises paths or calls into the kernel -- runs when the buffer set
// actually changes rather than on a timer.
std::size_t WatchSignature(const Editor& ed);

// termbox spells "not a whole event yet" three ways: TB_ERR_NO_EVENT, plus
// TB_ERR_NEED_MORE for a half-arrived escape sequence and bare TB_ERR for a
// utf-8 sequence cut by the read that carried it. Neither partial is a failure
// and neither may be returned as one -- the loop's outer handler treats an
// unrecognised code as fatal, so either would quit koi over a character that
// arrived in two reads. tb_poll_event hides both inside its own wait loop; a
// wait taken over from it has to hide them too.
bool IncompleteTermboxRead(int rv);

}  // namespace koi

#endif  // KOI_WATCH_H_
