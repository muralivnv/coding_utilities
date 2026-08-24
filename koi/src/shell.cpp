#include "shell.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>

#include "mmap_stream.h"
#include "navigate.h"
#include "selection.h"
#include "subprocess.h"
#include "unicode.h"

namespace koi {

void PutSelfOnPath() {
  std::error_code ec;
  const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return;
  const std::string dir = self.parent_path().string();
  if (dir.empty()) return;

  const char* current = std::getenv("PATH");
  const std::string_view rest{(current == nullptr) ? "" : current};
  if (rest.starts_with(dir) && ((rest.size() == dir.size()) || (rest[dir.size()] == ':'))) return;

  const std::string updated = rest.empty() ? dir : (dir + ":" + std::string{rest});
  setenv("PATH", updated.c_str(), 1);
}

namespace {

constexpr std::array<bool, 256> BuildSafeChars() {
  std::array<bool, 256> table{};
  for (char c = 'a'; c <= 'z'; ++c) table[static_cast<unsigned char>(c)] = true;
  for (char c = 'A'; c <= 'Z'; ++c) table[static_cast<unsigned char>(c)] = true;
  for (char c = '0'; c <= '9'; ++c) table[static_cast<unsigned char>(c)] = true;
  for (const char c : {'_', '-', '.', '/', ':', '=', '@', '+', ','}) {
    table[static_cast<unsigned char>(c)] = true;
  }
  return table;
}

Index PrimaryCursorByte(const Editor& ed) {
  return CursorOf(ed.doc.table, ed.doc.selections.Primary());
}

std::string PrimaryText(const Editor& ed) {
  const Selection& s = ed.doc.selections.Primary();
  if (s.IsEmpty()) return {};
  return ReadDocRange(ed.doc.table, s.Range());
}

std::string SelectionsText(const Editor& ed) {
  std::string out;
  for (const Selection& s : ed.doc.selections.Ranges()) {
    if (s.IsEmpty()) continue;
    if (!out.empty()) out += '\n';
    out += ReadDocRange(ed.doc.table, s.Range());
  }
  return out;
}

bool EndsWithAmpersand(std::string_view command) {
  while (!command.empty() && ((command.back() == ' ') || (command.back() == '\t'))) {
    command.remove_suffix(1);
  }
  return !command.empty() && (command.back() == '&');
}

}

std::string ShellQuote(std::string_view s) {
  static constexpr auto kSafe = BuildSafeChars();
  if (s.empty()) return "''";

  bool needs_quoting = false;
  for (const char c : s) {
    if (!kSafe[static_cast<unsigned char>(c)]) {
      needs_quoting = true;
      break;
    }
  }
  if (!needs_quoting) return std::string{s};

  std::string out;
  out.reserve(s.size() + 2);
  out += '\'';
  for (const char c : s) {
    if (c == '\'') {
      out += "'\"'\"'";
    } else {
      out += c;
    }
  }
  out += '\'';
  return out;
}

namespace {

std::string SelfExecutablePath() {
  static const std::string path = [] {
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string{"koi"} : self.string();
  }();
  return path;
}

}

std::string ExpandVariables(std::string_view command, const Editor& ed) {
  std::string out;
  out.reserve(command.size());

  for (size_t i = 0; i < command.size();) {
    if ((command[i] != '%') || (i + 1 >= command.size()) || (command[i + 1] != '{')) {
      out += command[i++];
      continue;
    }
    const size_t close = command.find('}', i + 2);
    if (close == std::string_view::npos) {
      out += command[i++];
      continue;
    }
    const std::string_view name = command.substr(i + 2, close - i - 2);

    std::string value;
    bool known = true;
    if (name == "koi") {
      value = SelfExecutablePath();
    } else if (name == "buffer_name") {
      value = HasDiskFile(ed.doc) ? ed.doc.file.string() : ed.doc.view_name;
    } else if (name == "basename") {
      value = ed.doc.file.filename().string();
    } else if (name == "extension") {
      value = ed.doc.file.extension().string();
      if (!value.empty() && (value.front() == '.')) value.erase(0, 1);
    } else if ((name == "cursor_line") || (name == "linenumber")) {
      value = std::to_string(LineAt(ed.doc.table, PrimaryCursorByte(ed)) + 1);
    } else if (name == "cursor_column") {
      value = std::to_string(ColumnForByte(ed.doc.table, PrimaryCursorByte(ed), ed.doc.tab_width) + 1);
    } else if (name == "selection") {
      value = PrimaryText(ed);
    } else {
      known = false;
    }

    if (!known) {
      out += command[i++];
      continue;
    }
    out += ShellQuote(value);
    i = close + 1;
  }
  return out;
}

void RunShellCommand(Editor& ed, std::string_view command, ShellMode mode) {
  const std::string expanded = ExpandVariables(command, ed);
  if (expanded.find_first_not_of(" \t") == std::string::npos) {
    ed.status.Warn("empty shell command");
    return;
  }

  const bool wants_output =
      (mode == ShellMode::kPipe) || (mode == ShellMode::kInsertOutput) ||
      (mode == ShellMode::kAppendOutput);
  const bool sends_input = (mode != ShellMode::kDiscard);

  std::optional<common::MmapStream> stdin_stream;
  if (sends_input) {
    const std::string text = SelectionsText(ed);
    stdin_stream = common::MmapStreamFromBytes(text.data(), text.size());
    if (!stdin_stream) {
      ed.status.Fail("could not stage stdin for the command");
      return;
    }
  }

  if ((mode == ShellMode::kDiscard) && !EndsWithAmpersand(expanded)) {
    if (ed.suspend_terminal) ed.suspend_terminal();
    const common::CmdResult result = common::RunCmdInteractive(expanded);
    if (ed.resume_terminal) ed.resume_terminal();
    ed.status = (result.exit_status == 0)
                    ? std::string{}
                    : ("command exited " + std::to_string(result.exit_status));
    return;
  }

  if (!wants_output) {
    const common::CmdResult result = common::RunCmdWithCapture(
        expanded, common::CaptureMode::kDevNull, common::CaptureMode::kDevNull,
        stdin_stream ? &*stdin_stream : nullptr);
    if (result.exit_status < 0) {
      ed.status.Fail("could not run: " + expanded);
    } else if (result.exit_status != 0) {
      ed.status.Warn("command exited " + std::to_string(result.exit_status));
    }
    return;
  }

  const common::CmdResult result =
      common::RunCmdWithCapture(expanded, common::CaptureMode::kPipe, common::CaptureMode::kPipe,
                                stdin_stream ? &*stdin_stream : nullptr);
  if (!result.output) {
    ed.status.Fail("could not run: " + expanded);
    return;
  }
  std::string text(result.output->buffer, result.output->size);
  if (result.exit_status != 0) {
    std::string head = text;
    if (head.size() > 120) {
      std::size_t cut = 0;
      while (cut < head.size()) {
        const std::size_t next = NextGraphemeInString(head, cut);
        if (next > 120) break;
        cut = next;
      }
      head.resize(cut);
    }
    ed.status.Warn("command exited " + std::to_string(result.exit_status) +
                (head.empty() ? "" : (": " + head)));
    return;
  }
  if (!IsWellFormedUtf8(text)) {
    ed.status.Fail("command produced invalid UTF-8");
    return;
  }

  UndoGroup group(ed.doc.table);
  auto ranges = ed.doc.selections.Ranges();

  // One Apply for every selection, not one Apply per selection. Apply validates
  // the whole change list against the pre-edit document before it mutates
  // anything, so the command lands on all of them or on none. Editing them one
  // at a time could fail partway -- the edits already made stood, `modified`
  // was already set, and the selection set was never replaced, so the cursors
  // still described pre-edit coordinates. Output beginning with a combining
  // mark did it: the edit on one selection moved its neighbour's end inside a
  // grapheme, and the next iteration was refused for exactly that.
  std::vector<Change> changes;
  std::vector<std::size_t> edited;  // which selection each change belongs to
  changes.reserve(ranges.size());
  edited.reserve(ranges.size());
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if (mode == ShellMode::kPipe) {
      if (ranges[i].IsEmpty() && text.empty()) continue;
      changes.push_back(Change{ranges[i].From(), ranges[i].To(), text});
    } else {
      if (text.empty()) continue;
      const Index at = (mode == ShellMode::kAppendOutput) ? ranges[i].To() : ranges[i].From();
      changes.push_back(Change{at, at, text});
    }
    edited.push_back(i);
  }
  if (changes.empty()) return;

  CursorState before;
  before.primary = static_cast<std::uint32_t>(ed.doc.selections.PrimaryIndex());
  for (const Selection& s : ranges) before.spans.push_back(CursorSpan{s.anchor, s.head});

  std::vector<Edit> edits;
  if (const ErrorCtx err = Apply(ed.doc.table, changes, before, CursorState{}, &edits); err) {
    ed.status.Fail(FormatErrorCtx(err));
    return;  // Apply refuses before it writes, so the document is untouched.
  }
  ed.doc.modified = true;

  // `edits[k]` is already in post-edit coordinates, so an edited selection takes
  // its span verbatim; anything between two changes rides the shift the change
  // before it produced.
  Index shift = 0;
  std::size_t k = 0;
  for (std::size_t i = 0; i < ranges.size(); ++i) {
    if ((k < edited.size()) && (edited[k] == i)) {
      ranges[i].anchor = edits[k].start_byte;
      ranges[i].head = edits[k].new_end_byte;
      shift = edits[k].new_end_byte - changes[k].to;
      ++k;
    } else {
      ranges[i].anchor += shift;
      ranges[i].head += shift;
    }
    ranges[i].goal_column = -1;
  }
  ed.doc.selections.Replace(ed.doc.table, std::move(ranges));
  if (IsExcerptView(ed.doc)) DropUnreachableEpochs(ed.doc);
}

namespace {

struct ClipboardTools {
  std::string copy;
  std::string paste;
};

bool OnPath(std::string_view program) {
  const common::CmdResult result =
      common::RunCmdWithCapture("command -v " + ShellQuote(program), common::CaptureMode::kDevNull,
                                common::CaptureMode::kDevNull, nullptr);
  return result.exit_status == 0;
}

const ClipboardTools& Tools() {
  static const ClipboardTools tools = [] {
    ClipboardTools found;

    static constexpr std::array<std::array<const char*, 3>, 4> kCandidates{{
        {"wl-copy", "wl-copy", "wl-paste --no-newline"},
        {"xclip", "xclip -selection clipboard -in", "xclip -selection clipboard -out"},
        {"xsel", "xsel --clipboard --input", "xsel --clipboard --output"},
        {"pbcopy", "pbcopy", "pbpaste"},
    }};
    for (const auto& candidate : kCandidates) {
      if (!OnPath(candidate[0])) continue;
      found.copy = candidate[1];
      found.paste = candidate[2];
      break;
    }
    return found;
  }();
  return tools;
}

}

bool HasClipboard() { return !Tools().copy.empty(); }

bool ClipboardCopy(std::string_view text) {
  const ClipboardTools& tools = Tools();
  if (tools.copy.empty()) return false;
  std::optional<common::MmapStream> input = common::MmapStreamFromBytes(text.data(), text.size());
  if (!input) return false;
  const common::CmdResult result = common::RunCmdWithCapture(
      tools.copy, common::CaptureMode::kDevNull, common::CaptureMode::kDevNull, &*input);
  return result.exit_status == 0;
}

bool ClipboardPaste(std::string& out) {
  const ClipboardTools& tools = Tools();
  if (tools.paste.empty()) return false;
  const common::CmdResult result = common::RunCmdWithCapture(
      tools.paste, common::CaptureMode::kPipe, common::CaptureMode::kDevNull, nullptr);
  if ((result.exit_status != 0) || !result.output) return false;
  std::string text(result.output->buffer, result.output->size);
  if (!IsWellFormedUtf8(text)) return false;
  out = std::move(text);
  return true;
}

}
