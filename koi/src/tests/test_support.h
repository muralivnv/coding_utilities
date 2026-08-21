// Fixtures, invariants and small helpers shared by more than one of the test
// files next to this one. Anything used by a single file lives in that file's
// own anonymous namespace instead.
#ifndef KOI_TESTS_TEST_SUPPORT_H_
#define KOI_TESTS_TEST_SUPPORT_H_

#include <algorithm>
#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <random>
#include <set>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "commands.h"
#include "render.h"
#include "crash.h"
#include "jumplist.h"
#include "keylog.h"
#include "navigate.h"
#include "project.h"
#include "query.h"
#include "regex.h"
#include "search.h"
#include "cli.h"
#include "symbols.h"
#include "syntax.h"
#include "textobject.h"
#include "theme.h"
#include "editor.h"
#include "keymap.h"
#include "piece_doc.h"
#include "selection.h"
#include "shell.h"
#include "sqlite.h"
#include "thread_pool.h"
#include "test_harness.h"
#include "unicode.h"

namespace vw = std::views;

namespace koi {

std::filesystem::path TempFixture(std::string_view name);

void RemoveAllQuietly(const std::filesystem::path& p);

void WriteFixtureFile(const std::filesystem::path& path, std::string_view contents);

std::string AssembleDocContents(const PieceTable& table);

void ExpectSameDoc(const PieceTable& table, const std::string& want, std::string_view context);

void ExpectOk(const ErrorCtx& err, std::string_view context);

PieceTable MakeTable(std::string text);

struct Rng {
  std::mt19937_64 gen;
  unsigned long long seed;
  explicit Rng(unsigned long long s);
  Index Pick(Index lo, Index hi);
};

ErrorCtx InsertFunctor(std::string_view txt, Interval doc_range, PieceTable& table,
                       std::string& brute_force);

ErrorCtx DeleteFunctor([[maybe_unused]] std::string_view txt, Interval doc_range, PieceTable& table,
                       std::string& brute_force);

constexpr std::string_view kEAcute = "e\xCC\x81";

constexpr std::string_view kCJK = "\xE6\x97\xA5";

constexpr std::string_view kFamily =
    "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7";

Index Cur(const PieceTable& table, const SelectionSet& sel);

Index Cur(const Editor& ed);

Index RangeTo(const SelectionSet& sel, std::size_t i);

struct LanguageSample {
  std::string_view language;
  std::string_view filename;
};

constexpr std::array kLanguageSamples{
    LanguageSample{"bash", "deploy.sh"},
    LanguageSample{"c", "main.c"},
    LanguageSample{"cmake", "CMakeLists.txt"},
    LanguageSample{"cpp", "main.cpp"},
    LanguageSample{"css", "site.css"},
    LanguageSample{"dart", "main.dart"},
    LanguageSample{"diff", "changes.diff"},
    LanguageSample{"go", "main.go"},
    LanguageSample{"html", "index.html"},
    LanguageSample{"javascript", "app.js"},
    LanguageSample{"json", "package.json"},
    LanguageSample{"make", "Makefile"},
    LanguageSample{"markdown", "README.md"},
    LanguageSample{"nix", "flake.nix"},
    LanguageSample{"python", "main.py"},
    LanguageSample{"rust", "lib.rs"},
    LanguageSample{"toml", "Cargo.toml"},
    LanguageSample{"tsx", "app.tsx"},
    LanguageSample{"typescript", "app.ts"},
    LanguageSample{"yaml", "ci.yaml"},
};

struct Scratch {
  std::filesystem::path dir;
  // Keyed on the pid. With a fixed name two runs share one directory, and the
  // first to finish removes the other's fixtures out from under it -- which
  // does not read as a collision, it reads as unrelated tests failing all over
  // the suite for no reason.
  explicit Scratch(std::string_view name);
  ~Scratch();
  Scratch(const Scratch&) = delete;
  Scratch& operator=(const Scratch&) = delete;
  std::filesystem::path Write(std::string_view name, std::string_view contents) const;
};

Key K(std::string_view text);

void TypeInto(Editor& ed, char c);

std::string UndoChainInvariants(const PieceTable& table);

std::string EditorInvariants(const Editor& ed);

std::string NumberedLines(int count);

void PumpUntilIdle(Editor& ed);

// A queries/<language>/ directory under the runtime root koi looks in first --
// $HOME/.config/ronin/koi, and $HOME here is this run's private one. What it
// puts there shadows what koi ships for the same language, which is the only
// way to hand the scanner a query it would never ship: one that piles up
// matches, or one that does not compile at all.
struct FakeQueryDir {
  std::filesystem::path dir;
  explicit FakeQueryDir(std::string_view language);
  ~FakeQueryDir();
  FakeQueryDir(const FakeQueryDir&) = delete;
  FakeQueryDir& operator=(const FakeQueryDir&) = delete;
  bool Ready() const;
  void Write(std::string_view file, std::string_view source) const;
  // Takes the whole directory away, so that neither the fs::exists probes nor
  // the read that follows them can succeed. What the scanner says afterwards is
  // therefore what it remembered, not what it re-derived.
  void Forget() const;
};

void OnAThreadOfItsOwn(const std::function<void()>& body);

std::string PilingUpQuery(int patterns, std::string_view first, std::string_view second);

std::string WideCalls(int arguments, int calls, bool spread);

// Milliseconds a body took, so that "did not hang" is an assertion and not a
// hope. The bounds below are loose on purpose: what they separate is a run that
// stopped at its budget from one that never stops at all, and the two differ by
// more than an order of magnitude.
template <typename Body>
long long MillisecondsOf(const Body& body) {
  const auto started = std::chrono::steady_clock::now();
  body();
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               started)
      .count();
}

struct Presser {
  KeyMaps maps{DefaultKeyMaps()};
  std::vector<Key> pending;
  void operator()(Editor& ed, std::string_view key);
};

}  // namespace koi

#endif  // KOI_TESTS_TEST_SUPPORT_H_
