#include "workbench.hpp"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <span>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "process.hpp"
#include "rcp/utf8.hpp"
#ifdef MYRCP_HAVE_VTERM
#include "term.hpp"
#else
namespace rcp {
class TermSession {};  // no terminal views without libvterm
}  // namespace rcp
#endif

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

int g_signal_pipe[2] = {-1, -1};

void on_signal(int sig) {
  const char c = sig == SIGWINCH ? 'w' : 'q';
  const int saved = errno;
  (void)!write(g_signal_pipe[1], &c, 1);
  errno = saved;
}

void install_signals() {
  if (g_signal_pipe[0] < 0 && pipe2(g_signal_pipe, O_CLOEXEC | O_NONBLOCK) != 0) return;
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  for (int s : {SIGWINCH, SIGTERM, SIGHUP}) sigaction(s, &sa, nullptr);
}

std::string env_or(const char* name, const char* def) {
  const char* v = std::getenv(name);
  return v && *v ? v : def;
}

// Subsequence match, case-insensitive (ASCII). Lower is better; -1 = no match.
int fuzzy_score(std::string_view hay, std::string_view needle) {
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; };
  int score = 0, last = -1;
  size_t h = 0;
  for (char nc : needle) {
    if (nc == ' ') continue;
    while (h < hay.size() && lower(hay[h]) != lower(nc)) ++h;
    if (h == hay.size()) return -1;
    score += last < 0 ? int(h) : int(h) - last - 1;
    last = int(h++);
  }
  return score;
}

std::vector<CommandInfo> builtin_commands() {
  std::vector<CommandInfo> cmds = {
      {"workbench.palette", "Show all commands", {"ctrl+p"}, "", ""},
      {"workbench.goto", "Go to Anything: files, views, problems (fzf)", {"ctrl+o"}, "", ""},
      {"workbench.quit", "Quit", {"ctrl+q"}, "", ""},
      {"workbench.nextView", "View: Next in the region", {"ctrl+pagedown"}, "", ""},
      {"workbench.prevView", "View: Previous in the region", {"ctrl+pageup"}, "", ""},
      {"workbench.back", "Go: Back to where you were", {"alt+left"}, "", ""},
      {"workbench.forward", "Go: Forward again", {"alt+right"}, "", ""},
      {"workbench.closeView", "View: Close", {"ctrl+w"}, "", ""},
      {"workbench.nextRegion", "View: Focus the next region", {"f6"}, "", ""},
      {"workbench.prevRegion", "View: Focus the previous region", {"shift+f6"}, "", ""},
      {"workbench.maximize", "View: Maximize the regions into one (again: restore)", {"alt+z"}, "", ""},
      {"workbench.move:main", "View: Move to the main area", {}, "", ""},
      {"workbench.move:side", "View: Move to the side", {}, "", ""},
      {"workbench.move:bottom", "View: Move to the bottom", {}, "", ""},
      {"workbench.side:wider", "View: Widen the side", {"ctrl+\\ shift+left"}, "", ""},
      {"workbench.side:narrower", "View: Narrow the side", {"ctrl+\\ shift+right"}, "", ""},
      {"workbench.bottom:taller", "View: Raise the bottom", {"ctrl+\\ shift+up"}, "", ""},
      {"workbench.bottom:shorter", "View: Lower the bottom", {"ctrl+\\ shift+down"}, "", ""},
      {"workbench.perspective.reset", "Perspective: Reset to its definition", {}, "", ""},
      {"workbench.restartHost", "Plugins: Restart hosts", {"ctrl+r"}, "", ""},
      {"workbench.showLog", "Plugins: Open host log in editor", {}, "", ""},
      {"workbench.cancelJobs", "Jobs: Cancel all", {}, "", ""},
      {"workbench.suspend", "Suspend to shell", {"ctrl+z"}, "", ""},
      {"workbench.problems", "Problems: Show", {"f8"}, "", ""},
      {"workbench.properties", "Properties: Show the selection", {"alt+enter"}, "", ""},
      {"workbench.contextMenu", "Show the context menu (or a right click)", {"shift+f10"}, "", ""},
      {"workbench.table.sort", "Table: Sort by the next column", {"alt+s"}, "", ""},
      {"workbench.table.reverse", "Table: Reverse the sort order", {"alt+r"}, "", ""},
      {"workbench.preferences", "Preferences: Show (theme, editor, plugins' settings)", {}, "", ""},
      {"workbench.editPrefs", "Preferences: Edit the file", {}, "", ""},
#ifdef MYRCP_HAVE_VTERM
      {"workbench.terminal", "Terminal: New shell", {"ctrl+t"}, "", ""},
#endif
  };
  for (const auto& t : themes()) cmds.push_back({"workbench.theme:" + t.name, "Theme: " + t.title, {}, "", ""});
  return cmds;
}

bool is_rows(std::string_view t) { return t == "list" || t == "table" || t == "tree"; }

bool inside(const Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

// Below these the side and the bottom fold into the main area's tabs.
constexpr int kSideMinCols = 140;
constexpr int kBottomMinRows = 34;

size_t ri(Region r) { return size_t(r); }

}  // namespace

Workbench::Workbench(HostOptions o)
    : hub_(std::move(o), *this),
      theme_(&theme_by_name("terminal")),
      builtins_(builtin_commands()),
      links_(hub_, {[this] { return focus_id_; }, [this](const std::string& view) { activate(view); },
                    [this](const std::string& text, bool error) { set_status(text, error ? Style::bar_error : Style::bar); }}) {
  declare_workbench_prefs(hub_, true);
  hub_.reload_prefs();
  prefs_changed({"workbench.theme", "workbench.mouse"});
}

void Workbench::prefs_changed(const std::vector<std::string>& ids) {
  links_.prefs_changed(ids);
  for (const auto& id : ids) {
    if (id == "workbench.theme") {
      theme_ = &theme_by_name(std::string(hub_.pref(id).str("terminal")));
      redraw_all();
    } else if (id == "workbench.mouse") {
      term_.set_mouse(hub_.pref(id).boolean(true));
    }
  }
}

std::string Workbench::editor_mode() const { return std::string(hub_.pref("workbench.editor_mode").str("suspend")); }

Workbench::~Workbench() = default;  // terminal sessions hang up on their programs

void Workbench::markers_changed() { links_.markers_changed(); }

void Workbench::open_location(const std::string& path, int line) {
  if (links_.go_to(path, line)) return;  // a linked editor jumps there instead of a new one
  open_editor_later(path, line, [](bool) {});
}

// ---- terminal views -----------------------------------------------------------------

#ifdef MYRCP_HAVE_VTERM
TermSession* Workbench::active_term() {
  const ViewData* v = active_view();
  if (!v || !v->native) return nullptr;
  const auto it = terms_.find(v->id);
  return it == terms_.end() ? nullptr : it->second.session.get();
}

std::string Workbench::open_term(const std::vector<std::string>& argv, const std::string& cwd,
                                 const std::string& title, std::function<void(bool)> done, Region region) {
  std::string err;
  // Started at the size of where it goes (resized when drawn, should that change).
  Rect r = regions_[ri(region_shown(region) ? region : Region::main)].rect;
  if (r.w <= 0) r = {0, 1, size_.cols, size_.rows - 2};
  auto session = TermSession::start({argv, cwd, std::max(2, r.w), std::max(1, r.h)}, err);
  if (!session) {
    set_status("Cannot start " + (argv.empty() ? "" : argv[0]) + ": " + err, Style::bar_error);
    if (done) done(false);
    return "";
  }
  const std::string id = "term:" + std::to_string(next_term_++);
  const bool own_title = title.empty();
  hub_.open_native(id, own_title ? std::filesystem::path(argv[0]).filename().string() : title, region);
  Term t;
  t.session = std::move(session);
  t.done = std::move(done);
  t.own_title = own_title;
  terms_[id] = std::move(t);
  activate(id);
  return id;
}

void Workbench::send_term_key(TermSession* t, const Key& k) { t->key(k); }

void Workbench::send_term_mouse(TermSession* t, const Mouse& m, int col, int row) { t->mouse(m, col, row); }

void Workbench::draw_term(Grid& g, Rect r, const std::string& id, bool focused) {
  const auto it = terms_.find(id);
  if (it == terms_.end()) return;
  it->second.session->resize(r.w, r.h);
  it->second.session->draw(g, r, focused);
}

void Workbench::term_ended(const std::string& id) {
  auto it = terms_.find(id);
  if (it == terms_.end()) return;
  auto done = std::move(it->second.done);
  terms_.erase(it);  // before the tab closes: close_active() comes back here otherwise
  links_.view_ended(id);
  hub_.close_view(id);
  vstate_.erase(id);
  if (done) done(true);  // the editor has exited: the plugin may reload the file
  dirty_ = true;
}

bool Workbench::open_popup(const std::vector<std::string>& argv, const std::string& title,
                           std::function<void(int)> done) {
  std::string err;
  const int w = std::max(20, size_.cols * 4 / 5), h = std::max(6, size_.rows * 2 / 3);
  auto session = TermSession::start({argv, "", w - 2, h - 2}, err);
  if (!session) {
    set_status("Cannot start " + (argv.empty() ? "" : argv[0]) + ": " + err, Style::bar_error);
    return false;
  }
  popup_ = std::make_unique<Popup>(Popup{std::move(session), std::move(done), title});
  dirty_ = true;
  return true;
}

void Workbench::popup_ended() {
  auto p = std::move(popup_);  // done() may open another
  const int status = p->session->exit_status();
  if (p->done) p->done(status);
  dirty_ = true;
}

void Workbench::draw_popup(Grid& g) {
  const int W = size_.cols, H = size_.rows;
  const int w = std::min(W, std::max(20, W * 4 / 5)), h = std::min(H, std::max(6, H * 2 / 3));
  const Rect box{(W - w) / 2, (H - h) / 2, w, h};
  g.fill(box, Style::normal);
  draw_box(g, box, popup_->title, Style::border_focus, Style::border_focus);
  g.set_layer(box, Layer::dialog);
  g.shadow(box);
  const Rect inner{box.x + 1, box.y + 1, box.w - 2, box.h - 2};
  popup_->session->resize(inner.w, inner.h);
  popup_->session->draw(g, inner, true);
}
#else
TermSession* Workbench::active_term() { return nullptr; }
std::string Workbench::open_term(const std::vector<std::string>&, const std::string&, const std::string&,
                                 std::function<void(bool)> done, Region) {
  set_status("This myrcp was built without libvterm: no terminal views", Style::bar_error);
  if (done) done(false);
  return "";
}
void Workbench::term_ended(const std::string&) {}
bool Workbench::open_popup(const std::vector<std::string>&, const std::string&, std::function<void(int)>) {
  set_status("This myrcp was built without libvterm: no Go to Anything", Style::bar_error);
  return false;
}
void Workbench::popup_ended() {}
void Workbench::draw_popup(Grid&) {}
void Workbench::send_term_key(TermSession*, const Key&) {}
void Workbench::send_term_mouse(TermSession*, const Mouse&, int, int) {}
void Workbench::draw_term(Grid&, Rect, const std::string&, bool) {}
#endif

void Workbench::open_editor_later(const std::string& path, int line, std::function<void(bool)> done) {
  if (links_.go_to(path, line)) return done(false);  // in the editor that is open: it keeps running
  if (editor_mode() != "tab") return done(open_editor(path, line));
  EditorLink* link = links_.start_link(editor_kind(EditorLinks::editor()));
  const std::string id = open_term(links_.command(path, line, link), "",
                                   std::filesystem::path(path).filename().string(), std::move(done));
  if (link && id.empty()) links_.drop(link);
  else if (link) link->view = id;
}

void Workbench::open_terminal(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title) {
  open_term(argv, cwd, title, nullptr);
}

// In the linked editor's own diff tab, else workbench.diff where the editor would run.
void Workbench::open_compare(const std::string& left, const std::string& right, const std::string& title,
                             std::function<void(bool)> done) {
  dirty_ = true;
  if (links_.diff(left, right)) return done(false);
  const auto cmd = links_.diff_command(left, right);
  const std::string name = title.empty() ? std::filesystem::path(left).filename().string() : title;
  const std::string mode = editor_mode();
  if (mode == "tab") {
    open_term(cmd, "", name, std::move(done));
    return;
  }
  if ((mode == "tmux-window" || mode == "tmux-split") && std::getenv("TMUX")) {
    std::vector<std::string> t{"tmux", mode == "tmux-split" ? "split-window" : "new-window"};
    if (mode == "tmux-split") t.emplace_back("-h");
    else t.insert(t.end(), {"-n", name});
    t.insert(t.end(), cmd.begin(), cmd.end());
    int rc = 0;
    run_output(t, &rc);
    set_status(rc == 0 ? "Comparing " + name + " in tmux" : "tmux could not compare " + name,
               rc == 0 ? Style::bar : Style::bar_error);
    return done(false);
  }
  term_.leave();
  const int rc = run_foreground(cmd);
  term_.enter();
  keys_.reset();
  redraw_all();
  if (rc > 1) set_status("Compare exited with status " + std::to_string(rc), Style::bar_error);  // diff: 1 = differ
  done(true);
}

// ---- main loop ------------------------------------------------------------------

int Workbench::run() {
  if (!term_.enter()) {
    std::fprintf(stderr, "myrcp: stdin is not a terminal\n");
    return 1;
  }
  install_signals();
  hub_.start();
  links_.start();

  while (!quit_) {
    expire_status();
    if (dirty_) {
      draw();
      dirty_ = false;
      // The selection follows what was just drawn (cursors as clamped there); when it
      // changed, Properties did too: draw again now, not at the next key.
      update_selection();
      if (dirty_) {
        draw();
        dirty_ = false;
      }
      for (const Region r : kRegions) {
        if (regions_[ri(r)].rect.w <= 0) continue;
        if (const ViewData* v = region_view(r); v && !v->loading && !v->native) request_windows(*v, vstate_[v->id]);
      }
    }
    std::vector<pollfd> fds{{term_.in_fd(), POLLIN, 0}, {g_signal_pipe[0], POLLIN, 0}};
    std::vector<Hub::PollToken> tokens;
    hub_.add_fds(fds, tokens);
    std::vector<std::string> term_ids;
#ifdef MYRCP_HAVE_VTERM
    for (const auto& [id, t] : terms_) {
      fds.push_back({t.session->fd(), POLLIN, 0});
      term_ids.push_back(id);
    }
    const size_t popup_at = fds.size();
    if (popup_) fds.push_back({popup_->session->fd(), POLLIN, 0});
#endif
    const size_t links_at = fds.size();
    links_.add_fds(fds);
    const size_t links_n = fds.size() - links_at;

    int timeout = keys_.pending() ? 30 : status_timeout_ms();
    if (const int t = links_.timeout_ms(); t >= 0 && (timeout < 0 || timeout > t)) timeout = t;
    const int r = poll(fds.data(), fds.size(), timeout);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (r == 0) {
      if (keys_.pending()) {
        keys_.flush();
        drain_keys();
      }
      continue;
    }
    if (fds[1].revents & POLLIN) {
      char sigs[32];
      const ssize_t k = read(g_signal_pipe[0], sigs, sizeof sigs);
      for (ssize_t i = 0; i < k; ++i) {
        if (sigs[i] == 'q') quit_ = true;
        else redraw_all();
      }
    }
    if (fds[0].revents & (POLLIN | POLLHUP)) {
      char buf[4096];
      const ssize_t k = read(term_.in_fd(), buf, sizeof buf);
      if (k == 0 || (k < 0 && errno != EINTR && errno != EAGAIN)) break;  // the terminal went away
      if (k > 0) {
        keys_.feed({buf, size_t(k)});
        drain_keys();
      }
    }
    for (size_t j = 0; j < tokens.size(); ++j) {
      if (fds[j + 2].revents & (POLLIN | POLLHUP)) hub_.on_ready(tokens[j]);
    }
#ifdef MYRCP_HAVE_VTERM
    for (size_t j = 0; j < term_ids.size(); ++j) {
      if (!(fds[2 + tokens.size() + j].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      auto it = terms_.find(term_ids[j]);
      if (it == terms_.end()) continue;
      dirty_ = true;
      if (!it->second.session->on_readable()) {
        term_ended(term_ids[j]);
      } else if (it->second.own_title && !it->second.session->title().empty()) {
        hub_.set_title(term_ids[j], it->second.session->title());
      }
    }
    if (popup_ && popup_at < fds.size() && (fds[popup_at].revents & (POLLIN | POLLHUP | POLLERR))) {
      dirty_ = true;
      if (!popup_->session->on_readable()) popup_ended();
    }
#endif
    if (links_.on_poll(fds.data() + links_at, links_n)) dirty_ = true;
  }
  hub_.save_state();
  hub_.stop();
  term_.leave();
  return 0;
}

void Workbench::drain_keys() {
  while (!quit_) {
    auto k = keys_.next();
    if (!k) break;
    handle_key(*k);
  }
}

// ---- views --------------------------------------------------------------------------

const ViewData* Workbench::active_view() {
  const auto& views = hub_.views();
  if (views.empty()) {
    focus_id_.clear();
    return nullptr;
  }
  const ViewData* v = nullptr;
  for (const auto& view : views) {
    if (view.id == focus_id_) v = &view;
  }
  if (!v) {  // gone (or none yet): its neighbour in the region takes over, else another region's
    v = region_view(focus_region_);
    for (const Region r : {Region::main, Region::bottom, Region::side}) {
      if (!v) v = region_view(r);
    }
    if (!v) v = &views.front();
    focus_id_ = v->id;
  }
  focus_region_ = where(*v);
  regions_[ri(focus_region_)].active = v->id;
  return v;
}

bool Workbench::region_shown(Region r) const {
  if (r == Region::main) return true;
  if (maximized_) return false;
  if (r == Region::side ? size_.cols < kSideMinCols : size_.rows < kBottomMinRows) return false;
  const auto& views = hub_.views();
  return std::any_of(views.begin(), views.end(), [r](const ViewData& v) { return v.region == r; });
}

Region Workbench::where(const ViewData& v) const { return region_shown(v.region) ? v.region : Region::main; }

std::vector<const ViewData*> Workbench::tabs_of(Region r) const {
  std::vector<const ViewData*> out;
  for (const auto& v : hub_.views()) {
    if (where(v) == r) out.push_back(&v);
  }
  return out;
}

const ViewData* Workbench::region_view(Region r) {
  const auto tabs = tabs_of(r);
  if (tabs.empty()) return nullptr;
  RegionState& rs = regions_[ri(r)];
  for (size_t i = 0; i < tabs.size(); ++i) {
    if (tabs[i]->id == rs.active) {
      rs.hint = i;
      return tabs[i];
    }
  }
  rs.hint = std::min(rs.hint, tabs.size() - 1);
  rs.active = tabs[rs.hint]->id;
  return tabs[rs.hint];
}

void Workbench::update_selection() {
  const ViewData* v = active_view();
  if (v && v->native) {  // an editor's tab: where its cursor is
    links_.select_in(v->id);
    return;
  }
  if (!v || v->loading) return;
  ViewState& vs = vstate_[v->id];
  const auto sel = view_selection(v->tree, vs.focus, [&](std::string_view w) {
    const auto it = vs.widgets.find(w);
    return it == vs.widgets.end() ? 0 : it->second.cursor;
  });
  // Only when it changed here: what a Neovim outside reports meanwhile stays the selection.
  if (!sel || (v->id == sel_view_ && *sel == sel_items_)) return;
  sel_view_ = v->id;
  sel_items_ = *sel;
  hub_.set_selection(v->id, *sel);
}

Place Workbench::place_here() {
  Place p;
  p.view = focus_id_;
  const auto it = vstate_.find(focus_id_);
  const ViewData* v = hub_.find_view(focus_id_);
  if (it == vstate_.end() || !v || v->native) return p;
  p.widget = it->second.focus;
  if (const Value* node = p.widget.empty() ? nullptr : find_widget(v->tree, p.widget); node && is_rows((*node)["t"].str())) {
    p.cursor = it->second.widgets[p.widget].cursor;
    if (const Value* row = row_at(*node, p.cursor)) p.key = item_key(*row, size_t(p.cursor));
  }
  return p;
}

namespace {
// Lines "kind<TAB>label<TAB>ref" into fzf: those given, then the files below the current
// directory (rg or fd when there, else find). The chosen line goes to the out file.
constexpr const char* kGotoScript = R"sh(
cands=$1 out=$2
{
  cat "$cands"
  if command -v rg >/dev/null 2>&1; then rg --files 2>/dev/null
  elif command -v fd >/dev/null 2>&1; then fd --type f 2>/dev/null
  else find . \( -name .git -o -name node_modules -o -name .venv -o -name __pycache__ \) -prune -o -type f -print 2>/dev/null | sed 's|^\./||'
  fi | awk 'BEGIN { OFS = "\t" } { print "file", $0, "file:" $0 }'
} | fzf --delimiter='\t' --with-nth=1,2 --tiebreak=length,index --layout=reverse --prompt='Go to › ' > "$out"
)sh";

std::string one_line(std::string s) {
  for (char& c : s) {
    if (c == '\t' || c == '\n' || c == '\r') c = ' ';
  }
  return s;
}
}  // namespace

void Workbench::go_to_anything() {
  const auto dir = std::filesystem::temp_directory_path();
  const std::string tag = "myrcp-" + std::to_string(getpid()) + "-goto";
  const std::string in = (dir / (tag + ".in")).string(), out = (dir / (tag + ".out")).string();
  {
    std::ofstream f(in);
    for (const auto& v : hub_.views()) f << "view\t" << one_line(v.title) << "\tview:" << v.id << "\n";
    for (const auto& m : hub_.markers()) {
      std::string where = m.path.empty() ? m.view : std::filesystem::path(m.path).filename().string();
      if (m.line > 0) where += ":" + std::to_string(m.line);
      f << "problem\t" << one_line(where + "  " + m.message) << "\t";
      if (!m.path.empty()) f << "loc:" << m.line << ":" << m.path << "\n";
      else f << "view:" << m.view << "\n";
    }
  }
  std::filesystem::remove(out);
  open_popup({"/bin/sh", "-c", kGotoScript, "myrcp-goto", in, out}, "Go to Anything",
             [this, in, out](int status) {
               std::string line;
               if (std::ifstream f(out); f) std::getline(f, line);
               std::filesystem::remove(in);
               std::filesystem::remove(out);
               if (status == 127) return set_status("Go to Anything runs fzf: it is not on the PATH", Style::bar_error);
               const auto tab = line.rfind('\t');
               if (status != 0 || tab == std::string::npos) return;
               const std::string ref = line.substr(tab + 1);
               if (ref.starts_with("view:")) {
                 if (hub_.find_view(ref.substr(5))) activate(ref.substr(5));
                 else hub_.open_view(ref.substr(5));
               } else if (ref.starts_with("loc:")) {
                 const auto colon = ref.find(':', 4);
                 open_location(ref.substr(colon + 1), std::atoi(ref.substr(4, colon - 4).c_str()));
               } else if (ref.starts_with("file:")) {
                 open_location(std::filesystem::absolute(ref.substr(5)).string(), 0);
               }
             });
}

void Workbench::go(bool back) {
  const auto exists = [&](const Place& p) { return hub_.find_view(p.view) != nullptr; };
  const auto to = back ? history_.back(place_here(), exists) : history_.forward(place_here(), exists);
  if (!to) return set_status(back ? "Nowhere to go back to" : "Nowhere to go forward to", Style::bar, 3);
  going_ = true;
  activate(to->view);
  going_ = false;
  const ViewData* v = hub_.find_view(to->view);
  if (!v || to->widget.empty()) return;
  ViewState& vs = vstate_[v->id];
  const Value* node = find_widget(v->tree, to->widget);
  if (!node) return;
  vs.focus = to->widget;
  vs.focus_fallback = false;
  if (!is_rows((*node)["t"].str())) return;
  int at = to->cursor;  // the row by its key, when it is among the rows sent
  const int count = row_count(*node);
  for (int i = 0; i < count; ++i) {
    if (const Value* row = row_at(*node, i); row && item_key(*row, size_t(i)) == to->key) {
      at = i;
      break;
    }
  }
  move_cursor(*v, vs, to->widget, *node, at);
}

void Workbench::activate(const std::string& id) {
  if (id != focus_id_ && !going_ && !focus_id_.empty()) history_.left(place_here());
  if (id != focus_id_) sel_view_.clear();  // a view focused again offers its row again
  focus_id_ = id;
  if (const ViewData* v = hub_.find_view(id)) {
    focus_region_ = where(*v);
    regions_[ri(focus_region_)].active = id;
  }
  dirty_ = true;
}

void Workbench::activate_index(size_t i) {
  if (i < hub_.views().size()) activate(hub_.views()[i].id);
}

void Workbench::focus_region(Region r) {
  if (const ViewData* v = region_view(r)) activate(v->id);
}

void Workbench::cycle_region(int step) {
  std::vector<Region> shown;
  for (const Region r : kRegions) {
    if (region_shown(r) && !tabs_of(r).empty()) shown.push_back(r);
  }
  if (shown.size() < 2) return;
  active_view();
  const auto at = size_t(std::find(shown.begin(), shown.end(), focus_region_) - shown.begin()) % shown.size();
  focus_region(shown[(at + shown.size() + size_t(step)) % shown.size()]);
}

void Workbench::focus_direction(std::string_view dir) {
  active_view();
  const Region from = focus_region_;
  Region to = from;
  if (dir == "left" && from == Region::side) to = Region::main;
  else if (dir == "right" && from == Region::main) to = Region::side;
  else if (dir == "down" && from != Region::bottom) to = Region::bottom;
  else if (dir == "up" && from == Region::bottom) to = Region::main;
  if (to != from && region_shown(to)) focus_region(to);
}

void Workbench::move_active(Region r) {
  const ViewData* v = active_view();
  if (!v) return;
  const std::string id = v->id;
  hub_.set_region(id, r);
  activate(id);
  if (r == Region::main) return;
  const char* name = r == Region::side ? "the side" : "the bottom";
  if (maximized_) {
    set_status(v->title + " is at " + name + ": shown there when the regions are restored (Alt+Z)", Style::bar);
  } else if (!region_shown(r)) {
    set_status(v->title + " is at " + name + ": shown there when the terminal is " +
                   (r == Region::side ? std::to_string(kSideMinCols) + " columns" : std::to_string(kBottomMinRows) + " rows") +
                   " or more",
               Style::bar);
  }
}

void Workbench::front_state(Arrangement& a) {
  for (const Region r : kRegions) {
    if (hub_.find_view(regions_[ri(r)].active)) a.shown[ri(r)] = regions_[ri(r)].active;
  }
  if (hub_.find_view(focus_id_)) a.focus = focus_id_;
}

void Workbench::arranged(const Arrangement& a) {
  for (const Region r : kRegions) {
    if (!a.shown[ri(r)].empty() && hub_.find_view(a.shown[ri(r)])) regions_[ri(r)].active = a.shown[ri(r)];
  }
  if (!a.focus.empty() && hub_.find_view(a.focus)) {
    activate(a.focus);
  } else if (!hub_.find_view(focus_id_)) {
    focus_region_ = Region::main;  // its shown view takes the focus
  }
  dirty_ = true;
}

void Workbench::view_closed(const std::string& id) {
  vstate_.erase(id);
  dirty_ = true;
}

void Workbench::close_active() {
  const ViewData* v = active_view();
  if (!v) return;
  const std::string id = v->id;
  if (terms_.contains(id)) return term_ended(id);  // hangs up on the program
  hub_.close_view(id);
  vstate_.erase(id);
}

std::vector<CommandInfo> Workbench::all_commands() const {
  std::vector<CommandInfo> all = builtins_;
  for (const auto& p : hub_.perspectives()) {
    const bool cur = p.id == hub_.perspective();
    all.push_back({"workbench.perspective:" + p.id, "Perspective: " + p.title + (cur ? " (current)" : ""), {}, "", ""});
  }
  all.insert(all.end(), hub_.commands().begin(), hub_.commands().end());
  return all;
}

void Workbench::send_event(const ViewData& v, std::string_view widget, std::string_view kind, Value value) {
  hub_.send_event(v.id, widget, kind, std::move(value));
}

void Workbench::request_windows(const ViewData& v, ViewState& vs) {
  for (auto& [id, ws] : vs.widgets) {
    if (ws.want_first < 0) {
      ws.sent_first = -1;
      continue;
    }
    if (ws.want_first == ws.sent_first && ws.want_count == ws.sent_count) continue;
    ws.sent_first = ws.want_first;
    ws.sent_count = ws.want_count;
    send_event(v, id, "window", Array{ws.want_first, ws.want_count});
  }
}

Value Workbench::form_values(const ViewData& v, const ViewState& vs) const {
  Map form;
  for_each_node(v.tree, [&](const Value& n) {
    if (n["t"].str() != "field") return;
    const auto id = n["id"].str();
    const auto it = vs.widgets.find(id);
    form.emplace_back(id, it != vs.widgets.end() && it->second.seen ? Value(it->second.text) : n["value"]);
  });
  return form;
}

// ---- keys -------------------------------------------------------------------------

void Workbench::handle_key(const Key& k) {
  if (k.name == "unknown" || k.name.empty()) return;
  dirty_ = true;
#ifdef MYRCP_HAVE_VTERM
  if (popup_) {  // it has every key, until it exits
    if (k.name != "mouse") send_term_key(popup_->session.get(), k);
    return;
  }
#endif
  if (k.name == "mouse") return handle_mouse(k.mouse);
  if (palette_.open) return palette_key(k);
  if (menu_.open) return menu_key(k);
  // Ctrl+\ makes the next key the workbench's: in a terminal view, where keys belong
  // to the program (Ctrl+\ twice sends one to it), and anywhere for the arrows that
  // move between regions and z that maximizes.
  TermSession* t = active_term();
  if (prefix_) {
    prefix_ = false;
    status_.clear();
    if (k.name == "ctrl+\\") {
      if (t) send_term_key(t, k);
      return;
    }
    if (k.name == "left" || k.name == "right" || k.name == "up" || k.name == "down") return focus_direction(k.name);
    if (k.name == "z") return run_command("workbench.maximize");
    if (k.name == "shift+left") return run_command("workbench.side:wider");
    if (k.name == "shift+right") return run_command("workbench.side:narrower");
    if (k.name == "shift+up") return run_command("workbench.bottom:taller");
    if (k.name == "shift+down") return run_command("workbench.bottom:shorter");
  } else if (k.name == "ctrl+\\") {
    prefix_ = true;
    set_status("Workbench key… (arrows: region, Shift+arrows: resize, z: maximize, Ctrl+P commands, Ctrl+W close)",
               Style::bar_accent, 10);
    return;
  } else if (t) {
    // Keys belong to the program, except tab switching.
    const bool tab_key = (k.name.size() == 5 && k.name.starts_with("alt+") && k.name[4] >= '1' && k.name[4] <= '9') ||
                         k.name == "alt+left" || k.name == "alt+right" || k.name == "ctrl+pageup" ||
                         k.name == "ctrl+pagedown";
    if (!tab_key) return send_term_key(t, k);
  }
  if (k.name.size() == 5 && k.name.starts_with("alt+") && k.name[4] >= '1' && k.name[4] <= '9')
    return activate_index(size_t(k.name[4] - '1'));
  const ViewData* v = active_view();
  const Value* modal = v && !v->loading ? find_modal(v->tree) : nullptr;
  if (modal && k.name == "escape") return send_event(*v, (*modal)["id"].str(), "dismiss", Value());
  if (v && !v->loading && widget_key(*v, vstate_[v->id], k)) return;
  const std::string active_base = v ? base_view(v->id) : "";
  const auto commands = all_commands();
  for (int pass = 0; pass < 2; ++pass) {
    for (const auto& c : commands) {
      if (modal && !c.plugin.empty()) continue;  // a dialog blocks the view's own keys
      const bool scope_ok = pass == 0 ? (!active_base.empty() && c.view == active_base) : c.view.empty();
      if (scope_ok && std::find(c.keys.begin(), c.keys.end(), k.name) != c.keys.end()) return run_command(c.id);
    }
  }
}

bool Workbench::widget_key(const ViewData& v, ViewState& vs, const Key& k) {
  if (k.name == "tab" || k.name == "shift+tab") {
    if (vs.focus_order.size() < 2) return !vs.focus_order.empty();
    auto it = std::find(vs.focus_order.begin(), vs.focus_order.end(), vs.focus);
    size_t i = it == vs.focus_order.end() ? 0 : size_t(it - vs.focus_order.begin());
    const size_t n = vs.focus_order.size();
    i = k.name == "tab" ? (i + 1) % n : (i + n - 1) % n;
    vs.focus = vs.focus_order[i];
    vs.focus_fallback = false;
    return true;
  }
  if (vs.focus.empty()) return false;
  const Value* node = find_widget(v.tree, vs.focus);
  if (!node) return false;
  WidgetState& ws = vs.widgets[vs.focus];
  const auto type = (*node)["t"].str();
  const std::string& name = k.name;

  if (type == "tree" || (type == "table" && !(*node)["tree"].is_nil())) {
    if (const Value* row = row_at(*node, ws.cursor)) {
      const std::string key = item_key(*row, size_t(ws.cursor));
      if (name == "right") return send_event(v, vs.focus, "expand", key), true;
      if (name == "left") return send_event(v, vs.focus, "collapse", key), true;
      if (name == "space" && !(*row)["check"].is_nil()) return send_event(v, vs.focus, "toggle", key), true;
    }
  }
  if (is_rows(type)) {
    const int count = row_count(*node);
    int c = ws.cursor;
    const int page = std::max(1, ws.page - 1);
    if (name == "up") c -= 1;
    else if (name == "down") c += 1;
    else if (name == "pageup") c -= page;
    else if (name == "pagedown") c += page;
    else if (name == "home") c = 0;
    else if (name == "end") c = count - 1;
    else if (name == "enter") {
      if (const Value* row = row_at(*node, ws.cursor)) send_event(v, vs.focus, "activate", item_key(*row, size_t(ws.cursor)));
      return true;
    } else {
      return false;
    }
    move_cursor(v, vs, vs.focus, *node, c);
    return true;
  }
  if (type == "log") {
    const int count = int((*node)["lines"].array().size());
    const int max_top = std::max(0, count - ws.page);
    int s = ws.scroll;
    if (name == "up") s -= 1;
    else if (name == "down") s += 1;
    else if (name == "pageup") s -= std::max(1, ws.page - 1);
    else if (name == "pagedown") s += std::max(1, ws.page - 1);
    else if (name == "home") s = 0;
    else if (name == "end") s = max_top;
    else return false;
    ws.scroll = std::clamp(s, 0, max_top);
    ws.follow = ws.scroll >= max_top;
    return true;
  }
  if (type == "button") {
    if (name == "enter" || name == "space") {
      send_event(v, vs.focus, "press", Map{{"form", form_values(v, vs)}});
      return true;
    }
    return false;
  }
  if (type == "checkbox") {
    if (name == "enter" || name == "space") {
      send_event(v, vs.focus, "toggle", !(*node)["checked"].boolean());
      return true;
    }
    return false;
  }
  if (type == "field") {
    std::string& t = ws.text;
    if (name == "paste" || !k.text.empty()) {
      std::string ins = k.text;
      for (char& ch : ins) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
      }
      t.insert(ws.caret, ins);
      ws.caret += ins.size();
    } else if (name == "backspace") {
      if (ws.caret > 0) {
        const size_t p = utf8::prev(t, ws.caret);
        t.erase(p, ws.caret - p);
        ws.caret = p;
      }
    } else if (name == "delete") {
      if (ws.caret < t.size()) t.erase(ws.caret, utf8::next(t, ws.caret) - ws.caret);
    } else if (name == "left") {
      ws.caret = utf8::prev(t, ws.caret);
    } else if (name == "right") {
      ws.caret = utf8::next(t, ws.caret);
    } else if (name == "home" || name == "ctrl+a") {
      ws.caret = 0;
    } else if (name == "end" || name == "ctrl+e") {
      ws.caret = t.size();
    } else if (name == "ctrl+u") {
      t.erase(0, ws.caret);
      ws.caret = 0;
    } else if (name == "ctrl+k") {
      t.erase(ws.caret);
    } else if (name == "enter") {
      ws.seen_value = t;  // the plugin echoing or clearing the value both take effect
      send_event(v, vs.focus, "submit", t);
    } else {
      return false;
    }
    return true;
  }
  return false;
}

void Workbench::move_cursor(const ViewData& v, ViewState& vs, const std::string& id, const Value& node, int to) {
  WidgetState& ws = vs.widgets[id];
  to = std::clamp(to, 0, std::max(0, row_count(node) - 1));
  if (to == ws.cursor) return;
  ws.cursor = to;
  if (const Value* row = row_at(node, to)) send_event(v, id, "select", item_key(*row, size_t(to)));
}

void Workbench::handle_mouse(const Mouse& m) {
  // Dragging the side's separator or the bottom's tab bar resizes them.
  if (m.button == "drag" || m.button == "release") {
    if (drag_ == Drag::none) return;
    const Rect body{0, 1, size_.cols, size_.rows - 2};
    if (drag_ == Drag::side) drag_sizes_.side = (size_.cols - m.x) * 100 / std::max(1, size_.cols);
    else drag_sizes_.bottom = (body.y + body.h - m.y) * 100 / std::max(1, body.h);
    drag_sizes_ = clamp_sizes(drag_sizes_);
    if (m.button == "release") {
      drag_ = Drag::none;
      hub_.set_sizes(drag_sizes_);  // saved once, when let go
    }
    return;
  }
  if (palette_.open) return palette_mouse(m);
  if (menu_.open) {
    menu_mouse(m);
    if (menu_.open || m.button != "right") return;  // a right click elsewhere: a menu there
  }
  if (m.button == "right") {  // a tab's menu
    for (const auto& t : tab_hits_) {
      if (m.y != t.y || m.x < t.x0 || m.x >= t.x1) continue;
      activate(t.id);
      if (const ViewData* v = hub_.find_view(t.id)) open_menu(m.x, m.y, tab_menu(*v));
      return;
    }
  }
  if (m.button == "left" && ((side_sep_x_ >= 0 && m.x == side_sep_x_ && (bottom_bar_y_ < 0 || m.y < bottom_bar_y_)) ||
                             (bottom_bar_y_ >= 0 && m.y == bottom_bar_y_))) {
    drag_ = m.y == bottom_bar_y_ ? Drag::bottom : Drag::side;
    drag_sizes_ = hub_.sizes();
  }
  if (m.button == "left") {  // the tab bars
    for (const auto& t : tab_hits_) {
      if (m.y == t.y && m.x >= t.x0 && m.x < t.x1) return activate(t.id);
    }
  }
  // The region under the pointer: a click focuses it, the wheel scrolls it as it is.
  const ViewData* v = nullptr;
  Rect area;
  for (const Region r : kRegions) {
    if (!inside(regions_[ri(r)].rect, m.x, m.y)) continue;
    v = region_view(r);
    area = regions_[ri(r)].rect;
    if (v && (m.button == "left" || m.button == "right") && r != focus_region_) activate(v->id);
  }
  if (!v) return;
#ifdef MYRCP_HAVE_VTERM
  if (v->native) {
    const auto it = terms_.find(v->id);
    if (m.button == "right" && (it == terms_.end() || !it->second.session->wants_mouse())) {
      return open_menu(m.x, m.y, tab_menu(*v));  // the program has no use for it
    }
    if (it != terms_.end()) send_term_mouse(it->second.session.get(), m, m.x - area.x, m.y - area.y);
    return;
  }
#endif
  if (m.button == "right" && !v->loading && !v->native) {
    // On a row: the cursor goes there first, so the commands act on it.
    ViewState& vs = vstate_[v->id];
    const Hit* h = nullptr;
    for (const auto& hit : vs.hits) {
      if (inside(hit.rect, m.x, m.y)) h = &hit;
    }
    if (h && std::find(vs.focus_order.begin(), vs.focus_order.end(), h->id) != vs.focus_order.end()) {
      vs.focus = h->id;
      vs.focus_fallback = false;
    }
    if (h && is_rows(h->type)) {
      if (const Value* node = find_widget(v->tree, h->id)) {
        const int idx = h->first + (m.y - h->rect.y);
        if (idx < row_count(*node)) move_cursor(*v, vs, h->id, *node, idx);
      }
    }
    return open_menu(m.x, m.y, view_menu(*v));
  }
  if (m.button == "right") return open_menu(m.x, m.y, tab_menu(*v));
  if (v->native || v->loading) return;
  ViewState& vs = vstate_[v->id];
  const Hit* h = nullptr;
  for (const auto& hit : vs.hits) {
    if (inside(hit.rect, m.x, m.y)) h = &hit;  // the last drawn is on top
  }
  if (!h) return;
  const std::string id = h->id;
  const Value* node = find_widget(v->tree, id);
  if (!node) return;
  WidgetState& ws = vs.widgets[id];
  const bool wheel = m.button == "wheelup" || m.button == "wheeldown";
  const int step = m.button == "wheelup" ? -3 : 3;
  if (h->type == "log") {
    if (!wheel) return;
    const int count = int((*node)["lines"].array().size());
    const int max_top = std::max(0, count - ws.page);
    ws.scroll = std::clamp(ws.scroll + step, 0, max_top);
    ws.follow = ws.scroll >= max_top;
    return;
  }
  if (wheel) {
    if (is_rows(h->type)) move_cursor(*v, vs, id, *node, ws.cursor + step);
    return;
  }
  if (m.button != "left") return;
  if (std::find(vs.focus_order.begin(), vs.focus_order.end(), id) != vs.focus_order.end()) {
    vs.focus = id;
    vs.focus_fallback = false;
  }
  if (h->type == "header") {  // a sortable table's title: by it, then the other way, then not
    int col = -1;
    for (size_t c = 0; c < h->col_x.size(); ++c) {
      if (m.x >= h->col_x[c]) col = int(c);
    }
    if (col < 0 || !(*node)["columns"].at(size_t(col))["sortable"].boolean(true)) return;
    const auto [now, desc] = sort_state(ws, *node);
    if (now == col && desc) sort_table(*v, vs, id, *node, -1, false);
    else sort_table(*v, vs, id, *node, col, now == col);
    return;
  }

  if (h->type == "button") {
    send_event(*v, id, "press", Map{{"form", form_values(*v, vs)}});
  } else if (h->type == "checkbox") {
    send_event(*v, id, "toggle", !(*node)["checked"].boolean());
  } else if (h->type == "field") {
    // Put the caret at the clicked column.
    int col = m.x - h->rect.x + ws.scroll;
    size_t i = 0;
    while (i < ws.text.size()) {
      size_t j = i;
      const int w = utf8::width(utf8::decode(ws.text, j));
      if (col < w) break;
      col -= w;
      i = j;
    }
    ws.caret = i;
  } else if (is_rows(h->type)) {
    const int idx = h->first + (m.y - h->rect.y);
    const Value* row = row_at(*node, idx);
    if (idx >= row_count(*node) || !row) return;
    const std::string key = item_key(*row, size_t(idx));
    if (h->type == "tree") {
      const int ax = h->rect.x + 2 * int((*row)["depth"].integer());
      if (m.x == ax && (*row)["expandable"].boolean()) {
        move_cursor(*v, vs, id, *node, idx);
        send_event(*v, id, (*row)["expanded"].boolean() ? "collapse" : "expand", key);
        return;
      }
      if (!(*row)["check"].is_nil() && m.x >= ax + 2 && m.x < ax + 5) {
        move_cursor(*v, vs, id, *node, idx);
        send_event(*v, id, "toggle", key);
        return;
      }
    }
    if (h->type == "table") {
      int col = -1;
      for (size_t c = 0; c < h->col_x.size(); ++c) {
        if (m.x >= h->col_x[c]) col = int(c);
      }
      if (const Value& tc = (*node)["tree"]; !tc.is_nil() && col == int(tc.integer(-1)) && (*row)["expandable"].boolean() &&
                                             m.x == h->col_x[size_t(col)] + 2 * int((*row)["depth"].integer())) {
        move_cursor(*v, vs, id, *node, idx);
        send_event(*v, id, (*row)["expanded"].boolean() ? "collapse" : "expand", key);
        return;
      }
      const bool clickable = col >= 0 && (*node)["columns"].at(size_t(col))["clickable"].boolean();
      if (clickable || m.shift || m.ctrl) {
        move_cursor(*v, vs, id, *node, idx);
        send_event(*v, id, "cell", Map{{"key", key}, {"column", col}, {"shift", m.shift}, {"ctrl", m.ctrl}});
        return;
      }
    }
    if (idx == ws.cursor) send_event(*v, id, "activate", key);  // a click on the cursor row opens it
    else move_cursor(*v, vs, id, *node, idx);
  }
}

void Workbench::palette_mouse(const Mouse& m) {
  const int count = int(palette_.matches.size());
  if (m.button == "wheelup" || m.button == "wheeldown") {
    palette_.cursor = std::clamp(palette_.cursor + (m.button == "wheelup" ? -1 : 1), 0, std::max(0, count - 1));
    return;
  }
  if (m.button != "left") return;
  if (!inside(palette_.box, m.x, m.y)) {
    palette_.open = false;
    return;
  }
  const int idx = palette_.scroll + (m.y - palette_.rows_y);
  if (m.y >= palette_.rows_y && idx < count && m.y < palette_.box.y + palette_.box.h - 1) {
    const std::string id = palette_.matches[size_t(idx)].id;
    palette_.open = false;
    run_command(id);
  }
}

void Workbench::palette_key(const Key& k) {
  const std::string& name = k.name;
  const int count = int(palette_.matches.size());
  if (name == "escape" || name == "ctrl+p" || name == "ctrl+q") {
    palette_.open = false;
  } else if (name == "enter") {
    if (count > 0) {
      const std::string id = palette_.matches[size_t(palette_.cursor)].id;
      palette_.open = false;
      run_command(id);
    }
  } else if (name == "up") {
    palette_.cursor = std::max(0, palette_.cursor - 1);
  } else if (name == "down") {
    palette_.cursor = std::min(count - 1, palette_.cursor + 1);
  } else if (name == "pageup") {
    palette_.cursor = std::max(0, palette_.cursor - 10);
  } else if (name == "pagedown") {
    palette_.cursor = std::max(0, std::min(count - 1, palette_.cursor + 10));
  } else if (name == "backspace") {
    if (!palette_.text.empty()) {
      palette_.text.erase(utf8::prev(palette_.text, palette_.text.size()));
      refilter();
    }
  } else if (name == "ctrl+u") {
    palette_.text.clear();
    refilter();
  } else if (name == "paste" || !k.text.empty()) {
    palette_.text += k.text;
    refilter();
  }
}

std::vector<Workbench::PaletteEntry> Workbench::palette_entries() {
  std::vector<PaletteEntry> out;
  const ViewData* v = active_view();
  const std::string base = v ? base_view(v->id) : "";
  for (const auto& c : all_commands()) {
    if (!c.view.empty() && c.view != base) continue;
    out.push_back({c.id, c.title, c.keys.empty() ? "" : c.keys.front()});
  }
  for (const auto& vi : hub_.view_infos()) out.push_back({"view.open:" + vi.id, "View: Open " + vi.title, ""});
  return out;
}

void Workbench::refilter() {
  struct Scored {
    int score;
    size_t index;
  };
  auto entries = palette_entries();
  std::vector<Scored> scored;
  for (size_t i = 0; i < entries.size(); ++i) {
    const int s = fuzzy_score(entries[i].title, palette_.text);
    if (s >= 0) scored.push_back({s, i});
  }
  std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score < b.score; });
  palette_.matches.clear();
  for (const auto& s : scored) palette_.matches.push_back(entries[s.index]);
  palette_.cursor = 0;
  palette_.scroll = 0;
}

std::pair<int, bool> Workbench::sort_state(const WidgetState& ws, const Value& node) {
  const Value& sorted = node["sorted"];
  if (!ws.sort_from.empty() && ws.sort_from == mp::encode(sorted)) return {ws.sort_col, ws.sort_desc};
  return {sorted.is_array() ? int(sorted.at(0).integer(-1)) : -1, sorted.is_array() && sorted.at(1).boolean()};
}

void Workbench::sort_table(const ViewData& v, ViewState& vs, const std::string& id, const Value& node, int column,
                           bool descending) {
  WidgetState& ws = vs.widgets[id];
  const Value* row = row_at(node, ws.cursor);
  const Value key = row ? Value(item_key(*row, size_t(ws.cursor))) : Value();
  if (ws.sort_from.empty() || ws.sort_from != mp::encode(node["sorted"])) ws.sort_from = mp::encode(node["sorted"]);
  ws.sort_col = column;
  ws.sort_desc = descending;
  send_event(v, id, "sort", Array{column, descending, key});
}

void Workbench::sort_focused(bool next_column) {
  const ViewData* v = active_view();
  if (!v || v->native || v->loading) return;
  ViewState& vs = vstate_[v->id];
  const Value* node = vs.focus.empty() ? nullptr : find_widget(v->tree, vs.focus);
  if (!node || !(*node)["sortable"].boolean()) {
    set_status("Not a table that sorts", Style::bar);
    return;
  }
  const auto& cols = (*node)["columns"].array();
  const auto [now, desc] = sort_state(vs.widgets[vs.focus], *node);
  if (!next_column) {
    if (now >= 0) sort_table(*v, vs, vs.focus, *node, now, !desc);
    return;
  }
  int col = now + 1;  // the next column that sorts; after the last, the plugin's order
  while (col < int(cols.size()) && !cols[size_t(col)]["sortable"].boolean(true)) ++col;
  if (col >= int(cols.size())) col = -1;
  sort_table(*v, vs, vs.focus, *node, col, false);
  if (col >= 0) {
    set_status("Sorted by " + std::string(cols[size_t(col)]["title"].str()), Style::bar);
  } else {
    set_status("In the plugin's order", Style::bar);
  }
}

void Workbench::run_command(const std::string& id) {
  dirty_ = true;
  if (id == "workbench.quit") {
    quit_ = true;
  } else if (id == "workbench.palette") {
    palette_.open = true;
    palette_.text.clear();
    refilter();
  } else if (id == "workbench.nextView" || id == "workbench.prevView") {
    // Within the focused region (Alt+1…9 reach any tab).
    if (const ViewData* v = active_view()) {
      const auto tabs = tabs_of(focus_region_);
      const size_t n = tabs.size();
      const size_t at = size_t(std::find(tabs.begin(), tabs.end(), v) - tabs.begin());
      if (n > 1 && at < n) activate(tabs[id == "workbench.nextView" ? (at + 1) % n : (at + n - 1) % n]->id);
    }
  } else if (id == "workbench.goto") {
    go_to_anything();
  } else if (id == "workbench.back" || id == "workbench.forward") {
    go(id == "workbench.back");
  } else if (id == "workbench.nextRegion" || id == "workbench.prevRegion") {
    cycle_region(id == "workbench.nextRegion" ? 1 : -1);
  } else if (id == "workbench.contextMenu") {
    context_menu_here();
  } else if (id == "workbench.table.sort" || id == "workbench.table.reverse") {
    sort_focused(id == "workbench.table.sort");
  } else if (id == "workbench.maximize") {
    const ViewData* v = active_view();
    const std::string keep = v ? v->id : "";
    maximized_ = !maximized_;
    if (!keep.empty()) activate(keep);
  } else if (id.starts_with("workbench.move:")) {
    move_active(region_from_name(id.substr(15)));
  } else if (id == "workbench.closeView") {
    close_active();
  } else if (id == "workbench.restartHost") {
    hub_.restart();
  } else if (id == "workbench.showLog") {
    open_editor(hub_.options().log_path, 0);
  } else if (id == "workbench.cancelJobs") {
    hub_.cancel_jobs();
  } else if (id == "workbench.suspend") {
    suspend();
  } else if (id == "workbench.problems") {
    hub_.open_view(Hub::kProblems);
    activate(Hub::kProblems);
  } else if (id == "workbench.properties") {
    const ViewData* v = active_view();
    const std::string back = v ? v->id : "";
    hub_.open_view(Hub::kProperties);
    // Shown beside the view it describes when there is room, which keeps the focus.
    if (const ViewData* p = hub_.find_view(Hub::kProperties); p && where(*p) != focus_region_) {
      regions_[ri(where(*p))].active = Hub::kProperties;
      if (!back.empty()) activate(back);
    } else {
      activate(Hub::kProperties);
    }
  } else if (id == "workbench.terminal") {
    open_term({env_or("SHELL", "/bin/sh")}, "", "", nullptr, Region::bottom);
  } else if (id.starts_with("workbench.perspective:")) {
    hub_.switch_perspective(id.substr(22));
  } else if (id == "workbench.perspective.reset") {
    hub_.reset_perspective();
  } else if (id.starts_with("workbench.side:") || id.starts_with("workbench.bottom:")) {
    Sizes s = hub_.sizes();
    const int step = id.ends_with("wider") || id.ends_with("taller") ? 4 : -4;
    (id.starts_with("workbench.side:") ? s.side : s.bottom) += step;
    hub_.set_sizes(s);
    const Region r = id.starts_with("workbench.side:") ? Region::side : Region::bottom;
    if (!region_shown(r)) set_status(std::string("No ") + (r == Region::side ? "side" : "bottom") + " shown to resize", Style::bar);
  } else if (id == "workbench.preferences") {
    hub_.open_view(Hub::kPreferences);
    activate(Hub::kPreferences);
  } else if (id == "workbench.editPrefs") {
    const std::string path = hub_.options().prefs_path;
    if (!std::filesystem::exists(path)) {  // something to start from
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
      std::ofstream(path) << "# myrcp preferences: [workbench] and a table per plugin. The Preferences view\n"
                             "# lists every setting; myrcp keeps your comments when it writes here.\n";
    }
    open_editor_later(path, 0, [this](bool) { hub_.reload_prefs(); });
  } else if (id.starts_with("workbench.theme:")) {
    if (const std::string err = hub_.set_pref("workbench.theme", id.substr(16)); !err.empty()) set_status(err, Style::bar_error);
    else if (hub_.pref_source(*hub_.find_pref("workbench.theme")).starts_with("$"))
      set_status("$MYRCP_THEME is set and wins over the preference", Style::bar);
    redraw_all();
  } else if (id.starts_with("view.open:")) {
    const std::string view = id.substr(10);
    hub_.open_view(view);
    activate(view);
  } else {
    // A plugin command acts on what the cursor is on: tell it the focused row.
    Map params;
    if (const ViewData* v = active_view()) {
      const ViewState& vs = vstate_[v->id];
      params.emplace_back("view", v->id);
      params.emplace_back("widget", vs.focus);
      const Value* node = vs.focus.empty() ? nullptr : find_widget(v->tree, vs.focus);
      if (node && is_rows((*node)["t"].str())) {
        const auto ws = vs.widgets.find(vs.focus);
        const int cursor = ws == vs.widgets.end() ? 0 : ws->second.cursor;
        if (const Value* row = row_at(*node, cursor)) {
          params.emplace_back("key", item_key(*row, size_t(cursor)));
          params.emplace_back("index", cursor);
        }
      }
    }
    if (const std::string err = hub_.run_command(id, std::move(params)); !err.empty()) set_status(err, Style::bar_error);
  }
}

// ---- drawing ------------------------------------------------------------------------

void Workbench::status(const std::string& text, std::string_view style, int seconds) {
  set_status(text, style == "error" ? Style::bar_error : style == "ok" ? Style::bar_accent : Style::bar, seconds);
}

void Workbench::set_status(std::string text, Style style, int seconds) {
  status_ = std::move(text);
  status_style_ = style;
  status_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  dirty_ = true;
}

void Workbench::expire_status() {
  if (!status_.empty() && std::chrono::steady_clock::now() >= status_until_) {
    status_.clear();
    dirty_ = true;
  }
}

int Workbench::status_timeout_ms() const {
  if (status_.empty()) return -1;
  const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(status_until_ - std::chrono::steady_clock::now());
  return int(std::max<int64_t>(0, left.count()) + 1);
}

void Workbench::draw() {
  size_ = term_.size();
  const int W = size_.cols, H = size_.rows;
  Grid g(W, H);
  const ViewData* active = active_view();
  for (auto& rs : regions_) rs.rect = {};
  tab_hits_.clear();
  side_sep_x_ = bottom_bar_y_ = -1;
  if (H < 4 || W < 20) {
    g.text(0, 0, "myrcp: terminal too small", Style::warning);
  } else {
    // The regions: main at the top left, the side at its right, the bottom below both.
    // Main's tabs are in the top bar, the side's there too above it, the bottom's in a
    // bar of their own.
    const Rect body{0, 1, W, H - 2};
    const bool side = region_shown(Region::side), bottom = region_shown(Region::bottom);
    const Sizes sizes = drag_ != Drag::none ? drag_sizes_ : hub_.sizes();
    const int bh = bottom ? std::clamp(body.h * sizes.bottom / 100, 6, body.h - 10) : 0;  // with its bar
    const int top_h = body.h - bh;
    const int sw = side ? std::clamp(W * sizes.side / 100, 28, W - 60) : 0;  // with the separator
    side_sep_x_ = side ? W - sw : -1;
    bottom_bar_y_ = bottom ? body.y + top_h : -1;
    regions_[ri(Region::main)].rect = {0, body.y, W - sw, top_h};
    if (side) regions_[ri(Region::side)].rect = {W - sw + 1, body.y, sw - 1, top_h};
    if (bottom) regions_[ri(Region::bottom)].rect = {0, body.y + top_h + 1, W, bh - 1};

    g.fill({0, 0, W, 1}, Style::bar);
    std::string brand = " " + hub_.options().app_name + " ";
    if (hub_.perspective() != Hub::kDefault) {
      const PerspectiveInfo* p = hub_.find_perspective(hub_.perspective());
      brand += "▸ " + (p ? p->title : hub_.perspective()) + " ";
    }
    int x = g.text(0, 0, brand, Style::bar_accent);
    x = g.text(x, 0, " ", Style::bar);
    draw_tabs(g, x, 0, W - sw, Region::main, region_view(Region::main));
    if (side) {
      const Style sep = focus_region_ == Region::side ? Style::border_focus : Style::border;
      g.put(W - sw, 0, U'│', Style::bar, W);
      for (int y = body.y; y < body.y + top_h; ++y) g.put(W - sw, y, U'│', sep, W);
      draw_tabs(g, W - sw + 1, 0, W, Region::side, region_view(Region::side));
    }
    if (bottom) {
      const int y = body.y + top_h;
      g.fill({0, y, W, 1}, Style::bar);
      draw_tabs(g, 1, y, W, Region::bottom, region_view(Region::bottom));
    }
    for (const Region r : kRegions) {
      if (regions_[ri(r)].rect.w > 0) draw_region(g, r, region_view(r));
    }

    // Status bar: an error, else jobs, else the status message on the left; key hints on the right.
    const int y = H - 1;
    g.fill({0, y, W, 1}, Style::bar);
    static constexpr std::pair<const char*, const char*> view_hints[] = {{"Tab", "focus"}, {"^P", "commands"}, {"^Q", "quit"}};
    static constexpr std::pair<const char*, const char*> split_hints[] = {
        {"Tab", "focus"}, {"F6", "region"}, {"^P", "commands"}, {"^Q", "quit"}};
    static constexpr std::pair<const char*, const char*> term_hints[] = {{"^\\", "workbench keys"}, {"Alt+1…9", "tabs"}};
    const bool in_term = active && active->native;
    using Hints = std::span<const std::pair<const char*, const char*>>;
    const auto hints = in_term ? Hints(term_hints) : side || bottom ? Hints(split_hints) : Hints(view_hints);
    int hints_w = 1;
    for (const auto& [k, what] : hints) hints_w += utf8::width(k) + utf8::width(what) + 3;
    const int hx = std::max(0, W - hints_w);
    int hxx = hx + 1;
    for (const auto& [k, what] : hints) {
      hxx = g.text(hxx, y, k, Style::bar_accent);
      hxx = g.text(hxx, y, std::string(" ") + what + "  ", Style::bar);
    }
    const auto& jobs = hub_.jobs();
    if (!status_.empty() && status_style_ == Style::bar_error) {
      g.text(0, y, " " + status_ + " ", status_style_, hx - 1);
    } else if (!jobs.empty()) {
      const auto& [id, job] = *jobs.begin();
      std::string s = " ⟳ " + job.title + " " + std::to_string(int(job.value * 100 + 0.5)) + "%";
      if (!job.label.empty()) s += " " + job.label;
      if (jobs.size() > 1) s += "  (+" + std::to_string(jobs.size() - 1) + " jobs)";
      g.text(0, y, s + " ", Style::bar_accent, hx - 1);
    } else if (!status_.empty()) {
      g.text(0, y, " " + status_ + " ", status_style_, hx - 1);
    }
    if (palette_.open) draw_palette(g);
    if (menu_.open) draw_menu(g);
    if (popup_) draw_popup(g);
  }
  term_.write(diff(front_, g, *theme_));
  front_ = std::move(g);
}

int Workbench::draw_tabs(Grid& g, int x, int y, int xe, Region r, const ViewData* shown) {
  // Numbered as Alt+1…9 reach them: in the hub's order, whichever region they are in.
  const auto& views = hub_.views();
  for (size_t i = 0; i < views.size(); ++i) {
    if (where(views[i]) != r) continue;
    const std::string label = " " + std::to_string(i + 1) + " " + views[i].title + " ";
    const Style st = &views[i] != shown ? Style::bar : r == focus_region_ ? Style::bar_active : Style::bar_accent;
    const int x0 = x;
    x = g.text(x, y, label, st, xe);
    tab_hits_.push_back({x0, x, y, views[i].id});
  }
  return x;
}

void Workbench::draw_region(Grid& g, Region r, const ViewData* v) {
  const Rect area = regions_[ri(r)].rect;
  const bool focused = r == focus_region_ && !palette_.open;
  if (!v) {
    const std::string a = hub_.ready() ? "No views open." : "Starting plugins…";
    const std::string b = "Ctrl+P shows all commands, Ctrl+Q quits.";
    const int y = area.y + area.h / 2;
    g.text(area.x + std::max(0, (area.w - utf8::width(a)) / 2), y - 1, a, Style::title, area.x + area.w);
    g.text(area.x + std::max(0, (area.w - utf8::width(b)) / 2), y, b, Style::dim, area.x + area.w);
  } else if (v->native) {
    draw_term(g, area, v->id, focused);
  } else if (v->loading) {
    g.text(area.x + 2, area.y + 1, "Loading " + v->title + "…", Style::dim, area.x + area.w);
  } else {
    render_view(g, area, v->tree, vstate_[v->id], focused);
  }
}

void Workbench::draw_palette(Grid& g) {
  const int W = g.width(), H = g.height();
  const int w = std::min(W - 4, 76), h = std::min(H - 3, 18);
  if (w < 20 || h < 4) return;
  const int x0 = (W - w) / 2, y0 = std::min(2, H - h);
  palette_.box = {x0, y0, w, h};
  palette_.rows_y = y0 + 2;
  g.fill({x0, y0, w, h}, Style::normal);
  draw_box(g, {x0, y0, w, h}, "Commands", Style::border_focus, Style::border_focus);
  g.set_layer({x0, y0, w, h}, Layer::dialog);  // what is drawn into it below stays there
  g.shadow({x0, y0, w, h});
  const int xe = x0 + w - 1;
  int x = g.text(x0 + 2, y0 + 1, "> ", Style::accent, xe);
  x = g.text(x, y0 + 1, palette_.text, Style::normal, xe - 1);
  g.put(x, y0 + 1, U' ', Style::selected, xe);

  const int rows = h - 3;
  const int count = int(palette_.matches.size());
  if (count == 0) {
    g.text(x0 + 2, y0 + 2, "No matching commands", Style::dim, xe);
    return;
  }
  palette_.cursor = std::clamp(palette_.cursor, 0, count - 1);
  if (palette_.cursor < palette_.scroll) palette_.scroll = palette_.cursor;
  if (palette_.cursor >= palette_.scroll + rows) palette_.scroll = palette_.cursor - rows + 1;
  for (int row = 0; row < rows && palette_.scroll + row < count; ++row) {
    const auto& e = palette_.matches[size_t(palette_.scroll + row)];
    const int y = y0 + 2 + row;
    const bool cur = palette_.scroll + row == palette_.cursor;
    const Style st = cur ? Style::selected : Style::normal;
    if (cur) g.fill({x0 + 1, y, w - 2, 1}, st);
    const int end = g.text(x0 + 2, y, e.title, st, xe - 1);
    if (!e.key.empty()) {
      const int kx = xe - 1 - utf8::width(e.key);
      if (kx > end) g.text(kx, y, e.key, cur ? st : Style::dim, xe);
    }
  }
}

// ---- the context menu ---------------------------------------------------------------

Workbench::PaletteEntry Workbench::entry(const std::string& id) {
  for (const std::vector<CommandInfo>* list : {static_cast<const std::vector<CommandInfo>*>(&builtins_), &hub_.commands()}) {
    for (const auto& c : *list) {
      if (c.id != id) continue;
      std::string key;
      for (const auto& k : c.keys) key += (key.empty() ? "" : " ") + k;
      return {c.id, c.title, key};
    }
  }
  return {id, id, ""};
}

std::vector<Workbench::PaletteEntry> Workbench::view_menu(const ViewData& v) {
  std::vector<PaletteEntry> items;
  const std::string base = base_view(v.id);
  for (const auto& c : hub_.commands()) {  // the view's own, as its manifest lists them
    if (c.view != base) continue;
    PaletteEntry e = entry(c.id);
    if (const auto colon = e.title.find(": "); colon != std::string::npos) e.title = e.title.substr(colon + 2);
    items.push_back(std::move(e));  // "Files: Open in editor" is "Open in editor" here
  }
  if (!items.empty()) items.push_back({});
  items.push_back(entry("workbench.properties"));
  const ViewState& vs = vstate_[v.id];
  if (const Value* node = vs.focus.empty() ? nullptr : find_widget(v.tree, vs.focus); node && (*node)["sortable"].boolean()) {
    items.push_back(entry("workbench.table.sort"));
    if ((*node)["sorted"].is_array()) items.push_back(entry("workbench.table.reverse"));
  }
  items.push_back({});
  for (auto& e : tab_menu(v)) items.push_back(std::move(e));
  return items;
}

std::vector<Workbench::PaletteEntry> Workbench::tab_menu(const ViewData& v) {
  std::vector<PaletteEntry> items;
  for (const Region r : kRegions) {
    if (r != v.region) items.push_back(entry(std::string("workbench.move:") + region_name(r)));
  }
  PaletteEntry max = entry("workbench.maximize");
  max.title = maximized_ ? "View: Restore the regions" : "View: Maximize";
  items.push_back(std::move(max));
  items.push_back(entry("workbench.closeView"));
  return items;
}

void Workbench::open_menu(int x, int y, std::vector<PaletteEntry> items) {
  if (items.empty()) return;
  menu_.open = true;
  menu_.x = x;
  menu_.y = y;
  menu_.items = std::move(items);
  menu_.cursor = 0;
  dirty_ = true;
}

void Workbench::context_menu_here() {
  const ViewData* v = active_view();
  if (!v) return;
  if (v->native || v->loading) return open_menu(4, 2, tab_menu(*v));
  // At the focused row as it was drawn, else at the top of the view.
  const ViewState& vs = vstate_[v->id];
  const Rect area = regions_[ri(where(*v))].rect;
  int x = area.x + 2, y = area.y + 1;
  for (const auto& h : vs.hits) {
    if (h.id != vs.focus) continue;
    const auto ws = vs.widgets.find(h.id);
    const int row = ws == vs.widgets.end() ? 0 : ws->second.cursor - h.first;
    x = h.rect.x + 2;
    y = h.rect.y + (is_rows(h.type) ? std::clamp(row, 0, h.rect.h - 1) : 0);
  }
  open_menu(x, y, view_menu(*v));
}

void Workbench::menu_key(const Key& k) {
  const int n = int(menu_.items.size());
  auto step = [&](int d) {  // past separators
    int c = menu_.cursor;
    for (int i = 0; i < n; ++i) {
      c = (c + d + n) % n;
      if (!menu_.items[size_t(c)].id.empty()) break;
    }
    menu_.cursor = c;
  };
  if (k.name == "escape" || k.name == "shift+f10" || k.name == "left") {
    menu_.open = false;
  } else if (k.name == "up") {
    step(-1);
  } else if (k.name == "down" || k.name == "tab") {
    step(1);
  } else if (k.name == "home") {
    menu_.cursor = n - 1, step(1);
  } else if (k.name == "end") {
    menu_.cursor = 0, step(-1);
  } else if (k.name == "enter" || k.name == "space" || k.name == "right") {
    const std::string id = menu_.items[size_t(menu_.cursor)].id;
    menu_.open = false;
    if (!id.empty()) run_command(id);
  } else {
    menu_.open = false;  // any other key: the menu goes, the key is the workbench's
    return handle_key(k);
  }
  dirty_ = true;
}

void Workbench::menu_mouse(const Mouse& m) {
  dirty_ = true;
  const Rect& b = menu_.box;
  const int idx = m.y - (b.y + 1);
  const bool on_item = inside(b, m.x, m.y) && idx >= 0 && idx < int(menu_.items.size());
  if (m.button == "wheelup" || m.button == "wheeldown") {
    menu_key(Key(m.button == "wheelup" ? "up" : "down"));
    return;
  }
  if (m.button != "left" && m.button != "right") return;
  if (!on_item) {
    menu_.open = false;  // a click elsewhere closes it
    return;
  }
  const std::string id = menu_.items[size_t(idx)].id;
  if (id.empty()) return;  // a separator
  menu_.open = false;
  run_command(id);
}

void Workbench::draw_menu(Grid& g) {
  const int W = g.width(), H = g.height();
  int tw = 0, kw = 0;
  for (const auto& e : menu_.items) {
    tw = std::max(tw, utf8::width(e.title));
    kw = std::max(kw, utf8::width(e.key));
  }
  const int w = std::min(W, tw + (kw ? kw + 3 : 0) + 4);
  const int h = std::min(H, int(menu_.items.size()) + 2);
  // Below and right of the pointer; above or left of it when there is no room.
  int x0 = menu_.x + 1, y0 = menu_.y + 1;
  if (x0 + w > W) x0 = std::max(0, menu_.x - w);
  if (y0 + h > H) y0 = std::max(0, H - h);
  menu_.box = {x0, y0, w, h};
  g.fill(menu_.box, Style::normal);
  draw_box(g, menu_.box, "", Style::border_focus, Style::border_focus);
  g.set_layer(menu_.box, Layer::dialog);
  g.shadow(menu_.box);
  const int xe = x0 + w - 1;
  for (int i = 0; i < h - 2; ++i) {
    const auto& e = menu_.items[size_t(i)];
    const int y = y0 + 1 + i;
    if (e.id.empty()) {
      g.put(x0, y, U'├', Style::border_focus, xe + 1);
      for (int x = x0 + 1; x < xe; ++x) g.put(x, y, U'─', Style::border_focus, xe);
      g.put(xe, y, U'┤', Style::border_focus, xe + 1);
      continue;
    }
    const bool cur = i == menu_.cursor;
    const Style st = cur ? Style::selected : Style::normal;
    if (cur) g.fill({x0 + 1, y, w - 2, 1}, st);
    g.text(x0 + 2, y, e.title, st, xe - 1);
    if (!e.key.empty()) g.text(xe - 1 - utf8::width(e.key), y, e.key, cur ? st : Style::dim, xe);
  }
}

void Workbench::redraw_all() {
  front_ = Grid();
  dirty_ = true;
}

// ---- handoffs -----------------------------------------------------------------------

bool Workbench::open_editor(const std::string& path, int line) {
  dirty_ = true;
  if (links_.go_to(path, line)) return false;
  const std::string mode = editor_mode();
  const std::string name = std::filesystem::path(path).filename().string();
  if ((mode == "tmux-window" || mode == "tmux-split") && std::getenv("TMUX")) {
    // An editor beside us in tmux is linked too: its pane id brings it forward later.
    EditorLink* link = links_.start_link(editor_kind(EditorLinks::editor()));
    std::vector<std::string> t{"tmux", mode == "tmux-split" ? "split-window" : "new-window", "-P", "-F", "#{pane_id}"};
    if (mode == "tmux-split") t.emplace_back("-h");
    else t.insert(t.end(), {"-n", name});
    const auto cmd = links_.command(path, line, link);
    t.insert(t.end(), cmd.begin(), cmd.end());
    int rc = 0;
    std::string pane = run_output(t, &rc);
    while (!pane.empty() && (pane.back() == '\n' || pane.back() == '\r')) pane.pop_back();
    if (rc != 0) {
      if (link) links_.drop(link);
      set_status("tmux could not open " + name, Style::bar_error);
      return false;
    }
    if (link) link->pane = pane;
    set_status("Opened " + name + " in tmux", Style::bar);
    return false;
  }
  const auto cmd = links_.command(path, line, nullptr);
  term_.leave();
  const int rc = run_foreground(cmd);
  term_.enter();
  keys_.reset();
  redraw_all();
  if (rc != 0) set_status("Editor exited with status " + std::to_string(rc), Style::bar_error);
  return true;
}

void Workbench::suspend() {
  term_.leave();
  raise(SIGTSTP);  // stops here until the shell continues us
  term_.enter();
  keys_.reset();
  redraw_all();
}

}  // namespace rcp
