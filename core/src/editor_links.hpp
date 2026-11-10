// The editors linked to a workbench (editor_link.hpp), for either front-end: the ones it
// starts (in a terminal view, in tmux), the user's own Neovim (workbench.nvim_server)
// and Vims (the shared socket, workbench.vim_socket). Their cursors become the
// selection, their own diagnostics a marker set "editor:<key>", our markers go to
// them, and files open in the live one instead of a new editor.
#pragma once

#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <vector>

#include "editor_link.hpp"
#include "hub.hpp"

namespace rcp {

class EditorLinks {
 public:
  // What the front-end does for them.
  struct Front {
    std::function<std::string()> focused;                   // the focused view's id
    std::function<void(const std::string& view)> show;      // bring a view's tab forward
    std::function<void(const std::string& text, bool error)> status;
  };
  EditorLinks(Hub& hub, Front front);
  ~EditorLinks();  // removes the script written for Vims
  EditorLinks(const EditorLinks&) = delete;
  EditorLinks& operator=(const EditorLinks&) = delete;

  void start();  // once the hub is up: listens for the user's Vims
  void prefs_changed(const std::vector<std::string>& ids);

  // $VISUAL (else $EDITOR, else vi), and what kind of editor it is.
  static std::string editor();
  // A link for an editor about to start (nullptr: not one we link). Give it its view
  // or pane once it runs, or drop() it when it could not start.
  EditorLink* start_link(EditorKind kind);
  void drop(EditorLink* link);
  void view_ended(const std::string& view);  // the terminal view it ran in closed
  // The command for the editor at path:line, told where to link when `link` is one.
  std::vector<std::string> command(const std::string& path, int line, const EditorLink* link);
  // The workbench.diff program for the two (vimdiff by default).
  std::vector<std::string> diff_command(const std::string& left, const std::string& right) const;

  // The one files open in: in the focused tab, else a tab, else tmux, else the user's own.
  EditorLink* live();
  // Opens path:line in the live editor and brings it forward; false when none is linked.
  bool go_to(const std::string& path, int line);
  // Compares two files in the live editor (a diff tab); false when none is linked.
  bool diff(const std::string& left, const std::string& right);
  void markers_changed();  // the hub's markers changed: every editor hears (but its own)
  void select_in(const std::string& view);  // that view's editor's cursor as the selection

  // The event loop's part: the fds to poll (appended), what came of them (the same
  // slice, in order), and how soon it wants a turn even when nothing came (-1: never).
  void add_fds(std::vector<pollfd>& fds);
  bool on_poll(const pollfd* fds, size_t n);  // true: something changed (draw again)
  int timeout_ms() const;

 private:
  EditorLink& add(std::unique_ptr<EditorLink> link);
  void select_cursor(const EditorLink& link, const EditorCursor& c);
  void bring_forward(EditorLink& link, const std::string& path);
  void connect();  // retries the user's Neovim, drops those that never came up
  void listen_for_vims();
  const std::string& vim_script_path();
  std::string sock(const char* kind);
  bool alive(const EditorLink* link) const;

  Hub& hub_;
  Front front_;
  std::vector<std::unique_ptr<EditorLink>> links_;
  std::map<const EditorLink*, EditorCursor> cursor_;  // where each one's cursor last was
  VimListener vim_listener_;
  std::string vim_script_;
  int next_ = 1;
  std::chrono::steady_clock::time_point server_retry_;
  bool started_ = false;
  std::vector<EditorLink*> polled_;  // the links add_fds() gave fds for, in order
  bool polled_listener_ = false;
};

}  // namespace rcp
