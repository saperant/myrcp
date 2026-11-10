#include "console.hpp"

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <sys/wait.h>
#include <unistd.h>

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

bool plain_word(const std::string& a) {
  if (a.empty()) return false;
  for (const char c : a) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || std::strchr("-_./=:,+@%", c))) return false;
  }
  return true;
}

// The command as a shell line: what the field shows (and runs when edited).
std::string command_line(const std::vector<std::string>& argv) {
  if (argv.size() == 3 && argv[0] == "/bin/sh" && argv[1] == "-c") return argv[2];
  std::string out;
  for (const auto& a : argv) {
    if (!out.empty()) out += ' ';
    if (plain_word(a)) {
      out += a;
      continue;
    }
    out += '\'';
    for (const char c : a) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    out += '\'';
  }
  return out;
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool contains_word(const std::string& low, std::string_view word) {
  for (size_t at = low.find(word); at != std::string::npos; at = low.find(word, at + 1)) {
    const bool before = at == 0 || !std::isalpha(static_cast<unsigned char>(low[at - 1]));
    const size_t end = at + word.size();
    const bool after = end >= low.size() || !std::isalpha(static_cast<unsigned char>(low[end]));
    if (before && after) return true;
  }
  return false;
}

std::string seconds_text(double s) {
  char buf[32];
  if (s < 60) std::snprintf(buf, sizeof buf, "%.1fs", s);
  else std::snprintf(buf, sizeof buf, "%dm%02ds", int(s) / 60, int(s) % 60);
  return buf;
}

}  // namespace

Consoles::~Consoles() {
  for (auto& [id, c] : consoles_) {
    if (c.running && c.pid > 0) {
      kill(-c.pid, SIGTERM);
      waitpid(c.pid, nullptr, 0);
    }
    for (int& fd : c.fds) {
      if (fd >= 0) ::close(fd);
    }
  }
}

std::string Consoles::run(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                          const std::string& name, std::string& error) {
  Console* c = nullptr;
  if (!name.empty()) {
    for (auto& [id, k] : consoles_) {
      if (k.name == name) c = &k;
    }
  }
  if (c && c->running) {
    error = c->title + " is still running: stop it first";
    return "";
  }
  if (!c) {
    const std::string id = "console#" + std::to_string(next_++);
    c = &consoles_[id];
    c->id = id;
    c->name = name;
  }
  c->argv = argv;
  c->cwd = cwd;
  c->title = title.empty() ? (argv.empty() ? "Console" : std::filesystem::path(argv[0]).filename().string()) : title;
  if (argv.empty()) return c->id;  // the user types the command
  if (!start(*c, error)) return "";
  return c->id;
}

bool Consoles::rerun(const std::string& id, std::string& error) {
  Console* c = find(id);
  if (!c) return false;
  if (c->running) {
    error = "It is still running: stop it first";
    return false;
  }
  return start(*c, error);
}

bool Consoles::start(Console& c, std::string& error) {
  if (c.argv.empty()) {
    error = "Nothing to run";
    return false;
  }
  int out[2], err[2];
  if (pipe(out) < 0) {
    error = std::strerror(errno);
    return false;
  }
  if (pipe(err) < 0) {
    error = std::strerror(errno);
    ::close(out[0]), ::close(out[1]);
    return false;
  }
  std::vector<char*> args;
  for (const auto& a : c.argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  const pid_t pid = fork();
  if (pid < 0) {
    error = std::strerror(errno);
    for (int fd : {out[0], out[1], err[0], err[1]}) ::close(fd);
    return false;
  }
  if (pid == 0) {
    setpgid(0, 0);  // Stop signals the whole group, and Ctrl+C on our terminal never reaches it
    const int null = open("/dev/null", O_RDONLY);
    if (null >= 0) dup2(null, 0);
    dup2(out[1], 1);
    dup2(err[1], 2);
    for (int fd : {out[0], out[1], err[0], err[1], null}) {
      if (fd > 2) ::close(fd);
    }
    if (!c.cwd.empty() && chdir(c.cwd.c_str()) != 0) {
      std::fprintf(stderr, "cannot enter %s: %s\n", c.cwd.c_str(), std::strerror(errno));
      _exit(127);
    }
    execvp(args[0], args.data());
    std::fprintf(stderr, "cannot run %s: %s\n", args[0], std::strerror(errno));
    _exit(127);
  }
  setpgid(pid, pid);
  ::close(out[1]);
  ::close(err[1]);
  for (int fd : {out[0], err[0]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
  c.fds[0] = out[0];
  c.fds[1] = err[0];
  c.pid = pid;
  c.running = true;
  c.status = 0;
  c.stops = 0;
  c.lines.clear();
  c.dropped = 0;
  c.partial[0].clear();
  c.partial[1].clear();
  c.follow = true;
  c.started = std::chrono::steady_clock::now();
  ++c.seq;
  return true;
}

void Consoles::stop(const std::string& id) {
  Console* c = find(id);
  if (!c || !c->running || c->pid <= 0) return;
  const int sig = c->stops == 0 ? SIGINT : c->stops == 1 ? SIGTERM : SIGKILL;
  ++c->stops;
  kill(-c->pid, sig);
}

void Consoles::close(const std::string& id) {
  const auto it = consoles_.find(id);
  if (it == consoles_.end()) return;
  Console& c = it->second;
  if (c.running && c.pid > 0) {
    kill(-c.pid, SIGKILL);
    waitpid(c.pid, nullptr, 0);
  }
  for (int& fd : c.fds) {
    if (fd >= 0) ::close(fd);
  }
  consoles_.erase(it);
}

Console* Consoles::find(std::string_view id) {
  const auto it = consoles_.find(id);
  return it == consoles_.end() ? nullptr : &it->second;
}

void Consoles::add_fds(std::vector<pollfd>& fds, std::vector<std::string>& ids) const {
  for (const auto& [id, c] : consoles_) {
    for (const int fd : c.fds) {
      if (fd < 0) continue;
      fds.push_back({fd, POLLIN, 0});
      ids.push_back(id);
    }
  }
}

bool Consoles::on_readable(const std::string& id, int fd) {
  Console* c = find(id);
  if (!c) return false;
  const int which = fd == c->fds[0] ? 0 : fd == c->fds[1] ? 1 : -1;
  if (which < 0) return false;
  char buf[8192];
  const ssize_t n = read(fd, buf, sizeof buf);
  if (n < 0 && (errno == EINTR || errno == EAGAIN)) return false;
  if (n > 0) {
    add_text(*c, which, {buf, size_t(n)});
    return true;
  }
  ::close(fd);  // its end of the output
  c->fds[which] = -1;
  if (!c->partial[which].empty()) push_line(*c, std::exchange(c->partial[which], ""), which == 1);
  if (c->fds[0] < 0 && c->fds[1] < 0) finished(*c);
  ++c->seq;
  return true;
}

void Consoles::add_text(Console& c, int which, std::string_view text) {
  std::string& part = c.partial[which];
  for (const char ch : text) {
    if (ch == '\n') {
      push_line(c, std::exchange(part, ""), which == 1);
    } else if (ch == '\r') {
      part.clear();  // a progress bar redrawing its line: the last one stays
    } else if (ch == '\t') {
      part.append(8 - part.size() % 8, ' ');
    } else if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\x1b') {
      part += ch;
    }
  }
  ++c.seq;
}

void Consoles::push_line(Console& c, std::string line, bool err) {
  // Colour codes would show as text: drop ESC [ ... letter.
  std::string clean;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\x1b' && i + 1 < line.size() && line[i + 1] == '[') {
      i += 2;
      while (i < line.size() && !std::isalpha(static_cast<unsigned char>(line[i]))) ++i;
      continue;
    }
    if (line[i] != '\x1b') clean += line[i];
  }
  c.lines.push_back({std::move(clean), err});
  if (c.lines.size() > kMaxLines) {
    c.lines.pop_front();
    ++c.dropped;
  }
}

void Consoles::finished(Console& c) {
  int st = 0;
  if (c.pid > 0 && waitpid(c.pid, &st, 0) == c.pid) {
    c.status = WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
  }
  c.pid = -1;
  c.running = false;
  c.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - c.started).count();
}

std::optional<std::pair<std::string, int>> Consoles::place_in(const std::string& line, const std::string& cwd) {
  namespace fs = std::filesystem;
  const auto exists = [&](const std::string& p) -> std::optional<std::string> {
    if (p.empty() || p.size() > 1024) return std::nullopt;
    std::error_code ec;
    const fs::path full = fs::path(p).is_absolute() || cwd.empty() ? fs::path(p) : fs::path(cwd) / p;
    if (fs::is_regular_file(full, ec)) return fs::absolute(full, ec).lexically_normal().string();
    return std::nullopt;
  };
  // Python: File "x.py", line 3
  if (const auto f = line.find("File \""); f != std::string::npos) {
    const auto q = line.find('"', f + 6);
    const auto l = line.find(", line ", q == std::string::npos ? f : q);
    if (q != std::string::npos && l != std::string::npos) {
      const int n = std::atoi(line.c_str() + l + 7);
      if (const auto p = exists(line.substr(f + 6, q - f - 6)); p && n > 0) return std::pair{*p, n};
    }
  }
  // path:line[:col] — the path starting the line or after white space, quote or paren.
  for (size_t i = 0; i < line.size(); ++i) {
    if (i > 0 && !std::strchr(" \t\"'(", line[i - 1])) continue;
    size_t j = i;
    while (j < line.size() && line[j] != ':' && !std::strchr(" \t\"'()", line[j])) ++j;
    if (j + 1 >= line.size() || line[j] != ':' || !std::isdigit(static_cast<unsigned char>(line[j + 1]))) continue;
    const int n = std::atoi(line.c_str() + j + 1);
    if (const auto p = exists(line.substr(i, j - i)); p && n > 0) return std::pair{*p, n};
  }
  return std::nullopt;
}

Value Consoles::tree(const Console& c) const {
  Array rows;
  const size_t n = c.lines.size();
  for (size_t i = 0; i < n; ++i) {
    const ConsoleLine& l = c.lines[i];
    const auto place = place_in(l.text, c.cwd);
    const std::string low = lower(l.text);
    std::string style = "normal";
    if (place) style = "accent";
    else if (contains_word(low, "error") || contains_word(low, "failed") || contains_word(low, "fatal")) style = "error";
    else if (contains_word(low, "warning")) style = "warning";
    else if (l.err) style = "dim";
    Map row{{"key", std::to_string(c.dropped + i)}, {"cells", Array{Map{{"text", l.text}, {"style", style}}}}};
    if (place) {
      row.emplace_back("sel", Array{Map{{"type", "console.line"},
                                        {"label", std::filesystem::path(place->first).filename().string() + ":" +
                                                      std::to_string(place->second)},
                                        {"path", place->first},
                                        {"line", place->second},
                                        {"props", Array{Array{"Output", l.text}}}}});
    }
    rows.push_back(std::move(row));
  }
  std::string state;
  std::string state_style = "dim";
  if (c.argv.empty()) {
    state = "Type a command, Enter runs it:";
  } else if (c.running) {
    state = "⟳ running";
    state_style = "accent";
  } else if (c.status == 0) {
    state = "✔ done in " + seconds_text(c.seconds);
    state_style = "ok";
  } else {
    state = "✘ exit " + std::to_string(c.status) + (c.status > 128 ? " (signal " + std::to_string(c.status - 128) + ")" : "") +
            " after " + seconds_text(c.seconds);
    state_style = "error";
  }
  Map table{{"t", "table"},     {"id", "out"},
            {"header", false},  {"columns", Array{Map{{"title", ""}}}},
            {"rows", rows},     {"offset", 0},
            {"total", int64_t(n)}, {"empty", c.running ? "(no output yet)" : c.argv.empty() ? "" : "(no output)"}};
  if (!c.argv.empty()) table.emplace_back("autofocus", true);
  if (c.follow && n > 0) table.emplace_back("cursor", Array{int64_t(n - 1), c.seq});
  return Map{{"t", "vbox"},
             {"children",
              Array{Map{{"t", "hbox"},
                        {"grow", 0},
                        {"gap", 2},
                        {"children", Array{Map{{"t", "text"}, {"text", state}, {"style", state_style}},
                                           Map{{"t", "field"}, {"id", "cmd"}, {"value", command_line(c.argv)}, {"grow", 1},
                                               {"autofocus", c.argv.empty()}}}}},
                    table,
                    Map{{"t", "text"},
                        {"style", "dim"},
                        {"text", Array{Array{"Enter", "accent"}, Array{" open file:line   ", "dim"},
                                       Array{"r", "accent"}, Array{" run again   ", "dim"},
                                       Array{"Ctrl+C", "accent"}, Array{" stop   ", "dim"},
                                       Array{"Ctrl+W", "accent"}, Array{" close   ", "dim"},
                                       Array{"the command: edit it, Enter runs it", "dim"}}}}}}};
}

bool Consoles::event(const std::string& id, std::string_view widget, std::string_view kind, const Value& value,
                     const std::function<void(const std::string&, int)>& open) {
  Console* c = find(id);
  if (!c) return false;
  if (widget == "out" && kind == "select") {
    const size_t at = size_t(std::atoll(std::string(value.str()).c_str()));
    c->follow = c->lines.empty() || at + 1 >= c->dropped + c->lines.size();  // at the end: follow it again
    return false;
  }
  if (widget == "out" && kind == "activate") {
    const size_t at = size_t(std::atoll(std::string(value.str()).c_str()));
    if (at < c->dropped || at - c->dropped >= c->lines.size()) return false;
    if (const auto place = place_in(c->lines[at - c->dropped].text, c->cwd)) open(place->first, place->second);
    return false;
  }
  if (widget == "cmd" && kind == "submit") {
    const std::string line(value.str());
    if (line.empty() || c->running) return false;
    if (c->argv.empty() || line != command_line(c->argv)) c->argv = {"/bin/sh", "-c", line};
    if (c->title == "Console") c->title = line.substr(0, line.find(' '));
    std::string error;
    start(*c, error);
    return true;
  }
  return false;
}

}  // namespace rcp
