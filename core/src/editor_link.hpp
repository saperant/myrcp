// Links to running editors, both ways: our problem markers show in the editor and we
// open files and jump in it; it tells us where its cursor is and what its own
// diagnostics say. Two kinds:
//  - Neovim, over its --listen socket (its msgpack-RPC API); its diagnostics are
//    Neovim's (LSP servers, linters);
//  - Vim (+channel), which connects to a socket of ours with a script of ours (JSON
//    channel messages); its diagnostics are its quickfix list (:make, :grep).
// The editor may run in a terminal view of ours, in a tmux window or split we opened,
// or be the user's own: a Neovim at workbench.nvim_server, a Vim that sourced the
// script (myrcp --vim-script) and found the socket at workbench.vim_socket.
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hub.hpp"
#include "rpc.hpp"

namespace rcp {

// Where the cursor of an editor is: a file and a line (1-based), as it reports them.
struct EditorCursor {
  std::string path, filetype, text;  // text: the line itself
  int line = 0, col = 0;
  bool modified = false;
};

class EditorLink {
 public:
  virtual ~EditorLink() = default;
  EditorLink(const EditorLink&) = delete;
  EditorLink& operator=(const EditorLink&) = delete;

  // Tries to connect; false while it is not there yet (the editor still starting).
  virtual bool connect() = 0;
  virtual bool connected() const = 0;
  virtual int fd() const = 0;
  // When readable. False: the editor went away.
  virtual bool on_readable() = 0;

  virtual void push_markers(const std::vector<Marker>& markers) = 0;
  virtual void go_to(const std::string& path, int line) = 0;
  // A new tab with the two side by side in diff mode.
  virtual void diff(const std::string& left, const std::string& right) = 0;
  virtual std::string name() const = 0;  // "Neovim", "Vim"

  // Which link this is (its markers' set is "editor:" + key()).
  const std::string& key() const { return key_; }
  bool owned() const { return owned_; }
  std::string view;  // the terminal view it runs in; empty: outside the workbench
  std::string pane;  // the tmux pane it runs in, to bring forward; empty: none known
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::chrono::steady_clock::time_point used = started;  // its cursor last moved

  std::function<void(const EditorCursor&)> on_cursor;
  std::function<void(std::vector<Marker>)> on_diagnostics;  // all of its own, each time
  std::function<void()> on_connected;

 protected:
  EditorLink(std::string key, bool owned) : key_(std::move(key)), owned_(owned) {}
  std::string key_;
  bool owned_;
  std::vector<Marker> markers_;  // what to push once linked
};

// owned: we started that Neovim on a socket of ours, which goes when the link does.
class NvimLink : public EditorLink {
 public:
  NvimLink(std::string sock, bool owned);
  ~NvimLink() override;
  bool connect() override;
  bool connected() const override { return rpc_ != nullptr; }
  int fd() const override { return fd_; }
  bool on_readable() override;
  void push_markers(const std::vector<Marker>& markers) override;
  void go_to(const std::string& path, int line) override;
  void diff(const std::string& left, const std::string& right) override;
  std::string name() const override { return "Neovim"; }

 private:
  int fd_ = -1;
  std::unique_ptr<Rpc> rpc_;
};

// For a Vim we start, we listen on a socket of our own for each Vim we start
// (so we know which tab or pane it is), and it connects when the script runs. A Vim of
// the user's own connects to the shared socket (VimListener) and arrives connected.
class VimLink : public EditorLink {
 public:
  explicit VimLink(std::string sock);       // we start that Vim: listen for it here
  VimLink(int conn, std::string key);       // the user's own, already connected
  ~VimLink() override;
  bool connect() override;  // accepts it, once it connects
  bool connected() const override { return conn_ >= 0; }
  int fd() const override { return conn_; }
  bool on_readable() override;
  void push_markers(const std::vector<Marker>& markers) override;
  void go_to(const std::string& path, int line) override;
  void diff(const std::string& left, const std::string& right) override;
  std::string name() const override { return "Vim"; }

 private:
  void send(const mp::Value& msg);
  void message(const mp::Value& m);
  std::string sock_;
  int listen_ = -1, conn_ = -1;
  std::string in_;  // what is read but not yet a whole line
};

// The shared socket Vims of the user's own connect to.
class VimListener {
 public:
  ~VimListener();
  // False when another myrcp has it (or it cannot be made).
  bool listen(const std::string& path);
  void close();
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }
  std::unique_ptr<VimLink> accept();

 private:
  int fd_ = -1;
  int next_ = 1;
  std::string path_;
};

enum class EditorKind { other, nvim, vim };
EditorKind editor_kind(const std::string& editor);  // by the command's name
std::string default_vim_socket();                   // $XDG_RUNTIME_DIR/myrcp-vim.sock
const char* vim_script();                          // what a Vim sources to link itself

}  // namespace rcp
