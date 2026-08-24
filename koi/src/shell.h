#ifndef KOI_SHELL_H_
#define KOI_SHELL_H_

#include <string>
#include <string_view>

#include "editor.h"

namespace koi {

void PutSelfOnPath();

std::string ShellQuote(std::string_view s);

std::string ExpandVariables(std::string_view command, const Editor& ed);

enum class ShellMode {
  kDiscard,
  kPipeTo,
  kPipe,
  kInsertOutput,
  kAppendOutput,
};

void RunShellCommand(Editor& ed, std::string_view command, ShellMode mode);

bool ClipboardCopy(std::string_view text);
bool ClipboardPaste(std::string& out);

bool HasClipboard();

}

#endif
