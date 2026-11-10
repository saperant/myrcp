#include "terminal.hpp"

#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>

namespace rcp {

bool Terminal::enter() {
  if (active_) return true;
  if (tcgetattr(in_, &saved_) != 0) return false;
  termios raw = saved_;
  raw.c_iflag &= ~tcflag_t(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~tcflag_t(OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= ~tcflag_t(ECHO | ICANON | IEXTEN | ISIG);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(in_, TCSAFLUSH, &raw) != 0) return false;
  active_ = true;
  // alternate screen, hide cursor, bracketed paste, clear
  write("\x1b[?1049h\x1b[?25l\x1b[?2004h\x1b[0m\x1b[H\x1b[2J");
  if (mouse_) write("\x1b[?1002h\x1b[?1006h");  // buttons, wheel and drags, SGR coordinates
  return true;
}

void Terminal::leave() {
  if (!active_) return;
  write("\x1b[?1002l\x1b[?1006l\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l");
  tcsetattr(in_, TCSAFLUSH, &saved_);
  active_ = false;
}

Size Terminal::size() const {
  winsize ws{};
  if (ioctl(out_, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) return {ws.ws_col, ws.ws_row};
  return {};
}

void Terminal::write(std::string_view s) const {
  while (!s.empty()) {
    const ssize_t n = ::write(out_, s.data(), s.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    s.remove_prefix(size_t(n));
  }
}

}  // namespace rcp
