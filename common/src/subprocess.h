#ifndef _SUBPROCESS_H_
#define _SUBPROCESS_H_

#include <optional>
#include <string>

#include "mmap_stream.h"

namespace common {

enum class CaptureMode {
  kPipe,     // redirect into the captured MmapStream
  kDevNull,  // discard
  kInherit   // leave the parent's fd in place (goes to the terminal)
};

struct CmdResult {
  // Captured output, or nullopt if the process could not be started.
  std::optional<MmapStream> output{std::nullopt};
  // The child's exit code, 128 + signal if it was killed, or -1 if it never ran
  // or could not be reaped.
  int exit_status{-1};
};

// Runs `shell_cmd` via $SHELL -c and waits for it.
// `cmd_stdin`, when given, is handed to the child on a memfd rather than a pipe,
// so there is no writer to deadlock against or to take a SIGPIPE from. An empty one
// still counts as given: the child gets an fd at EOF, never the caller's own stdin,
// which it would otherwise race for bytes while a producer is still writing.
CmdResult RunCmdWithCapture(const std::string& shell_cmd, CaptureMode stdout_mode, CaptureMode stderr_mode,
                            const MmapStream* const cmd_stdin = nullptr);

// Runs `shell_cmd` via $SHELL -c with the terminal handed over, and waits for it.
// This is for commands that own the screen while they run -- an editor, a pager,
// `git add -p`, a `read -p` confirmation -- so the caller has to give the screen up
// first and take it back afterwards.
//
// stdin and stdout are /dev/tty. stderr is captured into CmdResult::output *and*
// mirrored to the terminal as it arrives, because that one fd carries both things we
// want: diagnostics worth showing after the fact, and prompts that have to be on
// screen while the command is still waiting on them.
CmdResult RunCmdInteractive(const std::string& shell_cmd);

// Replaces the current process image with `shell_cmd`.
// stdin and stderr are reattached to /dev/tty so the command can read keystrokes
// and so its diagnostics stay visible even when our stdout is being captured.
// stdout is deliberately left as inherited: a become command is still expected to
// be able to write the caller's result. A full-screen become target that needs the
// screen for output should redirect itself (e.g. `vim {{@SELECTION@}} >/dev/tty`).
void BecomeCommand(const std::string& shell_cmd);

}  // namespace common

#endif  // _SUBPROCESS_H_
