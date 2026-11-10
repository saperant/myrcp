#include "term.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vterm.h>

#include "rcp/utf8.hpp"

namespace rcp {

struct TermSession::Callbacks {
  static int movecursor(VTermPos pos, VTermPos, int visible, void* user) {
    auto* t = static_cast<TermSession*>(user);
    t->cursor_row_ = pos.row;
    t->cursor_col_ = pos.col;
    t->cursor_visible_ = visible != 0;
    return 1;
  }
  static int settermprop(VTermProp prop, VTermValue* val, void* user) {
    auto* t = static_cast<TermSession*>(user);
    switch (prop) {
      case VTERM_PROP_TITLE:  // arrives in fragments
        if (val->string.initial) t->title_part_.clear();
        t->title_part_.append(val->string.str, val->string.len);
        if (val->string.final) t->title_ = t->title_part_;
        return 1;
      case VTERM_PROP_CURSORVISIBLE:
        t->cursor_visible_ = val->boolean != 0;
        return 1;
      case VTERM_PROP_MOUSE:
        t->mouse_mode_ = val->number;
        return 1;
      default:
        return 0;
    }
  }
  static void output(const char* s, size_t len, void* user) { static_cast<TermSession*>(user)->write_out({s, len}); }
};

std::unique_ptr<TermSession> TermSession::start(const Options& o, std::string& error) {
  if (o.argv.empty()) {
    error = "nothing to run";
    return nullptr;
  }
  std::unique_ptr<TermSession> t(new TermSession);
  t->cols_ = std::max(2, o.cols);
  t->rows_ = std::max(1, o.rows);
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(t->cols_);
  ws.ws_row = static_cast<unsigned short>(t->rows_);

  std::vector<char*> args;
  for (const auto& a : o.argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  const pid_t pid = forkpty(&t->master_, nullptr, nullptr, &ws);
  if (pid < 0) {
    error = std::string("forkpty: ") + std::strerror(errno);
    return nullptr;
  }
  if (pid == 0) {
    for (int s : {SIGINT, SIGQUIT, SIGTSTP, SIGPIPE, SIGWINCH, SIGTERM, SIGHUP, SIGCHLD}) signal(s, SIG_DFL);
    for (int fd = 3; fd < 1024; ++fd) close(fd);
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    if (!o.cwd.empty() && chdir(o.cwd.c_str()) != 0) _exit(126);
    execvp(args[0], args.data());
    const std::string msg = "myrcp: cannot run " + o.argv[0] + ": " + std::strerror(errno) + "\r\n";
    (void)!write(2, msg.data(), msg.size());
    _exit(127);
  }
  t->pid_ = pid;
  fcntl(t->master_, F_SETFD, FD_CLOEXEC);
  fcntl(t->master_, F_SETFL, fcntl(t->master_, F_GETFL) | O_NONBLOCK);

  t->vt_ = vterm_new(t->rows_, t->cols_);
  vterm_set_utf8(t->vt_, 1);
  vterm_output_set_callback(t->vt_, &Callbacks::output, t.get());
  t->screen_ = vterm_obtain_screen(t->vt_);
  static VTermScreenCallbacks cbs = [] {
    VTermScreenCallbacks c{};
    c.movecursor = &Callbacks::movecursor;
    c.settermprop = &Callbacks::settermprop;
    return c;
  }();
  vterm_screen_set_callbacks(t->screen_, &cbs, t.get());
  vterm_screen_enable_altscreen(t->screen_, 1);
  vterm_screen_reset(t->screen_, 1);
  return t;
}

TermSession::~TermSession() {
  if (pid_ > 0 && !exited_) {
    kill(-pid_, SIGHUP);
    kill(pid_, SIGHUP);
    reap(true);
  }
  if (master_ >= 0) close(master_);
  if (vt_) vterm_free(vt_);
}

void TermSession::reap(bool wait) {
  int st = 0;
  for (int i = 0; i < (wait ? 50 : 1); ++i) {
    const pid_t r = waitpid(pid_, &st, WNOHANG);
    if (r == pid_) {
      exited_ = true;
      status_ = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
      return;
    }
    if (r < 0) {
      exited_ = true;
      return;
    }
    if (wait) usleep(10000);
  }
  if (wait) {
    kill(pid_, SIGKILL);
    waitpid(pid_, &st, 0);
    exited_ = true;
  }
}

bool TermSession::on_readable() {
  char buf[16384];
  while (true) {
    const ssize_t n = read(master_, buf, sizeof buf);
    if (n > 0) {
      vterm_input_write(vt_, buf, size_t(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    reap(true);  // EOF or EIO: the program and its children closed the terminal
    return false;
  }
  return true;
}

void TermSession::write_out(std::string_view bytes) {
  while (!bytes.empty() && master_ >= 0) {
    const ssize_t n = write(master_, bytes.data(), bytes.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN) {
        usleep(1000);
        continue;
      }
      return;
    }
    bytes.remove_prefix(size_t(n));
  }
}

void TermSession::resize(int cols, int rows) {
  cols = std::max(2, cols);
  rows = std::max(1, rows);
  if (cols == cols_ && rows == rows_) return;
  cols_ = cols;
  rows_ = rows;
  vterm_set_size(vt_, rows_, cols_);
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(cols_);
  ws.ws_row = static_cast<unsigned short>(rows_);
  ioctl(master_, TIOCSWINSZ, &ws);  // the program gets SIGWINCH
}

namespace {

// "ctrl+alt+shift+x" → modifiers and the rest.
VTermModifier split_mods(std::string_view& name) {
  int mod = VTERM_MOD_NONE;
  for (bool more = true; more && name.size() > 1;) {
    more = false;
    for (auto [prefix, m] : {std::pair<std::string_view, int>{"ctrl+", VTERM_MOD_CTRL},
                             {"alt+", VTERM_MOD_ALT}, {"shift+", VTERM_MOD_SHIFT}}) {
      if (name.starts_with(prefix) && name.size() > prefix.size()) {
        mod |= m;
        name.remove_prefix(prefix.size());
        more = true;
      }
    }
  }
  return static_cast<VTermModifier>(mod);
}

VTermKey special_key(std::string_view n) {
  static constexpr std::pair<std::string_view, VTermKey> keys[] = {
      {"enter", VTERM_KEY_ENTER}, {"tab", VTERM_KEY_TAB},     {"backspace", VTERM_KEY_BACKSPACE},
      {"escape", VTERM_KEY_ESCAPE}, {"up", VTERM_KEY_UP},       {"down", VTERM_KEY_DOWN},
      {"left", VTERM_KEY_LEFT},   {"right", VTERM_KEY_RIGHT}, {"insert", VTERM_KEY_INS},
      {"delete", VTERM_KEY_DEL},  {"home", VTERM_KEY_HOME},   {"end", VTERM_KEY_END},
      {"pageup", VTERM_KEY_PAGEUP}, {"pagedown", VTERM_KEY_PAGEDOWN},
  };
  for (const auto& [name, key] : keys) {
    if (n == name) return key;
  }
  if (n.size() >= 2 && n[0] == 'f') {
    const int f = std::atoi(std::string(n.substr(1)).c_str());
    if (f >= 1 && f <= 12) return static_cast<VTermKey>(VTERM_KEY_FUNCTION(f));
  }
  return VTERM_KEY_NONE;
}

uint32_t color_of(const VTermColor& c, bool fg) {
  if (fg ? VTERM_COLOR_IS_DEFAULT_FG(&c) : VTERM_COLOR_IS_DEFAULT_BG(&c)) return 0;
  if (VTERM_COLOR_IS_INDEXED(&c)) return kColorIndexed | c.indexed.idx;
  return kColorRgb | (uint32_t(c.rgb.red) << 16) | (uint32_t(c.rgb.green) << 8) | c.rgb.blue;
}

}  // namespace

void TermSession::key(const Key& k) {
  if (k.name == "paste") return paste(k.text);
  std::string_view name = k.name;
  if (name == "shift+tab") {
    vterm_keyboard_key(vt_, VTERM_KEY_TAB, VTERM_MOD_SHIFT);
    return;
  }
  const VTermModifier mod = split_mods(name);
  if (name == "space") return vterm_keyboard_unichar(vt_, U' ', mod);
  if (const VTermKey key = special_key(name); key != VTERM_KEY_NONE) return vterm_keyboard_key(vt_, key, mod);
  // A character, possibly with ctrl/alt ("ctrl+\\", "alt+x", "ä").
  size_t i = 0;
  const char32_t c = utf8::decode(name, i);
  if (i == name.size()) vterm_keyboard_unichar(vt_, c, mod);
}

void TermSession::paste(std::string_view text) {
  vterm_keyboard_start_paste(vt_);  // bracketed only when the program asked for it
  write_out(text);
  vterm_keyboard_end_paste(vt_);
}

void TermSession::mouse(const Mouse& m, int col, int row) {
  if (!wants_mouse()) return;
  int mod = VTERM_MOD_NONE;
  if (m.shift) mod |= VTERM_MOD_SHIFT;
  if (m.ctrl) mod |= VTERM_MOD_CTRL;
  if (m.alt) mod |= VTERM_MOD_ALT;
  const auto vmod = static_cast<VTermModifier>(mod);
  vterm_mouse_move(vt_, row, col, vmod);
  const int button = m.button == "left" ? 1 : m.button == "middle" ? 2 : m.button == "right" ? 3
                     : m.button == "wheelup" ? 4 : m.button == "wheeldown" ? 5 : 0;
  if (button == 0) return;
  vterm_mouse_button(vt_, button, true, vmod);
  if (button <= 3) vterm_mouse_button(vt_, button, false, vmod);  // we see presses only
}

void TermSession::draw(Grid& g, Rect r, bool focused) const {
  VTermScreenCell cell;
  for (int row = 0; row < rows_ && row < r.h; ++row) {
    for (int col = 0; col < cols_ && col < r.w;) {
      vterm_screen_get_cell(screen_, VTermPos{row, col}, &cell);
      const char32_t ch = cell.chars[0] ? char32_t(cell.chars[0]) : U' ';
      uint8_t attrs = 0;
      if (cell.attrs.bold) attrs |= attr_bold;
      if (cell.attrs.underline) attrs |= attr_underline;
      if (cell.attrs.italic) attrs |= attr_italic;
      if (cell.attrs.reverse) attrs |= attr_reverse;
      if (cell.attrs.strike) attrs |= attr_strike;
      const bool cursor = focused && cursor_visible_ && row == cursor_row_ && col == cursor_col_;
      if (cursor) attrs ^= attr_reverse;  // our own cursor: the real one stays hidden
      g.put_raw(r.x + col, r.y + row, ch, color_of(cell.fg, true), color_of(cell.bg, false), attrs, r.x + r.w);
      col += std::max(1, int(cell.width));
    }
  }
}

}  // namespace rcp
