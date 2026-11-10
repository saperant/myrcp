#include "rcp/keys.hpp"

#include <charconv>
#include <vector>

#include "rcp/utf8.hpp"

namespace rcp {

namespace {

constexpr std::string_view kPasteEnd = "\x1b[201~";

std::string with_mods(int mods, std::string name) {
  // xterm modifier parameter: 1 + (shift 1 | alt 2 | ctrl 4)
  std::string prefix;
  if (mods > 1) {
    const int m = mods - 1;
    if (m & 4) prefix += "ctrl+";
    if (m & 2) prefix += "alt+";
    if (m & 1) prefix += "shift+";
  }
  return prefix + name;
}

std::string tilde_name(int code) {
  switch (code) {
    case 1: case 7: return "home";
    case 2: return "insert";
    case 3: return "delete";
    case 4: case 8: return "end";
    case 5: return "pageup";
    case 6: return "pagedown";
    case 11: return "f1";
    case 12: return "f2";
    case 13: return "f3";
    case 14: return "f4";
    case 15: return "f5";
    case 17: return "f6";
    case 18: return "f7";
    case 19: return "f8";
    case 20: return "f9";
    case 21: return "f10";
    case 23: return "f11";
    case 24: return "f12";
    default: return "";
  }
}

std::string letter_name(char final) {
  switch (final) {
    case 'A': return "up";
    case 'B': return "down";
    case 'C': return "right";
    case 'D': return "left";
    case 'H': return "home";
    case 'F': return "end";
    case 'P': return "f1";
    case 'Q': return "f2";
    case 'R': return "f3";
    case 'S': return "f4";
    default: return "";
  }
}

}  // namespace

void KeyDecoder::feed(std::string_view bytes) { buf_.append(bytes); }

void KeyDecoder::reset() {
  buf_.clear();
  paste_.clear();
  flush_ = false;
  in_paste_ = false;
}

size_t KeyDecoder::plain(size_t i, Key& k) const {
  const auto c = static_cast<unsigned char>(buf_[i]);
  if (c == '\r' || c == '\n') {
    k = {"enter", ""};
  } else if (c == '\t') {
    k = {"tab", ""};
  } else if (c == 0x7f || c == 0x08) {
    k = {"backspace", ""};
  } else if (c == 0) {
    k = {"ctrl+space", ""};
  } else if (c < 27) {
    k = {std::string("ctrl+") + char('a' + c - 1), ""};
  } else if (c == 27) {
    k = {"escape", ""};
  } else if (c < 32) {
    static constexpr const char* names[] = {"ctrl+\\", "ctrl+]", "ctrl+^", "ctrl+_"};
    k = {names[c - 28], ""};
  } else if (c == ' ') {
    k = {"space", " "};
  } else {
    const int n = utf8::seq_len(c);
    if (n == 0) {
      k = {"unknown", ""};
      return 1;
    }
    if (i + size_t(n) > buf_.size()) return 0;
    std::string s = buf_.substr(i, size_t(n));
    k = {s, s};
    return size_t(n);
  }
  return 1;
}

std::optional<Key> KeyDecoder::csi() {
  // buf_ starts with ESC [
  size_t i = 2;
  while (i < buf_.size() && buf_[i] >= 0x30 && buf_[i] <= 0x3f) ++i;  // parameters
  while (i < buf_.size() && buf_[i] >= 0x20 && buf_[i] <= 0x2f) ++i;  // intermediates
  if (i >= buf_.size()) {
    if (i > 64) {  // garbage; drop it
      buf_.clear();
      return Key{"unknown", ""};
    }
    if (!flush_) return std::nullopt;
    flush_ = false;
    buf_.erase(0, 2);
    return Key{"alt+[", ""};
  }
  const char final = buf_[i];
  const std::string params = buf_.substr(2, i - 2);
  buf_.erase(0, i + 1);

  if (!params.empty() && params[0] == '<' && (final == 'M' || final == 'm')) {  // SGR mouse
    int v[3] = {0, 0, 0};
    size_t s = 1;
    for (int& n : v) {
      size_t e = params.find(';', s);
      if (e == std::string::npos) e = params.size();
      std::from_chars(params.data() + s, params.data() + e, n);
      s = e + 1;
    }
    const int b = v[0];
    static constexpr const char* buttons[] = {"left", "middle", "right", ""};
    std::string button = (b & 64) ? ((b & 1) ? "wheeldown" : "wheelup") : buttons[b & 3];
    // Releases, and motion with the left button held (dragging); other motion is dropped.
    if (final == 'm') button = (b & 64) ? "" : "release";
    else if (b & 32) button = (b & 3) == 0 ? "drag" : "";
    if (button.empty()) return Key{"unknown", "", {}};
    return Key{"mouse", "", Mouse{v[1] - 1, v[2] - 1, button, bool(b & 4), bool(b & 8), bool(b & 16)}};
  }
  std::vector<int> p;
  if (!params.empty() && params[0] >= '0' && params[0] <= '9') {
    size_t s = 0;
    while (s <= params.size()) {
      size_t e = params.find(';', s);
      if (e == std::string::npos) e = params.size();
      int v = 0;
      std::from_chars(params.data() + s, params.data() + e, v);
      p.push_back(v);
      s = e + 1;
    }
  } else if (!params.empty()) {
    return Key{"unknown", ""};  // private sequences (mouse reports etc.)
  }
  const int mods = p.size() > 1 ? p[1] : 1;

  if (final == '~') {
    const int code = p.empty() ? 0 : p[0];
    if (code == 200) {
      in_paste_ = true;
      paste_.clear();
      if (auto k = next()) return k;
      return Key{"unknown", ""};  // the paste continues in later input
    }
    std::string name = tilde_name(code);
    if (name.empty()) return Key{"unknown", ""};
    return Key{with_mods(mods, name), ""};
  }
  if (final == 'Z') return Key{"shift+tab", ""};
  if (final == 'u' && !p.empty()) {  // kitty / fixterms: CSI codepoint ; mods u
    const auto cp = char32_t(p[0]);
    std::string name;
    if (cp == 13) name = "enter";
    else if (cp == 9) name = "tab";
    else if (cp == 27) name = "escape";
    else if (cp == 127) name = "backspace";
    else if (cp == 32) name = "space";
    else utf8::append(name, cp);
    return Key{with_mods(mods, name), mods > 1 ? "" : name};
  }
  std::string name = letter_name(final);
  if (name.empty()) return Key{"unknown", ""};
  return Key{with_mods(mods, name), ""};
}

std::optional<Key> KeyDecoder::next() {
  if (in_paste_) {
    const size_t end = buf_.find(kPasteEnd);
    if (end != std::string::npos) {
      paste_ += buf_.substr(0, end);
      buf_.erase(0, end + kPasteEnd.size());
      in_paste_ = false;
      Key k{"paste", std::move(paste_)};
      paste_.clear();
      return k;
    }
    // Keep a tail that could be the start of the terminator.
    const size_t keep = std::min(buf_.size(), kPasteEnd.size() - 1);
    paste_ += buf_.substr(0, buf_.size() - keep);
    buf_.erase(0, buf_.size() - keep);
    return std::nullopt;
  }
  if (buf_.empty()) {
    flush_ = false;
    return std::nullopt;
  }

  if (buf_[0] != '\x1b') {
    Key k;
    const size_t n = plain(0, k);
    if (n == 0) return std::nullopt;
    buf_.erase(0, n);
    return k;
  }

  if (buf_.size() == 1) {
    if (!flush_) return std::nullopt;
    flush_ = false;
    buf_.clear();
    return Key{"escape", ""};
  }
  if (buf_[1] == '[') return csi();
  if (buf_[1] == 'O') {
    if (buf_.size() < 3) {
      if (!flush_) return std::nullopt;
      flush_ = false;
      buf_.erase(0, 2);
      return Key{"alt+O", ""};
    }
    std::string name = letter_name(buf_[2]);
    buf_.erase(0, 3);
    return Key{name.empty() ? "unknown" : name, ""};
  }
  if (buf_[1] == '\x1b') {
    buf_.erase(0, 1);
    return Key{"escape", ""};
  }
  Key k;
  const size_t n = plain(1, k);
  if (n == 0) return std::nullopt;
  buf_.erase(0, 1 + n);
  return Key{"alt+" + k.name, ""};
}

}  // namespace rcp
