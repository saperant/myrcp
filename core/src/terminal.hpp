#pragma once

#include <string_view>
#include <termios.h>

namespace rcp {

struct Size {
  int cols = 80, rows = 24;
};

// Raw mode + alternate screen + bracketed paste. leave()/enter() also bracket
// editor handoffs and Ctrl+Z.
class Terminal {
 public:
  explicit Terminal(int in_fd = 0, int out_fd = 1) : in_(in_fd), out_(out_fd) {}
  // Report mouse presses (SGR mode). Shift+drag still selects text in most terminals.
  void set_mouse(bool on) {
    if (active_ && on != mouse_) write(on ? "\x1b[?1002h\x1b[?1006h" : "\x1b[?1002l\x1b[?1006l");
    mouse_ = on;
  }
  ~Terminal() { leave(); }
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;

  bool enter();
  void leave();
  bool active() const { return active_; }
  Size size() const;
  void write(std::string_view s) const;
  int in_fd() const { return in_; }

 private:
  int in_, out_;
  termios saved_{};
  bool active_ = false;
  bool mouse_ = true;
};

}  // namespace rcp
