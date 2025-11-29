#ifndef JACK_SRC_SUBPROCESS_H_
#define JACK_SRC_SUBPROCESS_H_

#include <string>

namespace jack {

struct ShellExecOutput {
  std::string output{};
  int exit_code{};
};

void SetEnv();
ShellExecOutput ShellExec(const char* cmd);

} // namespace jack

#endif // JACK_SRC_SUBPROCESS_H_
