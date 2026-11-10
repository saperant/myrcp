#include "outline.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>

#include "hub.hpp"
#include "service.hpp"

namespace rcp {

namespace outline {

namespace {

constexpr std::uintmax_t kMaxBytes = 4u << 20;  // bigger files get no outline
constexpr size_t kMaxLine = 600;                // longer lines are not matched (std::regex recurses)

using Parser = std::function<std::vector<Symbol>(const std::string&)>;

std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  std::istringstream in(text);
  std::string s;
  while (std::getline(in, s)) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    out.push_back(std::move(s));
  }
  return out;
}

bool matches_at_start(const std::string& line, const std::regex& rx, std::smatch& m) {
  return line.size() <= kMaxLine && std::regex_search(line, m, rx, std::regex_constants::match_continuous);
}

size_t indent_of(const std::string& line) {
  size_t n = 0;
  for (char c : line) {
    if (c == ' ') ++n;
    else if (c == '\t') n += 8 - n % 8;
    else break;
  }
  return n;
}

std::string rtrim(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
  return s;
}

// ---- Python: blocks by indentation, outside strings and brackets --------------------

std::vector<Symbol> outline_python(const std::string& text) {
  static const std::regex def_rx(R"(\s*(?:async\s+)?def\s+(\w+))"), class_rx(R"(\s*class\s+(\w+))"),
      const_rx(R"(([A-Z_][A-Z0-9_]*)\s*(?::[^=]*)?=(?!=))");
  struct Open {
    size_t indent;
    size_t symbol;
  };
  std::vector<Symbol> out;
  std::vector<Open> stack;
  int last_code = 0;
  int brackets = 0;
  std::string triple;  // inside a triple-quoted string: its quotes
  bool continued = false;
  const auto lines = lines_of(text);
  auto close_to = [&](size_t indent) {
    while (!stack.empty() && stack.back().indent >= indent) {
      out[stack.back().symbol].end = std::max(out[stack.back().symbol].line, last_code);
      stack.pop_back();
    }
  };
  for (size_t k = 0; k < lines.size(); ++k) {
    const std::string& line = lines[k];
    const int n = int(k) + 1;
    const bool starts = triple.empty() && brackets == 0 && !continued;
    // Scan the line: strings, comments, brackets.
    bool code = false;
    for (size_t i = 0; i < line.size(); ++i) {
      const char c = line[i];
      if (!triple.empty()) {
        if (line.compare(i, 3, triple) == 0) triple.clear(), i += 2;
        else if (c == '\\') ++i;
        code = true;
        continue;
      }
      if (c == '#') break;
      if (std::isspace(static_cast<unsigned char>(c))) continue;
      code = true;
      if (c == '"' || c == '\'') {
        if (line.compare(i, 3, std::string(3, c)) == 0) {
          triple = std::string(3, c);
          i += 2;
          continue;
        }
        for (++i; i < line.size() && line[i] != c; ++i)
          if (line[i] == '\\') ++i;
      } else if (c == '(' || c == '[' || c == '{') {
        ++brackets;
      } else if (c == ')' || c == ']' || c == '}') {
        brackets = std::max(0, brackets - 1);
      }
    }
    continued = !line.empty() && line.back() == '\\';
    if (!code) continue;
    if (starts) {
      const size_t indent = indent_of(line);
      close_to(indent);
      std::smatch m;
      const bool is_class = matches_at_start(line, class_rx, m);
      if (is_class || matches_at_start(line, def_rx, m)) {
        const bool in_class = !stack.empty() && out[stack.back().symbol].kind == "class";
        out.push_back({m[1].str(), is_class ? "class" : in_class ? "method" : "function", n, int(stack.size()), 0});
        stack.push_back({indent, out.size() - 1});
      } else if (indent == 0 && matches_at_start(line, const_rx, m) &&
                 std::any_of(m[1].first, m[1].second, [](char c) { return std::isalpha(static_cast<unsigned char>(c)); })) {
        out.push_back({m[1].str(), "constant", n, 0, n});
      }
    }
    last_code = n;
  }
  close_to(0);
  return out;
}

// ---- by shapes ------------------------------------------------------------------------

std::vector<Symbol> outline_markdown(const std::string& text) {
  static const std::regex rx(R"((#{1,6})\s+(.*?)\s*#*\s*$)");
  std::vector<Symbol> out;
  bool fence = false;
  int n = 0;
  for (const auto& line : lines_of(text)) {
    ++n;
    std::smatch m;
    if (line.starts_with("```") || line.starts_with("~~~")) fence = !fence;
    else if (!fence && matches_at_start(line, rx, m)) out.push_back({m[2].str(), "heading", n, int(m[1].length()) - 1, 0});
  }
  return out;
}

std::vector<Symbol> outline_sections(const std::string& text) {  // TOML, INI
  static const std::regex rx(R"(\s*(\[\[?)\s*([^\]]+?)\s*\]\]?\s*(#.*)?$)");
  std::vector<Symbol> out;
  int n = 0;
  for (const auto& line : lines_of(text)) {
    ++n;
    std::smatch m;
    if (matches_at_start(line, rx, m)) out.push_back({m[2].str(), m[1].str() == "[[" ? "table" : "section", n, 0, 0});
  }
  return out;
}

std::vector<Symbol> outline_yaml(const std::string& text) {
  static const std::regex rx(R"(( {0,4})([A-Za-z0-9_.\-"']+)\s*:(\s|$))");
  std::vector<Symbol> out;
  int n = 0;
  for (const auto& line : lines_of(text)) {
    ++n;
    std::smatch m;
    if (!matches_at_start(line, rx, m)) continue;
    const int depth = int(m[1].length()) / 2;
    if (depth > 1) continue;
    std::string name = m[2].str();
    name.erase(std::remove_if(name.begin(), name.end(), [](char c) { return c == '"' || c == '\''; }), name.end());
    out.push_back({name, "key", n, depth, 0});
  }
  return out;
}

// Line patterns (kind, regex whose first group is the name); depth from the indentation.
Parser by_patterns(std::vector<std::pair<std::string, std::string>> patterns) {
  auto compiled = std::make_shared<std::vector<std::pair<std::string, std::regex>>>();
  for (auto& [kind, rx] : patterns) compiled->emplace_back(kind, std::regex(rx));
  return [compiled](const std::string& text) {
    std::vector<Symbol> out;
    std::vector<size_t> indents;
    std::string prev;
    int n = 0;
    for (const auto& line : lines_of(text)) {
      ++n;
      const bool continued = prev.ends_with(",") || prev.ends_with("(") || prev.ends_with("&&") ||
                             prev.ends_with("||") || prev.ends_with("\\");  // the rest of a statement
      if (line.find_first_not_of(" \t") != std::string::npos) prev = rtrim(line);
      if (continued) continue;
      for (const auto& [kind, rx] : *compiled) {
        std::smatch m;
        if (!matches_at_start(line, rx, m)) continue;
        const size_t indent = indent_of(line);
        while (!indents.empty() && indents.back() >= indent) indents.pop_back();
        out.push_back({m[1].str(), kind, n, int(indents.size()), 0});
        indents.push_back(indent);
        break;
      }
    }
    return out;
  };
}

const Parser& c_like() {
  static const Parser p = by_patterns({
      {"namespace", R"(\s*namespace\s+([\w:]+)\s*\{)"},
      {"class", R"(\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|union|enum(?:\s+class)?)\s+(\w+)[^;]*$)"},
      {"function",
       R"(\s*(?!(?:if|for|while|switch|return|else|do|case|sizeof|catch|const|static_assert)\b))"
       R"((?:[\w:<>,*&~\[\]]+\s+)*?[\w:<>*&~]*?\b([A-Za-z_~][\w:~]*)\s*)"
       R"((?:\([^;{]*\)\s*(?:const|noexcept|override|final|\s)*(?:->\s*[\w:<>*&]+\s*)?\{?|\([^;{)]*,)\s*$)"},
  });
  return p;
}

const Parser& js() {
  static const Parser p = by_patterns({
      {"class", R"(\s*(?:export\s+)?(?:default\s+)?class\s+(\w+))"},
      {"function", R"(\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?function\s*\*?\s*(\w+))"},
      {"function", R"(\s*(?:export\s+)?(?:const|let|var)\s+(\w+)\s*=\s*(?:async\s*)?(?:\([^)]*\)|\w+)\s*=>)"},
      {"interface", R"(\s*(?:export\s+)?(?:interface|type)\s+(\w+))"},
      {"method", R"(\s+(?:static\s+|async\s+|get\s+|set\s+)*((?!if|for|while|switch)\w+)\s*\([^)]*\)\s*\{\s*$)"},
  });
  return p;
}

const Parser& rust() {
  static const Parser p = by_patterns({
      {"module", R"(\s*(?:pub(?:\([^)]*\))?\s+)?mod\s+(\w+))"},
      {"type", R"(\s*(?:pub(?:\([^)]*\))?\s+)?(?:struct|enum|trait|union)\s+(\w+))"},
      {"impl", R"(\s*impl(?:<[^>]*>)?\s+([\w:<>, ]+?)\s*(?:where.*)?\{)"},
      {"function", R"(\s*(?:pub(?:\([^)]*\))?\s+)?(?:const\s+|async\s+|unsafe\s+)*fn\s+(\w+))"},
  });
  return p;
}

const Parser& go() {
  static const Parser p = by_patterns({{"type", R"(type\s+(\w+))"}, {"function", R"(func\s+(?:\([^)]*\)\s*)?(\w+))"}});
  return p;
}

const Parser& shell() {
  static const Parser p = by_patterns({{"function", R"(\s*(?:function\s+)?([\w.:-]+)\s*\(\)\s*\{?)"}});
  return p;
}

const Parser* parser_for(const std::string& name, const std::string& first_line) {
  static const Parser py = outline_python, md = outline_markdown, sections = outline_sections, yaml = outline_yaml;
  static const std::map<std::string, const Parser*> by_ext = {
      {".py", &py},      {".pyi", &py},       {".md", &md},        {".markdown", &md}, {".toml", &sections},
      {".ini", &sections}, {".cfg", &sections}, {".yaml", &yaml},  {".yml", &yaml},    {".c", &c_like()},
      {".h", &c_like()}, {".cc", &c_like()},  {".cpp", &c_like()}, {".cxx", &c_like()}, {".hpp", &c_like()},
      {".hh", &c_like()}, {".hxx", &c_like()}, {".js", &js()},     {".mjs", &js()},    {".cjs", &js()},
      {".ts", &js()},    {".tsx", &js()},     {".jsx", &js()},     {".rs", &rust()},   {".go", &go()},
      {".sh", &shell()}, {".bash", &shell()}, {".zsh", &shell()}};
  std::string ext = std::filesystem::path(name).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (const auto it = by_ext.find(ext); it != by_ext.end()) return it->second;
  if (first_line.starts_with("#!")) {  // a script without an extension: by its #! line
    if (first_line.find("python") != std::string::npos) return &py;
    if (first_line.find("sh") != std::string::npos) return &shell();
  }
  return nullptr;
}

// Ends from the next symbol at the same or a shallower depth, where not known.
void fill_ends(std::vector<Symbol>& symbols, int lines) {
  for (size_t i = 0; i < symbols.size(); ++i) {
    if (symbols[i].end) continue;
    int next = lines;
    for (size_t j = i + 1; j < symbols.size(); ++j) {
      if (symbols[j].depth <= symbols[i].depth) {
        next = symbols[j].line - 1;
        break;
      }
    }
    symbols[i].end = std::max(symbols[i].line, next);
  }
}

}  // namespace

bool of_text(const std::string& name, const std::string& text, std::vector<Symbol>& out) {
  const Parser* p = parser_for(name, text.substr(0, std::min<size_t>(200, text.find('\n'))));
  if (!p) return false;
  out = (*p)(text);
  fill_ends(out, int(std::count(text.begin(), text.end(), '\n')) + 1);
  return true;
}

bool of_file(const std::string& path, std::vector<Symbol>& out, std::string& error) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    error = ec.message();
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  std::string first;
  std::getline(in, first);
  const std::string ext = std::filesystem::path(path).extension().string();
  if (!parser_for(path, first)) {
    error = "no outline for " + (ext.empty() ? std::filesystem::path(path).filename().string() : ext) + " files";
    return false;
  }
  if (size > kMaxBytes) {
    error = "too big to outline";
    return false;
  }
  in.seekg(0);
  std::ostringstream text;
  text << in.rdbuf();
  return of_text(path, text.str(), out);
}

int enclosing(const std::vector<Symbol>& symbols, int line) {
  int best = -1;
  for (size_t i = 0; i < symbols.size(); ++i) {
    const Symbol& s = symbols[i];
    if (s.line <= line && line <= s.end && (best < 0 || s.depth >= symbols[size_t(best)].depth)) best = int(i);
  }
  return best;
}

}  // namespace outline

namespace {

using mp::Array;
using mp::Map;
using mp::Value;

constexpr const char* kOutline = "outline";

const char* kind_style(const std::string& k) {
  static const std::map<std::string, const char*> m = {
      {"class", "accent"},   {"type", "accent"},  {"interface", "accent"}, {"namespace", "special"},
      {"module", "special"}, {"impl", "special"}, {"function", "ok"},      {"method", "ok"},
      {"constant", "info"},  {"heading", "bold"}, {"section", "bold"},     {"table", "bold"},
      {"key", "normal"}};
  const auto it = m.find(k);
  return it == m.end() ? "dim" : it->second;
}

const char* kind_mark(const std::string& k) {
  static const std::map<std::string, const char*> m = {
      {"class", "C"},    {"type", "T"},     {"interface", "I"}, {"namespace", "N"}, {"module", "M"},
      {"impl", "I"},     {"function", "ƒ"}, {"method", "m"},    {"constant", "K"},  {"heading", "#"},
      {"section", "§"},  {"table", "§"},    {"key", "·"}};
  const auto it = m.find(k);
  return it == m.end() ? "·" : it->second;
}

class OutlineService final : public Service {
 public:
  explicit OutlineService(Hub& hub) : Service(hub) {}

  std::vector<ViewInfo> views() const override { return {{kOutline, "Outline", "", false, Region::side}}; }
  std::vector<CommandInfo> commands() const override {
    return {{"outline.show", "Outline: Show", {}, "", ""},
            {"outline.follow", "Outline: Follow the selection (on/off)", {"f"}, kOutline, ""}};
  }

  void opened(const std::string&) override {
    const auto& s = hub_.selection();
    follow(s.view, s.items);
  }

  std::string title(const std::string&) override {
    return path_.empty() ? "Outline" : "Outline: " + std::filesystem::path(path_).filename().string();
  }

  bool command(const std::string& id, const std::string& view) override {
    if (id == "outline.show") return hub_.show_view(kOutline), true;
    if (id != "outline.follow" || view != kOutline) return false;
    following_ = !following_;
    hub_.notify_status(following_ ? "Outline follows the selection" : "Outline stays on this file", "normal", 3);
    hub_.service_changed(kOutline);
    return true;
  }

  void selection_changed(const std::string& from, const Value& items) override {
    if (!hub_.find_view(kOutline)) return;
    if (follow(from, items)) hub_.service_changed(kOutline);
  }

  void event(const std::string&, std::string_view widget, std::string_view kind, const Value& value) override {
    if (widget != "symbols" || kind != "activate" || path_.empty()) return;
    const std::string key(value.str());
    const size_t i = size_t(std::atoi(key.c_str()));
    if (!key.empty() && std::isdigit(static_cast<unsigned char>(key[0])) && i < symbols_.size())
      hub_.show_location(path_, symbols_[i].line);
  }

  Value tree(const std::string&) override {
    Array rows;
    for (size_t i = 0; i < symbols_.size(); ++i) {
      const auto& s = symbols_[i];
      rows.push_back(Map{{"key", std::to_string(i)},
                         {"text", Array{Array{std::string(kind_mark(s.kind)) + " ", kind_style(s.kind)},
                                        Array{s.name, int(i) == at_ ? "bold" : "normal"}}},
                         {"depth", s.depth},
                         {"detail", std::to_string(s.line)},
                         {"sel", Array{Map{{"type", "outline." + s.kind},
                                           {"label", s.name},
                                           {"path", path_},
                                           {"line", s.line},
                                           {"props", Array{Array{"Kind", s.kind}, Array{"Line", s.line}}}}}}});
    }
    const size_t total = rows.size();
    Map t{{"t", "tree"}, {"id", "symbols"}, {"empty", ""}, {"grow", 1}, {"rows", std::move(rows)}, {"offset", 0},
          {"total", total}};
    if (cursor_seq_) t.emplace_back("cursor", Array{cursor_, cursor_seq_});
    return Map{{"t", "vbox"},
               {"children", Array{Map{{"t", "text"},
                                      {"text", message_ + (following_ ? "" : "  (not following: f)")},
                                      {"style", "dim"},
                                      {"wrap", true}},
                                  std::move(t)}}};
  }

 private:
  // Outlines the file of the selection's first item with a path. True: changed.
  bool follow(const std::string& from, const Value& items) {
    if (!following_ || from == kOutline) return false;  // its own rows are the outline, not where to outline
    const Value* item = nullptr;
    for (const auto& i : items.array()) {
      if (!i["path"].str().empty()) {
        item = &i;
        break;
      }
    }
    if (!item) return false;
    const std::string path((*item)["path"].str());
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return false;
    const auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    const std::string name = std::filesystem::path(path).filename().string();
    if (path != path_ || mtime != mtime_) {
      path_ = path;
      mtime_ = mtime;
      std::string error;
      symbols_.clear();
      if (outline::of_file(path, symbols_, error))
        message_ = symbols_.empty() ? "Nothing to outline in " + name : std::to_string(symbols_.size()) + " in " + name;
      else
        message_ = name + ": " + error;
      at_ = -1;
    }
    const int line = int((*item)["line"].integer());
    const int at = line > 0 ? outline::enclosing(symbols_, line) : -1;
    if (at >= 0 && at != at_) cursor_ = at, ++cursor_seq_;
    at_ = at;
    return true;
  }

  std::string path_;
  std::filesystem::file_time_type mtime_{};
  std::vector<outline::Symbol> symbols_;
  int at_ = -1;  // the symbol around the selection's line
  int cursor_ = 0;
  int64_t cursor_seq_ = 0;
  bool following_ = true;
  std::string message_ = "The outline of the file the selection is in (a linked editor's cursor, a match, a problem).";
};

}  // namespace

std::unique_ptr<Service> make_outline_service(Hub& hub) { return std::make_unique<OutlineService>(hub); }

}  // namespace rcp
