#define TB_IMPL
#define TB_OPT_ATTR_W 64
#define TB_OPT_EGC
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#include <termbox2.h>
#pragma GCC diagnostic pop
#undef TB_IMPL

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <clocale>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <chrono>
#include <optional>
#include <system_error>
#include <tuple>
#include <string>
#include <string_view>
#include <vector>

#include "anchor.h"
#include "args.h"
#include "cli.h"
#include "commands.h"
#include "crash.h"
#include "editor.h"
#include "jumplist.h"
#include "keylog.h"
#include "navigate.h"
#include "project.h"
#include "render.h"
#include "keymap.h"
#include "printx.hpp"
#include "search.h"
#include "shell.h"
#include "syntax.h"
#include "theme.h"
#include "unicode.h"
#include "watch.h"

namespace koi {

static_assert(kAttrBold == TB_BOLD);
static_assert(kAttrUnderline == TB_UNDERLINE);
static_assert(kAttrReverse == TB_REVERSE);
static_assert(kAttrItalic == TB_ITALIC);
static_assert(kAttrBlink == TB_BLINK);
static_assert(kAttrHiBlack == TB_HI_BLACK);
static_assert(kAttrDim == TB_DIM);
static_assert(kAttrStrikeout == TB_STRIKEOUT);
static_assert(kAttrDefault == TB_DEFAULT);

namespace {

void RegisterCrashBuffers(Editor& ed) {
  static std::vector<CrashDoc> docs;
  docs.clear();
  const std::size_t buffers = BufferCount(ed);
  if (buffers == 0) {
    docs.push_back(CrashDoc{&ed.doc.table, &ed.doc.modified,
                            HasDiskFile(ed.doc) ? ed.doc.file.string() : ed.doc.view_name});
  } else {
    for (std::size_t i = 0; i < buffers; ++i) {
      const Document& doc = BufferAt(ed, i);
      docs.push_back(CrashDoc{&doc.table, &doc.modified,
                              HasDiskFile(doc) ? doc.file.string() : doc.view_name});
    }
  }
  SetCrashDocuments(docs);
}

bool TermboxEnter() {
  if (tb_init() != TB_OK) return false;
  // termbox opens the tty and its resize pipe without O_CLOEXEC, so every
  // `sh -c` a picker scan or a command job forks inherits all three -- a
  // pipeline holding the tty open outlives the editor's idea of it. Blunt but
  // total: koi owns every fd above the standard three at this point, its own
  // opens all set CLOEXEC already, and nothing it spawns needs to inherit
  // anything. Measured here: the store's three, the tty, the pipe's two -- 8 is
  // the highest, so 16 is room to spare.
  constexpr int kMaxInheritedFd = 16;
  for (int fd = 3; fd < kMaxInheritedFd; ++fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
  tb_set_output_mode(TB_OUTPUT_TRUECOLOR);
  tb_set_input_mode(TB_INPUT_ESC | TB_INPUT_MOUSE | TB_INPUT_PASTE | TB_INPUT_FOCUS);
  return true;
}

struct Termbox {
  Termbox() { ok = TermboxEnter(); }
  ~Termbox() {
    if (!ok) return;
    tb_send("\x1b[0 q", 5);
    tb_present();
    tb_shutdown();
  }
  Termbox(const Termbox&) = delete;
  Termbox& operator=(const Termbox&) = delete;
  bool ok{false};
};

Mode g_cursor_shape = Mode::kNormal;
bool g_cursor_shape_sent = false;

void SetCursorShape(Mode mode) {
  if (g_cursor_shape_sent && (mode == g_cursor_shape)) return;
  g_cursor_shape = mode;
  g_cursor_shape_sent = true;
  tb_send((mode == Mode::kInsert) ? "\x1b[6 q" : "\x1b[2 q", 5);
}

void Present(const Surface& frame) {
  tb_clear();
  std::array<uint32_t, 12> cps{};
  for (int y = 0; y < frame.height; ++y) {
    for (int x = 0; x < frame.width; ++x) {
      const Glyph& cell = frame.At(x, y);
      const auto fg = static_cast<uintattr_t>(cell.fg);
      const auto bg = static_cast<uintattr_t>(cell.bg);
      if (cell.text.size() <= 1) {
        tb_set_cell(x, y, cell.text.empty() ? ' ' : static_cast<uint32_t>(cell.text[0]), fg, bg);
        continue;
      }
      std::size_t n = 0;
      std::size_t i = 0;
      while ((i < cell.text.size()) && (n < cps.size())) {
        uint32_t cp = 0;
        const int len = tb_utf8_char_to_unicode(&cp, cell.text.data() + i);
        if (len <= 0) break;
        cps[n++] = cp;
        i += static_cast<std::size_t>(len);
      }
      if (n == 0) {
        tb_set_cell(x, y, ' ', fg, bg);
      } else {
        tb_set_cell_ex(x, y, cps.data(), n, fg, bg);
      }
    }
  }
  SetCursorShape(frame.cursor_insert ? Mode::kInsert : Mode::kNormal);
  if (frame.cursor_visible && (frame.cursor_x >= 0) && (frame.cursor_y >= 0)) {
    tb_set_cursor(frame.cursor_x, frame.cursor_y);
  } else {
    tb_hide_cursor();
  }
  tb_present();
}

void Render(Editor& ed) {
  static Surface frame;
  RenderTo(ed, frame, tb_width(), tb_height());
  Present(frame);
}

bool EventToKey(const tb_event& ev, Key& out) {
  Key key;
  if (ev.mod & TB_MOD_ALT) key.mods |= kModAlt;

  if ((ev.key == 0) && (ev.ch != 0)) {
    if (ev.mod & TB_MOD_CTRL) key.mods |= kModCtrl;
    if (ev.mod & TB_MOD_SHIFT) key.mods |= kModShift;
    key.code = ev.ch;
    out = key;
    return true;
  }

  if (ev.key >= 0x100) {
    if (ev.mod & TB_MOD_CTRL) key.mods |= kModCtrl;
    if (ev.mod & TB_MOD_SHIFT) key.mods |= kModShift;
  }

  switch (ev.key) {
    case TB_KEY_ESC: key.named = NamedKey::kEsc; break;
    case TB_KEY_ENTER: key.named = NamedKey::kRet; break;
    case TB_KEY_TAB: key.named = NamedKey::kTab; break;
    case TB_KEY_BACKSPACE: key.named = NamedKey::kBackspace; break;
    case TB_KEY_SPACE: key.code = ' '; break;
    case TB_KEY_BACKSPACE2: key.named = NamedKey::kBackspace; break;
    case TB_KEY_BACK_TAB:
      key.named = NamedKey::kTab;
      key.mods |= kModShift;
      break;
    case TB_KEY_DELETE: key.named = NamedKey::kDelete; break;
    case TB_KEY_INSERT: key.named = NamedKey::kInsert; break;
    case TB_KEY_ARROW_UP: key.named = NamedKey::kUp; break;
    case TB_KEY_ARROW_DOWN: key.named = NamedKey::kDown; break;
    case TB_KEY_ARROW_LEFT: key.named = NamedKey::kLeft; break;
    case TB_KEY_ARROW_RIGHT: key.named = NamedKey::kRight; break;
    case TB_KEY_HOME: key.named = NamedKey::kHome; break;
    case TB_KEY_END: key.named = NamedKey::kEnd; break;
    case TB_KEY_PGUP: key.named = NamedKey::kPageUp; break;
    case TB_KEY_PGDN: key.named = NamedKey::kPageDown; break;
    default: {
      if ((ev.key >= TB_KEY_F12) && (ev.key <= TB_KEY_F1)) {
        const int n = TB_KEY_F1 - ev.key;
        key.named = static_cast<NamedKey>(static_cast<int>(NamedKey::kF1) + n);
        break;
      }
      if ((ev.key >= TB_KEY_CTRL_A) && (ev.key <= TB_KEY_CTRL_Z)) {
        key.code = static_cast<std::uint32_t>('a' + (ev.key - TB_KEY_CTRL_A));
        key.mods |= kModCtrl;
        break;
      }
      return false;
    }
  }
  out = key;
  return true;
}

void HandleKey(Editor& ed, const KeyMaps& maps, const tb_event& ev, std::vector<Key>& pending) {
  Key key;
  if (!EventToKey(ev, key)) return;
  HandleKeyInput(ed, maps, key, pending);
}

int Run(const char* arg, std::optional<std::string> piped) {
  Editor ed;
  const Target target = (arg != nullptr) ? ParseTarget(arg) : Target{};
  if (const ErrorCtx err = LoadDocument(target.path, ed.doc); err) {
    rostd::printf<"koi: cannot open %s: %s\n">(target.path.c_str(), FormatErrorCtx(err).c_str());
    return 1;
  }
  if (piped) {
    if (!IsWellFormedUtf8(*piped)) {
      rostd::printf<"%s">("koi: stdin is not valid UTF-8\n");
      return 1;
    }
    ResetToOriginal(ed.doc.table, std::move(*piped));
    ed.doc.selections.Set(Selection{});
    ed.doc.selections.EnsureBlockCursors(ed.doc.table);
    ed.doc.modified = true;
  }
  GoToTarget(ed.doc, target);

  {
    // The project store first: the jump list is a view over its `locations`
    // since v4, and without it there is no list. A message with a store behind
    // it is the recycling warning, not a failure: the old database was corrupt
    // and this session is starting from an empty one. Saying nothing there
    // would be the silent data loss the whole gate exists to prevent.
    std::string project_error;
    ed.project = ProjectStore::Open(ProjectDbPath(), project_error);
    if (!project_error.empty()) {
      ed.status.Warn(ed.project ? ("project state " + project_error)
                                : ("project state off: " + project_error));
    }
    std::string jump_error;
    ed.jumps = JumpStore::Open(ed.project, PaneId(), jump_error);
    if (!ed.jumps && project_error.empty()) ed.status.Warn("jump list off: " + jump_error);
  }
  if (!target.has_line) RestoreLastPosition(ed);
  // The first buffer does not come through OpenFile, so its rows are adopted
  // and healed here instead -- otherwise the file koi was started on is the one
  // file whose anchors nothing tracks.
  AdoptAnchorRows(ed, ed.doc);
  StartAnchorHeal(ed, ed.doc.file, {}, false);
  RecordJump(ed);
  RecordVisitHere(ed);

  ed.live_document_changed = [](Editor& e) { RegisterCrashBuffers(e); };

  ed.draw_now = [](Editor& e) {
    FitFocusedViewport(e, tb_width(), tb_height());
    Render(e);
  };

  ed.suspend_terminal = [] {
    tb_shutdown();
    std::fputs("\x1b[2J\x1b[H", stdout);
    std::fflush(stdout);
  };
  ed.resume_terminal = [] {
    if (!TermboxEnter()) {
      std::fputs("koi: could not re-initialise the terminal after the child ran\n", stderr);
      std::exit(1);
    }
    g_cursor_shape_sent = false;
  };

  Termbox tb;
  if (!tb.ok) {
    rostd::printf<"%s">("koi: cannot initialise the terminal\n");
    return 1;
  }

  InstallCrashHandlers([] { tb_shutdown(); });
  RegisterCrashBuffers(ed);

  KeyMaps maps = DefaultKeyMaps();
  std::vector<std::string> config_errors;
  const auto load_config = [&maps, &ed, &config_errors] {
    for (const std::filesystem::path& path : ConfigPaths()) {
      std::ignore = LoadKeyMapConfig(path, maps, ed.settings, config_errors);
    }
  };

  const auto apply_recording = [&maps, &ed, &config_errors] {
    if (!ed.settings.record) {
      ed.recorder.reset();
      return;
    }
    if (!ed.recorder) {
      std::string error;
      ed.recorder = KeyRecorder::Open(KeyLogDbPath(), PaneId(), error);
      if (!ed.recorder) {
        config_errors.push_back("key recording off: " + error);
        return;
      }
      // Opened, but with something to say: the log was corrupt and has been
      // moved aside. config_errors is the channel this lambda already has to
      // the status line, so the warning rides out on it.
      if (!error.empty()) config_errors.push_back("key recording " + error);
    }
    ed.recorder->SetKeyMap(KeyMapFingerprint(maps));
  };

  const auto apply_theme = [&ed, &config_errors] {
    if (ApplyTheme(ed, ed.settings.theme)) return;
    ed.theme = BuiltinTheme();
    RefreshCaptureStyles(ed);
    config_errors.push_back("theme " + ed.settings.theme + " not found; using the built-in one");
  };

  // The file watcher and the little that surrounds it. `watch_signature` is a
  // hash of the file-backed buffers, compared once per turn so that Arm -- the
  // only part of this that canonicalises paths or calls into the kernel -- runs
  // when the buffer set actually changes rather than on a timer.
  FileWatcher watcher;
  std::vector<std::string> watch_paths;
  std::size_t watch_signature = kNoWatchSignature;
  // Set only while a burst of writes is settling; empty the rest of the time,
  // which is what keeps the idle wait a wait with no timeout on it.
  std::optional<std::chrono::steady_clock::time_point> disk_deadline;

  const auto rearm_watch = [&ed, &watcher, &watch_paths] {
    watch_paths.clear();
    for (std::size_t i = 0; i < BufferCount(ed); ++i) {
      const Document& doc = BufferAt(ed, i);
      if (HasDiskFile(doc)) watch_paths.push_back(doc.file.string());
    }
    // Every file-backed buffer, not only the ones a pane is drawing. Buffers
    // share directories, so the extra watches are usually none at all, and it
    // means a split or a buffer switch does not have to re-arm anything --
    // BufferOnScreen is CheckDiskChange's business and stays there.
    watcher.Arm(watch_paths);
  };

  const auto apply_watch = [&ed, &watcher, &watch_signature, &disk_deadline] {
    disk_deadline.reset();
    if (!ed.settings.auto_reload) {
      watcher.Stop();
      return;
    }
    std::ignore = watcher.Start();
    // Whatever was armed belongs to the watcher that has just been stopped and
    // started, or to a config that no longer applies.
    watch_signature = kNoWatchSignature;
  };

  load_config();
  apply_recording();
  apply_watch();
  // After the config, so the pool starts at scan-workers rather than growing
  // to it on the first scan.
  StartScanWorker(ed.settings.scan_workers);
  ed.doc.tab_width = ed.settings.tab_width;
  ed.doc.insert_spaces = ed.settings.insert_spaces;
  DetectIndentation(ed.doc);
  ed.doc.view.scrolloff = ed.settings.scrolloff;

  apply_theme();
  AttachSyntax(ed);

  if (!config_errors.empty()) {
    for (const std::string& problem : config_errors) {
      ed.status.Log(problem, StatusLevel::kWarning);
    }
    ed.status.Warn(std::to_string(config_errors.size()) + " config problem(s): " + config_errors.front());
  }

  {
    std::error_code recover_ec;
    const std::string recover = RecoveryPathFor(ed.doc.file.string());
    if (std::filesystem::exists(recover, recover_ec)) {
      ed.status.Fail("crash recovery file exists: " + recover);
    }
  }

  std::vector<Key> pending;

  constexpr std::size_t kPasteLimit = 32u * 1024u * 1024u;
  std::string paste;
  bool pasting = false;
  bool paste_truncated = false;

  Index drag_anchor = -1;
  int drag_divider = -1;

  std::optional<tb_event> queued;
  auto last_frame = std::chrono::steady_clock::now();
  constexpr auto kMaxFrameGap = std::chrono::milliseconds{100};

  std::size_t watched_active = static_cast<std::size_t>(-1);
  while (!ed.quit) {
    if (HangupRequested()) break;
    // What the turn before this one did: an edit is a boundary, and wherever
    // the cursor has ended up is where the linger clock starts. At the top
    // rather than after the dispatch below because an event leaves this loop by
    // a dozen different `continue`s -- a mouse click moves the caret too.
    NoteCommandBoundary(ed);
    RegisterCrashBuffers(ed);
    if (ed.active != watched_active) {
      watched_active = ed.active;
      MaybeRefreshExcerptView(ed);
    }
    if (!queued) {
      tb_event ahead{};
      if (tb_peek_event(&ahead, 0) == TB_OK) queued = ahead;
    }
    // The picker's scan, drained on every wake there is -- a timed-out poll or
    // an event just handled. What arrived joins the bottom of the band before
    // anything draws.
    PickerPumpScan(ed);
    // Saves this turn's commands made, handed over before anything drains: the
    // rename has already happened and its event is already queued, so the token
    // has to be in place before the read that would otherwise report it.
    for (const std::string& path : ed.self_writes) watcher.ExpectSelfWrite(path);
    ed.self_writes.clear();
    if (watcher.Fd() >= 0) {
      if (const std::size_t signature = WatchSignature(ed);
          (signature != watch_signature) || watcher.NeedsRearm()) {
        watch_signature = signature;
        rearm_watch();
      }
      // A read() that finds EAGAIN, on the turns the idle wait below did not
      // already do this. It is here for the loop's *other* waits -- pasting,
      // chord, draining, scanning, busy -- where termbox owns the wait and the
      // inotify fd is in no poll set at all.
      disk_deadline = NextDiskDeadline(disk_deadline, watcher.Drain(),
                                       std::chrono::steady_clock::now(),
                                       ed.settings.auto_reload_debounce_ms);
    }
    // Outside the guard above on purpose: the problem worth reporting most is
    // inotify_init1 failing, and that leaves Fd() at -1 -- so asking in there
    // would be asking exactly when the answer can never arrive.
    if (std::string problem = watcher.TakeProblem(); !problem.empty()) ed.status.Warn(problem);
    // The debounce, expired. Deliberately not a timer and not a thread: the
    // deadline is spent as the timeout of the wait below, and this is where it
    // is collected. Waiting narrows the window on a writer caught mid-write --
    // truncate-then-write in place is the shape that tears -- but does not
    // close it, and does not need to: a torn read leaves a stamp that does not
    // match what the writer finally leaves behind, so the next event brings us
    // straight back here and the file is read again.
    // Not while the user is halfway through something. A paste arrives as one
    // key event per character, each of them landing here with the buffer still
    // clean -- so a reload underneath it would take the buffer and the rest of
    // the paste would go into different text at an offset that means nothing.
    // Same for the other half-finished gestures: the reload moves the cursor,
    // and an armed find-char, a typed count, a held drag or a chord waiting on
    // its next key are all measured from where the cursor was. Held, not
    // dropped: the deadline stays armed and the first turn after the gesture
    // ends collects it.
    const bool mid_gesture = pasting || (drag_anchor >= 0) || !pending.empty() ||
                             (ed.pending_char != PendingChar::kNone) || (ed.pending_count != 0);
    if (disk_deadline && !mid_gesture && (std::chrono::steady_clock::now() >= *disk_deadline)) {
      disk_deadline.reset();
      CheckDiskChange(ed);
      MaybeRefreshExcerptView(ed);
    }
    const bool busy = PumpCommandJobs(ed, pasting || ed.prompt_active ||
                                              (ed.mode == Mode::kInsert) || queued.has_value() ||
                                              (drag_anchor >= 0) || (drag_divider >= 0));
    const auto now = std::chrono::steady_clock::now();
    const bool starved = (now - last_frame) >= kMaxFrameGap;
    if (!pasting && (!queued || starved)) {
      RefreshLiveExcerptViews(ed);
      FitFocusedViewport(ed, tb_width(), tb_height());
      Render(ed);
      last_frame = now;
    }

    constexpr int kChordTimeoutMs = 400;

    constexpr int kRecordFlushMs = 1500;
    constexpr int kPasteStallMs = 2000;
    tb_event ev{};
    if (queued) {
      ev = *queued;
      queued.reset();
    } else {
      const bool chord = !pending.empty() && (ed.mode == Mode::kInsert);
      const bool draining = ed.recorder && ed.recorder->Buffered();
      // A picker with a child still writing needs the loop to wake with
      // nothing typed, the way a running job does -- that wake is when its
      // rows land. Faster than the job poll because these rows are on screen
      // and a list that fills in visible steps reads as one that is working --
      // and ahead of it in the ternary for the same reason. A job left pending
      // by a prompt would otherwise halve the band's fill rate, and the job
      // pump runs on every wake, so the shorter timeout costs it nothing.
      const bool scanning = !pasting && PickerScanning(ed);
      constexpr int kJobPollMs = 80;
      constexpr int kPickerPollMs = 40;
      const int wait = chord ? kChordTimeoutMs : kRecordFlushMs;

      // The idle branch: nothing typed, nothing running, nothing to redraw.
      // termbox's own wait selects on the tty and its resize pipe and nothing
      // else, so an inotify fd handed to it would sit unread until the user
      // happened to press a key -- which is the entire feature. This is the one
      // branch that is replaced, and it keeps termbox's contract: block with no
      // timeout unless there is a debounce actually pending.
      //
      // Returns TB_OK with `ev` filled, a termbox error, or kLoopAgain for "go
      // round again, nothing happened". kLoopAgain is safe to be `continue`d on
      // because no editor state was touched -- and it must be, since TB_OK
      // with a zeroed event would be dispatched as a key press.
      constexpr int kLoopAgain = 1;  // TB_OK is 0 and every termbox error is negative.
      const auto idle_wait = [&]() -> int {
        int ttyfd = -1;
        int resizefd = -1;
        if ((watcher.Fd() < 0) || (tb_get_fds(&ttyfd, &resizefd) != TB_OK)) {
          return tb_poll_event(&ev);
        }
        // Not an optimisation -- correctness. termbox reads the tty in blocks
        // and parses events out of its own buffer, so bytes it has already
        // taken off the fd leave nothing for poll() to see: block first and koi
        // would sit there with an event in hand. Asked once, here, and again
        // below only when the tty actually became readable -- the wake that
        // matters for idle cost is the one from inotify, and that one has no
        // reason to go near termbox at all.
        if (const int ready = tb_peek_event(&ev, 0); !IncompleteTermboxRead(ready)) return ready;
        for (;;) {
          struct pollfd fds[3] = {{.fd = ttyfd, .events = POLLIN, .revents = 0},
                                  {.fd = resizefd, .events = POLLIN, .revents = 0},
                                  {.fd = watcher.Fd(), .events = POLLIN, .revents = 0}};
          int timeout = -1;
          if (disk_deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  *disk_deadline - std::chrono::steady_clock::now())
                                  .count();
            // Clamped for the same reason NextDiskDeadline clamps: a clock that
            // jumped is the one way `left` comes out silly.
            timeout = static_cast<int>(std::clamp<std::int64_t>(left, 0, kMaxDebounceMs));
          }
          const int ready = ::poll(fds, 3, timeout);
          if (ready < 0) {
            if (errno == EINTR) return kLoopAgain;  // the top of the loop re-checks the hangup
            // A poll set this one cannot poll is not something to retry in a
            // loop that would then spin. Drop the watcher and finish this wait
            // the way koi waited before there was one -- and say so, because
            // the fallback lasts for the rest of the run and a feature that
            // stops working without a word reads as one that never worked.
            // Read before close(), which is free to leave its own errno here.
            const std::string why{std::strerror(errno)};
            watcher.Stop();
            disk_deadline.reset();
            ed.status.Warn("auto-reload off: " + why + " -- :config-reload to try again");
            return tb_poll_event(&ev);
          }
          if (ready == 0) return kLoopAgain;  // the debounce expired; the top of the loop sweeps
          if ((fds[0].revents | fds[1].revents) != 0) {
            // Readable is not the same as decodable: a half-arrived escape
            // sequence gives termbox nothing to return, and falling through
            // with a zeroed `ev` would dispatch a key nobody pressed.
            const int got = tb_peek_event(&ev, 0);
            if (!IncompleteTermboxRead(got)) return got;
            // The partial codes join NO_EVENT here: all of them mean "not a
            // whole event yet", and all are answered by going back to poll. The
            // bytes that came are in termbox's buffer now, so the tty is no
            // longer readable and this cannot spin.
            continue;
          }
          // Only the watcher fired. inotify has no name filter in the kernel,
          // so this is every entry that moved in a directory holding one of
          // koi's files -- object files, .git churn, an agent's temporaries.
          // Drain says whether any of it was ours. When it was not, go back to
          // poll from right here rather than returning: a turn of the outer
          // loop would repaint the whole screen, and under a running build that
          // is a continuous redraw of a frame not one cell of which changed.
          disk_deadline = NextDiskDeadline(disk_deadline, watcher.Drain(),
                                           std::chrono::steady_clock::now(),
                                           ed.settings.auto_reload_debounce_ms);
        }
      };

      // An armed debounce bounds a wait that is only a polling interval: the
      // sweep runs between waits, so a fixed timeout collects it that late and
      // the reload lands past the quiet time the config promises. Only for the
      // waits whose expiry means nothing on its own -- the paste stall, the
      // chord timeout and the record flush above expire *into* an action, and
      // cutting one short would apply a paste or end a chord early. Those are
      // the gestures the sweep is held for anyway.
      const auto capped = [&disk_deadline, mid_gesture](int ms) {
        // A deadline the gesture guard is holding is nothing to hurry for --
        // shortening the wait for it would poll at the 1ms floor until the
        // gesture ends.
        if (!disk_deadline || mid_gesture) return ms;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              *disk_deadline - std::chrono::steady_clock::now())
                              .count();
        return static_cast<int>(std::clamp<std::int64_t>(left, 1, ms));
      };
      const int rc = pasting               ? tb_peek_event(&ev, kPasteStallMs)
                     : (chord || draining) ? tb_peek_event(&ev, wait)
                     : scanning            ? tb_peek_event(&ev, capped(kPickerPollMs))
                     : busy                ? tb_peek_event(&ev, capped(kJobPollMs))
                                           : idle_wait();
      if (rc == kLoopAgain) continue;
      // Reachable from every branch above, and never an error: termbox says
      // these when it is holding the front of an event and wants the rest of it
      // -- an escape sequence for NEED_MORE, a multi-byte character for TB_ERR.
      // NO_EVENT stays out of it because the timeout branches below read that
      // one as their deadline expiring. The break is for codes that mean the
      // terminal is gone; falling into it here quit the editor over a keystroke
      // that arrived in two reads.
      if ((rc == TB_ERR_NEED_MORE) || (rc == TB_ERR)) continue;
      if (rc != TB_OK) {
        if (HangupRequested()) break;
        if (rc == TB_ERR_NO_EVENT) {
          if (pasting) {
            pasting = false;
            ApplyPaste(ed, paste);
            ed.status.Warn("paste never ended -- applied what arrived");
            paste.clear();
            paste.shrink_to_fit();
            continue;
          }
          if (chord) {
            if (ed.recorder) ed.recorder->NoteChordTimeout(ed, pending);
            FlushPendingAsText(ed, pending);
            ed.status.clear();
            continue;
          }
          if (draining) {
            ed.recorder->Flush();
            continue;
          }
          // Nothing typed, which is the turn the scan is polled for: the pump
          // at the top of the loop is what the wake was for.
          if (busy || scanning) continue;
          break;
        }
        if ((rc == TB_ERR_POLL) && (tb_last_errno() == EINTR)) continue;
        break;
      }
    }

    const auto drop_pending_input = [&ed, &pending] {
      if (ed.mode == Mode::kInsert) FlushPendingAsText(ed, pending);
      if (!pending.empty() || (ed.pending_char != PendingChar::kNone) ||
          (ed.pending_count != 0)) {
        ed.status.clear();
      }
      pending.clear();
      ed.pending_char = PendingChar::kNone;
      // A click moves the caret and a wheel moves the view, and a leap left
      // armed across either one would go on offering labels measured from
      // somewhere the user has already walked away from.
      ed.leap = {};
      ed.pending_count = 0;
    };

    // An event arrived, so the sitting-still that came before it is over and
    // its length is known. This is the only clock the linger rule gets: the
    // poll above blocks with no timeout when there is nothing to do.
    NoteInputBoundary(ed);

    if (ev.type == TB_EVENT_PASTE_BEGIN) {
      drop_pending_input();
      paste.clear();
      paste_truncated = false;
      pasting = true;
      continue;
    }
    if (ev.type == TB_EVENT_PASTE_END) {
      pasting = false;
      ApplyPaste(ed, paste);
      if (paste_truncated) {
        ed.status.Warn("paste truncated at " + std::to_string(kPasteLimit / (1024 * 1024)) + " MiB");
      }
      paste.clear();
      paste.shrink_to_fit();
      continue;
    }
    if (pasting) {
      if ((ev.type == TB_EVENT_KEY) && (ev.ch != 0)) {
        if (paste.size() < kPasteLimit) {
          paste += KeyText(Key{.code = ev.ch});
        } else {
          paste_truncated = true;
        }
      }
      continue;
    }
    if (ev.type == TB_EVENT_FOCUS_IN) {
      // Note what this does and what the watcher path deliberately does not:
      // dropping pending input is right here, because the user's attention
      // actually left the terminal and a half-typed chord is stale. A file
      // written behind koi's back is not that -- eating an armed find-char
      // because a formatter ran would be the feature making the editor worse.
      drop_pending_input();
      // The sweep below is the one the debounce was going to ask for.
      disk_deadline.reset();
      CheckDiskChange(ed);
      MaybeRefreshExcerptView(ed);
      continue;
    }
    if (ev.type == TB_EVENT_FOCUS_OUT) {

      if (ed.recorder) ed.recorder->Flush();
      continue;
    }

    if ((ev.type == TB_EVENT_KEY) && (ev.key == TB_KEY_ESC) && (ev.ch == 0)) {
      tb_event follow{};
      if (tb_peek_event(&follow, 0) == TB_OK) {
        if ((follow.type != TB_EVENT_KEY) ||
            ((follow.key == TB_KEY_ESC) && (follow.ch == 0))) {
          queued = follow;
        } else if (follow.ch == '[') {
          std::string seq;
          bool complete = false;
          for (int i = 0; i < 64; ++i) {
            tb_event next{};
            if (tb_peek_event(&next, 25) != TB_OK) break;
            if (next.type != TB_EVENT_KEY) break;
            if ((next.ch >= 0x40) && (next.ch <= 0x7E)) {
              seq += static_cast<char>(next.ch);
              complete = true;
              break;
            }
            if ((next.ch < 0x20) || (next.ch > 0x3F)) break;
            seq += static_cast<char>(next.ch);
          }
          Key decoded;
          if (!complete || !DecodeCsiKey(seq, decoded)) continue;
          tb_event synth{};
          synth.type = TB_EVENT_KEY;
          if (decoded.named != NamedKey::kNone) {
            synth.key = (decoded.named == NamedKey::kEsc)   ? TB_KEY_ESC
                        : (decoded.named == NamedKey::kRet) ? TB_KEY_ENTER
                        : (decoded.named == NamedKey::kTab) ? TB_KEY_TAB
                                                            : TB_KEY_BACKSPACE2;
          } else {
            synth.ch = decoded.code;
          }
          if (decoded.mods & kModAlt) synth.mod |= TB_MOD_ALT;
          if (decoded.mods & kModCtrl) synth.mod |= TB_MOD_CTRL;
          if (decoded.mods & kModShift) synth.mod |= TB_MOD_SHIFT;
          ev = synth;
        } else {
          follow.mod |= TB_MOD_ALT;
          ev = follow;
        }
      }
    }
    if (ev.type == TB_EVENT_RESIZE) {
      HandleResize(ed);
      continue;
    }
    if (ev.type == TB_EVENT_MOUSE) {
      if (ed.prompt_active) continue;
      // Before the click or the wheel is looked at: either one moves the
      // caret or the view, and a capture armed by a key does not survive it.
      drop_pending_input();
      ed.status.clear();
      ed.jump_branch = false;

      const bool motion = (ev.mod & TB_MOD_MOTION) != 0;

      if ((drag_divider >= 0) && motion && (ev.key == TB_KEY_MOUSE_LEFT)) {
        DragDivider(ed, drag_divider, ev.x, ev.y, tb_width(), tb_height());
        continue;
      }
      if (ev.key == TB_KEY_MOUSE_RELEASE) {
        drag_anchor = -1;
        drag_divider = -1;
        continue;
      }
      if (!motion && (ev.key == TB_KEY_MOUSE_LEFT)) {
        drag_divider = DividerAtPoint(ed, ev.x, ev.y, tb_width(), tb_height());
        if (drag_divider >= 0) {
          drag_anchor = -1;
          continue;
        }
      }

      Rect area{};
      const int under = WindowAtPoint(ed, ev.x, ev.y, tb_width(), tb_height(), area);
      const bool dragging = (drag_anchor >= 0) && motion;

      if ((under >= 0) && !dragging) FocusWindowAt(ed, under);
      const Rect content = PaneContent(ed, area, tb_width());
      const int text_rows = std::max(1, content.h - 1);
      const int local_y = (under >= 0) ? (ev.y - content.y) : -1;
      const int click_y = dragging ? std::clamp(local_y, 0, text_rows - 1) : local_y;
      if ((ev.key == TB_KEY_MOUSE_LEFT) && (click_y >= 0) && (click_y < text_rows)) {
        const int gutter = GutterWidth(ed, content.w);
        const Index at = PositionAtScreen(ed, WrapOf(ed, gutter, content.w), gutter,
                                          ev.x - content.x, click_y);
        if (dragging) {
          // MinWidth1, not a bare caret: the press selected the grapheme it
          // landed on, and PutCursor only re-holds an anchor grapheme that is
          // actually held -- a zero-width anchor would drop the pressed
          // grapheme out of every leftward drag.
          ed.doc.selections.Set(PutCursor(
              ed.doc.table, MinWidth1(ed.doc.table, Selection{drag_anchor, drag_anchor, -1}), at,
              true));
        } else {
          drag_anchor = at;
          ed.doc.selections.Set(Selection{at, at, -1});
        }
        ApplyModeInvariants(ed);
      }
      if (ev.key == TB_KEY_MOUSE_WHEEL_DOWN) {
        RunCommands(ed, {"scroll_down", "scroll_down", "scroll_down"});
      } else if (ev.key == TB_KEY_MOUSE_WHEEL_UP) {
        RunCommands(ed, {"scroll_up", "scroll_up", "scroll_up"});
      }
      continue;
    }
    if (ev.type != TB_EVENT_KEY) continue;

    const std::string previous_status = ed.status;
    ed.status.clear();
    HandleKey(ed, maps, ev, pending);
    if (ed.status.empty() && !pending.empty()) ed.status = previous_status;

    if (ed.reload_config) {
      ed.reload_config = false;
      maps = DefaultKeyMaps();
      config_errors.clear();
      ed.settings = Settings{};
      load_config();
      // Grow-only: a raised scan-workers applies now, a lowered one on the
      // next start (see EnsureScanWorker for why shrinking is not done live).
      StartScanWorker(ed.settings.scan_workers);
      ed.doc.tab_width = ed.settings.tab_width;
      ed.doc.insert_spaces = ed.settings.insert_spaces;
      DetectIndentation(ed.doc);

      apply_theme();
      apply_recording();
      // auto-reload takes effect here rather than at the next start: the
      // watcher is stopped or started outright, and the next turn re-arms it.
      apply_watch();
      if (config_errors.empty()) {
        ed.status = "reloaded " + DisplayPath(ConfigPath());
      } else {
        for (const std::string& problem : config_errors) {
          ed.status.Log(problem, StatusLevel::kWarning);
        }
        ed.status.Warn(std::to_string(config_errors.size()) + " config problem(s): " +
                       config_errors.front());
      }
    }
    FitFocusedViewport(ed, tb_width(), tb_height());
  }
  ShutdownEditor(ed);
  return 0;
}

}
}

int main(int argc, char** argv) {
  std::setlocale(LC_CTYPE, "");
  koi::PutSelfOnPath();
  const common::Args args(argc, argv);
  constexpr std::string_view kCliHelpMessage = R"CLI(
Usage: koi [file[:line[:col]]]
       command | koi

  no file      a new, unnamed buffer -- :w <path> to name it
  piped stdin  becomes an unnamed buffer; keys still come from the terminal

Non-interactive modes. Each writes to stdout, edits nothing, and never opens a
terminal, so they compose in a pipeline and can back a picker's preview window.

  --render-mode [file]        highlighted text, for a preview window
      --language <lang>       override the language; inferred from the file's
                              extension otherwise, and the only way to say what
                              text arriving on stdin is
      --line-range <a>:<b>    only these lines; either end may be left out
      --highlight-line <n>    mark that line -- a band behind it normally, or
                              the excerpt-match colour under --no-syntax
      --line-numbers          add a gutter
      --no-syntax             skip the grammar; the theme, the gutter and
                              --highlight-line still work. Costs a millisecond
                              or two instead of up to a quarter second, which is
                              what a preview that redraws per keystroke needs
      --no-color              plain text

  --symbol-mode --files <path>...   ( `--files -` reads them from stdin )
      --definitions           where names are introduced
      --references            where they are used
                              prints path:line:col:name, one per line, which is
                              a thing koi can be handed straight back
      --containing <word>     only captures with that whole word in them
      --hot-first             the rows this project's database ranks highest
                              first, then the rest of the scan in file order.
                              Nothing is buffered but that head
      --from <path>           the file --hot-first scores against
      --hot-limit <n>         how many of the database's top symbols name a
                              file for the head (default 200)
      --picker-rows           the editor's own picker rows -- the name, the
                              file and line dimmed, the payload hidden behind
                              padding -- rather than the bare row

  --overview --files <path>...
      --filter <file>         call names to leave out, one per line
                              prints includes, classes with their members and
                              methods, and per-function call graphs
)CLI";

  if (args.Has("-h") || args.Has("--help")) {
    rostd::printf<"%s">(kCliHelpMessage);
    return EXIT_SUCCESS;
  }
  if (args.Has("--version")) {
    rostd::printf<"%s">(PROJECT_VERSION); // defined in root CMakeLists.txt
    return EXIT_SUCCESS;
  }
  if (args.Has("--render-mode")) return koi::RunRenderMode(argc, argv);
  if (args.Has("--symbol-mode")) return koi::RunSymbolMode(argc, argv);
  if (args.Has("--overview")) return koi::RunOverviewMode(argc, argv);

  const char* file = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (argv[i][0] != '-') {
      file = argv[i];
      break;
    }
  }

  std::optional<std::string> piped;
  if ((file == nullptr) && (isatty(STDIN_FILENO) == 0)) {
    std::string text((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
    piped = std::move(text);
  }
  return koi::Run(file, std::move(piped));
}
