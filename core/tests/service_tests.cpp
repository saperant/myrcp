// Tests of the hub's services: Search and Tasks (search.cpp), Outline (outline.cpp).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <poll.h>
#include <string>
#include <vector>

#include "hub.hpp"
#include "outline.hpp"
#include "process.hpp"
#include "search.hpp"

namespace {

namespace fs = std::filesystem;
using rcp::mp::Map;
using rcp::mp::Value;

int failures = 0, checks = 0;

#define CHECK(cond)                                                         \
  do {                                                                      \
    ++checks;                                                               \
    if (!(cond)) {                                                          \
      ++failures;                                                           \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
    }                                                                       \
  } while (0)

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream(p) << text;
}

fs::path project() {
  const fs::path work = fs::temp_directory_path() / ("rcp-search-" + std::to_string(getpid()));
  fs::remove_all(work);
  write(work / "src" / "a.py", "x = 1  # TODO: name it\nneedle = 2\n");
  write(work / "src" / "b.py", "# FIXME: broken\nprint('needle', 'Needle')\n");
  write(work / "notes.txt", "no match here\n");
  return work;
}

// A node with this id anywhere in a tree.
const Value* widget(const Value& tree, std::string_view id) {
  if (tree["id"].is_str() && tree["id"].str() == id) return &tree;
  for (const auto& c : tree["children"].array())
    if (const Value* hit = widget(c, id)) return hit;
  return nullptr;
}

std::vector<std::string> keys(const Value& node) {
  std::vector<std::string> out;
  for (const auto& r : node["rows"].array()) out.emplace_back(r["key"].str());
  return out;
}

void test_parse() {
  const rcp::search::Query q{"needle", false, false};
  const auto rg = rcp::search::parse("src/b.py:2:8:print('needle')\n./x.py:10:1:a:b\nnot a match\n", q, true);
  CHECK(rg.size() == 2);
  CHECK(rg[0].path == "src/b.py" && rg[0].line == 2 && rg[0].col == 8 && rg[0].text == "print('needle')");
  CHECK(rg[1].path == "x.py" && rg[1].line == 10 && rg[1].text == "a:b");
  // grep has no column: it is found here, case-insensitively unless asked.
  const auto grep = rcp::search::parse("./b.py:2:print('Needle', 'needle')\n", q, false);
  CHECK(grep.size() == 1 && grep[0].col == 8);
  const auto exact = rcp::search::parse("./b.py:2:print('Needle', 'needle')\n", {"needle", false, true}, false);
  CHECK(exact.size() == 1 && exact[0].col == 18);
  CHECK((rcp::search::split_tags(" FIXME, ,TODO ") == std::vector<std::string>{"FIXME", "TODO"}));
}

void test_search_run() {
  const fs::path work = project();
  std::vector<rcp::search::Match> found;
  std::string error;
  CHECK(rcp::search::run(work.string(), {"needle", false, false}, found, error));
  std::vector<std::string> paths;
  for (const auto& m : found) paths.push_back(m.path);
  std::sort(paths.begin(), paths.end());
  CHECK((paths == std::vector<std::string>{"src/a.py", "src/b.py"}));
  // What version control ignores is left out (rg, git grep).
  if (rcp::on_path("git")) {
    write(work / "build" / "gen.py", "needle\n");
    write(work / ".gitignore", "build/\n");
    rcp::run_output({"git", "-C", work.string(), "init", "-q"});
    CHECK(rcp::search::run(work.string(), {"needle", false, false}, found, error));
    CHECK(std::none_of(found.begin(), found.end(), [](const auto& m) { return m.path.starts_with("build/"); }));
  }
  // Tasks: the tag and the comment from it on.
  const std::vector<std::string> tags{"FIXME", "TODO"};
  CHECK(rcp::search::run(work.string(), rcp::search::tasks_query(tags), found, error));
  auto tasks = rcp::search::tasks_in(found, tags);
  std::sort(tasks.begin(), tasks.end(), [](const auto& a, const auto& b) { return a.match.path < b.match.path; });
  CHECK(tasks.size() == 2);
  CHECK(tasks.size() == 2 && tasks[0].tag == "TODO" && tasks[0].match.text == "TODO: name it");
  CHECK(tasks.size() == 2 && tasks[1].tag == "FIXME" && tasks[1].match.line == 1);
  fs::remove_all(work);
}

constexpr const char* kPy = R"("""m"""
LIMIT = 3


class Shape:
    def area(self):
        return 0

    async def draw(self):
        pass


def main():
    print(Shape().area())
)";

void test_outline_parsers() {
  using rcp::outline::Symbol;
  std::vector<Symbol> s;
  CHECK(rcp::outline::of_text("m.py", kPy, s));
  std::vector<std::string> got;
  for (const auto& x : s) got.push_back(x.name + "/" + x.kind + "/" + std::to_string(x.line) + "/" + std::to_string(x.depth));
  CHECK((got == std::vector<std::string>{"LIMIT/constant/2/0", "Shape/class/5/0", "area/method/6/1", "draw/method/9/1",
                                         "main/function/13/0"}));
  CHECK(s[size_t(rcp::outline::enclosing(s, 7))].name == "area");
  CHECK(s[size_t(rcp::outline::enclosing(s, 14))].name == "main");
  // Brackets and strings do not end a block.
  CHECK(rcp::outline::of_text("n.py", "def f(a,\nb):\n    x = '''\ndef g():\n'''\n    return x\n", s));
  CHECK(s.size() == 1 && s[0].name == "f" && s[0].end == 6);

  CHECK(rcp::outline::of_text("doc.md", "# Top\n```\n# not a heading\n```\n## Sub\ntext\n", s));
  CHECK(s.size() == 2 && s[0].name == "Top" && s[0].depth == 0 && s[1].name == "Sub" && s[1].depth == 1);

  CHECK(rcp::outline::of_text("a.cpp",
                              "namespace x {\nclass Grid {\n public:\n  int width() const {\n    return w_;\n  }\n};\n"
                              "int run(int a,\n        int b) {\n  if (a) {\n  }\n}\n}  // namespace x\n",
                              s));
  got.clear();
  for (const auto& x : s) got.push_back(x.name + "/" + x.kind);
  CHECK((got == std::vector<std::string>{"x/namespace", "Grid/class", "width/function", "run/function"}));

  CHECK(rcp::outline::of_text("c.toml", "[a]\nk = 1\n[[b.c]]\n", s));
  CHECK(s.size() == 2 && s[1].name == "b.c" && s[1].kind == "table");
  CHECK(rcp::outline::of_text("run", "#!/bin/sh\nbuild() {\n  :\n}\n", s));
  CHECK(s.size() == 1 && s[0].name == "build");
  CHECK(!rcp::outline::of_text("data.bin", "x", s));
}

// ---- through the hub ------------------------------------------------------------------

struct Listener : rcp::Hub::Listener {
  std::string opened, shown;
  void open_location(const std::string& path, int line) override { opened = path + ":" + std::to_string(line); }
  void view_shown(const std::string& id) override { shown = id; }
};

// Runs the hub's event loop until `done` or a few seconds have gone.
bool pump(rcp::Hub& hub, const std::function<bool()>& done) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!done()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::vector<pollfd> fds;
    std::vector<rcp::Hub::PollToken> tokens;
    hub.add_fds(fds, tokens);
    if (fds.empty()) continue;
    if (poll(fds.data(), fds.size(), 100) <= 0) continue;
    for (size_t i = 0; i < fds.size(); ++i)
      if (fds[i].revents) hub.on_ready(tokens[i]);
  }
  return true;
}

void test_services_in_the_hub() {
  const fs::path work = project();
  const fs::path before = fs::current_path();
  fs::current_path(work);
  Listener l;
  rcp::HostOptions o;
  o.python = "true";  // no plugins: the services need none
  {
    rcp::Hub hub(o, l);
    hub.start();
    CHECK(hub.builtin_view("search") && hub.builtin_view("tasks") && hub.builtin_view("outline"));
    CHECK(hub.find_pref("search.tags") != nullptr);

    // Ctrl+F: the Search view, shown; a pattern submitted there.
    CHECK(hub.run_command("search.find", Map{}).empty());
    CHECK(l.shown == "search");
    const rcp::ViewData* v = hub.find_view("search");
    CHECK(v && v->region == rcp::Region::bottom && widget(v->tree, "pattern"));
    hub.send_event("search", "pattern", "submit", Value("needle"));
    CHECK(pump(hub, [&] { return hub.find_view("search")->title == "Search: needle" &&
                                 !keys(*widget(hub.find_view("search")->tree, "matches")).empty(); }));
    v = hub.find_view("search");
    const Value* matches = widget(v->tree, "matches");
    CHECK((keys(*matches) == std::vector<std::string>{"f:src/a.py", "m:src/a.py:2:1", "f:src/b.py", "m:src/b.py:2:8"}));
    CHECK(matches->operator[]("rows").at(0)["detail"].str() == "1 match");
    CHECK(matches->operator[]("rows").at(3)["sel"].at(0)["line"].integer() == 2);

    // A file closes to its row; Enter on a match opens it in the editor.
    hub.send_event("search", "matches", "collapse", Value("f:src/b.py"));
    CHECK(keys(*widget(hub.find_view("search")->tree, "matches")).size() == 3);
    CHECK(hub.run_command("search.expandAll", Map{{"view", "search"}}).empty());
    hub.send_event("search", "matches", "activate", Value("m:src/b.py:2:8"));
    CHECK(l.opened == (work / "src" / "b.py").string() + ":2");

    // Match case: Needle only.
    hub.send_event("search", "case", "toggle", Value(true));
    hub.send_event("search", "pattern", "submit", Value("Needle"));
    CHECK(pump(hub, [&] {
      return keys(*widget(hub.find_view("search")->tree, "matches")) ==
             std::vector<std::string>{"f:src/b.py", "m:src/b.py:2:18"};
    }));

    // Tasks: the most urgent first.
    CHECK(hub.run_command("tasks.show", Map{}).empty());
    CHECK(pump(hub, [&] { return hub.find_view("tasks")->title == "Tasks (2)"; }));
    const Value* tasks = widget(hub.find_view("tasks")->tree, "tasks");
    CHECK((keys(*tasks) == std::vector<std::string>{"src/b.py:1", "src/a.py:1"}));
    CHECK(tasks->operator[]("rows").at(0)["cells"].at(0)["text"].str() == "FIXME");

    // Outline: follows the selection's file and line; Enter goes to a symbol.
    write(work / "m.py", kPy);
    CHECK(hub.run_command("outline.show", Map{}).empty());
    CHECK(hub.find_view("outline") && hub.find_view("outline")->region == rcp::Region::side);
    hub.set_selection("term:1", rcp::mp::Array{Map{{"type", "text"}, {"label", "m.py:10"},
                                                   {"path", (work / "m.py").string()}, {"line", 10}}});
    v = hub.find_view("outline");
    CHECK(v->title == "Outline: m.py");
    const Value* symbols = widget(v->tree, "symbols");
    CHECK(symbols && symbols->operator[]("rows").array().size() == 5 && symbols->operator[]("cursor").at(0).integer() == 3);
    hub.send_event("outline", "symbols", "activate", Value("4"));
    CHECK(l.opened == (work / "m.py").string() + ":13");
    // Its own rows do not move it; with following off, nothing does.
    hub.set_selection("outline", rcp::mp::Array{Map{{"path", (work / "src" / "a.py").string()}, {"line", 1}}});
    CHECK(hub.find_view("outline")->title == "Outline: m.py");
    hub.close_view("search");
    CHECK(!hub.find_view("search"));
    hub.stop();
  }
  fs::current_path(before);
  fs::remove_all(work);
}

}  // namespace

int service_tests() {
  test_parse();
  test_search_run();
  test_outline_parsers();
  test_services_in_the_hub();
  std::printf("services: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
