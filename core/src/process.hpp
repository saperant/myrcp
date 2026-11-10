#pragma once

#include <string>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace rcp {

using Env = std::vector<std::pair<std::string, std::string>>;

struct Child {
  pid_t pid = -1;
  int to_child = -1;    // child's stdin
  int from_child = -1;  // child's stdout
};

// Starts argv with piped stdin/stdout, stderr to stderr_fd, in its own process group
// (so Ctrl+C in a handed-off editor never reaches it). Returns pid -1 on failure.
Child spawn_piped(const std::vector<std::string>& argv, const Env& env, int stderr_fd);

// Runs argv in the foreground on the terminal and waits. Returns the exit status.
int run_foreground(const std::vector<std::string>& argv);

// Starts argv fully detached (double fork); does not wait for it.
bool run_detached(const std::vector<std::string>& argv);

// Runs argv to the end (stdin and stderr /dev/null) and returns what it printed.
std::string run_output(const std::vector<std::string>& argv, int* status = nullptr);

// Starts argv in cwd (empty: ours) with stdin /dev/null and stdout and stderr piped,
// both non-blocking, in its own process group. Returns pid -1 on failure.
struct Reader {
  pid_t pid = -1;
  int out = -1, err = -1;
};
Reader spawn_reader(const std::vector<std::string>& argv, const std::string& cwd);

// Whether a program of that name is on $PATH.
bool on_path(const std::string& name);

// Waits up to timeout_ms for pid, then kills its process group. Returns the exit status.
int reap(pid_t pid, int timeout_ms);

}  // namespace rcp
