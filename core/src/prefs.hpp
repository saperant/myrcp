// Preferences: settings that the workbench and plugins declare, kept in a TOML file the
// user may also edit by hand. The file is updated in place: comments, blank lines and
// the order of what is there survive myrcp's writes.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "rcp/msgpack.hpp"

namespace rcp {

// A declared setting. `id` is "<section>.<key>": "workbench.theme", "demo.hidden".
struct PrefInfo {
  std::string id, title, description;
  std::string plugin;  // empty: the workbench's
  std::string type;    // "string", "bool", "int", "choice", "paths" (a list of paths)
  mp::Value def;       // the default
  std::vector<std::string> choices;
  std::string env;  // an environment variable that, while set, wins over the file
};

// Parses `text` as a value of the setting's type (env values, what a field holds);
// "paths" splits at ':'. Returns nil with `error` set when it does not fit.
mp::Value pref_from_text(const PrefInfo& p, std::string_view text, std::string& error);
// The value as a field shows it (paths joined with ':').
std::string pref_to_text(const mp::Value& v);
// Whether `v` fits the type (and the choices).
bool pref_fits(const PrefInfo& p, const mp::Value& v);

// A subset of TOML: [section] tables; key = "string" | 'literal' | true | false |
// integer | [array of those], arrays across lines; # comments. Enough for settings.
class PrefsFile {
 public:
  // Reads the file; a missing one is empty. On a syntax error returns false, with
  // `error` naming the line, and keeps what it read before it.
  bool load(const std::string& path, std::string& error);
  bool parse(std::string_view text, std::string& error);
  const mp::Value& get(const std::string& id) const;  // nil when absent
  // Sets (or with nil removes) a value, rewriting only its own lines.
  void set(const std::string& id, const mp::Value& v);
  std::string text() const;
  bool save(const std::string& path, std::string& error) const;

 private:
  struct Entry {
    mp::Value value;
    size_t first = 0, count = 0;  // its lines
  };
  std::vector<std::string> lines_;
  std::map<std::string, Entry> entries_;
  std::map<std::string, size_t> sections_;  // section → its header line
};

}  // namespace rcp
