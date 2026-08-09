#include "sidebar.h"

#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>

#include "printx.hpp"

#include "project.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kReset = "\033[0m";
constexpr std::string_view kBold = "\033[1m";
constexpr std::string_view kDim = "\033[2m";
constexpr std::string_view kYellow = "\033[33m";
constexpr std::string_view kCyan = "\033[36m";
constexpr std::string_view kMagenta = "\033[35m";

constexpr int kMaxLineDigits = 5;

int Width(std::string_view text) { return static_cast<int>(DisplayWidth(text, 0)); }

std::string Basename(std::string_view path) { return fs::path{path}.filename().string(); }

struct Row {
  bool present{false};
  // Never carries escapes: RenderSection runs it through AppendPrintable, which
  // would turn any ESC in here -- ours as readily as a file name's -- into a
  // control picture. Emphasis is a flag so the escapes can be written outside
  // the filtered payload.
  std::string text;
  std::string detail;
  bool emphasise{false};
};

void RenderSection(std::string_view title, std::string_view labels, const std::vector<Row>& rows,
                   int columns, std::string_view colour, std::vector<std::string>& out) {
  std::string head = "── ";
  head += title;
  head += ' ';
  std::string rule{kDim};
  rule += head;
  for (int i = Width(head); i < columns; ++i) rule += "─";
  rule += kReset;
  out.push_back(std::move(rule));

  for (size_t i = 0; i < labels.size(); ++i) {
    const std::string label{labels.substr(i, 1)};
    if ((i >= rows.size()) || !rows[i].present) {
      out.push_back(" " + std::string{kDim} + label + std::string{kReset} + "  " +
                    std::string{kDim} + "—" + std::string{kReset});
      continue;
    }
    // The row text is a file name or a symbol taken from the project database,
    // which comes from whatever the file-filter command found -- so it is
    // repository-controlled, and a name carrying an escape sequence would drive
    // the terminal the sidebar is drawing into.
    std::string line = " " + std::string{colour} + label + std::string{kReset} + "  ";
    if (rows[i].emphasise) line += kBold;
    AppendPrintable(line, rows[i].text);
    if (rows[i].emphasise) line += kReset;
    line += kDim;
    AppendPrintable(line, rows[i].detail);
    line += kReset;
    out.push_back(std::move(line));
  }
}

Row NameAndLine(std::string_view path, Index line, int width, bool emphasise) {
  Row row;
  row.present = true;
  row.text = TruncateToWidth(Basename(path), width);
  row.emphasise = emphasise;
  row.detail = ":" + std::to_string(line);
  return row;
}

std::vector<Row> PinRows(const std::vector<Pin>& pins, std::string_view current, int columns) {
  const int width = std::max(4, columns - 5 - kMaxLineDigits);
  std::vector<Row> rows;
  rows.reserve(pins.size());
  for (const Pin& pin : pins) {
    if (pin.path.empty()) {
      rows.emplace_back();
      continue;
    }
    rows.push_back(NameAndLine(pin.path, pin.line, width, pin.path == current));
  }
  return rows;
}

std::vector<Row> TrailRows(const std::vector<FileVisit>& trail, int columns) {
  const int width = std::max(4, columns - 5 - kMaxLineDigits);
  std::vector<Row> rows;
  for (size_t i = 0; (i < trail.size()) && (i < static_cast<size_t>(kTrailSlots)); ++i) {
    rows.push_back(NameAndLine(trail[i].path, trail[i].line, width, false));
  }
  return rows;
}

std::vector<Row> SymbolRowsFor(const std::vector<SymbolVisit>& hot, int columns) {
  const int budget = std::max(10, columns - 6 - kMaxLineDigits);
  const int name_width = std::max(6, (budget * 3) / 5);
  const int file_width = std::max(4, budget - name_width);

  std::vector<Row> rows;
  rows.reserve(hot.size());
  for (const SymbolVisit& visit : hot) {
    Row row;
    row.present = true;
    const std::string name = TruncateToWidth(visit.symbol, name_width);
    row.text = name + std::string(static_cast<size_t>(std::max(0, name_width - Width(name))), ' ') +
               " ";
    row.detail =
        TruncateToWidth(Basename(visit.file), file_width) + ":" + std::to_string(visit.line);
    rows.push_back(std::move(row));
  }
  return rows;
}

struct Size {
  int columns{40};
  int rows{24};
  friend bool operator==(const Size&, const Size&) = default;
};

Size TerminalSize() {
  winsize ws{};
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) && (ws.ws_col > 0) && (ws.ws_row > 0)) {
    return Size{ws.ws_col, ws.ws_row};
  }
  return Size{};
}

volatile std::sig_atomic_t g_stop = 0;

void RequestStop(int) { g_stop = 1; }

void Write(std::string_view text) {
  std::fwrite(text.data(), 1, text.size(), stdout);
}

void ShowCursor() {
  Write("\033[?25h");
  std::fflush(stdout);
}

}

std::string TruncateToWidth(std::string_view text, int width) {
  if (width <= 0) return {};
  if (Width(text) <= width) return std::string{text};
  if (width == 1) return "…";

  const int budget = width - 1;
  int used = 0;
  size_t at = 0;
  while (at < text.size()) {
    const size_t next = NextGraphemeInString(text, at);
    if (next <= at) break;
    // DisplayWidth from the column reached so far, not GraphemeWidth: the two
    // disagree about tab, which GraphemeWidth deliberately calls zero because
    // its width is positional. Measuring the cut with it made every tab free,
    // so a tab-bearing name was judged too wide by the check above and then ran
    // the loop to the end anyway -- and came back wider than the width asked
    // for, with an ellipsis appended to the whole of it. This is the same
    // accounting Width() uses, so `used` is exactly the width of the prefix and
    // the result is `used` + 1 columns at most.
    const int w = static_cast<int>(DisplayWidth(text.substr(at, next - at), 0, used));
    if (used + w > budget) break;
    used += w;
    at = next;
  }
  return std::string{text.substr(0, at)} + "…";
}

std::vector<std::string> SidebarLines(ProjectStore& store, int columns) {
  columns = std::max(20, columns);
  const std::vector<Pin> pins = store.Pins();
  // TrailRows draws at most kTrailSlots of them, and this runs on every refresh
  // -- once a second when there is no inotify to wait on.
  const Trail trail = TrailOf(store, kTrailSlots);

  std::vector<std::string> lines;
  RenderSection("Pins", kPinLabels, PinRows(pins, trail.current, columns), columns, kYellow, lines);
  lines.emplace_back();
  RenderSection("Trail", kTrailLabels, TrailRows(trail.entries, columns), columns, kCyan, lines);
  lines.emplace_back();
  RenderSection("Symbols", kSymbolLabels, SymbolRowsFor(store.HotSymbols(kHotSymbolSlots), columns),
                columns, kMagenta, lines);
  return lines;
}

int RunSidebar() {
  std::string error;
  const std::shared_ptr<ProjectStore> store = ProjectStore::Open(ProjectDbPath(), error);
  if (store == nullptr) {
    rostd::fprintf<"koi --sidebar: %s\n">(stderr, error.c_str());
    return 1;
  }

  std::signal(SIGTERM, RequestStop);
  std::signal(SIGHUP, RequestStop);
  std::signal(SIGINT, RequestStop);

  const fs::path dir = ProjectDbPath().parent_path();
  const int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  int watch = -1;
  if (notify >= 0) {
    watch = inotify_add_watch(notify, dir.c_str(),
                              IN_MODIFY | IN_CREATE | IN_MOVED_TO | IN_CLOSE_WRITE);
  }
  const bool watching = (notify >= 0) && (watch >= 0);

  Write("\033[?25l\033[2J");
  Size size = TerminalSize();
  bool dirty = true;

  while (g_stop == 0) {
    if (dirty) {
      const std::vector<std::string> lines = SidebarLines(*store, size.columns);
      std::string frame = "\033[H";
      const size_t drawn = std::min(lines.size(), static_cast<size_t>(std::max(0, size.rows)));
      for (size_t i = 0; i < drawn; ++i) {
        frame += lines[i];
        frame += "\033[0K";
        if (i + 1 < static_cast<size_t>(size.rows)) frame += "\r\n";
      }
      frame += "\033[0J";
      Write(frame);
      std::fflush(stdout);
      dirty = false;
    }

    pollfd fds{notify, POLLIN, 0};
    const int ready = poll(&fds, (notify >= 0) ? 1 : 0, 1000);
    if ((ready < 0) && (errno != EINTR)) break;

    if ((ready > 0) && ((fds.revents & POLLIN) != 0)) {
      usleep(150 * 1000);
      alignas(alignof(inotify_event)) char buffer[4096];
      while (read(notify, buffer, sizeof(buffer)) > 0) {
      }
      dirty = true;
    }

    if (const Size now = TerminalSize(); now != size) {
      size = now;
      dirty = true;
    }
    if (!watching) dirty = true;
  }

  if (watch >= 0) inotify_rm_watch(notify, watch);
  if (notify >= 0) close(notify);
  ShowCursor();
  return 0;
}

}
