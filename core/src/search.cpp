#include "search.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sys/wait.h>
#include <unistd.h>

#include "hub.hpp"
#include "process.hpp"
#include "service.hpp"

namespace rcp {

namespace search {

namespace {

constexpr const char* kSkipDirs[] = {".git",        ".hg",         ".svn",          "node_modules",
                                     ".venv",       "__pycache__", ".mypy_cache",   ".ruff_cache"};

bool in_git(const std::string& root) {
  if (!on_path("git")) return false;
  int st = 0;
  const std::string out = run_output({"git", "-C", root.empty() ? "." : root, "rev-parse", "--is-inside-work-tree"}, &st);
  return st == 0 && out.starts_with("true");
}

std::string escape_regex(const std::string& s) {
  static const std::string special = R"(\^$.|?*+()[]{})";
  std::string out;
  for (char c : s) {
    if (special.find(c) != std::string::npos) out += '\\';
    out += c;
  }
  return out;
}

bool digits(const std::string& s) { return !s.empty() && std::all_of(s.begin(), s.end(), ::isdigit); }

std::string trim(std::string s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

}  // namespace

std::vector<std::string> command(const std::string& root, const Query& q, bool& has_col) {
  has_col = true;
  if (on_path("rg")) {
    std::vector<std::string> c{"rg", "--line-number", "--column", "--no-heading", "--color", "never", "--max-columns", "400"};
    if (!q.regex) c.push_back("--fixed-strings");
    c.push_back(q.match_case ? "--case-sensitive" : "--smart-case");
    c.insert(c.end(), {"--", q.pattern, "."});
    return c;
  }
  if (in_git(root)) {
    std::vector<std::string> c{"git", "grep", "-n", "--column", "-I", "--no-color", "--untracked", "--exclude-standard"};
    c.push_back(q.regex ? "-E" : "-F");
    if (!q.match_case) c.push_back("-i");
    c.insert(c.end(), {"-e", q.pattern});
    return c;
  }
  has_col = false;
  std::vector<std::string> c{"grep", "-rnI", "--color=never"};
  for (const char* d : kSkipDirs) c.push_back(std::string("--exclude-dir=") + d);
  c.push_back(q.regex ? "-E" : "-F");
  if (!q.match_case) c.push_back("-i");
  c.insert(c.end(), {"-e", q.pattern, "."});
  return c;
}

const char* tool_name(const std::string& root) { return on_path("rg") ? "ripgrep" : in_git(root) ? "git grep" : "grep"; }

std::vector<Match> parse(const std::string& out, const Query& q, bool has_col) {
  std::optional<std::regex> found;
  if (!has_col) {
    try {
      auto flags = std::regex::ECMAScript;
      if (!q.match_case) flags |= std::regex::icase;
      found.emplace(q.regex ? q.pattern : escape_regex(q.pattern), flags);
    } catch (const std::regex_error&) {
    }
  }
  std::vector<Match> matches;
  size_t at = 0;
  while (at < out.size() && matches.size() < kLimit) {
    size_t end = out.find('\n', at);
    if (end == std::string::npos) end = out.size();
    const std::string raw = out.substr(at, end - at);
    at = end + 1;
    // path:line:(col:)text; the text may hold colons itself.
    const size_t c1 = raw.find(':');
    if (c1 == std::string::npos) continue;
    const size_t c2 = raw.find(':', c1 + 1);
    if (c2 == std::string::npos) continue;
    const std::string line = raw.substr(c1 + 1, c2 - c1 - 1);
    if (!digits(line)) continue;
    Match m;
    m.path = raw.substr(0, c1);
    if (m.path.starts_with("./")) m.path.erase(0, 2);
    m.line = std::stoi(line);
    if (has_col) {
      const size_t c3 = raw.find(':', c2 + 1);
      if (c3 == std::string::npos) continue;
      const std::string col = raw.substr(c2 + 1, c3 - c2 - 1);
      m.col = digits(col) ? std::stoi(col) : 1;
      m.text = raw.substr(c3 + 1);
    } else {
      m.text = raw.substr(c2 + 1);
      std::smatch sm;
      if (found && std::regex_search(m.text, sm, *found)) m.col = int(sm.position(0)) + 1;
    }
    matches.push_back(std::move(m));
  }
  return matches;
}

bool run(const std::string& root, const Query& q, std::vector<Match>& out, std::string& error) {
  bool has_col = true;
  const auto argv = command(root, q, has_col);
  Reader r = spawn_reader(argv, root);
  if (r.pid < 0) {
    error = "cannot run " + argv[0];
    return false;
  }
  std::string text, err;
  pollfd fds[2] = {{r.out, POLLIN, 0}, {r.err, POLLIN, 0}};
  int open = 2;
  char buf[65536];
  while (open > 0) {
    if (poll(fds, 2, -1) < 0 && errno != EINTR) break;
    for (int k = 0; k < 2; ++k) {
      if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      const ssize_t n = read(fds[k].fd, buf, sizeof buf);
      if (n > 0) (k == 0 ? text : err).append(buf, size_t(n));
      else if (n == 0 || (errno != EAGAIN && errno != EINTR)) close(fds[k].fd), fds[k].fd = -1, --open;
    }
  }
  int st = 0;
  while (waitpid(r.pid, &st, 0) < 0 && errno == EINTR) {
  }
  const int code = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
  if (code > 1) {  // 1: nothing found
    err = trim(err);
    error = err.empty() ? argv[0] + " failed" : err.substr(err.rfind('\n') == std::string::npos ? 0 : err.rfind('\n') + 1);
    return false;
  }
  out = parse(text, q, has_col);
  return true;
}

std::vector<std::string> split_tags(const std::string& text) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= text.size()) {
    const size_t end = std::min(text.find(',', at), text.size());
    if (std::string t = trim(text.substr(at, end - at)); !t.empty()) out.push_back(t);
    at = end + 1;
  }
  return out;
}

Query tasks_query(const std::vector<std::string>& tags) {
  std::string alt;
  for (const auto& t : tags) alt += (alt.empty() ? "" : "|") + escape_regex(t);
  return {"\\b(" + alt + ")\\b", true, true};
}

std::vector<Task> tasks_in(const std::vector<Match>& matches, const std::vector<std::string>& tags) {
  std::vector<Task> out;
  if (tags.empty()) return out;
  const std::regex word(tasks_query(tags).pattern);
  for (const auto& m : matches) {
    std::smatch sm;
    if (!std::regex_search(m.text, sm, word)) continue;
    const size_t at = size_t(sm.position(0));
    out.push_back({sm[1].str(), Match{m.path, m.line, int(at) + 1, trim(m.text.substr(at))}});
  }
  return out;
}

}  // namespace search

namespace {

using mp::Array;
using mp::Map;
using mp::Value;
using search::Match;

constexpr const char* kResults = "search";
constexpr const char* kTasks = "tasks";
constexpr const char* kDefaultTags = "FIXME, XXX, TODO, HACK";

std::string plural(size_t n, const char* one, const char* many) {
  return std::to_string(n) + " " + (n == 1 ? one : many);
}

std::string squote(const std::string& s) { return "'" + s + "'"; }

std::string trim_copy(const std::string& s) {
  const auto b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

Value text(Value t, std::string_view style = "", bool wrap = false) {
  Map n{{"t", "text"}, {"text", std::move(t)}};
  if (!style.empty()) n.emplace_back("style", style);
  if (wrap) n.emplace_back("wrap", true);
  return n;
}

Value sel(const std::string& root, const Match& m, const char* what) {
  const std::string path = (std::filesystem::path(root) / m.path).string();
  const std::string line = trim_copy(m.text);
  return Array{Map{{"type", "search.match"},
                   {"label", m.path + ":" + std::to_string(m.line)},
                   {"path", path},
                   {"line", m.line},
                   {"props", Array{Array{what, line}}}}};
}

}  // namespace

// A program the service runs, read as the event loop finds it readable.
struct SearchRun {
  Reader r;
  std::string out, err;
  bool has_col = true;
  search::Query q;
  std::string tool;
  bool tasks = false;  // for the Tasks view, not Search
};

class SearchService final : public Service {
 public:
  explicit SearchService(Hub& hub) : Service(hub) {}
  ~SearchService() override {
    for (auto* r : {&search_, &scan_}) stop(*r);
  }

  std::vector<ViewInfo> views() const override {
    return {{kResults, "Search", "", false, Region::bottom}, {kTasks, "Tasks", "", false, Region::bottom}};
  }
  std::vector<CommandInfo> commands() const override {
    return {{"search.find", "Search: Find in files", {"ctrl+f"}, "", ""},
            {"search.again", "Search: Search again", {"f5"}, kResults, ""},
            {"search.expandAll", "Search: Open all files", {"*"}, kResults, ""},
            {"search.collapseAll", "Search: Close all files", {"/"}, kResults, ""},
            {"tasks.show", "Tasks: Show (TODO, FIXME, ...)", {}, "", ""},
            {"tasks.refresh", "Tasks: Scan again", {"r", "f5"}, kTasks, ""}};
  }
  std::vector<PrefInfo> prefs() const override {
    return {{"search.tags", "Task tags",
             "The words that make a comment a task, the first ones the most urgent (comma separated).", "", "string",
             kDefaultTags, {}, ""}};
  }

  void opened(const std::string& view) override {
    std::error_code ec;
    if (view == kTasks) {
      tasks_root_ = std::filesystem::current_path(ec).string();
      scan();
    } else {
      root_ = std::filesystem::current_path(ec).string();
    }
  }
  void closed(const std::string& view) override {
    if (view == kTasks) stop(scan_);
    else stop(search_);
  }

  std::string title(const std::string& view) override {
    if (view == kTasks) return tasks_.empty() ? "Tasks" : "Tasks (" + std::to_string(tasks_.size()) + ")";
    return pattern_.empty() ? "Search" : "Search: " + pattern_;
  }

  Value tree(const std::string& view) override { return view == kTasks ? tasks_tree() : results_tree(); }

  bool command(const std::string& id, const std::string& view) override {
    if (id == "search.find") return hub_.show_view(kResults), true;
    if (id == "tasks.show") return hub_.show_view(kTasks), true;
    if (id == "tasks.refresh") return view == kTasks && (scan(), true);
    if (view != kResults) return false;
    if (id == "search.again") {
      if (!pattern_.empty()) find(pattern_);
    } else if (id == "search.expandAll") {
      closed_.clear();
      flatten();
    } else if (id == "search.collapseAll") {
      closed_.clear();
      for (const auto& [path, ms] : files_) closed_.insert(path);
      flatten();
      move_cursor(0);
    } else {
      return false;
    }
    hub_.service_changed(kResults);
    return true;
  }

  void event(const std::string& view, std::string_view widget, std::string_view kind, const Value& value) override {
    if (view == kTasks) {
      if (widget == "tasks" && kind == "activate") open_key(tasks_root_, std::string(value.str()));
      return;
    }
    if (widget == "pattern" && kind == "submit") {
      find(std::string(value.str()));
    } else if ((widget == "regex" || widget == "case") && kind == "toggle") {
      (widget == "regex" ? q_.regex : q_.match_case) = value.boolean();
      if (!pattern_.empty()) find(pattern_);
    } else if (widget == "matches") {
      const std::string key(value.str());
      if (kind == "activate") activate(key);
      else if (kind == "expand") expand(key);
      else if (kind == "collapse") collapse(key);
    }
    hub_.service_changed(kResults);
  }

  void prefs_changed() override {
    if (hub_.find_view(kTasks)) hub_.service_changed(kTasks);
  }

  void add_fds(std::vector<pollfd>& fds) const override {
    for (const auto* r : {&search_, &scan_}) {
      if (!r->has_value()) continue;
      for (int fd : {(*r)->r.out, (*r)->r.err})
        if (fd >= 0) fds.push_back({fd, POLLIN, 0});
    }
  }

  void on_readable(int fd) override {
    for (auto* r : {&search_, &scan_}) {
      if (!r->has_value()) continue;
      SearchRun& run = **r;
      const bool out = fd == run.r.out;
      if (!out && fd != run.r.err) continue;
      char buf[65536];
      for (;;) {
        const ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) {
          (out ? run.out : run.err).append(buf, size_t(n));
          continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return;
        close(fd);
        (out ? run.r.out : run.r.err) = -1;
        break;
      }
      if (run.r.out < 0 && run.r.err < 0) finished(*r);
      return;
    }
  }

 private:
  // ---- running ------------------------------------------------------------------------
  void stop(std::optional<SearchRun>& r) {
    if (!r) return;
    for (int fd : {r->r.out, r->r.err})
      if (fd >= 0) close(fd);
    kill(-r->r.pid, SIGTERM);
    reap(r->r.pid, 200);
    r.reset();
  }

  bool start(std::optional<SearchRun>& slot, const std::string& root, const search::Query& q, bool tasks) {
    stop(slot);
    SearchRun run;
    run.q = q;
    run.tasks = tasks;
    run.tool = search::tool_name(root);
    const auto argv = search::command(root, q, run.has_col);
    run.r = spawn_reader(argv, root);
    if (run.r.pid < 0) return false;
    slot = std::move(run);
    return true;
  }

  void finished(std::optional<SearchRun>& slot) {
    SearchRun run = std::move(*slot);
    slot.reset();
    int st = 0;
    while (waitpid(run.r.pid, &st, 0) < 0 && errno == EINTR) {
    }
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
    std::string error;
    if (code > 1) {
      std::string e = trim_copy(run.err);
      if (const size_t nl = e.rfind('\n'); nl != std::string::npos) e.erase(0, nl + 1);
      error = e.empty() ? "the search failed" : e;
    }
    auto matches = error.empty() ? search::parse(run.out, run.q, run.has_col) : std::vector<Match>{};
    if (run.tasks) scanned(std::move(matches), error);
    else found(std::move(matches), error, run.tool);
  }

  // ---- Search -------------------------------------------------------------------------
  void find(const std::string& pattern) {
    pattern_ = trim_copy(pattern);
    if (pattern_.empty()) return;
    q_.pattern = pattern_;
    message_ = "Searching for " + pattern_ + "…";
    if (!start(search_, root_, q_, false)) message_ = "Search failed: cannot run the search program";
  }

  void found(std::vector<Match> matches, const std::string& error, const std::string& tool) {
    if (!error.empty()) {
      message_ = "Search failed: " + error;
      hub_.service_changed(kResults);
      return;
    }
    files_.clear();
    order_.clear();
    for (auto& m : matches) {
      auto [it, fresh] = files_.try_emplace(m.path);
      if (fresh) order_.push_back(m.path);
      it->second.push_back(std::move(m));
    }
    std::sort(order_.begin(), order_.end(), [](const std::string& a, const std::string& b) {
      return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
                                          [](char x, char y) { return std::tolower(x) < std::tolower(y); });
    });
    closed_.clear();
    const size_t n = matches.size(), f = files_.size();
    const std::string more = n >= search::kLimit ? " (the first " + std::to_string(search::kLimit) + ")" : "";
    const std::string counted = plural(n, "match", "matches") + more + " in " + plural(f, "file", "files");
    const std::string where = "for " + squote(pattern_) + " in " + root_;
    message_ = n ? counted + " " + where + " (" + tool + ")" : "Nothing found " + where;
    flatten();
    move_cursor(0);
    hub_.notify_status(n ? "Search: " + counted : "Search: nothing found for " + squote(pattern_), "normal", 5);
    hub_.service_changed(kResults);
  }

  void flatten() {
    rows_.clear();
    for (const auto& path : order_) {
      rows_.push_back({path, -1});
      if (!closed_.contains(path))
        for (size_t i = 0; i < files_[path].size(); ++i) rows_.push_back({path, int(i)});
    }
  }

  void move_cursor(size_t i) { cursor_ = {i, cursor_.second + 1}; }

  // "f:<path>" or "m:<path>:<line>:<col>" → the file and the match's index (-1: the file).
  std::pair<std::string, int> where(const std::string& key) const {
    if (key.starts_with("f:")) return {key.substr(2), -1};
    if (!key.starts_with("m:")) return {"", -2};
    const size_t c2 = key.rfind(':'), c1 = key.rfind(':', c2 - 1);
    if (c1 == std::string::npos || c1 < 2) return {"", -2};
    const std::string path = key.substr(2, c1 - 2);
    const int line = std::atoi(key.c_str() + c1 + 1), col = std::atoi(key.c_str() + c2 + 1);
    const auto it = files_.find(path);
    if (it == files_.end()) return {path, -2};
    for (size_t i = 0; i < it->second.size(); ++i)
      if (it->second[i].line == line && it->second[i].col == col) return {path, int(i)};
    return {path, -2};
  }

  void activate(const std::string& key) {
    const auto [path, i] = where(key);
    if (i == -1) return closed_.contains(path) ? expand(key) : collapse(key);
    if (i >= 0) hub_.show_location((std::filesystem::path(root_) / path).string(), files_[path][size_t(i)].line);
  }

  void expand(const std::string& key) {
    const auto [path, i] = where(key);
    if (i == -1 && closed_.erase(path)) flatten();
  }

  void collapse(const std::string& key) {
    const auto [path, i] = where(key);
    if (i == -1 && !closed_.contains(path)) {
      closed_.insert(path);
      flatten();
    } else if (i >= 0) {  // a match: to its file
      for (size_t r = 0; r < rows_.size(); ++r)
        if (rows_[r].first == path && rows_[r].second < 0) move_cursor(r);
    }
  }

  Value results_tree() {
    Array rows;
    for (const auto& [path, i] : rows_) {
      const auto& ms = files_[path];
      if (i < 0) {
        rows.push_back(Map{{"key", "f:" + path},
                           {"text", Map{{"text", path}, {"style", "bold"}}},
                           {"depth", 0},
                           {"expandable", true},
                           {"expanded", !closed_.contains(path)},
                           {"detail", plural(ms.size(), "match", "matches")},
                           {"sel", sel(root_, ms.front(), "First match")}});
        continue;
      }
      const Match& m = ms[size_t(i)];
      std::string t = trim_copy(m.text);
      if (t.size() > 300) t.resize(300);
      rows.push_back(Map{{"key", "m:" + path + ":" + std::to_string(m.line) + ":" + std::to_string(m.col)},
                         {"text", Array{Array{std::to_string(m.line) + ": ", "dim"}, Array{t, "normal"}}},
                         {"depth", 1},
                         {"sel", sel(root_, m, "Line")}});
    }
    const size_t total = rows.size();
    Map tree{{"t", "tree"}, {"id", "matches"}, {"empty", ""}, {"grow", 1}, {"rows", std::move(rows)},
             {"offset", 0}, {"total", total}};
    if (cursor_.second) tree.emplace_back("cursor", Array{cursor_.first, cursor_.second});
    Map pattern{{"t", "field"}, {"id", "pattern"}, {"value", pattern_}, {"placeholder", "Find in files…"}, {"grow", 1}};
    if (!hub_.find_view(kResults) || rows_.empty()) pattern.emplace_back("autofocus", true);
    return Map{
        {"t", "vbox"},
        {"children",
         Array{Map{{"t", "hbox"},
                   {"gap", 2},
                   {"grow", 0},
                   {"children", Array{std::move(pattern),
                                      Map{{"t", "checkbox"}, {"id", "regex"}, {"label", "regex"}, {"checked", q_.regex}},
                                      Map{{"t", "checkbox"}, {"id", "case"}, {"label", "match case"},
                                          {"checked", q_.match_case}}}}},
               text(message_, "dim"), std::move(tree),
               text(Array{Array{"Enter", "accent"}, " open   ", Array{"F5", "accent"}, " again   ",
                          Array{"*", "accent"}, "/", Array{"/", "accent"}, " open/close all files"},
                    "dim")}}};
  }

  // ---- Tasks --------------------------------------------------------------------------
  std::vector<std::string> tags() const {
    const Value v = hub_.pref("search.tags");
    return search::split_tags(v.is_str() ? std::string(v.str()) : kDefaultTags);
  }

  void scan() {
    const auto t = tags();
    if (t.empty()) {
      tasks_.clear();
      tasks_message_ = "No task tags (the preference Task tags).";
      return;
    }
    tasks_message_ = "Scanning…";
    if (!start(scan_, tasks_root_, search::tasks_query(t), true)) tasks_message_ = "Scanning failed";
  }

  void scanned(std::vector<Match> matches, const std::string& error) {
    if (!error.empty()) {
      tasks_message_ = "Scanning failed: " + error;
      hub_.service_changed(kTasks);
      return;
    }
    const auto t = tags();
    tasks_ = search::tasks_in(matches, t);
    auto rank = [&](const std::string& tag) {
      return size_t(std::find(t.begin(), t.end(), tag) - t.begin());
    };
    std::stable_sort(tasks_.begin(), tasks_.end(), [&](const search::Task& a, const search::Task& b) {
      return std::tuple(rank(a.tag), a.match.path, a.match.line) < std::tuple(rank(b.tag), b.match.path, b.match.line);
    });
    tasks_message_.clear();
    for (const auto& tag : t) {
      const auto n = std::count_if(tasks_.begin(), tasks_.end(), [&](const search::Task& k) { return k.tag == tag; });
      if (n) tasks_message_ += (tasks_message_.empty() ? "" : "   ") + tag + " " + std::to_string(n);
    }
    if (tasks_message_.empty()) tasks_message_ = "No tasks.";
    tasks_message_ += "   in " + tasks_root_;
    hub_.service_changed(kTasks);
  }

  Value tasks_tree() {
    const auto t = tags();
    std::set<std::string> urgent(t.begin(), t.begin() + std::min<size_t>(2, t.size()));
    Array rows;
    for (const auto& k : tasks_) {
      const std::string at = k.match.path + ":" + std::to_string(k.match.line);
      std::string body = k.match.text;
      if (body.size() > 300) body.resize(300);
      rows.push_back(Map{{"key", at},
                         {"cells", Array{Map{{"text", k.tag}, {"style", urgent.contains(k.tag) ? "error" : "warning"}},
                                         body, at}},
                         {"sel", sel(tasks_root_, k.match, "Task")}});
    }
    const size_t total = rows.size();
    return Map{{"t", "vbox"},
               {"children",
                Array{text(tasks_message_, "dim"),
                      Map{{"t", "table"},
                          {"id", "tasks"},
                          {"columns", Array{Map{{"title", "Tag"}, {"width", 6}}, Map{{"title", "Task"}, {"grow", 3}},
                                            Map{{"title", "Where"}, {"grow", 1}}}},
                          {"rows", std::move(rows)},
                          {"offset", 0},
                          {"total", total},
                          {"empty", scan_ ? "" : "No tasks."},
                          {"grow", 1}}}}};
  }

  void open_key(const std::string& root, const std::string& key) {
    const size_t c = key.rfind(':');
    if (c == std::string::npos) return;
    hub_.show_location((std::filesystem::path(root) / key.substr(0, c)).string(), std::atoi(key.c_str() + c + 1));
  }

  std::string root_, tasks_root_;
  std::string pattern_, message_ = "Type what to look for and press Enter.";
  search::Query q_;
  std::map<std::string, std::vector<Match>> files_;
  std::vector<std::string> order_;  // the files, by name
  std::set<std::string> closed_;
  std::vector<std::pair<std::string, int>> rows_;  // (file, -1) or (file, its match's index)
  std::pair<size_t, int64_t> cursor_{0, 0};  // a cursor request: [index, seq]
  std::optional<SearchRun> search_, scan_;
  std::vector<search::Task> tasks_;
  std::string tasks_message_ = "Scanning…";
};

std::unique_ptr<Service> make_search_service(Hub& hub) { return std::make_unique<SearchService>(hub); }

}  // namespace rcp
