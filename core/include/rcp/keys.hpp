// Terminal input bytes → named keys. Names are what manifests bind:
// "a", "A", "space", "enter", "tab", "shift+tab", "backspace", "escape", "up",
// "pagedown", "f5", "ctrl+p", "alt+1", "ctrl+alt+shift+right", "paste"
// (bracketed paste, the pasted text in Key::text), and "mouse" (SGR mouse reports,
// details in Key::mouse).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rcp {

struct Mouse {
  int x = 0, y = 0;    // cell, 0-based
  std::string button;  // "left", "middle", "right", "wheelup", "wheeldown"
  bool shift = false, alt = false, ctrl = false;
  bool operator==(const Mouse&) const = default;
};

struct Key {
  Key() = default;
  Key(std::string n, std::string t = {}, Mouse m = {}) : name(std::move(n)), text(std::move(t)), mouse(std::move(m)) {}

  std::string name;
  std::string text;  // what typing it inserts; empty for non-printing keys
  Mouse mouse;       // name == "mouse": presses and wheel turns (releases and motion are dropped)
  bool operator==(const Key&) const = default;
};

class KeyDecoder {
 public:
  void feed(std::string_view bytes);
  // The next complete key, or nullopt when more input is needed.
  std::optional<Key> next();
  // A lone ESC (or an unfinished escape sequence) waits for more bytes. After a short
  // quiet period the caller flushes it, and it becomes "escape" (or alt+<key>).
  bool pending() const { return !buf_.empty() && !in_paste_; }
  void flush() { flush_ = true; }
  void reset();

 private:
  // Decodes a key without ESC prefix at buf_[i]; returns bytes used, 0 when incomplete.
  size_t plain(size_t i, Key& k) const;
  std::optional<Key> csi();
  std::string buf_;
  std::string paste_;
  bool flush_ = false;
  bool in_paste_ = false;
};

}  // namespace rcp
