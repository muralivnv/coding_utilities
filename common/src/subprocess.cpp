#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif  // _GNU_SOURCE

#include "subprocess.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <optional>
#include <string>

namespace common {

namespace {

// Copies `in` into an in-memory file and rewinds it, so the child can read its
// stdin without a writer on the other end of a pipe.
int MakeStdinMemfd(const MmapStream& in) {
  const int fd = memfd_create("cmd_stdin", MFD_CLOEXEC);
  if (fd == -1)
    return -1;

  size_t written = 0;
  while (written < in.size) {
    const ssize_t n = write(fd, in.buffer + written, in.size - written);
    if (n > 0) {
      written += n;
    } else if ((n == -1) && (errno == EINTR)) {
      continue;
    } else {
      close(fd);
      return -1;
    }
  }

  if (lseek(fd, 0, SEEK_SET) == -1) {
    close(fd);
    return -1;
  }
  return fd;
}

void CloseIfOpen(int fd) {
  if (fd != -1)
    close(fd);
}

// Short writes are not errors, so loop; a failing fd (no /dev/tty, say) just stops.
void WriteAll(int fd, const char* data, size_t len) {
  if (fd == -1)
    return;

  size_t written = 0;
  while (written < len) {
    const ssize_t n = write(fd, data + written, len - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
    } else if ((n == -1) && (errno == EINTR)) {
      continue;
    } else {
      return;
    }
  }
}

int WaitForChild(pid_t pid) {
  int status = 0;
  while (waitpid(pid, &status, 0) == -1) {
    // termbox installs its SIGWINCH handler without SA_RESTART, so a resize while
    // the child runs interrupts us. Retrying is what keeps it from going zombie.
    if (errno != EINTR)
      return -1;
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status))
    return 128 + WTERMSIG(status);
  return -1;
}

}  // namespace

CmdResult RunCmdWithCapture(const std::string& shell_cmd, CaptureMode stdout_mode, CaptureMode stderr_mode,
                            const MmapStream* const cmd_stdin) {
  int output_pipe[2] = {-1, -1};
  int stdin_fd = -1;

  bool need_output_pipe = (stdout_mode == CaptureMode::kPipe) || (stderr_mode == CaptureMode::kPipe);

  if (need_output_pipe) {
    if (pipe2(output_pipe, O_CLOEXEC) == -1) {
      return CmdResult{};
    }
  }

  if (cmd_stdin && cmd_stdin->buffer && (cmd_stdin->size > 0)) {
    stdin_fd = MakeStdinMemfd(*cmd_stdin);
    if (stdin_fd == -1) {
      if (need_output_pipe) {
        close(output_pipe[0]);
        close(output_pipe[1]);
      }
      return CmdResult{};
    }
  }

  pid_t pid = fork();
  if (pid == -1) {
    if (need_output_pipe) {
      close(output_pipe[0]);
      close(output_pipe[1]);
    }
    if (stdin_fd != -1)
      close(stdin_fd);
    return CmdResult{};
  }

  if (pid == 0) {
    if (stdin_fd != -1) {
      dup2(stdin_fd, STDIN_FILENO);
      close(stdin_fd);
    }

    auto redirect = [&](CaptureMode mode, int target_fd) {
      if (mode == CaptureMode::kPipe) {
        dup2(output_pipe[1], target_fd);
      } else if (mode == CaptureMode::kDevNull) {
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull != -1) {
          dup2(devnull, target_fd);
          close(devnull);
        }
      }
    };

    redirect(stdout_mode, STDOUT_FILENO);
    redirect(stderr_mode, STDERR_FILENO);

    if (need_output_pipe) {
      close(output_pipe[0]);
      close(output_pipe[1]);
    }

    // $SHELL rather than /bin/sh is deliberate: become actions rely on `export -f`
    // shell functions being visible to the child. Keep the escaping POSIX.
    const char* shell = std::getenv("SHELL");
    if (!shell)
      shell = "/bin/sh";

    execl(shell, shell, "-c", shell_cmd.c_str(), nullptr);
    _exit(127);
  }

  if (stdin_fd != -1)
    close(stdin_fd);
  if (need_output_pipe)
    close(output_pipe[1]);

  CmdResult result;

  if (need_output_pipe) {
    result.output = ReadFdsToMmap({output_pipe[0]});
    close(output_pipe[0]);
  } else {
    result.output.emplace();
  }

  result.exit_status = WaitForChild(pid);
  return result;
}

CmdResult RunCmdInteractive(const std::string& shell_cmd) {
  int err_pipe[2] = {-1, -1};
  if (pipe2(err_pipe, O_CLOEXEC) == -1)
    return CmdResult{};

  // The mirror needs somewhere to keep what it has seen. A memfd rather than a string
  // so the result can be handed back as an MmapStream like every other captured
  // command, and the caller's line views stay valid however it is moved.
  const int spool_fd = memfd_create("interactive_stderr", MFD_CLOEXEC);
  const int tty_out = open("/dev/tty", O_WRONLY | O_CLOEXEC);

  pid_t pid = fork();
  if (pid == -1) {
    close(err_pipe[0]);
    close(err_pipe[1]);
    CloseIfOpen(spool_fd);
    CloseIfOpen(tty_out);
    return CmdResult{};
  }

  if (pid == 0) {
    // stdin and stdout go straight to the terminal. Our own stdout is usually the pipe
    // the caller reads the final selection from, and a full-screen program drawing
    // into that pipe is exactly the corruption this exists to avoid.
    const int tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (tty_fd != -1) {
      dup2(tty_fd, STDIN_FILENO);
      dup2(tty_fd, STDOUT_FILENO);
      close(tty_fd);
    }
    dup2(err_pipe[1], STDERR_FILENO);
    close(err_pipe[0]);
    close(err_pipe[1]);

    const char* shell = std::getenv("SHELL");
    if (!shell)
      shell = "/bin/sh";

    execl(shell, shell, "-c", shell_cmd.c_str(), nullptr);
    _exit(127);
  }

  close(err_pipe[1]);

  // Small reads, forwarded the moment they arrive: a prompt the command is blocked on
  // has to reach the screen before we go back to waiting, so this cannot batch.
  char buf[1024];
  while (true) {
    const ssize_t n = read(err_pipe[0], buf, sizeof(buf));
    if (n > 0) {
      WriteAll(tty_out, buf, static_cast<size_t>(n));
      WriteAll(spool_fd, buf, static_cast<size_t>(n));
    } else if (n == 0) {
      break;
    } else if (errno == EINTR) {
      continue;  // a resize while the command runs, not the end of its output
    } else {
      break;
    }
  }
  close(err_pipe[0]);
  CloseIfOpen(tty_out);

  CmdResult result;
  result.exit_status = WaitForChild(pid);

  if ((spool_fd != -1) && (lseek(spool_fd, 0, SEEK_SET) != -1)) {
    result.output = ReadFdsToMmap({spool_fd});
  } else {
    result.output.emplace();
  }
  CloseIfOpen(spool_fd);

  return result;
}

void BecomeCommand(const std::string& shell_cmd) {
  // Reattach stdin so interactive commands like `vim` or `less` can read
  // keystrokes, and stderr so failures stay visible even when our stdout is being
  // captured by a command substitution. stdout is left alone on purpose so the
  // command can still deliver the caller's result.
  int tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
  if (tty_fd != -1) {
    dup2(tty_fd, STDIN_FILENO);
    dup2(tty_fd, STDERR_FILENO);
    close(tty_fd);
  }

  const char* shell = std::getenv("SHELL");
  if (!shell)
    shell = "/bin/sh";

  // Replace the current process image with the new command. Its exit status
  // becomes ours, which is how the caller learns that it failed.
  execl(shell, shell, "-c", shell_cmd.c_str(), nullptr);

  // execl only returns if an error occurred (e.g. shell binary not found)
  perror("execl failed in BecomeCommand");
  _exit(127);
}

}  // namespace common
