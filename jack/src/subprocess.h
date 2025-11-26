#ifndef JACK_SRC_SUBPROCESS_H_
#define JACK_SRC_SUBPROCESS_H_

#include <string>

namespace jack {

void SetEnv();
std::string Exec(const std::vector<std::string>& cmd);
std::string ShellExec(const std::string& cmd);

} // namespace jack

#endif // JACK_SRC_SUBPROCESS_H_
