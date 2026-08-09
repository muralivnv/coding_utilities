#include "cli.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "args.h"
#include "editor.h"
#include "keymap.h"
#include "printx.hpp"
#include "project.h"
#include "query.h"
#include "syntax.h"
#include "theme.h"
#include "unicode.h"

namespace koi {
namespace {

namespace fs = std::filesystem;

void AppendColor(std::string& out, const Color& color, bool foreground) {
  if (!color.set) return;
  out += foreground ? ";38;2;" : ";48;2;";
  const unsigned rgb = color.rgb;
  out += std::to_string((rgb >> 16) & 0xFFu);
  out += ';';
  out += std::to_string((rgb >> 8) & 0xFFu);
  out += ';';
  out += std::to_string(rgb & 0xFFu);
}

std::string Sgr(const Style& style) {
  std::string out = "\x1b[0";
  if ((style.mods & kModBold) != 0) out += ";1";
  if ((style.mods & kModDim) != 0) out += ";2";
  if ((style.mods & kModItalic) != 0) out += ";3";
  if ((style.mods & kModUnderlined) != 0) out += ";4";
  if ((style.mods & kModSlowBlink) != 0) out += ";5";
  if ((style.mods & kModRapidBlink) != 0) out += ";6";
  if ((style.mods & kModReversed) != 0) out += ";7";
  if ((style.mods & kModHidden) != 0) out += ";8";
  if ((style.mods & kModCrossedOut) != 0) out += ";9";
  AppendColor(out, style.fg, true);
  AppendColor(out, style.bg, false);
  out += 'm';
  return out;
}

bool SameStyle(const Style& a, const Style& b) {
  return (a.mods == b.mods) && (a.fg.set == b.fg.set) && (a.bg.set == b.bg.set) &&
         (!a.fg.set || (a.fg.rgb == b.fg.rgb)) && (!a.bg.set || (a.bg.rgb == b.bg.rgb));
}

Theme ThemeForCli() {
  Settings settings;
  KeyMaps maps;
  std::vector<std::string> errors;
  for (const fs::path& config : ConfigPaths()) {
    std::ignore = LoadKeyMapConfig(config, maps, settings, errors);
  }
  Theme theme;
  std::string error;
  if (!LoadTheme(settings.theme, theme, error)) return BuiltinTheme();
  return theme;
}

}

std::string RenderToAnsi(std::string_view text, const RenderOptions& options, std::string& error) {
  error.clear();
  PieceTable table;
  ResetToMapped(table, text, nullptr);

  const Index doc_len = DocLength(table);
  Index lines = LineCount(table);
  if (doc_len == 0) {
    lines = 0;
  } else if (ReadDocRange(table, Interval(doc_len - 1, doc_len)) == "\n") {
    --lines;
  }
  const Index first = std::max<Index>(1, options.first_line);
  const Index last = (options.last_line > 0) ? std::min(options.last_line, lines) : lines;

  const Index paint_from = (lines > 0) ? LineStart(table, first - 1) : 0;
  const Index paint_to = (last >= LineCount(table)) ? doc_len : LineStart(table, last);

  std::vector<CaptureId> captures;
  std::vector<Style> capture_styles;
  Theme theme;
  if (options.color && options.syntax && !options.language.empty()) {
    theme = ThemeForCli();
    if (std::shared_ptr<Syntax> syntax = OpenSyntaxForLanguage(options.language, error)) {
      syntax->Sync(table);
      syntax->Paint(table, Interval(paint_from, paint_to), captures);
      for (const std::string& name : syntax->CaptureNames()) {
        capture_styles.push_back(theme.Get(name));
      }
    }
  } else if (options.color && (options.highlight_line > 0)) {
    theme = ThemeForCli();
  }
  // Two different jobs, so two different keys. With syntax on, the marked line
  // is one line among many that are already coloured, and it is told apart by
  // the band behind it -- `ui.cursorline.primary`, the same as in the editor.
  // With syntax off there is nothing else on the screen wearing a colour, so
  // the line marks itself in the foreground, in `ui.excerpt.match`: the colour
  // an excerpt view gives the thing that was searched for. A preview then reads
  // the way an excerpt view does, which is the point of it.
  const Style cursorline =
      (options.highlight_line > 0)
          ? theme.Get(options.syntax ? "ui.cursorline.primary" : "ui.excerpt.match")
          : Style{};

  const int number_width = static_cast<int>(std::to_string(std::max<Index>(1, last)).size());

  std::string out;
  out.reserve(text.size() + (text.size() / 4));
  for (Index line = first; line <= last; ++line) {
    const bool banded = options.color && (line == options.highlight_line);
    const Style band = banded ? cursorline : Style{};
    const Interval content = LineContentRange(table, line - 1);
    const Index begin = content.empty() ? LineStart(table, line - 1) : *content.begin();
    const Index end = begin + static_cast<Index>(content.size());

    Style current;
    bool wrote_style = false;
    const auto set_style = [&](const Style& style) {
      if (!options.color || SameStyle(style, current)) return;
      out += Sgr(style);
      current = style;
      wrote_style = true;
    };

    if (options.line_numbers) {
      const std::string number = std::to_string(line);
      set_style(banded ? theme.Get("ui.linenr.selected").Over(band) : theme.Get("ui.linenr"));
      out.append(static_cast<size_t>(std::max<int>(0, number_width - std::ssize(number))), ' ');
      out += number;
      out += ' ';
    }

    const auto style_at = [&](Index byte) {
      const Index rel = byte - paint_from;
      if ((rel >= 0) && (rel < std::ssize(captures))) {
        const CaptureId id = captures[static_cast<size_t>(rel)];
        if ((id != kNoCapture) && ((id - 1) < std::ssize(capture_styles))) {
          return capture_styles[static_cast<size_t>(id - 1)].Over(band);
        }
      }
      return band;
    };

    std::string chunk;
    std::string carried;
    for (Index at = begin; at < end;) {
      const Style style = style_at(at);
      Index run = at + 1;
      while ((run < end) && SameStyle(style, style_at(run))) ++run;
      set_style(style);
      // Filtered even under --no-color: this is what backs the picker's preview
      // window, so it runs over whatever file the cursor happens to be on, and
      // raw document bytes on stdout are escape sequences the terminal obeys.
      //
      // A style run ends wherever the capture ids stop agreeing, which is not
      // promised to be a codepoint boundary, so the filter is told there is
      // more coming and its leftover goes in front of the next run's bytes.
      ReadDocRangeInto(table, Interval(at, run), chunk);
      if (!carried.empty()) chunk.insert(0, carried);
      const std::size_t held = AppendPrintable(out, chunk, /*more=*/run < end);
      carried.assign(chunk, chunk.size() - held, held);
      at = run;
    }

    if (banded) {
      set_style(band);
      out += "\x1b[K";
    }
    if (wrote_style) out += "\x1b[0m";
    out += '\n';
  }
  return out;
}

bool WriteSymbols(std::span<const std::string> paths, const SymbolModeOptions& options,
                  std::FILE* out, std::string& error) {
  error.clear();

  constexpr size_t kBatch = 64 * 1024;
  std::string batch;
  batch.reserve(kBatch + 1024);
  const auto flush = [&] {
    if (batch.empty()) return;
    std::fwrite(batch.data(), 1, batch.size(), out);
    batch.clear();
  };
  const auto emit = [&](const Symbol& symbol) {
    // A picker row is refused outright when the path holds a tab or a newline:
    // the row would come back naming a different file. Skipped rather than
    // written as a blank line -- these rows are somebody's picker input.
    const std::string row =
        options.picker_rows ? SymbolPickerRow(symbol) : FormatSymbolRow(symbol);
    if (row.empty()) return;
    batch += row;
    batch += '\n';
    if (batch.size() >= kBatch) flush();
  };

  std::unordered_set<std::string> hot_paths;
  std::unordered_map<std::string, int> written;
  std::string head_error;
  if (options.order == SymbolOrder::kHotFirst) {
    std::string db_error;
    const std::shared_ptr<ProjectStore> store = ProjectStore::Open(ProjectDbPath(), db_error);
    std::vector<std::string> hot = (store == nullptr)
                                       ? std::vector<std::string>{}
                                       : store->HotFiles(options.hot_limit, options.from);
    if (!hot.empty()) {
      // Keyed against the project root by the store; scanned, and compared with
      // `paths`, from wherever this process was started (ResolveStorePath).
      const fs::path root = ProjectRoot();
      for (std::string& path : hot) path = ResolveStorePath(root, path);
      std::vector<Symbol> head = CollectSymbols(hot, options.kind, head_error, options.containing);
      head.resize(std::min(store->RankSymbols(head, options.from), head.size()));
      for (const Symbol& symbol : head) {
        ++written[FormatSymbolRow(symbol)];
        emit(symbol);
      }
      hot_paths.insert(hot.begin(), hot.end());
    }
  }

  for (Symbol&& symbol : ScanSymbols(paths, options.kind, error, options.containing)) {
    if (!written.empty() && hot_paths.contains(symbol.path)) {
      const auto owed = written.find(FormatSymbolRow(symbol));
      if ((owed != written.end()) && (owed->second > 0)) {
        --owed->second;
        continue;
      }
    }
    emit(symbol);
  }
  if (error.empty()) error = head_error;
  flush();
  return error.empty();
}

std::vector<std::string> ReadFilterFile(const fs::path& path) {
  std::vector<std::string> words;
  if (path.empty()) return words;
  std::ifstream in{path};
  if (!in) return words;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && ((line.back() == '\r') || (line.back() == ' '))) line.pop_back();
    size_t at = 0;
    while ((at < line.size()) && (line[at] == ' ')) ++at;
    line.erase(0, at);
    if (line.empty() || (line.front() == '#')) continue;
    words.push_back(line);
  }
  return words;
}

namespace {

bool ParseLineRange(std::string_view text, Index& first, Index& last) {
  const size_t colon = text.find(':');
  if (colon == std::string_view::npos) return false;
  const std::string_view lo = text.substr(0, colon);
  const std::string_view hi = text.substr(colon + 1);
  const auto number = [](std::string_view s, Index& out) {
    if (s.empty()) return true;
    long long value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if ((ec != std::errc{}) || (ptr != s.data() + s.size()) || (value < 0)) return false;
    out = static_cast<Index>(value);
    return true;
  };
  return number(lo, first) && number(hi, last);
}

bool ParseCount(std::string_view text, Index& out) {
  long long value = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if ((ec != std::errc{}) || (ptr != text.data() + text.size()) || (value < 0)) return false;
  out = static_cast<Index>(value);
  return true;
}

constexpr std::array<std::string_view, 7> kTakesValue = {
    "--line-range", "--highlight-line", "--language", "--containing",
    "--from",       "--hot-limit",      "--filter"};

std::vector<std::string> Positionals(int argc, char** argv) {
  std::vector<std::string> out;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg.starts_with("-")) {
      if (std::ranges::find(kTakesValue, arg) != kTakesValue.end()) ++i;
      continue;
    }
    out.emplace_back(arg);
  }
  return out;
}

std::string ReadStdin() {
  std::string text;
  char buffer[65536];
  while (const size_t n = std::fread(buffer, 1, sizeof(buffer), stdin)) text.append(buffer, n);
  return text;
}

}

int RunRenderMode(int argc, char** argv) {
  const common::Args args(argc, argv);
  RenderOptions options;
  options.color = !args.Has("--no-color");
  options.syntax = !args.Has("--no-syntax");
  options.line_numbers = args.Has("--line-numbers");

  if (const auto value = args.Value({"--line-range"})) {
    if (!ParseLineRange(*value, options.first_line, options.last_line)) {
      rostd::fprintf<"koi: --line-range wants <start>:<end>, got \"%.*s\"\n">(
          stderr, static_cast<int>(value->size()), value->data());
      return 2;
    }
  }
  if (const auto value = args.Value({"--highlight-line"})) {
    if (!ParseCount(*value, options.highlight_line)) {
      rostd::fprintf<"%s">(stderr, "koi: --highlight-line wants a line number\n");
      return 2;
    }
  }

  const std::vector<std::string> files =
      Positionals(argc, argv);

  std::string text;
  std::string_view view;
  if (files.empty()) {
    text = ReadStdin();
    view = text;
  } else {
    std::error_code read_ec;
    text = ReadWholeFile(files.front(), read_ec);
    if (read_ec) {
      const std::string why = read_ec.message();
      rostd::fprintf<"koi: cannot read %s: %s\n">(stderr, files.front().c_str(), why.c_str());
      return 1;
    }
    view = text;
  }

  if (const auto value = args.Value({"--language"})) {
    options.language = std::string{*value};
    const auto known = KnownLanguages();
    if (std::ranges::find(known, options.language) == known.end()) {
      rostd::fprintf<"koi: no grammar for language \"%s\"\n">(stderr, options.language.c_str());
      options.language.clear();
    }
  } else if (!files.empty()) {
    options.language = std::string{LanguageForPath(fs::path{files.front()})};
  }

  std::string error;
  const std::string out = RenderToAnsi(view, options, error);
  if (!error.empty()) rostd::fprintf<"koi: %s\n">(stderr, error.c_str());
  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

int RunSymbolMode(int argc, char** argv) {
  const common::Args args(argc, argv);
  const bool definitions = args.Has("--definitions");
  const bool references = args.Has("--references");
  if (!definitions && !references) {
    rostd::fprintf<"%s">(stderr, "koi: --symbol-mode wants --definitions, --references, or both\n");
    return 2;
  }

  SymbolModeOptions options;
  options.kind = (definitions && references) ? SymbolKind::kBoth
                 : definitions               ? SymbolKind::kDefinitions
                                             : SymbolKind::kReferences;
  options.picker_rows = args.Has("--picker-rows");
  if (args.Has("--hot-first")) options.order = SymbolOrder::kHotFirst;
  if (const auto value = args.Value({"--containing"})) {
    std::string_view word = *value;
    while (!word.empty() && (std::isspace(static_cast<unsigned char>(word.front())) != 0)) {
      word.remove_prefix(1);
    }
    while (!word.empty() && (std::isspace(static_cast<unsigned char>(word.back())) != 0)) {
      word.remove_suffix(1);
    }
    options.containing = std::string{word};
  }
  if (const auto value = args.Value({"--from"})) options.from = std::string{*value};
  if (const auto value = args.Value({"--hot-limit"})) {
    Index limit = 0;
    if (!ParseCount(*value, limit)) {
      rostd::fprintf<"%s">(stderr, "koi: --hot-limit wants a count\n");
      return 2;
    }
    options.hot_limit = static_cast<int>(std::min<Index>(limit, 100000));
  }

  std::vector<std::string> paths;
  const std::span<char*> tail{argv + 1, static_cast<size_t>(std::max(0, argc - 1))};
  if (std::ranges::any_of(tail, [](const char* one) { return std::string_view{one} == "-"; })) {
    const std::string listed = ReadStdin();
    for (size_t at = 0; at < listed.size();) {
      const size_t eol = std::min(listed.find('\n', at), listed.size());
      std::string_view path{listed.data() + at, eol - at};
      while (!path.empty() && (path.back() == '\r')) path.remove_suffix(1);
      if (!path.empty()) paths.emplace_back(path);
      at = eol + 1;
    }
  } else {
    if (const auto listed = args.MultiValue({"--files"}, true)) {
      for (const std::string_view path : *listed) paths.emplace_back(path);
    }
    if (paths.empty()) paths = Positionals(argc, argv);
    if (paths.empty()) {
      rostd::fprintf<"%s">(stderr, "koi: --symbol-mode wants --files <path>... (or - for stdin)\n");
      return 2;
    }
  }

  std::string error;
  const bool ok = WriteSymbols(paths, options, stdout, error);
  if (!error.empty()) rostd::fprintf<"koi: %s\n">(stderr, error.c_str());
  return ok ? 0 : 1;
}

int RunOverviewMode(int argc, char** argv) {
  const common::Args args(argc, argv);
  std::vector<std::string> paths;
  if (const auto listed = args.MultiValue({"--files"}, true)) {
    for (const std::string_view path : *listed) paths.emplace_back(path);
  }
  if (paths.empty()) paths = Positionals(argc, argv);
  if (paths.empty()) {
    rostd::fprintf<"%s">(stderr, "koi: --overview wants --files <path>...\n");
    return 2;
  }

  std::vector<std::string> filter;
  if (const auto value = args.Value({"--filter"})) filter = ReadFilterFile(fs::path{*value});

  std::string error;
  for (const std::string_view section : OverviewSections(paths, filter, error)) {
    std::fwrite(section.data(), 1, section.size(), stdout);
  }
  if (!error.empty()) rostd::fprintf<"koi: %s\n">(stderr, error.c_str());
  return error.empty() ? 0 : 1;
}

}
