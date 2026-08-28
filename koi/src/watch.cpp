#include "watch.h"

// Declarations only -- TB_IMPL stays in koi.cpp, which owns the one definition
// of termbox. The option macros match that translation unit so the prototypes
// this file sees are the prototypes it links against.
#define TB_OPT_ATTR_W 64
#define TB_OPT_EGC
#include <termbox2.h>

#include <limits.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#include "editor.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

// Watched on the *directory*, and the mask is chosen for what a directory watch
// can see rather than what a file watch would have said.
//
//   IN_CLOSE_WRITE  a writer that wrote in place, and finished. IN_MODIFY is
//                   the obvious alternative and the wrong one: it fires once
//                   per write() call, so a large file is hundreds of events
//                   every one of which lands mid-write. CLOSE_WRITE is one
//                   event, at the moment the bytes are all there.
//   IN_MOVED_TO     rename-into-place, which is the case this whole file is
//                   shaped around -- see Arm.
//   IN_MOVED_FROM   the first half of a swap, and "your file just left".
//   IN_CREATE       a path a buffer names but that does not exist yet, brought
//                   into being: `koi newfile.md` while something generates it,
//                   or a checkout putting a deleted file back.
//   IN_DELETE       the same in reverse; the buffer keeps its text and the
//                   sweep finds nothing to stat, but koi should look.
//   IN_ATTRIB       `touch`, a chmod that makes the buffer read-only, and the
//                   link-count change a hard-linked rewrite shows up as.
//   IN_DELETE_SELF  the watched directory itself went. Arrives with no name,
//                   so the basename filter below must let nameless events by.
//   IN_MOVE_SELF    likewise, renamed rather than removed.
//   IN_ONLYDIR      not an event: refuse to add the watch at all if the path is
//                   not a directory. Without it a parent replaced by a regular
//                   file between the canonicalisation and the add would be
//                   watched as a file, which is the failure mode this design
//                   exists to avoid.
//
// Deliberately absent: IN_OPEN, IN_ACCESS and IN_MODIFY, each of which turns an
// ordinary build into a wake per syscall for no information koi does not
// already get from CLOSE_WRITE and MOVED_TO. The cost of leaving IN_MODIFY out
// is a writer that holds its fd open forever and never closes it -- that one is
// invisible until the next focus-in, which is where koi was before this file.
constexpr std::uint32_t kMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
                                IN_DELETE | IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;

// Splitting a file path into the directory to watch and the basename to filter
// on -- and the reason it is the directory and not the file.
//
// inotify watches an inode, not a name. koi's own AtomicWriteFile writes a temp
// file beside the target and renames it into place, and so do clang-format,
// gofmt, `git checkout`, and every coding agent that wants a write to be
// all-or-nothing. Every one of those replaces the inode. A watch added on the
// file therefore fires exactly once and then silently follows the orphaned old
// inode forever, reporting nothing while the file is rewritten under it -- no
// error, no IN_IGNORED, just a watch on bytes nobody can reach any more. The
// containing directory has a stable inode across all of that, and it is where
// IN_MOVED_TO arrives carrying the basename. Do not "simplify" this back to a
// watch on the file.
//
// Symlinks. `path` is canonicalised whole, so a buffer on `link.txt ->
// ../real/thing.txt` is watched as `real/` + `thing.txt` -- which is right,
// because that is the inode a writer rewrites and the one ExternallyModified
// stats through to. The symlink's own directory is added as a second entry when
// it differs, so re-pointing the link is caught too; the pair collapses to one
// entry for the ordinary case where the path is not a link at all.
void DirAndName(const std::string& path, std::vector<std::pair<std::string, std::string>>& out) {
  const fs::path resolved = CanonicalOf(fs::path{path});
  if (resolved.has_parent_path() && !resolved.filename().empty()) {
    out.emplace_back(resolved.parent_path().native(), resolved.filename().native());
  }
  std::error_code ec;
  const fs::path literal = fs::absolute(fs::path{path}, ec);
  if (ec || !literal.has_parent_path() || literal.filename().empty()) return;
  // The parent through CanonicalOf as well, so a symlinked *directory* does not
  // become a second map entry for the one inode inotify would hand back a
  // single watch descriptor for.
  std::pair<std::string, std::string> spelled{CanonicalOf(literal.parent_path()).native(),
                                              literal.filename().native()};
  if (out.empty() || (out.front() != spelled)) out.push_back(std::move(spelled));
}

}  // namespace

FileWatcher::~FileWatcher() { Stop(); }

void FileWatcher::Note(const std::string& what) {
  if (problem_spent_ || !problem_.empty()) return;
  problem_ = what;
}

std::string FileWatcher::TakeProblem() {
  if (problem_.empty()) return {};
  problem_spent_ = true;
  return std::exchange(problem_, std::string{});
}

bool FileWatcher::Start() {
  if (fd_ >= 0) return true;
  fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0) {
    Note("auto-reload off: " + std::string{std::strerror(errno)} +
         " -- files changed on disk are picked up on focus instead");
    return false;
  }
  return true;
}

void FileWatcher::Stop() {
  // Closing the instance drops every watch on it, which is why no rm_watch loop
  // is needed here -- and why one would be wrong after a failed init.
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  dirs_.clear();
  by_wd_.clear();
  self_writes_.clear();
  rearm_ = false;
  retry_at_.reset();
  // The one line is one line per watcher, not one per process. `:config-reload`
  // and toggling auto-reload both come through here, and a failure the user has
  // already been told about once -- and may have gone and fixed -- has to be
  // able to say so again on the other side.
  problem_.clear();
  problem_spent_ = false;
}

bool FileWatcher::NeedsRearm() const {
  if (rearm_) return true;
  return retry_at_.has_value() && (std::chrono::steady_clock::now() >= *retry_at_);
}

void FileWatcher::DropOwner(int wd, Dir* dir) {
  auto held = by_wd_.find(wd);
  if (held == by_wd_.end()) return;
  std::vector<Dir*>& owners = held->second;
  std::erase(owners, dir);
  if (!owners.empty()) return;
  // Last one out. Only now is the descriptor nobody's, and only now may it go
  // back -- an rm_watch while another spelling still held it would have taken a
  // live watch away and dropped whatever was queued on it.
  by_wd_.erase(held);
  ::inotify_rm_watch(fd_, wd);
}

void FileWatcher::ExpectSelfWrite(const std::string& path) {
  // Nothing is watching, so nothing will report the write, so there is nothing
  // to mute -- and a token left here would be the one that outlives its window.
  if ((fd_ < 0) || path.empty()) return;
  std::vector<std::pair<std::string, std::string>> pairs;
  DirAndName(path, pairs);
  if (pairs.empty()) return;
  const auto now = std::chrono::steady_clock::now();
  // The window swept here as well as at match time: this is the only place the
  // list grows, so pruning on the way in is what bounds it for a koi whose
  // saves never land in a watched directory at all.
  std::erase_if(self_writes_,
                [&](const SelfWrite& token) { return (now - token.at) > self_write_window_; });
  // The front pair only. AtomicWriteFile resolves the path down to the inode it
  // writes, so the canonical spelling is where the rename lands; the literal
  // one DirAndName also yields is the symlink's own directory, which this save
  // never touches.
  self_writes_.push_back(SelfWrite{pairs.front().first, pairs.front().second, now});
}

bool FileWatcher::TakeSelfWrite(int wd, std::string_view name) {
  if (self_writes_.empty()) return false;
  const auto now = std::chrono::steady_clock::now();
  bool took = false;
  for (auto it = self_writes_.begin(); it != self_writes_.end();) {
    if ((now - it->at) > self_write_window_) {
      it = self_writes_.erase(it);
      continue;
    }
    // Resolved through dirs_ rather than compared against a path the event does
    // not carry: the descriptor is what says which directory this is, and going
    // by it means every spelling sharing one descriptor spends the token.
    if (!took && (it->name == name)) {
      if (const auto dir = dirs_.find(it->dir); (dir != dirs_.end()) && (dir->second.wd == wd)) {
        it = self_writes_.erase(it);
        took = true;
        continue;
      }
    }
    ++it;
  }
  return took;
}

void FileWatcher::Arm(const std::vector<std::string>& paths) {
  rearm_ = false;
  retry_at_.reset();
  if (fd_ < 0) return;

  std::unordered_map<std::string, NameSet> wanted;
  std::vector<std::pair<std::string, std::string>> pairs;
  for (const std::string& path : paths) {
    if (path.empty()) continue;
    pairs.clear();
    DirAndName(path, pairs);
    for (auto& [dir, name] : pairs) wanted[dir].insert(std::move(name));
  }

  for (auto it = dirs_.begin(); it != dirs_.end();) {
    if (wanted.contains(it->first)) {
      ++it;
      continue;
    }
    if (it->second.wd >= 0) DropOwner(it->second.wd, &it->second);
    it = dirs_.erase(it);
  }

  for (auto& [dir, names] : wanted) {
    Dir& held = dirs_[dir];
    held.names = std::move(names);
    // Unconditional, including for a directory that already has a watch on it:
    // adding a watch for an inode this instance already watches returns the
    // same descriptor and does no more than update the mask, so the call is
    // also the only way to find out whether a descriptor koi still holds is
    // live. A watch destroyed while the queue was overflowing left no
    // IN_IGNORED behind to say it went, and a held-but-dead descriptor is a
    // directory nothing will ever report again.
    const int wd = ::inotify_add_watch(fd_, dir.c_str(), kMask);
    if (wd < 0) {
      // ENOSPC is the interesting one -- max_user_watches, which is a system
      // limit the user can raise and would want to know about. ENOENT and
      // ENOTDIR are ordinary: a buffer on a path whose directory is not there.
      if (errno == ENOSPC) {
        Note("auto-reload is out of inotify watches -- raise "
             "fs.inotify.max_user_watches");
      }
      if (held.wd >= 0) {
        // Handed back the same way as below: the descriptor may be another
        // entry's too, and it is not this one's alone to give up.
        DropOwner(held.wd, &held);
        held.wd = -1;
      }
      // The entry stays, at wd == -1, and the deadline is what brings Arm back
      // to it: a directory that is not there yet is a normal thing to have a
      // buffer under, and nothing else would ever ask again.
      retry_at_ = std::chrono::steady_clock::now() + retry_delay_;
      continue;
    }
    if (held.wd == wd) continue;
    // Two spellings of one directory share the single descriptor inotify hands
    // back per inode, so this only lets go of this entry's hold on the old one.
    if (held.wd >= 0) DropOwner(held.wd, &held);
    held.wd = wd;
    // Appended, never assigned: the entry already on this descriptor is the
    // other spelling of the same directory and its names still have to be
    // matched against.
    by_wd_[wd].push_back(&held);
  }
}

std::vector<int> FileWatcher::WatchDescriptors() const {
  std::vector<int> out;
  out.reserve(by_wd_.size());
  for (const auto& [wd, dir] : by_wd_) out.push_back(wd);
  std::ranges::sort(out);
  return out;
}

bool FileWatcher::Drain() {
  if (fd_ < 0) return false;

  // alignas because the events come back packed and the first one starts at the
  // front of this buffer: read into a plain char[] and every struct field is a
  // misaligned load, which is a fault on some architectures, a sanitizer report
  // here, and works by luck the rest of the time. Sized well past one maximal
  // event so a directory churning under a build is drained in a few reads
  // rather than dozens.
  alignas(struct inotify_event) char buf[8192];
  static_assert(sizeof(buf) >= sizeof(struct inotify_event) + NAME_MAX + 1);

  bool interesting = false;
  for (;;) {
    const ssize_t n = ::read(fd_, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      // EAGAIN: drained. Anything else is a broken fd, and the caller finding
      // out by getting nothing is the same degradation as the watcher being off.
      break;
    }
    if (n == 0) break;

    const char* p = buf;
    const char* const end = buf + n;
    while ((p + sizeof(struct inotify_event)) <= end) {
      // Walked by each event's own len, not by a fixed stride: the name is a
      // variable-length tail and len is the only thing that says where the next
      // event starts.
      const auto* ev = reinterpret_cast<const struct inotify_event*>(p);
      const char* const name_at = p + sizeof(struct inotify_event);
      const std::uint32_t len = ev->len;
      if ((name_at + len) > end) break;
      p = name_at + len;

      // wd == -1, and the kernel dropped events it could not queue. There is no
      // way to know which, so the answer is "all of them" -- including, since
      // IN_IGNORED queues like anything else and is dropped like anything else,
      // the news that a watch died. The re-arm is what revalidates the
      // descriptors that survived the burst on paper only.
      if ((ev->mask & IN_Q_OVERFLOW) != 0) {
        rearm_ = true;
        interesting = true;
        continue;
      }
      auto held = by_wd_.find(ev->wd);
      if (held == by_wd_.end()) continue;
      if ((ev->mask & IN_IGNORED) != 0) {
        // The watch is gone: directory removed, renamed away, or unmounted. Let
        // go of the descriptor -- the kernel will hand the number out again --
        // and ask for a re-arm, which puts the watch back if the directory
        // comes back.
        // Every spelling: the inode went, so it went for all of them.
        for (Dir* owner : held->second) owner->wd = -1;
        by_wd_.erase(held);
        rearm_ = true;
        interesting = true;
        continue;
      }
      // IN_MOVE_SELF: the watched directory itself was renamed. The kernel
      // keeps the watch across it and sends no IN_IGNORED, ever -- the watch
      // follows the inode to its new path -- so an entry left as it is would be
      // deaf at the path koi asked for and loud about every matching basename
      // written under the new one. Let it go the way an IN_IGNORED does and ask
      // for the re-arm, which puts a watch back on the old path if something
      // recreates it. IN_DELETE_SELF rides along because it is the same
      // invalidation arriving a moment before the kernel's own IN_IGNORED,
      // which then finds no descriptor and is dropped by the lookup above.
      if ((ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF)) != 0) {
        ::inotify_rm_watch(fd_, ev->wd);
        for (Dir* owner : held->second) owner->wd = -1;
        by_wd_.erase(held);
        rearm_ = true;
        interesting = true;
        continue;
      }
      // Nameless events -- the directory itself moved or was deleted -- carry
      // no basename to filter on, so they always count.
      if (len == 0) {
        interesting = true;
        continue;
      }
      // The kernel NUL-pads the name out to len; strnlen finds the real end
      // without walking past the buffer. string_view over those bytes, compared
      // against a transparent set: no allocation on the path taken by every
      // build artefact that happens to share a directory with a buffer.
      const std::string_view name{name_at, ::strnlen(name_at, len)};
      bool ours = false;
      for (const Dir* owner : held->second) {
        if (owner->names.contains(name)) {
          ours = true;
          break;
        }
      }
      // koi's own save, already accounted for: the token is spent and the event
      // goes no further. Only the one event -- the next write of this name with
      // no token behind it is news again.
      if (ours && !TakeSelfWrite(ev->wd, name)) interesting = true;
    }
  }
  return interesting;
}

std::optional<std::chrono::steady_clock::time_point> NextDiskDeadline(
    std::optional<std::chrono::steady_clock::time_point> pending, bool drained,
    std::chrono::steady_clock::time_point now, std::int64_t debounce_ms) {
  // Already armed wins over a fresh event: that, and only that, is what keeps a
  // burst from pushing its own deadline out of reach.
  if (!drained || pending.has_value()) return pending;
  return now + std::chrono::milliseconds{std::clamp<std::int64_t>(debounce_ms, 0, kMaxDebounceMs)};
}

std::size_t WatchSignature(const Editor& ed) {
  std::size_t hash = 1469598103934665603ull;
  const auto mix = [&hash](std::size_t value) { hash = (hash ^ value) * 1099511628211ull; };
  for (std::size_t i = 0; i < BufferCount(ed); ++i) {
    const Document& doc = BufferAt(ed, i);
    if (!HasDiskFile(doc)) continue;
    mix(static_cast<std::size_t>(doc.id));
    // native(), not string(): the latter is a copy of every path on every turn,
    // for a hash that throws it away again.
    mix(std::hash<std::string_view>{}(doc.file.native()));
  }
  return (hash == kNoWatchSignature) ? (kNoWatchSignature + 1) : hash;
}

bool IncompleteTermboxRead(int rv) {
  return (rv == TB_ERR_NO_EVENT) || (rv == TB_ERR_NEED_MORE) || (rv == TB_ERR);
}

}  // namespace koi
