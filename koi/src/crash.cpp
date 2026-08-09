#include "crash.h"

#include <fcntl.h>
#include <unistd.h>

#include <csignal>
#include <cstring>
#include <exception>
#include <filesystem>

namespace koi {
namespace {

constexpr int kMaxCrashDocs = 32;

struct Slot {
  const PieceTable* table{nullptr};
  const bool* modified{nullptr};
  bool dumped{false};
  bool partial{false};
  char recover_path[2048]{};
};

// Two lists, swapped by a single write to g_live.
//
// SetCrashDocuments runs once per event-loop iteration, so a fatal signal
// landing while it is halfway through is not a remote case -- it is most of the
// time. A handler reading a half-rebuilt list sees the new table pointer beside
// the previous entry's recover_path, and dumps one buffer's text into another
// buffer's recovery file; or it sees a count from before a buffer closed and
// walks a PieceTable that has since been destroyed, which faults inside the
// fatal handler and loses every unsaved buffer rather than saving them. Filling
// the list the handler is not looking at, then publishing it with one
// sig_atomic_t store, leaves no torn state to observe.
Slot g_docs[2][kMaxCrashDocs];
volatile std::sig_atomic_t g_doc_count[2] = {0, 0};
volatile std::sig_atomic_t g_live = 0;
void (*g_restore_terminal)() = nullptr;
volatile std::sig_atomic_t g_hangup = 0;

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

const char* SignalName(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    default: return "signal";
  }
}

void WriteRaw(int fd, const char* s, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t n = write(fd, s + done, len - done);
    if (n > 0) {
      done += static_cast<size_t>(n);
    } else if ((n == -1) && (errno == EINTR)) {
      continue;
    } else {
      return;
    }
  }
}

void WriteRaw(int fd, const char* s) { WriteRaw(fd, s, std::strlen(s)); }

bool DumpTo(int fd, const PieceTable& table) {
  Index steps = 0;
  const Index limit = (table.tree.Bytes() + 1) * 2;
  for (pt::Cursor cur{table.tree, 0}; cur.Valid(); ) {
    if (++steps > limit) return false;
    const pt::Piece& p = cur.CurrentPiece();
    const char* base = nullptr;
    Index size = 0;
    if (p.from_original == 0) {
      base = table.modified.data();
      size = static_cast<Index>(table.modified.size());
    } else if (table.original_mapped != nullptr) {
      base = table.original_mapped;
      size = table.original_mapped_size;
    } else {
      base = table.original.data();
      size = static_cast<Index>(table.original.size());
    }
    const Index from = static_cast<Index>(p.src_start);
    const Index len = static_cast<Index>(p.length);
    if ((base == nullptr) || (from < 0) || (len < 0) || (from + len > size)) return false;
    WriteRaw(fd, base + from, static_cast<size_t>(len));
    if (!cur.Next()) break;
  }
  return true;
}

int WriteRecoveryFilesRaw() {
  const int live = static_cast<int>(g_live);
  const int count = static_cast<int>(g_doc_count[live]);
  int written = 0;
  for (int i = 0; i < count; ++i) {
    Slot& s = g_docs[live][i];
    s.dumped = false;
    s.partial = false;
    if ((s.table == nullptr) || (s.modified == nullptr) || !*s.modified) continue;
    if (s.recover_path[0] == '\0') continue;
    const int fd = open(s.recover_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd == -1) continue;
    const bool ok = DumpTo(fd, *s.table);
    fsync(fd);
    close(fd);
    if (ok) {
      s.dumped = true;
      ++written;
    } else {
      // DumpTo gives up on a piece that does not point where it should, which
      // is exactly the state a crash tends to arrive in. Whatever reached the
      // file is kept -- most of a buffer beats none of it -- but it is short,
      // and saying so is the difference between the user reading it as their
      // work and reading it as a fragment.
      s.partial = true;
    }
  }
  return written;
}

void ReportDumped(int fd) {
  const int live = static_cast<int>(g_live);
  const int count = static_cast<int>(g_doc_count[live]);
  for (int i = 0; i < count; ++i) {
    const Slot& s = g_docs[live][i];
    if (s.dumped) {
      WriteRaw(fd, "koi: unsaved buffer written to ");
    } else if (s.partial) {
      WriteRaw(fd, "koi: INCOMPLETE unsaved buffer written to ");
    } else {
      continue;
    }
    WriteRaw(fd, s.recover_path);
    WriteRaw(fd, "\n");
  }
}

void FatalHandler(int sig) {
  for (const int s : kFatalSignals) std::signal(s, SIG_DFL);
  WriteRecoveryFilesRaw();
  if (g_restore_terminal != nullptr) g_restore_terminal();
  WriteRaw(STDERR_FILENO, "\nkoi: fatal ");
  WriteRaw(STDERR_FILENO, SignalName(sig));
  WriteRaw(STDERR_FILENO, "\n");
  ReportDumped(STDERR_FILENO);
  raise(sig);
}

[[noreturn]] void TerminateHandler() {
  std::signal(SIGABRT, SIG_DFL);
  WriteRecoveryFilesRaw();
  if (g_restore_terminal != nullptr) g_restore_terminal();
  WriteRaw(STDERR_FILENO, "\nkoi: unhandled exception");
  if (std::current_exception()) {
    try {
      throw;
    } catch (const std::exception& e) {
      WriteRaw(STDERR_FILENO, ": ");
      WriteRaw(STDERR_FILENO, e.what());
    } catch (...) {
    }
  }
  WriteRaw(STDERR_FILENO, "\n");
  ReportDumped(STDERR_FILENO);
  std::abort();
}

void HangupHandler(int) { g_hangup = 1; }

}

std::string RecoveryPathFor(const std::string& path) {
  if (path.empty()) return "koi-unnamed.koi-recover";
  const std::filesystem::path p{path};
  return (p.parent_path() / ("." + p.filename().string() + ".koi-recover")).string();
}

void SetCrashDocuments(std::span<const CrashDoc> docs) {
  // Build into the list the handler is not reading, then publish.
  const int next = (g_live == 0) ? 1 : 0;
  int n = 0;
  for (const CrashDoc& doc : docs) {
    if (n >= kMaxCrashDocs) break;
    Slot& s = g_docs[next][n];
    s.table = doc.table;
    s.modified = doc.modified;
    const std::string recover = (doc.path.empty() && (n > 0))
                                    ? ("koi-unnamed-" + std::to_string(n) + ".koi-recover")
                                    : RecoveryPathFor(doc.path);
    if (recover.size() < sizeof(s.recover_path)) {
      std::memcpy(s.recover_path, recover.c_str(), recover.size() + 1);
    } else {
      s.recover_path[0] = '\0';
    }
    ++n;
  }
  g_doc_count[next] = n;
  g_live = next;
}

void SetCrashDocument(const PieceTable* table, const bool* modified, const std::string& path) {
  const CrashDoc doc{table, modified, path};
  SetCrashDocuments(std::span{&doc, 1});
}

void InstallCrashHandlers(void (*restore_terminal)()) {
  g_restore_terminal = restore_terminal;
  for (const int s : kFatalSignals) std::signal(s, FatalHandler);
  std::set_terminate(TerminateHandler);

  struct sigaction graceful{};
  graceful.sa_handler = HangupHandler;
  sigemptyset(&graceful.sa_mask);
  graceful.sa_flags = 0;
  sigaction(SIGHUP, &graceful, nullptr);
  sigaction(SIGTERM, &graceful, nullptr);
}

bool HangupRequested() { return g_hangup != 0; }

bool WriteRecoveryFile() { return WriteRecoveryFilesRaw() > 0; }

}
