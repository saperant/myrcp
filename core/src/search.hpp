// Search in files and the tasks in them (TODO, FIXME, ...), as Eclipse's Search and
// Tasks views. ripgrep does the searching when it is installed, else git grep in a work
// tree, else grep; each leaves out what version control ignores as well as it can.
#pragma once

#include <string>
#include <vector>

namespace rcp::search {

inline constexpr size_t kLimit = 10000;  // matches kept; a search with more says so

struct Match {
  std::string path;  // relative to the root
  int line = 0, col = 1;  // 1-based
  std::string text;
};

struct Query {
  std::string pattern;
  bool regex = false, match_case = false;
};

// The program that prints path:line[:col]:text, one match per line, and whether it
// prints the column (grep does not).
std::vector<std::string> command(const std::string& root, const Query& q, bool& has_col);
const char* tool_name(const std::string& root);  // "ripgrep", "git grep" or "grep"

// rg's and git grep's path:line:col:text, or grep's path:line:text (the column found here).
std::vector<Match> parse(const std::string& out, const Query& q, bool has_col);

// Runs a search to the end. Returns false with `error` when the program failed.
bool run(const std::string& root, const Query& q, std::vector<Match>& out, std::string& error);

struct Task {
  std::string tag;
  Match match;  // its text from the tag on
};
// The tasks pattern for these tags (a regex for rg/grep -E), and the tasks in matches of it.
Query tasks_query(const std::vector<std::string>& tags);
std::vector<Task> tasks_in(const std::vector<Match>& matches, const std::vector<std::string>& tags);
// "FIXME, XXX, TODO, HACK" → the tags, the most urgent first.
std::vector<std::string> split_tags(const std::string& text);

}  // namespace rcp::search
