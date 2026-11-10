// A program running in a pseudo-terminal, its screen emulated by libvterm (which
// Neovim uses too): what a terminal view shows. vim, nvim, a shell, htop.
#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <vector>

#include "rcp/grid.hpp"
#include "rcp/keys.hpp"

struct VTerm;  // libvterm's, kept out of this header
struct VTermScreen;

namespace rcp {

class TermSession {
 public:
  struct Options {
    std::vector<std::string> argv;
    std::string cwd;  // empty: the current directory
    int cols = 80, rows = 24;
  };

  // Starts argv in a new pty. nullptr (and `error`) when it cannot.
  static std::unique_ptr<TermSession> start(const Options& o, std::string& error);
  ~TermSession();  // hangs up on the program (SIGHUP) and reaps it
  TermSession(const TermSession&) = delete;
  TermSession& operator=(const TermSession&) = delete;

  int fd() const { return master_; }
  // Call when fd() is readable. Returns false once the program has exited.
  bool on_readable();
  bool exited() const { return exited_; }
  int exit_status() const { return status_; }

  void resize(int cols, int rows);
  int cols() const { return cols_; }
  int rows() const { return rows_; }

  // Input, encoded as the program asked for it (application cursor keys, etc.).
  void key(const Key& k);
  void paste(std::string_view text);
  // A press or wheel turn at a cell of the view; only when the program asked for the mouse.
  bool wants_mouse() const { return mouse_mode_ != 0; }
  void mouse(const Mouse& m, int col, int row);

  void draw(Grid& g, Rect r, bool focused) const;
  const std::string& title() const { return title_; }

  // Internal: libvterm's callbacks.
  struct Callbacks;

 private:
  TermSession() = default;
  void write_out(std::string_view bytes);
  void reap(bool wait);

  int master_ = -1;
  pid_t pid_ = -1;
  ::VTerm* vt_ = nullptr;
  ::VTermScreen* screen_ = nullptr;
  int cols_ = 80, rows_ = 24;
  int cursor_row_ = 0, cursor_col_ = 0;
  bool cursor_visible_ = true;
  int mouse_mode_ = 0;
  std::string title_, title_part_;
  bool exited_ = false;
  int status_ = 0;
};

}  // namespace rcp
