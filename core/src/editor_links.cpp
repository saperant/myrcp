#include "editor_links.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <unistd.h>

#include "process.hpp"

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

EditorLinks::EditorLinks(Hub& hub, Front front) : hub_(hub), front_(std::move(front)) {}

EditorLinks::~EditorLinks() {
  links_.clear();
  if (!vim_script_.empty()) std::remove(vim_script_.c_str());
}

void EditorLinks::start() {
  started_ = true;
  listen_for_vims();
}

void EditorLinks::prefs_changed(const std::vector<std::string>& ids) {
  for (const auto& id : ids) {
    if (id == "workbench.vim_socket" && started_) listen_for_vims();
  }
}

std::string EditorLinks::editor() {
  for (const char* name : {"VISUAL", "EDITOR"}) {
    if (const char* v = std::getenv(name); v && *v) return v;
  }
  return "vi";
}

std::string EditorLinks::sock(const char* kind) {
  return (std::filesystem::temp_directory_path() /
          ("myrcp-" + std::to_string(getpid()) + "-" + kind + "-" + std::to_string(next_++) + ".sock"))
      .string();
}

const std::string& EditorLinks::vim_script_path() {
  if (vim_script_.empty()) {
    const auto path = std::filesystem::temp_directory_path() / ("myrcp-" + std::to_string(getpid()) + ".vim");
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fputs(vim_script(), f);
      if (std::fclose(f) == 0) vim_script_ = path.string();
    }
  }
  return vim_script_;
}

EditorLink* EditorLinks::start_link(EditorKind kind) {
  if (kind == EditorKind::nvim) return &add(std::make_unique<NvimLink>(sock("nvim"), true));
  if (kind == EditorKind::vim && !vim_script_path().empty()) return &add(std::make_unique<VimLink>(sock("vim")));
  return nullptr;
}

EditorLink& EditorLinks::add(std::unique_ptr<EditorLink> owned) {
  links_.push_back(std::move(owned));
  EditorLink& link = *links_.back();
  const std::string set = "editor:" + link.key();
  link.on_cursor = [this, &link](const EditorCursor& c) {
    cursor_[&link] = c;
    link.used = std::chrono::steady_clock::now();
    // From an editor in a tab while that tab has the focus; from one outside always (it
    // only moves while it is the one in use).
    if (link.view.empty() || link.view == front_.focused()) select_cursor(link, c);
  };
  link.on_diagnostics = [this, set](std::vector<Marker> ms) { hub_.set_markers(set, std::move(ms)); };
  link.push_markers(hub_.markers_without("editor:"));
  return link;
}

void EditorLinks::select_cursor(const EditorLink& link, const EditorCursor& c) {
  const std::string name = std::filesystem::path(c.path).filename().string();
  Array props{Array{"Line", std::to_string(c.line)}, Array{"Column", std::to_string(c.col)}};
  if (!c.filetype.empty()) props.push_back(Array{"Type", c.filetype});
  props.push_back(Array{"Modified", c.modified ? "yes (not saved)" : "no"});
  props.push_back(Array{"Text", c.text});
  hub_.set_selection(link.view.empty() ? link.name() : link.view,
                     Array{Map{{"type", "file"}, {"label", name + ":" + std::to_string(c.line)}, {"path", c.path},
                               {"line", c.line}, {"props", props}}});
}

void EditorLinks::select_in(const std::string& view) {
  for (const auto& [link, c] : cursor_) {
    if (link->view == view) select_cursor(*link, c);
  }
}

bool EditorLinks::alive(const EditorLink* link) const {
  return std::any_of(links_.begin(), links_.end(), [&](const auto& l) { return l.get() == link; });
}

void EditorLinks::drop(EditorLink* link) {
  if (!alive(link)) return;
  hub_.set_markers("editor:" + link->key(), {});  // its diagnostics go with it
  cursor_.erase(link);
  std::erase(polled_, link);
  std::erase_if(links_, [&](const auto& l) { return l.get() == link; });
}

void EditorLinks::view_ended(const std::string& view) {
  std::vector<EditorLink*> gone;
  for (auto& l : links_) {
    if (l->view == view) gone.push_back(l.get());
  }
  for (auto* l : gone) drop(l);
}

std::vector<std::string> EditorLinks::command(const std::string& path, int line, const EditorLink* link) {
  // The editor as we see it, passed along: a tmux window has the tmux server's
  // environment, not ours. A linked one is told where to link (see editor_link.hpp).
  std::vector<std::string> cmd{"/bin/sh", "-c", "e=$1; shift; exec $e \"$@\"", "myrcp-editor", editor()};
  if (dynamic_cast<const NvimLink*>(link)) {
    cmd.insert(cmd.end(), {"--listen", link->key()});
  } else if (dynamic_cast<const VimLink*>(link)) {
    cmd.insert(cmd.end(), {"--cmd", "let g:myrcp_socket = '" + link->key() + "'", "-S", vim_script_path()});
  }
  if (line > 0) cmd.push_back("+" + std::to_string(line));
  cmd.push_back(path);
  return cmd;
}

std::vector<std::string> EditorLinks::diff_command(const std::string& left, const std::string& right) const {
  std::string diff(hub_.pref("workbench.diff").str("vimdiff"));
  if (diff.empty()) diff = "vimdiff";
  return {"/bin/sh", "-c", "d=$1; shift; exec $d \"$@\"", "myrcp-diff", diff, left, right};
}

void EditorLinks::listen_for_vims() {
  std::string path(hub_.pref("workbench.vim_socket").str());
  if (path.empty()) path = default_vim_socket();
  if (vim_listener_.fd() >= 0 && vim_listener_.path() == path) return;
  vim_listener_.close();
  // Vims started in our terminals (a shell tab) find it there.
  if (vim_listener_.listen(path)) setenv("MYRCP_VIM_SOCKET", path.c_str(), 1);
}

void EditorLinks::connect() {
  const auto now = std::chrono::steady_clock::now();
  // The user's own Neovim (workbench.nvim_server): tried every two seconds until it is there.
  const std::string server(hub_.pref("workbench.nvim_server").str());
  EditorLink* theirs = nullptr;
  for (auto& l : links_) {
    if (!l->owned() && dynamic_cast<NvimLink*>(l.get())) theirs = l.get();
  }
  if (theirs && theirs->key() != server) drop(theirs), theirs = nullptr;
  if (!server.empty() && !theirs && now >= server_retry_) {
    server_retry_ = now + std::chrono::seconds(2);
    EditorLink& l = add(std::make_unique<NvimLink>(server, false));
    if (!l.connect()) drop(&l);
    else if (front_.status) front_.status("Linked to your Neovim at " + server, false);
  }
  std::vector<EditorLink*> stale;
  for (auto& l : links_) {
    if (l->connected() || l->connect()) continue;
    if (!l->view.empty()) continue;  // its tab may still be starting it; view_ended() drops it
    if (now - l->started > std::chrono::seconds(10)) stale.push_back(l.get());  // never came up
  }
  for (auto* l : stale) drop(l);
}

EditorLink* EditorLinks::live() {
  // The one in the focused tab, else one in a tab, else in tmux, else the user's own
  // (the one used last).
  const std::string focused = front_.focused ? front_.focused() : "";
  EditorLink* best = nullptr;
  int rank = 99;
  for (auto& l : links_) {
    if (!l->connected()) continue;
    const int r = l->view == focused && !l->view.empty() ? 0 : !l->view.empty() ? 1 : !l->pane.empty() ? 2 : 3;
    if (r < rank || (r == rank && best && l->used > best->used)) best = l.get(), rank = r;
  }
  return best;
}

void EditorLinks::bring_forward(EditorLink& l, const std::string& path) {
  const std::string name = std::filesystem::path(path).filename().string();
  if (!l.view.empty()) {
    if (front_.show) front_.show(l.view);
  } else if (!l.pane.empty()) {
    run_detached({"tmux", "select-window", "-t", l.pane});
    run_detached({"tmux", "select-pane", "-t", l.pane});
    if (front_.status) front_.status("Opened " + name + " in " + l.name() + " (tmux)", false);
  } else if (front_.status) {
    front_.status("Opened " + name + " in your " + l.name(), false);
  }
}

bool EditorLinks::go_to(const std::string& path, int line) {
  EditorLink* l = live();
  if (!l) return false;
  l->go_to(path, line);
  bring_forward(*l, path);
  return true;
}

bool EditorLinks::diff(const std::string& left, const std::string& right) {
  EditorLink* l = live();
  if (!l) return false;
  l->diff(left, right);
  bring_forward(*l, left);
  return true;
}

void EditorLinks::markers_changed() {
  const auto markers = hub_.markers_without("editor:");  // never an editor's own back to it
  for (auto& l : links_) l->push_markers(markers);
}

void EditorLinks::add_fds(std::vector<pollfd>& fds) {
  connect();
  polled_.clear();
  for (auto& l : links_) {
    if (!l->connected()) continue;
    fds.push_back({l->fd(), POLLIN, 0});
    polled_.push_back(l.get());
  }
  polled_listener_ = vim_listener_.fd() >= 0;
  if (polled_listener_) fds.push_back({vim_listener_.fd(), POLLIN, 0});
}

bool EditorLinks::on_poll(const pollfd* fds, size_t n) {
  bool changed = false;
  const std::vector<EditorLink*> links = polled_;  // drop() edits polled_
  for (size_t j = 0; j < links.size() && j < n; ++j) {
    if (!(fds[j].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    if (!alive(links[j])) continue;  // dropped meanwhile
    changed = true;
    if (!links[j]->on_readable()) drop(links[j]);  // the editor went away (its tab, if any, follows)
  }
  if (polled_listener_ && links.size() < n && (fds[links.size()].revents & POLLIN)) {
    if (auto vim = vim_listener_.accept()) {  // a Vim of the user's own
      add(std::move(vim));
      if (front_.status) front_.status("Linked to your Vim", false);
      changed = true;
    }
  }
  return changed;
}

int EditorLinks::timeout_ms() const {
  const auto now = std::chrono::steady_clock::now();
  const bool starting = std::any_of(links_.begin(), links_.end(), [&](const auto& l) {
    return !l->connected() && l->owned() && now - l->started < std::chrono::seconds(10);
  });
  if (starting) return 100;  // until its socket is there
  if (!hub_.pref("workbench.nvim_server").str().empty()) return 2000;
  return -1;
}

}  // namespace rcp
