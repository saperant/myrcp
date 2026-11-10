#include "workbench.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#include "gobj.hpp"
#include "process.hpp"
#include "rcp/layout.hpp"
#ifdef MYRCP_HAVE_VTE
#include <vte/vte.h>
#endif

namespace rcp::gtk {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

size_t ri(Region r) { return size_t(r); }
bool is_rows(std::string_view t) { return t == "list" || t == "table" || t == "tree"; }

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

// The workbench's own commands, with the keys they have here. Region resizing is the
// panes' handles; the palette's "Theme" entries are the terminal's palettes (GTK
// follows the desktop's theme).
std::vector<CommandInfo> builtin_commands() {
  return {
      {"workbench.palette", "Show all commands", {"ctrl+p"}, "", ""},
      {"workbench.goto", "Go to Anything: files, views, problems", {"ctrl+o"}, "", ""},
      {"workbench.quit", "Quit", {"ctrl+q"}, "", ""},
      {"workbench.nextView", "View: Next in the region", {"ctrl+pagedown"}, "", ""},
      {"workbench.prevView", "View: Previous in the region", {"ctrl+pageup"}, "", ""},
      {"workbench.back", "Go: Back to where you were", {"alt+left"}, "", ""},
      {"workbench.forward", "Go: Forward again", {"alt+right"}, "", ""},
      {"workbench.closeView", "View: Close", {"ctrl+w"}, "", ""},
      {"workbench.nextRegion", "View: Focus the next region", {"f6"}, "", ""},
      {"workbench.prevRegion", "View: Focus the previous region", {"shift+f6"}, "", ""},
      {"workbench.maximize", "View: Maximize its region (again: restore)", {"alt+z"}, "", ""},
      {"workbench.move:main", "View: Move to the main area", {}, "", ""},
      {"workbench.move:side", "View: Move to the side", {}, "", ""},
      {"workbench.move:bottom", "View: Move to the bottom", {}, "", ""},
      {"workbench.perspective.reset", "Perspective: Reset to its definition", {}, "", ""},
      {"workbench.restartHost", "Plugins: Restart hosts", {"ctrl+r"}, "", ""},
      {"workbench.showLog", "Plugins: Open host log in editor", {}, "", ""},
      {"workbench.cancelJobs", "Jobs: Cancel all", {}, "", ""},
      {"workbench.problems", "Problems: Show", {"f8"}, "", ""},
      {"workbench.properties", "Properties: Show the selection", {"alt+enter"}, "", ""},
      {"workbench.contextMenu", "Show the context menu (or a right click)", {"shift+f10", "menu"}, "", ""},
      {"workbench.table.sort", "Table: Sort by the next column", {"alt+s"}, "", ""},
      {"workbench.table.reverse", "Table: Reverse the sort order", {"alt+r"}, "", ""},
      {"workbench.preferences", "Preferences: Show (editor, plugins' settings)", {}, "", ""},
      {"workbench.editPrefs", "Preferences: Edit the file", {}, "", ""},
#ifdef MYRCP_HAVE_VTE
      {"workbench.terminal", "Terminal: New shell", {"ctrl+t"}, "", ""},
#endif
  };
}

// A key as myrcp names it ("ctrl+p", "alt+1", "shift+f6", "e", ".", "pagedown").
std::string key_name(guint keyval, GdkModifierType state) {
  const bool ctrl = state & GDK_CONTROL_MASK, alt = state & GDK_ALT_MASK;
  bool shift = state & GDK_SHIFT_MASK;
  std::string base;
  switch (keyval) {
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: case GDK_KEY_ISO_Enter: base = "enter"; break;
    case GDK_KEY_Escape: base = "escape"; break;
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: case GDK_KEY_KP_Tab: base = "tab"; break;
    case GDK_KEY_BackSpace: base = "backspace"; break;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: base = "delete"; break;
    case GDK_KEY_Insert: case GDK_KEY_KP_Insert: base = "insert"; break;
    case GDK_KEY_Home: case GDK_KEY_KP_Home: base = "home"; break;
    case GDK_KEY_End: case GDK_KEY_KP_End: base = "end"; break;
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: base = "pageup"; break;
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: base = "pagedown"; break;
    case GDK_KEY_Up: case GDK_KEY_KP_Up: base = "up"; break;
    case GDK_KEY_Down: case GDK_KEY_KP_Down: base = "down"; break;
    case GDK_KEY_Left: case GDK_KEY_KP_Left: base = "left"; break;
    case GDK_KEY_Right: case GDK_KEY_KP_Right: base = "right"; break;
    case GDK_KEY_space: base = "space"; break;
    case GDK_KEY_Menu: base = "menu"; break;
    default:
      if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12) {
        base = "f" + std::to_string(keyval - GDK_KEY_F1 + 1);
        break;
      }
      // A printable key: the character itself, shifted already unless Ctrl/Alt are down.
      const gunichar u = gdk_keyval_to_unicode((ctrl || alt) ? gdk_keyval_to_lower(keyval) : keyval);
      if (!u || g_unichar_iscntrl(u)) return "";
      char buf[8];
      base.assign(buf, size_t(g_unichar_to_utf8(u, buf)));
      if (!ctrl && !alt) shift = false;
  }
  std::string name;
  if (ctrl) name += "ctrl+";
  if (alt) name += "alt+";
  if (shift) name += "shift+";
  return name + base;
}

// The same as a GTK accelerator, for menus to show.
std::string accel_of(std::string_view key) {
  if (key.find(' ') != std::string_view::npos) return "";  // a chord
  std::string out;
  while (true) {
    if (key.starts_with("ctrl+") && key.size() > 5) out += "<Control>", key.remove_prefix(5);
    else if (key.starts_with("alt+") && key.size() > 4) out += "<Alt>", key.remove_prefix(4);
    else if (key.starts_with("shift+") && key.size() > 6) out += "<Shift>", key.remove_prefix(6);
    else break;
  }
  static const std::map<std::string, std::string, std::less<>> names = {
      {"enter", "Return"}, {"escape", "Escape"}, {"tab", "Tab"}, {"backspace", "BackSpace"},
      {"delete", "Delete"}, {"insert", "Insert"}, {"home", "Home"}, {"end", "End"}, {"pageup", "Page_Up"},
      {"pagedown", "Page_Down"}, {"up", "Up"}, {"down", "Down"}, {"left", "Left"}, {"right", "Right"},
      {"space", "space"}, {"menu", "Menu"}};
  if (const auto it = names.find(key); it != names.end()) return out + it->second;
  if (key.size() >= 2 && key[0] == 'f' && std::isdigit(static_cast<unsigned char>(key[1]))) return out + "F" + std::string(key.substr(1));
  const gunichar u = g_utf8_get_char_validated(std::string(key).c_str(), gssize(key.size()));
  if (u == gunichar(-1) || u == gunichar(-2)) return "";
  const char* n = gdk_keyval_name(gdk_unicode_to_keyval(u));
  return n ? out + n : "";
}

// Where a GTK widget is a terminal view's, or text being typed: keys belong to it.
bool in_terminal(GtkWidget* w) {
#ifdef MYRCP_HAVE_VTE
  for (; w; w = gtk_widget_get_parent(w)) {
    if (VTE_IS_TERMINAL(w)) return true;
  }
#else
  (void)w;
#endif
  return false;
}
bool in_text(GtkWidget* w) {
  return w && (GTK_IS_EDITABLE(w) || (GTK_IS_TEXT_VIEW(w) && gtk_text_view_get_editable(GTK_TEXT_VIEW(w))));
}

}  // namespace

// ---- the palette and Go to Anything: a search over entries, in a window of its own ----

class QuickPick {
 public:
  struct Item {
    std::string id, title, detail;
  };
  QuickPick(GtkWindow* parent, const std::string& placeholder, std::vector<Item> items,
            std::function<void(const std::string&)> done)
      : items_(std::move(items)), done_(std::move(done)) {
    window_ = gtk_window_new();
    gtk_widget_add_css_class(window_, "rcp-pick");
    gtk_window_set_transient_for(GTK_WINDOW(window_), parent);
    gtk_window_set_modal(GTK_WINDOW(window_), true);
    gtk_window_set_decorated(GTK_WINDOW(window_), false);
    gtk_window_set_default_size(GTK_WINDOW(window_), 640, 440);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    for (auto* f : {gtk_widget_set_margin_start, gtk_widget_set_margin_end, gtk_widget_set_margin_top, gtk_widget_set_margin_bottom})
      f(box, 8);
    entry_ = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(entry_), placeholder.c_str());
    gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(entry_), 30);
    list_ = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list_), GTK_SELECTION_BROWSE);
    GtkWidget* sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), list_);
    gtk_widget_set_vexpand(sc, true);
    gtk_box_append(GTK_BOX(box), entry_);
    gtk_box_append(GTK_BOX(box), sc);
    gtk_window_set_child(GTK_WINDOW(window_), box);

    on<void, GtkSearchEntry*>(entry_, "search-changed", [this](GtkSearchEntry*) { refilter(); });
    on<void, GtkSearchEntry*>(entry_, "activate", [this](GtkSearchEntry*) {
      if (gtk_editable_get_text(GTK_EDITABLE(entry_)) != filtered_) refilter();  // typed faster than the search delay
      choose(gtk_list_box_get_selected_row(GTK_LIST_BOX(list_)));
    });
    on<void, GtkSearchEntry*>(entry_, "stop-search", [this](GtkSearchEntry*) { finish(""); });
    on<void, GtkListBox*, GtkListBoxRow*>(list_, "row-activated", [this](GtkListBox*, GtkListBoxRow* r) { choose(r); });
    on<gboolean, GtkWindow*>(window_, "close-request", [this](GtkWindow*) -> gboolean {
      finish("");
      return true;
    });
    on<void, GtkWindow*, GParamSpec*>(window_, "notify::is-active", [this](GtkWindow* w, GParamSpec*) {
      if (shown_ && !gtk_window_is_active(w)) finish("");  // clicked elsewhere
    });
    GtkEventController* kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(window_, kc);
    on<gboolean, GtkEventControllerKey*, guint, guint, GdkModifierType>(
        kc, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType) -> gboolean {
          int step = 0;
          if (key == GDK_KEY_Down || key == GDK_KEY_KP_Down) step = 1;
          else if (key == GDK_KEY_Up || key == GDK_KEY_KP_Up) step = -1;
          else if (key == GDK_KEY_Page_Down) step = 12;
          else if (key == GDK_KEY_Page_Up) step = -12;
          else if (key == GDK_KEY_Escape) return finish(""), true;
          if (!step) return false;
          move(step);
          return true;
        });
    refilter();
    gtk_window_present(GTK_WINDOW(window_));
    gtk_widget_grab_focus(entry_);
    shown_ = true;
  }
  ~QuickPick() { gtk_window_destroy(GTK_WINDOW(window_)); }

 private:
  void refilter() {
    const std::string text = gtk_editable_get_text(GTK_EDITABLE(entry_));
    filtered_ = text;
    struct Scored {
      int score;
      size_t index;
    };
    std::vector<Scored> scored;
    for (size_t i = 0; i < items_.size(); ++i) {
      const int s = fuzzy_score(items_[i].title, text);
      if (s >= 0) scored.push_back({s, i});
    }
    std::stable_sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) { return a.score < b.score; });
    gtk_list_box_remove_all(GTK_LIST_BOX(list_));
    shown_ids_.clear();
    for (size_t k = 0; k < scored.size() && k < 300; ++k) {
      const Item& it = items_[scored[k].index];
      GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
      gtk_widget_add_css_class(row, "rcp-pick-row");
      GtkWidget* t = gtk_label_new(it.title.c_str());
      gtk_label_set_xalign(GTK_LABEL(t), 0);
      gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
      gtk_widget_set_hexpand(t, true);
      gtk_box_append(GTK_BOX(row), t);
      if (!it.detail.empty()) {
        GtkWidget* d = gtk_label_new(it.detail.c_str());
        gtk_widget_add_css_class(d, "dim-label");
        gtk_box_append(GTK_BOX(row), d);
      }
      gtk_list_box_append(GTK_LIST_BOX(list_), row);
      shown_ids_.push_back(it.id);
    }
    if (GtkListBoxRow* first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), 0))
      gtk_list_box_select_row(GTK_LIST_BOX(list_), first);
  }
  void move(int step) {
    const int n = int(shown_ids_.size());
    if (n == 0) return;
    GtkListBoxRow* cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(list_));
    const int at = std::clamp((cur ? gtk_list_box_row_get_index(cur) : 0) + step, 0, n - 1);
    GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list_), at);
    gtk_list_box_select_row(GTK_LIST_BOX(list_), row);
    // Into view, the focus staying in the search entry.
    graphene_rect_t b;
    if (gtk_widget_compute_bounds(GTK_WIDGET(row), list_, &b)) {
      GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(gtk_widget_get_ancestor(list_, GTK_TYPE_SCROLLED_WINDOW)));
      const double v = gtk_adjustment_get_value(adj), page = gtk_adjustment_get_page_size(adj);
      if (b.origin.y < v) gtk_adjustment_set_value(adj, b.origin.y);
      else if (b.origin.y + b.size.height > v + page) gtk_adjustment_set_value(adj, b.origin.y + b.size.height - page);
    }
  }
  void choose(GtkListBoxRow* r) {
    if (!r) return;
    const int i = gtk_list_box_row_get_index(r);
    if (i >= 0 && size_t(i) < shown_ids_.size()) finish(shown_ids_[size_t(i)]);
  }
  void finish(const std::string& id) {
    if (finished_) return;
    finished_ = true;
    gtk_widget_set_visible(window_, false);
    done_(id);  // the owner deletes us later, not from inside our own signal
  }

  std::vector<Item> items_;
  std::vector<std::string> shown_ids_;
  std::string filtered_;  // the text the list shows matches for
  std::function<void(const std::string&)> done_;
  GtkWidget* window_;
  GtkWidget* entry_;
  GtkWidget* list_;
  bool shown_ = false, finished_ = false;
};

// ---- the poll source ----------------------------------------------------------------

namespace {
struct PollSource {
  GSource source;
  Workbench* wb;
};
GSourceFuncs g_poll_funcs = {
    [](GSource* s, gint* timeout) -> gboolean {
      reinterpret_cast<PollSource*>(s)->wb->poll_prepare(s, timeout);
      return false;
    },
    [](GSource* s) -> gboolean { return reinterpret_cast<PollSource*>(s)->wb->poll_check(); },
    [](GSource* s, GSourceFunc, gpointer) -> gboolean {
      reinterpret_cast<PollSource*>(s)->wb->poll_dispatch();
      return G_SOURCE_CONTINUE;
    },
    nullptr, nullptr, nullptr};
}  // namespace

void Workbench::poll_prepare(GSource* src, int* timeout) {
  for (auto& f : gfds_) g_source_remove_poll(src, &f);
  std::vector<pollfd> fds;
  tokens_.clear();
  hub_.add_fds(fds, tokens_);
  links_at_ = fds.size();
  links_.add_fds(fds);
  gfds_.clear();
  gfds_.reserve(fds.size());
  for (const auto& p : fds) gfds_.push_back({p.fd, gushort(G_IO_IN | G_IO_HUP | G_IO_ERR), 0});
  for (auto& f : gfds_) g_source_add_poll(src, &f);
  *timeout = links_.timeout_ms();
}

bool Workbench::poll_check() const {
  return std::any_of(gfds_.begin(), gfds_.end(), [](const GPollFD& f) { return f.revents != 0; });
}

void Workbench::poll_dispatch() {
  const std::vector<GPollFD> got = gfds_;  // hub calls may change what is polled
  const std::vector<Hub::PollToken> tokens = tokens_;
  for (size_t j = 0; j < tokens.size() && j < got.size(); ++j) {
    if (got[j].revents) hub_.on_ready(tokens[j]);
  }
  std::vector<pollfd> lf;
  for (size_t j = links_at_; j < got.size(); ++j) lf.push_back({got[j].fd, short(G_IO_IN), short(got[j].revents)});
  if (links_.on_poll(lf.data(), lf.size())) queue_sync();
}

// ---- setup ----------------------------------------------------------------------------

Workbench::Workbench(HostOptions o)
    : hub_(std::move(o), *this),
      links_(hub_, {[this] { return focus_id_; }, [this](const std::string& view) { activate(view); },
                    [this](const std::string& text, bool error) { set_status(text, error ? "error" : "normal"); }}),
      builtins_(builtin_commands()) {
  declare_workbench_prefs(hub_, false);
  hub_.reload_prefs();
}

Workbench::~Workbench() {
  for (guint* src : {&sync_source_, &focus_source_, &sizes_timer_, &status_timer_}) {
    if (*src) g_source_remove(std::exchange(*src, 0));
  }
  if (poll_source_) {
    g_source_destroy(poll_source_);
    g_source_unref(poll_source_);
  }
  pick_.reset();
  for (auto& [id, p] : pages_) {
    p.view.reset();
    g_object_unref(p.page);
  }
  if (app_) g_object_unref(app_);
}

int Workbench::run(int argc, char** argv) {
  (void)argc;
  app_ = gtk_application_new("io.github.myrcp.Workbench", G_APPLICATION_NON_UNIQUE);
  on<void, GApplication*>(app_, "activate", [this](GApplication*) { activate_app(); });
  char* args[] = {argv[0], nullptr};
  const int rc = g_application_run(G_APPLICATION(app_), 1, args);
  hub_.save_state();
  hub_.stop();
  return rc ? rc : exit_code_;
}

void Workbench::activate_app() {
  build_window();
  install_css();
  measure();
  hub_.start();
  links_.start();
  poll_source_ = g_source_new(&g_poll_funcs, sizeof(PollSource));
  reinterpret_cast<PollSource*>(poll_source_)->wb = this;
  g_source_attach(poll_source_, nullptr);
  gtk_window_present(GTK_WINDOW(window_));
  queue_sync();
}

void Workbench::install_css() {
  GtkCssProvider* css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css, R"css(
    .rcp-statusbar { padding: 3px 10px; border-top: 1px solid alpha(currentColor, 0.12); }
    .rcp-statusbar label.error { color: @error_color; }
    .rcp-statusbar label.success { color: @success_color; }
    .rcp-statusbar progressbar { min-width: 120px; }
    button.rcp-arrow { min-width: 18px; min-height: 18px; padding: 0; margin: 0; }
    label.rcp-clickable { text-decoration-line: underline; }
    notebook.rcp-focused > header { box-shadow: inset 0 -2px @theme_selected_bg_color; }
    .rcp-tab button { min-width: 16px; min-height: 16px; padding: 0; margin-left: 2px; }
    window.rcp-pick { border-radius: 10px; border: 1px solid alpha(currentColor, 0.2); }
    .rcp-pick-row { padding: 4px 8px; }
    frame.rcp-panel > label { font-weight: bold; }
  )css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
}

// The UI font's character and line sizes (the IR's hints are in those), and whether the
// theme is dark (text styles are coloured to read on it).
void Workbench::measure() {
  PangoLayout* l = gtk_widget_create_pango_layout(window_, "0000000000");
  int w = 0, h = 0;
  pango_layout_get_pixel_size(l, &w, &h);
  g_object_unref(l);
  set_metrics(w / 10, h);
  GdkRGBA fg;
  gtk_widget_get_color(window_, &fg);
  set_dark(0.299 * fg.red + 0.587 * fg.green + 0.114 * fg.blue > 0.5);
}

GtkWidget* Workbench::make_notebook(Region r) {
  GtkWidget* nb = gtk_notebook_new();
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(nb), true);
  gtk_notebook_set_group_name(GTK_NOTEBOOK(nb), "myrcp");  // tabs drag between the regions
  gtk_widget_set_hexpand(nb, true);
  gtk_widget_set_vexpand(nb, true);
  on<void, GtkNotebook*, GtkWidget*, guint>(nb, "switch-page", [this](GtkNotebook*, GtkWidget* page, guint) {
    if (syncing_) return;
    if (Page* p = page_of(page)) {
      // A tab clicked: that view has the focus, once it shows.
      const std::string id = p->id;
      g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
        auto* args = static_cast<std::pair<Workbench*, std::string>*>(d);
        args->first->activate(args->second);
        return G_SOURCE_REMOVE;
      }, new std::pair<Workbench*, std::string>(this, id), [](gpointer d) { delete static_cast<std::pair<Workbench*, std::string>*>(d); });
    }
  });
  on<void, GtkNotebook*, GtkWidget*, guint>(nb, "page-added", [this, r](GtkNotebook*, GtkWidget* page, guint) {
    if (syncing_) return;
    if (Page* p = page_of(page); p && p->region != r) {  // dragged here from another region
      p->region = r;
      hub_.set_region(p->id, r);
    }
    queue_sync();
  });
  on<void, GtkNotebook*, GtkWidget*, guint>(nb, "page-removed", [this](GtkNotebook*, GtkWidget*, guint) { queue_sync(); });
  return nb;
}

void Workbench::build_window() {
  window_ = gtk_application_window_new(app_);
  gtk_window_set_default_size(GTK_WINDOW(window_), 1280, 820);
  const std::string cwd = std::filesystem::current_path().filename().string();
  gtk_window_set_title(GTK_WINDOW(window_), ((cwd.empty() ? "" : cwd + " — ") + hub_.options().app_name).c_str());

  // Commands as one action, so menus can name any of them.
  GSimpleActionGroup* actions = g_simple_action_group_new();
  GSimpleAction* run = g_simple_action_new("run", G_VARIANT_TYPE_STRING);
  on<void, GSimpleAction*, GVariant*>(run, "activate", [this](GSimpleAction*, GVariant* v) {
    run_command(g_variant_get_string(v, nullptr));
  });
  g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(run));
  g_object_unref(run);
  gtk_widget_insert_action_group(window_, "rcp", G_ACTION_GROUP(actions));
  g_object_set_data_full(G_OBJECT(window_), "rcp-actions", actions, g_object_unref);

  GtkWidget* header = gtk_header_bar_new();
  GtkWidget* nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(nav, "linked");
  back_button_ = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_set_tooltip_text(back_button_, "Back to where you were (Alt+←)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(back_button_), "rcp.run");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(back_button_), g_variant_new_string("workbench.back"));
  forward_button_ = gtk_button_new_from_icon_name("go-next-symbolic");
  gtk_widget_set_tooltip_text(forward_button_, "Forward again (Alt+→)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(forward_button_), "rcp.run");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(forward_button_), g_variant_new_string("workbench.forward"));
  gtk_box_append(GTK_BOX(nav), back_button_);
  gtk_box_append(GTK_BOX(nav), forward_button_);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), nav);
  perspective_button_ = gtk_menu_button_new();
  gtk_widget_set_tooltip_text(perspective_button_, "Perspective: the arrangement of views for a task");
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), perspective_button_);

  main_menu_button_ = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(main_menu_button_), "open-menu-symbolic");
  gtk_menu_button_set_primary(GTK_MENU_BUTTON(main_menu_button_), true);
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), main_menu_button_);
  GtkWidget* palette = gtk_button_new_from_icon_name("system-search-symbolic");
  gtk_widget_set_tooltip_text(palette, "All commands (Ctrl+P)");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(palette), "rcp.run");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(palette), g_variant_new_string("workbench.palette"));
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), palette);
  gtk_window_set_titlebar(GTK_WINDOW(window_), header);

  for (const Region r : kRegions) notebooks_[ri(r)] = make_notebook(r);
  hpaned_ = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_set_start_child(GTK_PANED(hpaned_), notebooks_[ri(Region::main)]);
  gtk_paned_set_end_child(GTK_PANED(hpaned_), notebooks_[ri(Region::side)]);
  gtk_paned_set_shrink_start_child(GTK_PANED(hpaned_), false);
  gtk_paned_set_shrink_end_child(GTK_PANED(hpaned_), false);
  vpaned_ = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
  gtk_paned_set_start_child(GTK_PANED(vpaned_), hpaned_);
  gtk_paned_set_end_child(GTK_PANED(vpaned_), notebooks_[ri(Region::bottom)]);
  gtk_paned_set_shrink_start_child(GTK_PANED(vpaned_), false);
  gtk_paned_set_shrink_end_child(GTK_PANED(vpaned_), false);
  gtk_widget_set_vexpand(vpaned_, true);
  for (GtkWidget* p : {hpaned_, vpaned_}) {
    on<void, GtkPaned*, GParamSpec*>(p, "notify::max-position", [this](GtkPaned*, GParamSpec*) { apply_sizes(); });
    on<void, GtkPaned*, GParamSpec*>(p, "notify::position", [this](GtkPaned*, GParamSpec*) {
      if (applying_sizes_) return;
      if (sizes_timer_) g_source_remove(sizes_timer_);
      sizes_timer_ = g_timeout_add(400, [](gpointer d) -> gboolean {
        static_cast<Workbench*>(d)->sizes_dragged();
        return G_SOURCE_REMOVE;
      }, this);
    });
  }

  GtkWidget* status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_add_css_class(status, "rcp-statusbar");
  status_label_ = gtk_label_new(nullptr);
  gtk_label_set_xalign(GTK_LABEL(status_label_), 0);
  gtk_label_set_ellipsize(GTK_LABEL(status_label_), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(status_label_, true);
  gtk_box_append(GTK_BOX(status), status_label_);
  jobs_box_ = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  jobs_label_ = gtk_label_new(nullptr);
  gtk_label_set_ellipsize(GTK_LABEL(jobs_label_), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(jobs_label_), 50);
  jobs_bar_ = gtk_progress_bar_new();
  gtk_widget_set_valign(jobs_bar_, GTK_ALIGN_CENTER);
  GtkWidget* cancel = gtk_button_new_from_icon_name("process-stop-symbolic");
  gtk_widget_add_css_class(cancel, "flat");
  gtk_widget_set_tooltip_text(cancel, "Cancel all jobs");
  gtk_actionable_set_action_name(GTK_ACTIONABLE(cancel), "rcp.run");
  gtk_actionable_set_action_target_value(GTK_ACTIONABLE(cancel), g_variant_new_string("workbench.cancelJobs"));
  gtk_box_append(GTK_BOX(jobs_box_), jobs_label_);
  gtk_box_append(GTK_BOX(jobs_box_), jobs_bar_);
  gtk_box_append(GTK_BOX(jobs_box_), cancel);
  gtk_widget_set_visible(jobs_box_, false);
  gtk_box_append(GTK_BOX(status), jobs_box_);

  GtkWidget* body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(body), vpaned_);
  gtk_box_append(GTK_BOX(body), status);
  gtk_window_set_child(GTK_WINDOW(window_), body);

  GtkEventController* kc = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
  gtk_widget_add_controller(window_, kc);
  on<gboolean, GtkEventControllerKey*, guint, guint, GdkModifierType>(
      kc, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType state) -> gboolean {
        return key_pressed(key, state);
      });
  on<void, GtkWindow*, GParamSpec*>(window_, "notify::focus-widget", [this](GtkWindow*, GParamSpec*) { focus_changed(); });
  on<gboolean, GtkWindow*>(window_, "close-request", [this](GtkWindow*) -> gboolean {
    closing();
    return false;
  });
  // From here on the widgets are going: nothing more to draw or focus.
  on<void, GtkWidget*>(window_, "destroy", [this](GtkWidget*) {
    closing_ = true;
    for (guint* src : {&sync_source_, &focus_source_, &sizes_timer_, &status_timer_}) {
      if (*src) g_source_remove(std::exchange(*src, 0));
    }
    status_label_ = nullptr;
  });
  update_regions();
}

// The window is about to go: what its regions show is kept for saving (the hub saves
// again when it stops, after the widgets are gone).
void Workbench::closing() {
  if (closing_) return;
  front_state(final_);
  closing_ = true;
  hub_.save_state();
}

// ---- views and regions -------------------------------------------------------------

void Workbench::queue_sync() {
  if (sync_source_ || closing_) return;
  sync_source_ = g_idle_add_full(G_PRIORITY_HIGH_IDLE + 20, [](gpointer d) -> gboolean {
    static_cast<Workbench*>(d)->sync();
    return G_SOURCE_REMOVE;
  }, this, nullptr);
}

Workbench::Page* Workbench::page_of(GtkWidget* w) {
  for (; w; w = gtk_widget_get_parent(w)) {
    if (const char* id = static_cast<const char*>(g_object_get_data(G_OBJECT(w), "rcp-view"))) {
      const auto it = pages_.find(id);
      return it == pages_.end() ? nullptr : &it->second;
    }
  }
  return nullptr;
}

Workbench::Page& Workbench::add_page(const ViewData& v) {
  Page p;
  p.id = v.id;
  p.region = v.region;
#ifdef MYRCP_HAVE_VTE
  if (const auto t = terms_.find(v.id); t != terms_.end()) {
    p.page = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p.page), t->second.widget);
  }
#endif
  if (!p.page && v.native) {
    p.page = gtk_label_new("This view belongs to another front-end.");
  } else if (!p.page) {
    p.view = std::make_unique<ViewWidget>(*this, v.id);
    p.page = p.view->widget();
  }
  g_object_ref_sink(p.page);
  g_object_set_data_full(G_OBJECT(p.page), "rcp-view", g_strdup(v.id.c_str()), g_free);

  // The tab: its title and a close button; a right click has the tab's menu.
  GtkWidget* tab = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  gtk_widget_add_css_class(tab, "rcp-tab");
  p.label = gtk_label_new(v.title.c_str());
  GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_set_focusable(close, false);
  gtk_widget_set_tooltip_text(close, "Close (Ctrl+W)");
  gtk_box_append(GTK_BOX(tab), p.label);
  gtk_box_append(GTK_BOX(tab), close);
  const std::string id = v.id;
  on<void, GtkButton*>(close, "clicked", [this, id](GtkButton*) { close_view(id); });
  GtkGesture* g = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
  gtk_widget_add_controller(tab, GTK_EVENT_CONTROLLER(g));
  on<void, GtkGestureClick*, int, double, double>(g, "pressed", [this, id, tab](GtkGestureClick* g, int, double x, double y) {
    const guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (b == GDK_BUTTON_MIDDLE) return close_view(id);
    if (b != GDK_BUTTON_SECONDARY) return;
    activate(id);
    if (const ViewData* v = hub_.find_view(id)) show_menu(tab_menu(*v), tab, x, y);
  });

  // In tab order: after the views opened before it in that region.
  GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(v.region)]);
  int pos = 0;
  for (const auto& o : hub_.views()) {
    if (o.id == v.id) break;
    if (const auto it = pages_.find(o.id); it != pages_.end() && it->second.region == v.region) ++pos;
  }
  gtk_notebook_insert_page(nb, p.page, tab, pos);
  gtk_notebook_set_tab_reorderable(nb, p.page, true);
  gtk_notebook_set_tab_detachable(nb, p.page, true);
  return pages_.emplace(v.id, std::move(p)).first->second;
}

void Workbench::remove_page(const std::string& id) {
  const auto it = pages_.find(id);
  if (it == pages_.end()) return;
  Page& p = it->second;
  if (GtkNotebook* nb = notebook_of(p.page)) gtk_notebook_remove_page(nb, gtk_notebook_page_num(nb, p.page));
  p.view.reset();
  g_object_unref(p.page);
  pages_.erase(it);
}

// The notebook a page is in (its parent is the notebook's own stack, not the notebook).
GtkNotebook* Workbench::notebook_of(GtkWidget* page) const {
  for (GtkWidget* nb : notebooks_) {
    if (gtk_notebook_page_num(GTK_NOTEBOOK(nb), page) >= 0) return GTK_NOTEBOOK(nb);
  }
  return nullptr;
}

void Workbench::move_page(Page& p, Region r) {
  GtkNotebook* from = notebook_of(p.page);
  if (from == GTK_NOTEBOOK(notebooks_[ri(r)])) {
    p.region = r;
    return;
  }
  GtkWidget* tab = from ? gtk_notebook_get_tab_label(from, p.page) : nullptr;
  if (tab) g_object_ref(tab);
  if (from) gtk_notebook_remove_page(from, gtk_notebook_page_num(from, p.page));
  GtkNotebook* to = GTK_NOTEBOOK(notebooks_[ri(r)]);
  gtk_notebook_append_page(to, p.page, tab);
  gtk_notebook_set_tab_reorderable(to, p.page, true);
  gtk_notebook_set_tab_detachable(to, p.page, true);
  if (tab) g_object_unref(tab);
  p.region = r;
}

void Workbench::sync() {
  sync_source_ = 0;
  syncing_ = true;
  std::set<std::string> live;
  for (const auto& v : hub_.views()) {
    live.insert(v.id);
    const auto it = pages_.find(v.id);
    Page& p = it == pages_.end() ? add_page(v) : it->second;
    if (p.region != v.region) move_page(p, v.region);
    if (gtk_label_get_text(GTK_LABEL(p.label)) != v.title) gtk_label_set_text(GTK_LABEL(p.label), v.title.c_str());
    if (p.view && (!(v.tree == p.seen) || v.loading != p.seen_loading || p.seen.is_nil())) {
      p.seen = v.tree;
      p.seen_loading = v.loading;
      p.view->update(v.tree, v.loading);
    }
  }
  for (auto it = pages_.begin(); it != pages_.end();) {
    const std::string id = (it++)->first;
    if (!live.contains(id)) remove_page(id);
  }
  for (const Region r : kRegions) {  // what an applied arrangement shows
    std::string& want = want_shown_[ri(r)];
    if (want.empty()) continue;
    if (const auto it = pages_.find(want); it != pages_.end()) {
      GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(it->second.region)]);
      gtk_notebook_set_current_page(nb, gtk_notebook_page_num(nb, it->second.page));
      want.clear();
    }
  }
  syncing_ = false;
  if (!want_focus_.empty() && pages_.contains(want_focus_)) {
    const std::string id = std::exchange(want_focus_, "");
    activate(id);
  }
  if (focus_id_.empty() || !pages_.contains(focus_id_)) {  // a view closed: its region's next one
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(focus_region_)]);
    if (gtk_notebook_get_n_pages(nb) == 0) nb = GTK_NOTEBOOK(notebooks_[ri(Region::main)]);
    if (GtkWidget* cur = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb))) {
      if (Page* p = page_of(cur)) {
        const std::string id = p->id;
        activate(id);
      }
    }
  }
  update_regions();
  update_menus();
  update_jobs();
  gtk_widget_set_sensitive(back_button_, history_.can_back());
  gtk_widget_set_sensitive(forward_button_, history_.can_forward());
  update_selection();
}

const ViewData* Workbench::active_view() const {
  return focus_id_.empty() ? nullptr : const_cast<Hub&>(hub_).find_view(focus_id_);
}

void Workbench::set_focus_view(const std::string& id) {
  if (id == focus_id_) return;
  if (!going_ && !focus_id_.empty()) history_.left(place_here());
  sel_view_.clear();  // a view focused again offers its row again
  focus_id_ = id;
  if (const auto it = pages_.find(id); it != pages_.end()) focus_region_ = it->second.region;
  update_regions();
  gtk_widget_set_sensitive(back_button_, history_.can_back());
  gtk_widget_set_sensitive(forward_button_, history_.can_forward());
  update_selection();
}

void Workbench::activate(const std::string& id) {
  if (closing_) return;
  const auto it = pages_.find(id);
  if (it == pages_.end()) {  // not drawn yet: once it is
    want_focus_ = id;
    return queue_sync();
  }
  Page& p = it->second;
  if (maximized_ && p.region != focus_region_) maximized_ = false;
  set_focus_view(id);
  GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(p.region)]);
  const bool was = syncing_;
  syncing_ = true;
  gtk_notebook_set_current_page(nb, gtk_notebook_page_num(nb, p.page));
  syncing_ = was;
  update_regions();
  focus_later();
}

// Keyboard focus into the focused view once its page is laid out (a list's rows exist
// only then); until then GTK would give it to the page's first focusable widget.
void Workbench::focus_later() {
  if (focus_source_ || closing_) return;
  focus_source_ = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
    auto* wb = static_cast<Workbench*>(d);
    wb->focus_source_ = 0;
    const auto it = wb->pages_.find(wb->focus_id_);
    if (it == wb->pages_.end()) return G_SOURCE_REMOVE;
    if (it->second.view) {
      it->second.view->grab_focus();
    } else {
#ifdef MYRCP_HAVE_VTE
      if (const auto t = wb->terms_.find(wb->focus_id_); t != wb->terms_.end()) gtk_widget_grab_focus(t->second.widget);
#endif
    }
    return G_SOURCE_REMOVE;
  }, this, nullptr);
}

void Workbench::focus_changed() {
  GtkWidget* f = gtk_root_get_focus(GTK_ROOT(window_));
  if (Page* p = page_of(f); p && p->id != focus_id_) set_focus_view(p->id);
}

void Workbench::close_view(const std::string& id) {
  if (terms_.contains(id)) return term_ended(id);  // hangs up on the program
  hub_.close_view(id);
  queue_sync();
}

void Workbench::cycle_tab(int step) {
  GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(focus_region_)]);
  const int n = gtk_notebook_get_n_pages(nb);
  if (n < 2) return;
  const int at = (gtk_notebook_get_current_page(nb) + step + n) % n;
  if (Page* p = page_of(gtk_notebook_get_nth_page(nb, at))) activate(std::string(p->id));
}

void Workbench::cycle_region(int step) {
  std::vector<Region> shown;
  for (const Region r : kRegions) {
    if (gtk_widget_get_visible(notebooks_[ri(r)]) && gtk_notebook_get_n_pages(GTK_NOTEBOOK(notebooks_[ri(r)])) > 0)
      shown.push_back(r);
  }
  if (shown.size() < 2) return;
  const size_t at = size_t(std::find(shown.begin(), shown.end(), focus_region_) - shown.begin()) % shown.size();
  GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(shown[(at + shown.size() + size_t(step)) % shown.size()])]);
  if (Page* p = page_of(gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb)))) activate(std::string(p->id));
}

// Regions without tabs are hidden; maximized, only the focused one shows.
void Workbench::update_regions() {
  int shown = 0;
  for (const Region r : kRegions) {
    GtkWidget* nb = notebooks_[ri(r)];
    const bool has = gtk_notebook_get_n_pages(GTK_NOTEBOOK(nb)) > 0;
    const bool vis = maximized_ ? r == focus_region_ : (r == Region::main || has);
    gtk_widget_set_visible(nb, vis);
    shown += vis && has;
  }
  gtk_widget_set_visible(hpaned_, gtk_widget_get_visible(notebooks_[ri(Region::main)]) ||
                                      gtk_widget_get_visible(notebooks_[ri(Region::side)]));
  for (const Region r : kRegions) {
    if (r == focus_region_ && shown > 1) gtk_widget_add_css_class(notebooks_[ri(r)], "rcp-focused");
    else gtk_widget_remove_css_class(notebooks_[ri(r)], "rcp-focused");
  }
}

void Workbench::toggle_maximize() {
  maximized_ = !maximized_;
  update_regions();
}

void Workbench::apply_sizes() {
  const Sizes s = hub_.sizes();
  applying_sizes_ = true;
  if (const int w = gtk_widget_get_width(hpaned_); w > 0) gtk_paned_set_position(GTK_PANED(hpaned_), w - w * s.side / 100);
  if (const int h = gtk_widget_get_height(vpaned_); h > 0) gtk_paned_set_position(GTK_PANED(vpaned_), h - h * s.bottom / 100);
  applying_sizes_ = false;
}

// A pane's handle was dragged: the regions' sizes, kept with the perspective.
void Workbench::sizes_dragged() {
  sizes_timer_ = 0;
  Sizes s = hub_.sizes();
  const int w = gtk_widget_get_width(hpaned_), h = gtk_widget_get_height(vpaned_);
  if (w > 0 && gtk_widget_get_visible(notebooks_[ri(Region::side)]))
    s.side = 100 - gtk_paned_get_position(GTK_PANED(hpaned_)) * 100 / w;
  if (h > 0 && gtk_widget_get_visible(notebooks_[ri(Region::bottom)]))
    s.bottom = 100 - gtk_paned_get_position(GTK_PANED(vpaned_)) * 100 / h;
  s = clamp_sizes(s);
  if (!(s == hub_.sizes())) hub_.set_sizes(s);
}

void Workbench::update_selection() {
  const ViewData* v = active_view();
  if (v && v->native) {  // an editor's tab: where its cursor is
    links_.select_in(v->id);
    return;
  }
  const auto it = pages_.find(focus_id_);
  if (!v || v->loading || it == pages_.end() || !it->second.view) return;
  ViewWidget& vw = *it->second.view;
  const auto sel = view_selection(v->tree, vw.focus(), [&](std::string_view w) { return vw.cursor(w); });
  if (!sel || (v->id == sel_view_ && *sel == sel_items_)) return;
  sel_view_ = v->id;
  sel_items_ = *sel;
  hub_.set_selection(v->id, *sel);
}

Place Workbench::place_here() {
  Place p;
  p.view = focus_id_;
  const auto it = pages_.find(focus_id_);
  const ViewData* v = hub_.find_view(focus_id_);
  if (it == pages_.end() || !it->second.view || !v) return p;
  p.widget = it->second.view->focus();
  if (const Value* node = p.widget.empty() ? nullptr : find_widget(v->tree, p.widget); node && is_rows((*node)["t"].str())) {
    p.cursor = it->second.view->cursor(p.widget);
    if (const Value* row = row_at(*node, p.cursor)) p.key = item_key(*row, size_t(p.cursor));
  }
  return p;
}

void Workbench::go(bool back) {
  const auto exists = [&](const Place& p) { return hub_.find_view(p.view) != nullptr; };
  const auto to = back ? history_.back(place_here(), exists) : history_.forward(place_here(), exists);
  if (!to) return set_status(back ? "Nowhere to go back to" : "Nowhere to go forward to", "normal", 3);
  going_ = true;
  activate(to->view);
  going_ = false;
  const ViewData* v = hub_.find_view(to->view);
  const auto it = pages_.find(to->view);
  if (!v || it == pages_.end() || !it->second.view || to->widget.empty()) return;
  const Value* node = find_widget(v->tree, to->widget);
  if (!node || !is_rows((*node)["t"].str())) return;
  int at = to->cursor;  // the row by its key, when it is among the rows sent
  const int count = row_count(*node);
  for (int i = 0; i < count; ++i) {
    if (const Value* row = row_at(*node, i); row && item_key(*row, size_t(i)) == to->key) {
      at = i;
      break;
    }
  }
  it->second.view->set_cursor(to->widget, at);
}

void Workbench::front_state(Arrangement& a) {
  if (closing_) {
    for (const Region r : kRegions) {
      if (hub_.find_view(final_.shown[ri(r)])) a.shown[ri(r)] = final_.shown[ri(r)];
    }
    if (hub_.find_view(final_.focus)) a.focus = final_.focus;
    return;
  }
  for (const Region r : kRegions) {
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(r)]);
    if (Page* p = page_of(gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb)))) a.shown[ri(r)] = p->id;
  }
  if (hub_.find_view(focus_id_)) a.focus = focus_id_;
}

void Workbench::arranged(const Arrangement& a) {
  want_shown_ = a.shown;
  if (!a.focus.empty()) want_focus_ = a.focus;
  apply_sizes();
  queue_sync();
}

// ---- what views' widgets do ------------------------------------------------------------

void Workbench::event(const std::string& view, std::string_view widget, std::string_view kind, Value value) {
  hub_.send_event(view, widget, kind, std::move(value));
}

void Workbench::cursor_moved(const std::string& view) {
  if (view != focus_id_) set_focus_view(view);
  else update_selection();
}

void Workbench::focused(const std::string& view) { set_focus_view(view); }

void Workbench::context_menu(const std::string& view, GtkWidget* at, double x, double y) {
  set_focus_view(view);
  if (const ViewData* v = hub_.find_view(view)) show_menu(view_menu(*v), at, x, y);
}

// ---- commands ------------------------------------------------------------------------

std::vector<CommandInfo> Workbench::all_commands() const {
  std::vector<CommandInfo> all = builtins_;
  for (const auto& p : hub_.perspectives()) {
    const bool cur = p.id == hub_.perspective();
    all.push_back({"workbench.perspective:" + p.id, "Perspective: " + p.title + (cur ? " (current)" : ""), {}, "", ""});
  }
  all.insert(all.end(), hub_.commands().begin(), hub_.commands().end());
  return all;
}

std::vector<Workbench::Entry> Workbench::palette_entries() const {
  std::vector<Entry> out;
  const ViewData* v = active_view();
  const std::string base = v ? base_view(v->id) : "";
  for (const auto& c : all_commands()) {
    if (!c.view.empty() && c.view != base) continue;
    out.push_back({c.id, c.title, c.keys.empty() ? "" : c.keys.front()});
  }
  for (const auto& vi : hub_.view_infos()) out.push_back({"view.open:" + vi.id, "View: Open " + vi.title, ""});
  return out;
}

Workbench::Entry Workbench::entry(const std::string& id) const {
  for (const auto& c : all_commands()) {
    if (c.id == id) return {c.id, c.title, c.keys.empty() ? "" : c.keys.front()};
  }
  return {id, id, ""};
}

std::vector<Workbench::Entry> Workbench::view_menu(const ViewData& v) {
  std::vector<Entry> items;
  const std::string base = base_view(v.id);
  for (const auto& c : hub_.commands()) {  // the view's own, as its manifest lists them
    if (c.view != base) continue;
    Entry e = entry(c.id);
    if (const auto colon = e.title.find(": "); colon != std::string::npos) e.title = e.title.substr(colon + 2);
    items.push_back(std::move(e));  // "Files: Open in editor" is "Open in editor" here
  }
  if (!items.empty()) items.push_back({});
  items.push_back(entry("workbench.properties"));
  if (const auto it = pages_.find(v.id); it != pages_.end() && it->second.view) {
    const std::string focus = it->second.view->focus();
    if (const Value* node = focus.empty() ? nullptr : find_widget(v.tree, focus); node && (*node)["sortable"].boolean()) {
      items.push_back(entry("workbench.table.sort"));
      if ((*node)["sorted"].is_array()) items.push_back(entry("workbench.table.reverse"));
    }
  }
  items.push_back({});
  for (auto& e : tab_menu(v)) items.push_back(std::move(e));
  return items;
}

std::vector<Workbench::Entry> Workbench::tab_menu(const ViewData& v) {
  std::vector<Entry> items;
  for (const Region r : kRegions) {
    if (r != v.region) items.push_back(entry(std::string("workbench.move:") + region_name(r)));
  }
  Entry max = entry("workbench.maximize");
  max.title = maximized_ ? "View: Restore the regions" : "View: Maximize";
  items.push_back(std::move(max));
  items.push_back(entry("workbench.closeView"));
  return items;
}

namespace {
// Entries as a menu model: an empty id starts a new section.
GMenu* menu_of(const std::vector<std::pair<std::string, std::string>>& items,
               const std::function<std::string(const std::string&)>& accel = nullptr) {
  GMenu* menu = g_menu_new();
  GMenu* section = g_menu_new();
  auto flush = [&] {
    if (g_menu_model_get_n_items(G_MENU_MODEL(section)) > 0) g_menu_append_section(menu, nullptr, G_MENU_MODEL(section));
    g_object_unref(section);
    section = g_menu_new();
  };
  for (const auto& [id, title] : items) {
    if (id.empty()) {
      flush();
      continue;
    }
    GMenuItem* it = g_menu_item_new(title.c_str(), nullptr);
    g_menu_item_set_action_and_target_value(it, "rcp.run", g_variant_new_string(id.c_str()));
    if (accel) {
      if (const std::string a = accel(id); !a.empty()) g_menu_item_set_attribute(it, "accel", "s", a.c_str());
    }
    g_menu_append_item(section, it);
    g_object_unref(it);
  }
  flush();
  g_object_unref(section);
  return menu;
}
}  // namespace

void Workbench::show_menu(const std::vector<Entry>& items, GtkWidget* at, double x, double y) {
  if (closing_) return;
  std::vector<std::pair<std::string, std::string>> list;
  std::map<std::string, std::string> keys;
  for (const auto& e : items) {
    list.emplace_back(e.id, e.title);
    keys[e.id] = e.key;
  }
  GMenu* menu = menu_of(list, [&](const std::string& id) { return accel_of(keys[id]); });
  GtkWidget* pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
  g_object_unref(menu);
  // Its own way to the commands: `at` may be in a dialog window.
  gtk_widget_insert_action_group(pop, "rcp", G_ACTION_GROUP(g_object_get_data(G_OBJECT(window_), "rcp-actions")));
  gtk_widget_set_parent(pop, at);
  const GdkRectangle r{int(x), int(y), 1, 1};
  gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
  gtk_popover_set_has_arrow(GTK_POPOVER(pop), false);
  gtk_widget_set_halign(pop, GTK_ALIGN_START);
  g_object_ref(pop);
  on<void, GtkPopover*>(pop, "closed", [](GtkPopover* p) {
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
      if (gtk_widget_get_parent(GTK_WIDGET(d))) gtk_widget_unparent(GTK_WIDGET(d));
      return G_SOURCE_REMOVE;
    }, p, g_object_unref);
  });
  gtk_popover_popup(GTK_POPOVER(pop));
}

void Workbench::update_menus() {
  std::string sig = hub_.perspective() + "|";
  for (const auto& p : hub_.perspectives()) sig += p.id + ":" + p.title + ",";
  sig += "|";
  for (const auto& v : hub_.view_infos()) sig += v.id + ":" + v.title + ",";
  if (sig == menus_seen_) return;
  menus_seen_ = sig;

  std::vector<std::pair<std::string, std::string>> persp;
  std::string current = hub_.perspective();
  for (const auto& p : hub_.perspectives()) {
    persp.emplace_back("workbench.perspective:" + p.id, p.title);
    if (p.id == hub_.perspective()) current = p.title;
  }
  persp.emplace_back("", "");
  persp.emplace_back("workbench.perspective.reset", "Reset to its definition");
  GMenu* pm = menu_of(persp);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(perspective_button_), G_MENU_MODEL(pm));
  g_object_unref(pm);
  gtk_menu_button_set_label(GTK_MENU_BUTTON(perspective_button_), current.c_str());

  const auto key = [this](const std::string& id) { return accel_of(entry(id).key); };
  GMenu* views = menu_of([&] {
    std::vector<std::pair<std::string, std::string>> v;
    for (const auto& vi : hub_.view_infos()) {
      if (!Hub::is_hub_view(vi.id)) v.emplace_back("view.open:" + vi.id, vi.title);
    }
    return v;
  }());
  GMenu* main = menu_of({{"workbench.palette", "All Commands…"}, {"workbench.goto", "Go to Anything…"}, {"", ""},
                         {"workbench.problems", "Problems"}, {"workbench.properties", "Properties"},
                         {"workbench.preferences", "Preferences"}, {"workbench.editPrefs", "Edit the Preferences File"},
#ifdef MYRCP_HAVE_VTE
                         {"workbench.terminal", "New Terminal"},
#endif
                         {"", ""}, {"workbench.restartHost", "Restart Plugins"}, {"workbench.showLog", "Plugin Host Log"},
                         {"", ""}, {"workbench.quit", "Quit"}},
                        key);
  g_menu_insert_submenu(main, 1, "Open View", G_MENU_MODEL(views));
  g_object_unref(views);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(main_menu_button_), G_MENU_MODEL(main));
  g_object_unref(main);
}

void Workbench::update_jobs() {
  const auto& jobs = hub_.jobs();
  gtk_widget_set_visible(jobs_box_, !jobs.empty());
  if (jobs.empty()) return;
  const auto& [id, j] = *jobs.begin();
  std::string text = j.title + (j.label.empty() ? "" : ": " + j.label);
  if (jobs.size() > 1) text += " (+" + std::to_string(jobs.size() - 1) + " more)";
  gtk_label_set_text(GTK_LABEL(jobs_label_), text.c_str());
  if (j.value > 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(jobs_bar_), std::clamp(j.value, 0.0, 1.0));
  else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(jobs_bar_));
}

void Workbench::sort_focused(bool next_column) {
  const ViewData* v = active_view();
  const auto it = pages_.find(focus_id_);
  if (!v || it == pages_.end() || !it->second.view) return;
  const std::string focus = it->second.view->focus();
  const Value* node = focus.empty() ? nullptr : find_widget(v->tree, focus);
  if (!node || !(*node)["sortable"].boolean()) return set_status("Nothing to sort here", "normal", 3);
  const Value& s = (*node)["sorted"];
  const int col = s.is_array() ? int(s.at(0).integer(-1)) : -1;
  bool desc = s.is_array() && s.at(1).boolean();
  const auto& cols = (*node)["columns"].array();
  int to = col;
  if (next_column) {
    desc = false;
    for (size_t k = 1; k <= cols.size(); ++k) {
      const int c = int((size_t(col + 1) + k - 1) % cols.size());
      if (cols[size_t(c)]["sortable"].boolean(true)) {
        to = c;
        break;
      }
    }
  } else {
    if (col < 0) return set_status("The table is not sorted", "normal", 3);
    desc = !desc;
  }
  const int cur = it->second.view->cursor(focus);
  const Value* row = row_at(*node, cur);
  event(v->id, focus, "sort", Array{to, desc, row ? Value(item_key(*row, size_t(cur))) : Value()});
}

void Workbench::run_command(const std::string& id) {
  if (id == "workbench.quit") {
    closing();
    gtk_window_destroy(GTK_WINDOW(window_));
    return;
  } else if (id == "workbench.palette") {
    std::vector<QuickPick::Item> items;
    for (const auto& e : palette_entries()) {
      std::string key = e.key;
      if (const std::string a = accel_of(key); !a.empty()) {
        guint k = 0;
        GdkModifierType m{};
        if (gtk_accelerator_parse(a.c_str(), &k, &m)) {
          char* label = gtk_accelerator_get_label(k, m);
          key = label;
          g_free(label);
        }
      }
      items.push_back({e.id, e.title, key});
    }
    pick_ = std::make_unique<QuickPick>(GTK_WINDOW(window_), "Run a command…", std::move(items), [this](const std::string& chosen) {
      g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
        auto* a = static_cast<std::pair<Workbench*, std::string>*>(d);
        a->first->pick_.reset();
        if (!a->second.empty()) a->first->run_command(a->second);
        else a->first->activate(std::string(a->first->focus_id_));
        return G_SOURCE_REMOVE;
      }, new std::pair<Workbench*, std::string>(this, chosen), [](gpointer d) { delete static_cast<std::pair<Workbench*, std::string>*>(d); });
    });
  } else if (id == "workbench.goto") {
    go_to_anything();
  } else if (id == "workbench.nextView" || id == "workbench.prevView") {
    cycle_tab(id == "workbench.nextView" ? 1 : -1);
  } else if (id == "workbench.back" || id == "workbench.forward") {
    go(id == "workbench.back");
  } else if (id == "workbench.nextRegion" || id == "workbench.prevRegion") {
    cycle_region(id == "workbench.nextRegion" ? 1 : -1);
  } else if (id == "workbench.contextMenu") {
    const auto it = pages_.find(focus_id_);
    const ViewData* v = active_view();
    if (it == pages_.end() || !v) return;
    double x = 0, y = 0;
    GtkWidget* at = it->second.view ? it->second.view->cursor_widget(x, y) : nullptr;
    if (at) show_menu(view_menu(*v), at, x, y);
    else show_menu(tab_menu(*v), it->second.label, 0, 0);
  } else if (id == "workbench.table.sort" || id == "workbench.table.reverse") {
    sort_focused(id == "workbench.table.sort");
  } else if (id == "workbench.maximize") {
    toggle_maximize();
  } else if (id.starts_with("workbench.move:")) {
    if (focus_id_.empty()) return;
    const std::string view = focus_id_;
    hub_.set_region(view, region_from_name(id.substr(15)));
    maximized_ = false;
    want_focus_ = view;
    queue_sync();
  } else if (id == "workbench.closeView") {
    if (!focus_id_.empty()) close_view(std::string(focus_id_));
  } else if (id == "workbench.restartHost") {
    hub_.restart();
  } else if (id == "workbench.showLog") {
    open_editor_later(hub_.options().log_path, 0, [](bool) {});
  } else if (id == "workbench.cancelJobs") {
    hub_.cancel_jobs();
  } else if (id == "workbench.problems") {
    hub_.open_view(Hub::kProblems);
    activate(Hub::kProblems);
  } else if (id == "workbench.properties") {
    // Shown beside the view it describes when it opens elsewhere, which keeps the focus.
    const std::string back = focus_id_;
    hub_.open_view(Hub::kProperties);
    const ViewData* p = hub_.find_view(Hub::kProperties);
    if (p && p->region != focus_region_) want_shown_[ri(p->region)] = Hub::kProperties;
    else activate(Hub::kProperties);
    queue_sync();
  } else if (id == "workbench.terminal") {
    open_term({env_or("SHELL", "/bin/sh")}, "", "", nullptr, Region::bottom);
  } else if (id.starts_with("workbench.perspective:")) {
    hub_.switch_perspective(id.substr(22));
  } else if (id == "workbench.perspective.reset") {
    hub_.reset_perspective();
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
  } else if (id.starts_with("view.open:")) {
    const std::string view = id.substr(10);
    hub_.open_view(view);
    activate(view);
  } else {
    // A plugin command acts on what the cursor is on: tell it the focused row.
    Map params;
    if (const ViewData* v = active_view()) {
      params.emplace_back("view", v->id);
      if (const auto it = pages_.find(v->id); it != pages_.end() && it->second.view) {
        const std::string focus = it->second.view->focus();
        params.emplace_back("widget", focus);
        const Value* node = focus.empty() ? nullptr : find_widget(v->tree, focus);
        if (node && is_rows((*node)["t"].str())) {
          const int cursor = it->second.view->cursor(focus);
          if (const Value* row = row_at(*node, cursor)) {
            params.emplace_back("key", item_key(*row, size_t(cursor)));
            params.emplace_back("index", cursor);
          }
        }
      }
    }
    if (const std::string err = hub_.run_command(id, std::move(params)); !err.empty()) set_status(err, "error");
  }
  queue_sync();
}

// Keys: Alt+1…9 pick a tab of the focused region; others run the command bound to them
// (the active view's first). Navigation keys stay the widgets'; plain keys are not
// commands while text is typed; a terminal view keeps its keys but for tab switching,
// and Ctrl+Shift+<key> there is the workbench's Ctrl+<key>.
bool Workbench::key_pressed(guint keyval, GdkModifierType state) {
  state = GdkModifierType(state & gtk_accelerator_get_default_mod_mask());
  std::string name = key_name(keyval, state);
  if (name.empty()) return false;
  GtkWidget* f = gtk_root_get_focus(GTK_ROOT(window_));
  if (name.size() == 5 && name.starts_with("alt+") && name[4] >= '1' && name[4] <= '9') {
    GtkNotebook* nb = GTK_NOTEBOOK(notebooks_[ri(focus_region_)]);
    if (Page* p = page_of(gtk_notebook_get_nth_page(nb, name[4] - '1'))) activate(std::string(p->id));
    return true;
  }
  // With Ctrl or Alt, or a function key: never text being typed.
  const std::string_view last = std::string_view(name).substr(name.find_last_of('+') + 1);
  const bool fkey = last.size() >= 2 && last[0] == 'f' && std::isdigit(static_cast<unsigned char>(last[1]));
  const bool chord = (state & (GDK_CONTROL_MASK | GDK_ALT_MASK)) || fkey;
  if (in_terminal(f)) {
    const bool tab_key = name == "alt+left" || name == "alt+right" || name == "ctrl+pageup" || name == "ctrl+pagedown";
    if (name.starts_with("ctrl+shift+")) name = "ctrl+" + name.substr(11);
    else if (!tab_key) return false;
  } else {
    static const std::set<std::string, std::less<>> widget_keys = {
        "enter", "up", "down", "left", "right", "pageup", "pagedown", "home", "end", "tab", "shift+tab", "space", "escape"};
    if (widget_keys.contains(name)) return false;
    if (!chord && in_text(f)) return false;
  }
  const ViewData* v = active_view();
  const bool modal = v && !v->loading && find_modal(v->tree);
  const std::string active_base = v ? base_view(v->id) : "";
  const auto commands = all_commands();
  for (int pass = 0; pass < 2; ++pass) {
    for (const auto& c : commands) {
      if (modal && !c.plugin.empty()) continue;  // a dialog blocks the view's own keys
      const bool scope_ok = pass == 0 ? (!active_base.empty() && c.view == active_base) : c.view.empty();
      if (scope_ok && std::find(c.keys.begin(), c.keys.end(), name) != c.keys.end()) {
        run_command(c.id);
        return true;
      }
    }
  }
  return false;
}

// Go to Anything: views, problems and the files below the working directory.
void Workbench::go_to_anything() {
  std::vector<QuickPick::Item> items;
  for (const auto& v : hub_.views()) items.push_back({"view:" + v.id, v.title, "open view"});
  std::set<std::string> open;
  for (const auto& v : hub_.views()) open.insert(base_view(v.id));
  for (const auto& vi : hub_.view_infos()) {
    if (!open.contains(vi.id)) items.push_back({"view:" + vi.id, vi.title, "view"});
  }
  const auto markers = hub_.markers();
  for (size_t i = 0; i < markers.size() && i < 2000; ++i) {
    const Marker& m = markers[i];
    std::string where = m.path.empty() ? m.view : std::filesystem::path(m.path).filename().string();
    if (m.line > 0) where += ":" + std::to_string(m.line);
    items.push_back({"problem:" + std::to_string(i), m.message, m.severity + " · " + where});
  }
  int rc = 0;
  std::string files = run_output({"rg", "--files"}, &rc);
  if (rc != 0) files = run_output({"git", "ls-files"}, &rc);
  if (rc != 0) files = run_output({"find", ".", "-type", "f", "-not", "-path", "*/.git/*"}, &rc);
  std::istringstream in(files);
  int n = 0;
  for (std::string line; std::getline(in, line) && n < 50000; ++n) {
    if (line.starts_with("./")) line.erase(0, 2);
    if (!line.empty()) items.push_back({"file:" + line, line, ""});
  }
  pick_ = std::make_unique<QuickPick>(GTK_WINDOW(window_), "Go to a file, view or problem…", std::move(items),
                                      [this, markers](const std::string& chosen) {
    struct Args {
      Workbench* wb;
      std::string chosen;
      std::vector<Marker> markers;
    };
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
      auto* a = static_cast<Args*>(d);
      Workbench& wb = *a->wb;
      wb.pick_.reset();
      const std::string& c = a->chosen;
      if (c.starts_with("view:")) {
        wb.hub_.open_view(c.substr(5));
        wb.activate(c.substr(5));
      } else if (c.starts_with("file:")) {
        wb.open_location(std::filesystem::absolute(c.substr(5)).string(), 0);
      } else if (c.starts_with("problem:")) {
        const size_t i = std::stoul(c.substr(8));
        if (i < a->markers.size()) {
          const Marker& m = a->markers[i];
          if (!m.view.empty()) {
            wb.hub_.open_view(m.view);
            wb.activate(m.view);
          }
          if (!m.path.empty()) wb.open_location(m.path, m.line);
        }
      } else {
        wb.activate(std::string(wb.focus_id_));
      }
      return G_SOURCE_REMOVE;
    }, new Args{this, chosen, markers}, [](gpointer d) { delete static_cast<Args*>(d); });
  });
}

// ---- status ------------------------------------------------------------------------

void Workbench::set_status(const std::string& text, std::string_view style, int seconds) {
  if (!status_label_) return;
  gtk_label_set_text(GTK_LABEL(status_label_), text.c_str());
  gtk_widget_remove_css_class(status_label_, "error");
  gtk_widget_remove_css_class(status_label_, "success");
  if (style == "error") gtk_widget_add_css_class(status_label_, "error");
  else if (style == "ok") gtk_widget_add_css_class(status_label_, "success");
  if (status_timer_) g_source_remove(status_timer_);
  status_timer_ = 0;
  if (seconds > 0) {
    status_timer_ = g_timeout_add_seconds(guint(seconds), [](gpointer d) -> gboolean {
      auto* wb = static_cast<Workbench*>(d);
      wb->status_timer_ = 0;
      gtk_label_set_text(GTK_LABEL(wb->status_label_), "");
      return G_SOURCE_REMOVE;
    }, this);
  }
}

// ---- editors and terminal views -----------------------------------------------------

bool Workbench::open_editor(const std::string& path, int line) {
  open_editor_later(path, line, [](bool) {});
  return false;  // plugins then expect the file to be edited later
}

void Workbench::open_location(const std::string& path, int line) {
  if (links_.go_to(path, line)) return queue_sync();  // a linked editor jumps there
  open_editor_later(path, line, [](bool) {});
}

// Files open in $VISUAL/$EDITOR in a terminal view, linked both ways when it is a Vim or
// Neovim; or in the linked editor already open.
void Workbench::open_editor_later(const std::string& path, int line, std::function<void(bool)> done) {
  if (links_.go_to(path, line)) return done(false);  // in the editor that is open: it keeps running
  EditorLink* link = links_.start_link(editor_kind(EditorLinks::editor()));
  const std::string id = open_term(links_.command(path, line, link), "", std::filesystem::path(path).filename().string(),
                                   std::move(done));
  if (link && id.empty()) links_.drop(link);
  else if (link) link->view = id;
}

void Workbench::open_terminal(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title) {
  open_term(argv, cwd, title, nullptr);
}

void Workbench::open_compare(const std::string& left, const std::string& right, const std::string& title,
                             std::function<void(bool)> done) {
  if (links_.diff(left, right)) return done(false);  // in the linked editor's diff tab
  open_term(links_.diff_command(left, right), "", title.empty() ? std::filesystem::path(left).filename().string() : title,
            std::move(done));
}

#ifdef MYRCP_HAVE_VTE
std::string Workbench::open_term(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                                 std::function<void(bool)> done, Region region) {
  if (argv.empty()) return "";
  const std::string id = "term:" + std::to_string(next_term_++);
  GtkWidget* vte = vte_terminal_new();
  vte_terminal_set_scrollback_lines(VTE_TERMINAL(vte), 10000);
  vte_terminal_set_mouse_autohide(VTE_TERMINAL(vte), true);
  gtk_widget_set_hexpand(vte, true);
  gtk_widget_set_vexpand(vte, true);
  Term t;
  t.widget = vte;
  t.done = std::move(done);
  t.own_title = title.empty();
  terms_[id] = std::move(t);
  on<void, VteTerminal*, int>(vte, "child-exited", [this, id](VteTerminal*, int) { term_ended(id); });
  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  on<void, VteTerminal*>(vte, "window-title-changed", [this, id](VteTerminal* t) {
    const auto it = terms_.find(id);
    const char* title = vte_terminal_get_window_title(t);
    if (it != terms_.end() && it->second.own_title && title && *title) hub_.set_title(id, title);
  });
  G_GNUC_END_IGNORE_DEPRECATIONS
  std::vector<char*> args;
  for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
  args.push_back(nullptr);
  struct Spawned {
    Workbench* wb;
    std::string id, program;
  };
  vte_terminal_spawn_async(
      VTE_TERMINAL(vte), VTE_PTY_DEFAULT, cwd.empty() ? nullptr : cwd.c_str(), args.data(), nullptr, G_SPAWN_SEARCH_PATH,
      nullptr, nullptr, nullptr, -1, nullptr,
      [](VteTerminal*, GPid, GError* error, gpointer d) {
        auto* s = static_cast<Spawned*>(d);
        if (error) {
          s->wb->set_status("Cannot start " + s->program + ": " + error->message, "error");
          s->wb->term_ended(s->id);
        }
        delete s;
      },
      new Spawned{this, id, argv[0]});
  hub_.open_native(id, title.empty() ? std::filesystem::path(argv[0]).filename().string() : title, region);
  activate(id);
  return id;
}

void Workbench::term_ended(const std::string& id) {
  const auto it = terms_.find(id);
  if (it == terms_.end()) return;
  auto done = std::move(it->second.done);
  terms_.erase(it);
  links_.view_ended(id);
  hub_.close_view(id);
  queue_sync();
  if (done) done(true);
}
#else
std::string Workbench::open_term(const std::vector<std::string>& argv, const std::string&, const std::string&,
                                 std::function<void(bool)> done, Region) {
  set_status("Running " + (argv.empty() ? std::string("programs") : argv[0]) +
                 " needs terminal views: this myrcp-gtk was built without VTE (libvte-2.91-gtk4)",
             "error");
  if (done) done(false);
  return "";
}
void Workbench::term_ended(const std::string&) {}
#endif

}  // namespace rcp::gtk
