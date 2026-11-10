// Outline: the structure of the file the workbench selection is in, as Eclipse's
// Outline and NetBeans' Navigator. Python by its indentation; Markdown, TOML/INI, YAML,
// C/C++, JavaScript/TypeScript, Rust, Go and shell scripts by their lines' shapes.
#pragma once

#include <string>
#include <vector>

namespace rcp::outline {

struct Symbol {
  std::string name;
  std::string kind;  // class, function, method, heading, section, key, ...
  int line = 0;      // 1-based
  int depth = 0;
  int end = 0;  // its last line
};

// The outline of a file; false with `error` when it has none (no parser, too big).
bool of_file(const std::string& path, std::vector<Symbol>& out, std::string& error);
// The outline of text as a file of that name would have it (its extension, its #! line).
bool of_text(const std::string& name, const std::string& text, std::vector<Symbol>& out);
// The innermost symbol around a line; -1 when none.
int enclosing(const std::vector<Symbol>& symbols, int line);

}  // namespace rcp::outline
