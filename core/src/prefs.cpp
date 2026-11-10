#include "prefs.hpp"

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace rcp {

using mp::Array;
using mp::Value;

// ---- values of a setting's type -----------------------------------------------------

namespace {

std::string trim(std::string_view s) {
  const auto b = s.find_first_not_of(" \t\r");
  if (b == std::string_view::npos) return "";
  return std::string(s.substr(b, s.find_last_not_of(" \t\r") - b + 1));
}

std::string lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

Value pref_from_text(const PrefInfo& p, std::string_view text, std::string& error) {
  const std::string t = p.type == "string" ? std::string(text) : trim(text);
  if (p.type == "bool") {
    const std::string l = lower(t);
    if (l == "1" || l == "true" || l == "yes" || l == "on") return true;
    if (l == "0" || l == "false" || l == "no" || l == "off") return false;
    error = "“" + t + "” is not true or false";
    return Value();
  }
  if (p.type == "int") {
    int64_t n = 0;
    const auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
    if (ec != std::errc() || end != t.data() + t.size() || t.empty()) {
      error = "“" + t + "” is not a whole number";
      return Value();
    }
    return n;
  }
  if (p.type == "choice") {
    if (std::find(p.choices.begin(), p.choices.end(), t) == p.choices.end()) {
      std::string all;
      for (const auto& c : p.choices) all += (all.empty() ? "" : ", ") + c;
      error = "“" + t + "” is not one of " + all;
      return Value();
    }
    return t;
  }
  if (p.type == "paths") {
    Array out;
    size_t s = 0;
    while (s <= t.size()) {
      size_t e = t.find(':', s);
      if (e == std::string::npos) e = t.size();
      if (const std::string part = trim(std::string_view(t).substr(s, e - s)); !part.empty()) out.emplace_back(part);
      s = e + 1;
    }
    return out;
  }
  return t;
}

std::string pref_to_text(const Value& v) {
  if (std::holds_alternative<bool>(v.v)) return v.boolean() ? "true" : "false";
  if (v.is_int()) return std::to_string(v.integer());
  if (v.is_array()) {
    std::string out;
    for (const auto& s : v.array()) out += (out.empty() ? "" : ":") + std::string(s.str());
    return out;
  }
  return std::string(v.str());
}

bool pref_fits(const PrefInfo& p, const Value& v) {
  if (p.type == "bool") return std::holds_alternative<bool>(v.v);
  if (p.type == "int") return v.is_int();
  if (p.type == "choice") return v.is_str() && std::find(p.choices.begin(), p.choices.end(), v.str()) != p.choices.end();
  if (p.type == "paths") {
    return v.is_array() && std::all_of(v.array().begin(), v.array().end(), [](const Value& s) { return s.is_str(); });
  }
  return v.is_str();
}

// ---- the TOML subset ----------------------------------------------------------------

namespace {

struct Incomplete {};  // the value goes on past the text: read another line

class Reader {
 public:
  explicit Reader(std::string_view s) : s_(s) {}

  Value value() {
    space(false);
    if (eof()) throw Incomplete{};
    const char c = s_[i_];
    if (c == '"') return basic();
    if (c == '\'') return literal();
    if (c == '[') return array();
    if (s_.substr(i_, 4) == "true") return i_ += 4, Value(true);
    if (s_.substr(i_, 5) == "false") return i_ += 5, Value(false);
    if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) return integer();
    throw std::string("a value is a string, true, false, a whole number or an array");
  }
  // Only blanks or a comment may follow.
  void end() {
    space(false);
    if (!eof() && s_[i_] != '#') throw std::string("unexpected “" + std::string(s_.substr(i_, 1)) + "” after the value");
  }

 private:
  bool eof() const { return i_ >= s_.size(); }
  void space(bool newlines) {
    while (!eof()) {
      const char c = s_[i_];
      if (c == ' ' || c == '\t' || c == '\r') ++i_;
      else if (newlines && c == '\n') ++i_;
      else if (newlines && c == '#') { while (!eof() && s_[i_] != '\n') ++i_; }
      else break;
    }
  }
  Value basic() {
    std::string out;
    for (++i_;; ++i_) {
      if (eof() || s_[i_] == '\n') throw std::string("a string without its closing quote");
      const char c = s_[i_];
      if (c == '"') return ++i_, Value(out);
      if (c != '\\') {
        out += c;
        continue;
      }
      if (++i_ >= s_.size()) throw std::string("a string without its closing quote");
      switch (s_[i_]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        default: throw std::string("an unknown escape \\") + s_[i_];
      }
    }
  }
  Value literal() {
    const size_t e = s_.find('\'', i_ + 1);
    if (e == std::string_view::npos || s_.substr(i_, e - i_).find('\n') != std::string_view::npos)
      throw std::string("a string without its closing quote");
    std::string out(s_.substr(i_ + 1, e - i_ - 1));
    i_ = e + 1;
    return out;
  }
  Value integer() {
    std::string digits;
    if (s_[i_] == '+' || s_[i_] == '-') digits += s_[i_++];
    while (!eof() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '_')) {
      if (s_[i_] != '_') digits += s_[i_];
      ++i_;
    }
    int64_t n = 0;
    const char* b = digits.data() + (digits[0] == '+');
    const auto [p, ec] = std::from_chars(b, digits.data() + digits.size(), n);
    if (ec != std::errc() || p != digits.data() + digits.size()) throw std::string("not a whole number");
    return n;
  }
  Value array() {
    Array out;
    ++i_;
    for (;;) {
      space(true);
      if (eof()) throw Incomplete{};
      if (s_[i_] == ']') return ++i_, Value(std::move(out));
      out.push_back(value());
      space(true);
      if (eof()) throw Incomplete{};
      if (s_[i_] == ',') ++i_;
      else if (s_[i_] != ']') throw std::string("array items are separated by commas");
    }
  }

  std::string_view s_;
  size_t i_ = 0;
};

bool bare_key(std::string_view k) {
  return !k.empty() && std::all_of(k.begin(), k.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
  });
}

std::string quote(std::string_view s) {
  std::string out = "\"";
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\', out += c;
    else if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c == '\r') out += "\\r";
    else out += c;
  }
  return out + "\"";
}

std::string toml(const Value& v) {
  if (std::holds_alternative<bool>(v.v)) return v.boolean() ? "true" : "false";
  if (v.is_int()) return std::to_string(v.integer());
  if (v.is_array()) {
    std::string out = "[";
    for (const auto& x : v.array()) out += (out.size() > 1 ? ", " : "") + toml(x);
    return out + "]";
  }
  return quote(v.str());
}

std::pair<std::string, std::string> split_id(const std::string& id) {
  const auto dot = id.rfind('.');
  if (dot == std::string::npos) return {"", id};
  return {id.substr(0, dot), id.substr(dot + 1)};
}

}  // namespace

bool PrefsFile::load(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return parse("", error);  // none yet
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (!parse(text, error)) {
    error = path + ":" + error;
    return false;
  }
  return true;
}

bool PrefsFile::parse(std::string_view text, std::string& error) {
  lines_.clear();
  entries_.clear();
  sections_.clear();
  size_t s = 0;
  while (s < text.size()) {
    size_t e = text.find('\n', s);
    if (e == std::string_view::npos) e = text.size();
    lines_.emplace_back(text.substr(s, e - s));
    s = e + 1;
  }
  std::string section;
  for (size_t i = 0; i < lines_.size(); ++i) {
    const std::string line = trim(lines_[i]);
    if (line.empty() || line[0] == '#') continue;
    const std::string where = std::to_string(i + 1) + ": ";
    if (line[0] == '[') {
      const auto close = line.find(']');
      if (close == std::string::npos || !trim(line.substr(close + 1)).empty() || trim(line.substr(1, close - 1)).empty()) {
        error = where + "a section is [name]";
        return false;
      }
      section = trim(line.substr(1, close - 1));
      sections_[section] = i;
      continue;
    }
    const auto eq = line.find('=');
    std::string key = eq == std::string::npos ? "" : trim(line.substr(0, eq));
    if (key.size() >= 2 && key.front() == '"' && key.back() == '"') key = key.substr(1, key.size() - 2);
    else if (!bare_key(key)) key.clear();
    if (key.empty()) {
      error = where + "expected key = value";
      return false;
    }
    std::string value = line.substr(eq + 1);
    size_t count = 1;
    for (;;) {
      try {
        Reader r(value);
        Value v = r.value();
        r.end();
        entries_[section.empty() ? key : section + "." + key] = {std::move(v), i, count};
        break;
      } catch (const Incomplete&) {
        if (i + count >= lines_.size()) {
          error = where + "an array without its closing ]";
          return false;
        }
        value += "\n" + lines_[i + count++];
      } catch (const std::string& msg) {
        error = where + msg;
        return false;
      }
    }
    i += count - 1;
  }
  return true;
}

const Value& PrefsFile::get(const std::string& id) const {
  const auto it = entries_.find(id);
  return it == entries_.end() ? Value::nil() : it->second.value;
}

void PrefsFile::set(const std::string& id, const Value& v) {
  // Lines from `pos` on move by `d`.
  auto shift = [&](size_t pos, std::ptrdiff_t d) {
    for (auto& [k, e] : entries_) {
      if (e.first >= pos) e.first = size_t(std::ptrdiff_t(e.first) + d);
    }
    for (auto& [k, line] : sections_) {
      if (line >= pos) line = size_t(std::ptrdiff_t(line) + d);
    }
  };
  const auto [section, key] = split_id(id);
  const std::string line = (bare_key(key) ? key : quote(key)) + " = " + (v.is_nil() ? "" : toml(v));
  if (const auto it = entries_.find(id); it != entries_.end()) {
    const size_t first = it->second.first, count = it->second.count;
    lines_.erase(lines_.begin() + std::ptrdiff_t(first), lines_.begin() + std::ptrdiff_t(first + count));
    entries_.erase(it);
    shift(first + 1, -std::ptrdiff_t(count));
    if (v.is_nil()) return;
    lines_.insert(lines_.begin() + std::ptrdiff_t(first), line);
    shift(first, 1);
    entries_[id] = {v, first, 1};
    return;
  }
  if (v.is_nil()) return;
  size_t pos;
  if (const auto sec = sections_.find(section); sec != sections_.end() || section.empty()) {
    // After the section's last setting (or its header).
    pos = section.empty() ? 0 : sec->second + 1;
    for (const auto& [k, e] : entries_) {
      if (split_id(k).first == section) pos = std::max(pos, e.first + e.count);
    }
  } else {
    if (!lines_.empty() && !trim(lines_.back()).empty()) lines_.emplace_back();
    sections_[section] = lines_.size();
    lines_.push_back("[" + section + "]");
    pos = lines_.size();
  }
  shift(pos, 1);
  lines_.insert(lines_.begin() + std::ptrdiff_t(pos), line);
  entries_[id] = {v, pos, 1};
}

std::string PrefsFile::text() const {
  std::string out;
  for (const auto& l : lines_) out += l + "\n";
  return out;
}

bool PrefsFile::save(const std::string& path, std::string& error) const {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    const std::string data = text();
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) {
      error = "cannot write " + tmp;
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) error = "cannot write " + path + ": " + ec.message();
  return !ec;
}

}  // namespace rcp
