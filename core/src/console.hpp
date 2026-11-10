// Consoles: programs run for their output, as Eclipse's Console and NetBeans' Output
// windows. Each has a view of its own ("console#<n>") that keeps the output after the
// program exits; a line naming path:line opens there in the editor; Stop and Rerun.
// The hub owns them, so the workbench shows them and any plugin can start one.
#pragma once

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/types.h>
#include <vector>

#include "rcp/msgpack.hpp"

namespace rcp {

struct ConsoleLine {
  std::string text;
  bool err = false;  // from stderr
};

struct Console {
  std::string id;     // "console#<n>"
  std::string title;  // the tab's
  std::vector<std::string> argv;
  std::string cwd;
  std::string name;  // the plugin's name for it: a new run with the same name reuses it
  std::deque<ConsoleLine> lines;
  size_t dropped = 0;  // lines dropped off the top (kMaxLines)
  std::string partial[2];  // stdout's, stderr's unfinished line
  pid_t pid = -1;
  int fds[2] = {-1, -1};  // stdout, stderr
  bool running = false;
  int status = 0;         // exit status (128 + signal when killed)
  int stops = 0;          // Stop asked this many times: SIGINT, then SIGTERM, then SIGKILL
  int seq = 0;            // bumped with every change: the cursor follows the end
  bool follow = true;     // the cursor at the last line while it runs
  std::chrono::steady_clock::time_point started;
  double seconds = 0;     // how long the last run took
};

class Consoles {
 public:
  static constexpr size_t kMaxLines = 20000;

  ~Consoles();  // stops what still runs

  // Runs argv in cwd: in the console of that name when there is one (its output
  // cleared), else a new one. Returns its view id; empty (and `error`) when it fails.
  std::string run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                  const std::string& name, std::string& error);
  bool rerun(const std::string& id, std::string& error);
  void stop(const std::string& id);
  void close(const std::string& id);  // stops it and forgets it
  Console* find(std::string_view id);
  const std::map<std::string, Console, std::less<>>& all() const { return consoles_; }

  // The event loop's part: fds to poll (with which console each is), what came.
  void add_fds(std::vector<pollfd>& fds, std::vector<std::string>& ids) const;
  bool on_readable(const std::string& id, int fd);  // true: its view changed

  // Its view: the command, its state, the lines (a table of one column).
  mp::Value tree(const Console& c) const;
  // An event from its view; `open` shows a place in the editor. True: changed.
  bool event(const std::string& id, std::string_view widget, std::string_view kind, const mp::Value& value,
             const std::function<void(const std::string& path, int line)>& open);

  // path:line[:col] at the start of a line or after a space (a compiler's, a test
  // runner's, Python's 'File "x.py", line 3'): the place, resolved against cwd, when
  // that file exists.
  static std::optional<std::pair<std::string, int>> place_in(const std::string& line, const std::string& cwd);

 private:
  bool start(Console& c, std::string& error);
  void add_text(Console& c, int which, std::string_view text);
  void push_line(Console& c, std::string line, bool err);
  void finished(Console& c);

  std::map<std::string, Console, std::less<>> consoles_;
  int next_ = 1;
};

}  // namespace rcp
