#include "process.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rcp {

namespace {

std::vector<char*> c_argv(const std::vector<std::string>& argv) {
  std::vector<char*> out;
  for (const auto& a : argv) out.push_back(const_cast<char*>(a.c_str()));
  out.push_back(nullptr);
  return out;
}

void default_signals() {
  for (int s : {SIGINT, SIGQUIT, SIGTSTP, SIGPIPE, SIGWINCH, SIGTERM}) signal(s, SIG_DFL);
}

void close_from(int lowest) {
  for (int fd = lowest; fd < 1024; ++fd) close(fd);
}

}  // namespace

Child spawn_piped(const std::vector<std::string>& argv, const Env& env, int stderr_fd) {
  int in_pipe[2], out_pipe[2];
  if (pipe2(in_pipe, O_CLOEXEC) != 0) return {};
  if (pipe2(out_pipe, O_CLOEXEC) != 0) {
    close(in_pipe[0]);
    close(in_pipe[1]);
    return {};
  }
  auto args = c_argv(argv);
  const pid_t pid = fork();
  if (pid < 0) {
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]}) close(fd);
    return {};
  }
  if (pid == 0) {
    setpgid(0, 0);
    dup2(in_pipe[0], 0);
    dup2(out_pipe[1], 1);
    if (stderr_fd >= 0) dup2(stderr_fd, 2);
    close_from(3);
    default_signals();
    for (const auto& [k, v] : env) setenv(k.c_str(), v.c_str(), 1);
    execvp(args[0], args.data());
    const std::string msg = "myrcp: cannot run " + argv[0] + ": " + std::strerror(errno) + "\n";
    (void)!write(2, msg.data(), msg.size());
    _exit(127);
  }
  close(in_pipe[0]);
  close(out_pipe[1]);
  return {pid, in_pipe[1], out_pipe[0]};
}

int run_foreground(const std::vector<std::string>& argv) {
  auto args = c_argv(argv);
  struct sigaction ign{}, old_int{}, old_quit{};
  ign.sa_handler = SIG_IGN;
  sigaction(SIGINT, &ign, &old_int);
  sigaction(SIGQUIT, &ign, &old_quit);
  const pid_t pid = fork();
  if (pid == 0) {
    default_signals();
    execvp(args[0], args.data());
    _exit(127);
  }
  int status = -1;
  if (pid > 0) {
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
  sigaction(SIGINT, &old_int, nullptr);
  sigaction(SIGQUIT, &old_quit, nullptr);
  if (pid < 0) return -1;
  return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

bool run_detached(const std::vector<std::string>& argv) {
  auto args = c_argv(argv);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    if (fork() == 0) {
      setsid();
      const int null = open("/dev/null", O_RDWR);
      for (int fd = 0; fd < 3; ++fd) dup2(null, fd);
      close_from(3);
      default_signals();
      execvp(args[0], args.data());
      _exit(127);
    }
    _exit(0);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

std::string run_output(const std::vector<std::string>& argv, int* status) {
  int out[2];
  if (pipe2(out, O_CLOEXEC) != 0) return "";
  auto args = c_argv(argv);
  const pid_t pid = fork();
  if (pid < 0) {
    close(out[0]);
    close(out[1]);
    return "";
  }
  if (pid == 0) {
    const int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(out[1], 1);
    dup2(null, 2);
    close_from(3);
    default_signals();
    execvp(args[0], args.data());
    _exit(127);
  }
  close(out[1]);
  std::string text;
  char buf[4096];
  for (;;) {
    const ssize_t n = read(out[0], buf, sizeof buf);
    if (n > 0) text.append(buf, size_t(n));
    else if (n == 0 || errno != EINTR) break;
  }
  close(out[0]);
  int st = 0;
  while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
  }
  if (status) *status = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
  return text;
}

Reader spawn_reader(const std::vector<std::string>& argv, const std::string& cwd) {
  int out[2], err[2];
  if (pipe2(out, O_CLOEXEC) != 0) return {};
  if (pipe2(err, O_CLOEXEC) != 0) {
    close(out[0]);
    close(out[1]);
    return {};
  }
  auto args = c_argv(argv);
  const pid_t pid = fork();
  if (pid < 0) {
    for (int fd : {out[0], out[1], err[0], err[1]}) close(fd);
    return {};
  }
  if (pid == 0) {
    setpgid(0, 0);
    const int null = open("/dev/null", O_RDWR);
    dup2(null, 0);
    dup2(out[1], 1);
    dup2(err[1], 2);
    close_from(3);
    default_signals();
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    execvp(args[0], args.data());
    const std::string msg = "cannot run " + argv[0] + ": " + std::strerror(errno) + "\n";
    (void)!write(2, msg.data(), msg.size());
    _exit(127);
  }
  close(out[1]);
  close(err[1]);
  fcntl(out[0], F_SETFL, fcntl(out[0], F_GETFL) | O_NONBLOCK);
  fcntl(err[0], F_SETFL, fcntl(err[0], F_GETFL) | O_NONBLOCK);
  return {pid, out[0], err[0]};
}

bool on_path(const std::string& name) {
  const char* path = std::getenv("PATH");
  std::string dirs = path ? path : "/usr/bin:/bin";
  size_t at = 0;
  while (at <= dirs.size()) {
    const size_t end = std::min(dirs.find(':', at), dirs.size());
    const std::string dir = end > at ? dirs.substr(at, end - at) : ".";
    if (access((dir + "/" + name).c_str(), X_OK) == 0) return true;
    at = end + 1;
  }
  return false;
}

int reap(pid_t pid, int timeout_ms) {
  if (pid <= 0) return -1;
  int status = 0;
  for (int waited = 0; waited <= timeout_ms; waited += 10) {
    const pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (r < 0) return -1;
    poll(nullptr, 0, 10);
  }
  kill(-pid, SIGKILL);
  kill(pid, SIGKILL);
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

}  // namespace rcp
