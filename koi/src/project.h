#ifndef KOI_PROJECT_H_
#define KOI_PROJECT_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "piece_doc.h"
#include "symbols.h"

namespace koi {

inline constexpr int kPinSlots = 4;
inline constexpr int kTrailSlots = 5;
inline constexpr int kHotSymbolSlots = 7;
inline constexpr std::string_view kPinLabels = "1234";
inline constexpr std::string_view kTrailLabels = "56789";
inline constexpr std::string_view kSymbolLabels = "ijklaeo";

struct FileVisit {
  std::string path;
  Index line{1};
  Index column{0};
  double last_ts{0};
};

struct SymbolVisit {
  std::string symbol;
  std::string file;
  Index line{0};
};

struct Pin {
  std::string path;
  Index line{1};
  Index column{0};
};

struct ProjectStore {
  // Null means there is no store and `error` says why. Non-null with a
  // NON-EMPTY `error` is a warning, not a failure: the database on disk was
  // corrupt, has been renamed to `<db>.corrupt`, and this store is a fresh
  // empty one. Callers that only branch on null keep working; callers with a
  // status line should show it.
  static std::shared_ptr<ProjectStore> Open(const std::filesystem::path& db, std::string& error);

  virtual ~ProjectStore() = default;

  virtual void RecordVisit(std::string_view path, Index line, Index column) = 0;

  virtual void RecordEdit(std::string_view path, Index line, Index column) = 0;

  virtual void RecordSymbolVisit(std::string_view symbol, std::string_view file, Index line) = 0;

  virtual void RecordCoVisit(std::string_view from_file, std::string_view to_file) = 0;

  // The `want` most recently visited files, newest first. `want <= 0` asks for
  // every row -- which is what this used to do unconditionally, and what made a
  // once-per-second sidebar refresh a full table scan plus one stat() per row
  // to draw five lines. A caller that shows n rows asks for n.
  //
  // A row whose file is no longer on disk is skipped, so the answer can be
  // shorter than `want` even when the table is longer.
  virtual std::vector<FileVisit> RecentFiles(int want) = 0;

  virtual bool LastVisit(std::string_view path, Index& line, Index& column) = 0;

  // The `want` files with the highest frecency (visits and edits, decayed by
  // how long ago the file was last touched). Same `want` rules as RecentFiles.
  //
  // The one caller in the editor passes 0 on purpose: the file picker ranks
  // every path the file filter produced, so a cap there would not shorten a
  // list, it would silently drop files out of frecency order and back into
  // find(1) order.
  virtual std::vector<FileVisit> FrecentFiles(int want) = 0;

  virtual std::vector<SymbolVisit> HotSymbols(int limit) = 0;

  virtual size_t RankSymbols(std::vector<Symbol>& rows, std::string_view current_file) = 0;

  virtual std::vector<std::string> HotFiles(int limit, std::string_view current_file) = 0;

  virtual std::vector<Pin> Pins() = 0;

  virtual void SetPin(int slot, std::string_view file, Index line, Index column) = 0;

  virtual void ClearPin(int slot) = 0;

  virtual int FileCount() = 0;
};

struct Trail {
  std::string current;
  std::vector<FileVisit> entries;
};

// The most recent file, and up to `entries` files behind it. Pass what will be
// displayed: the trail is read on every sidebar refresh, and the count is what
// stops that from reading the whole visit history. `entries <= 0` means all of
// it.
Trail TrailOf(ProjectStore& store, int entries);

inline constexpr int kDefaultHotFileLimit = 200;

// The project-root rule itself: walk up from `from` looking for a `.git` or
// `.ronin` marker, never going above `stop` (koi's $HOME), and fall back to
// `from` when there is no marker to find. An empty `stop` means "walk to the
// filesystem root".
//
// Exposed because ProjectRoot() answers from a process-lifetime cache -- the
// walk runs once, on the first call, and nothing can make it run again -- so
// this is the only door the rule can be tested through.
std::filesystem::path FindProjectRoot(const std::filesystem::path& from,
                                      const std::filesystem::path& stop);

std::filesystem::path ProjectRoot();

// A path read back out of the project database, turned into a path valid from
// koi's current directory -- which is what everything that opens, stats or
// compares one expects. Stored paths are keyed against the project root, so the
// two agree only when koi was started there.
//
// The root overload is for a loop over rows: deriving it once is the same
// saving the row reads inside the store make. Apply this exactly once, where
// store data leaves the store; a relative result resolves against the current
// directory, so putting one back through would move it again.
std::string ResolveStorePath(const std::filesystem::path& root, std::string_view key);
std::string ResolveStorePath(std::string_view key);

std::filesystem::path ProjectDbPath();

void SetProjectDbPath(std::filesystem::path path);

void SetProjectRoot(std::filesystem::path path);

std::string FlattenPathComponent(std::string_view path);

std::string ProjectDirName(const std::filesystem::path& project);

std::filesystem::path LastPickerStatePath();

std::filesystem::path KeyLogDbPath();

std::filesystem::path SidebarPanePath();

}

#endif
