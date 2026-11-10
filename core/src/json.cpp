#include "json.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

void put_utf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += char(c);
  } else if (c < 0x800) {
    out += char(0xC0 | (c >> 6));
    out += char(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += char(0xE0 | (c >> 12));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  } else {
    out += char(0xF0 | (c >> 18));
    out += char(0x80 | ((c >> 12) & 0x3F));
    out += char(0x80 | ((c >> 6) & 0x3F));
    out += char(0x80 | (c & 0x3F));
  }
}

void put_string(std::string& out, std::string_view s) {
  out += '"';
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += ch;  // UTF-8 as is
        }
    }
  }
  out += '"';
}

void put(std::string& out, const Value& v) {
  if (v.is_nil()) {
    out += "null";
  } else if (const auto* b = std::get_if<bool>(&v.v)) {
    out += *b ? "true" : "false";
  } else if (const auto* i = std::get_if<int64_t>(&v.v)) {
    out += std::to_string(*i);
  } else if (const auto* u = std::get_if<uint64_t>(&v.v)) {
    out += std::to_string(*u);
  } else if (const auto* d = std::get_if<double>(&v.v)) {
    if (!std::isfinite(*d)) {
      out += "null";
    } else {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%.17g", *d);
      out += buf;
      if (std::string_view(buf).find_first_of(".e") == std::string_view::npos) out += ".0";  // stays a real
    }
  } else if (v.is_str()) {
    put_string(out, v.str());
  } else if (const auto* bin = std::get_if<mp::Bin>(&v.v)) {
    put_string(out, bin->data);
  } else if (v.is_array()) {
    out += '[';
    bool first = true;
    for (const auto& e : v.array()) {
      if (!first) out += ',';
      first = false;
      put(out, e);
    }
    out += ']';
  } else if (v.is_map()) {
    out += '{';
    bool first = true;
    for (const auto& [k, e] : v.map()) {
      if (!first) out += ',';
      first = false;
      if (k.is_str()) put_string(out, k.str());
      else put_string(out, to_json(k));  // JSON keys are strings
      out += ':';
      put(out, e);
    }
    out += '}';
  } else {
    out += "null";  // ext types have no JSON form
  }
}

struct Parser {
  std::string_view s;
  size_t i = 0;
  int depth = 0;

  void ws() {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }
  bool lit(std::string_view word) {
    if (s.substr(i, word.size()) != word) return false;
    i += word.size();
    return true;
  }
  bool hex4(uint32_t& c) {
    if (i + 4 > s.size()) return false;
    c = 0;
    for (int k = 0; k < 4; ++k) {
      const char h = s[i++];
      c <<= 4;
      if (h >= '0' && h <= '9') c |= uint32_t(h - '0');
      else if (h >= 'a' && h <= 'f') c |= uint32_t(h - 'a' + 10);
      else if (h >= 'A' && h <= 'F') c |= uint32_t(h - 'A' + 10);
      else return false;
    }
    return true;
  }
  std::optional<std::string> string() {
    if (i >= s.size() || s[i] != '"') return std::nullopt;
    ++i;
    std::string out;
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return std::nullopt;
      switch (s[i++]) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return std::nullopt;
          if (cp >= 0xD800 && cp < 0xDC00 && s.substr(i, 2) == "\\u") {  // a surrogate pair
            i += 2;
            uint32_t lo = 0;
            if (!hex4(lo)) return std::nullopt;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          put_utf8(out, cp);
          break;
        }
        default: return std::nullopt;
      }
    }
    return std::nullopt;
  }
  std::optional<Value> number() {
    const size_t start = i;
    bool real = false;
    if (i < s.size() && s[i] == '-') ++i;
    while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' || s[i] == 'E' ||
                            s[i] == '+' || s[i] == '-')) {
      if (s[i] == '.' || s[i] == 'e' || s[i] == 'E') real = true;
      ++i;
    }
    const std::string_view t = s.substr(start, i - start);
    if (t.empty() || t == "-") return std::nullopt;
    if (!real) {
      int64_t n = 0;
      const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), n);
      if (ec == std::errc() && p == t.data() + t.size()) return Value(n);
    }
    double d = 0;
    const auto [p, ec] = std::from_chars(t.data(), t.data() + t.size(), d);
    if (ec != std::errc() || p != t.data() + t.size()) return std::nullopt;
    return Value(d);
  }
  std::optional<Value> value() {
    ws();
    if (i >= s.size() || ++depth > 200) return std::nullopt;
    std::optional<Value> out;
    const char c = s[i];
    if (c == '{') {
      ++i;
      Map m;
      ws();
      if (i < s.size() && s[i] == '}') {
        ++i;
        out = Value(std::move(m));
      } else {
        while (true) {
          ws();
          auto k = string();
          if (!k) return std::nullopt;
          ws();
          if (i >= s.size() || s[i++] != ':') return std::nullopt;
          auto e = value();
          if (!e) return std::nullopt;
          m.emplace_back(Value(std::move(*k)), std::move(*e));
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == '}') {
            ++i;
            break;
          }
          return std::nullopt;
        }
        out = Value(std::move(m));
      }
    } else if (c == '[') {
      ++i;
      Array a;
      ws();
      if (i < s.size() && s[i] == ']') {
        ++i;
      } else {
        while (true) {
          auto e = value();
          if (!e) return std::nullopt;
          a.push_back(std::move(*e));
          ws();
          if (i < s.size() && s[i] == ',') {
            ++i;
            continue;
          }
          if (i < s.size() && s[i] == ']') {
            ++i;
            break;
          }
          return std::nullopt;
        }
      }
      out = Value(std::move(a));
    } else if (c == '"') {
      if (auto str = string()) out = Value(std::move(*str));
    } else if (lit("true")) {
      out = Value(true);
    } else if (lit("false")) {
      out = Value(false);
    } else if (lit("null")) {
      out = Value();
    } else {
      out = number();
    }
    --depth;
    return out;
  }
};

}  // namespace

std::string to_json(const Value& v) {
  std::string out;
  put(out, v);
  return out;
}

std::optional<Value> from_json(std::string_view text) {
  Parser p{text};
  auto v = p.value();
  p.ws();
  if (!v || p.i != text.size()) return std::nullopt;
  return v;
}

}  // namespace rcp
