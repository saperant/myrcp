// The GTK front-end: the workbench's concepts as a GTK 4 application. The hub's regions
// are notebooks in panes (main and side above, the bottom below; tabs drag between
// them), views are rendered by render.hpp, commands run from a palette window, menus
// and key bindings, context menus are popover menus, jobs and the status sit in a
// status bar, and terminal views (shells, editors) are VTE widgets when built with VTE.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gtk/gtk.h>

#include "editor_links.hpp"
#include "history.hpp"
#include "hub.hpp"
#include "render.hpp"

namespace rcp::gtk {

class QuickPick;

class Workbench : public Hub::Listener, public ViewHost {
 public:
  explicit Workbench(HostOptions o);
  ~Workbench() override;
  Workbench(const Workbench&) = delete;
  Workbench& operator=(const Workbench&) = delete;
  int run(int argc, char** argv);

  // Hub::Listener
  void changed() override { queue_sync(); }
  void status(const std::string& text, std::string_view style, int seconds) override { set_status(text, style, seconds); }
  bool open_editor(const std::string& path, int line) override;
  void open_editor_later(const std::string& path, int line, std::function<void(bool)> done) override;
  void open_terminal(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title) override;
  void open_compare(const std::string& left, const std::string& right, const std::string& title,
                    std::function<void(bool)> done) override;
  void open_location(const std::string& path, int line) override;
  void markers_changed() override { links_.markers_changed(); }
  void view_shown(const std::string& id) override { activate(id); }
  void view_closed(const std::string& id) override { (void)id, queue_sync(); }
  void front_state(Arrangement& a) override;
  void arranged(const Arrangement& a) override;
  void prefs_changed(const std::vector<std::string>& ids) override { links_.prefs_changed(ids); }

  // ViewHost
  void event(const std::string& view, std::string_view widget, std::string_view kind, mp::Value value) override;
  void cursor_moved(const std::string& view) override;
  void focused(const std::string& view) override;
  void context_menu(const std::string& view, GtkWidget* at, double x, double y) override;

  // The event loop's part (a GSource of ours polls the hub's and the editors' fds).
  void poll_prepare(GSource* src, int* timeout);
  bool poll_check() const;
  void poll_dispatch();

 private:
  struct Entry {  // a command as the palette and menus list it
    std::string id, title, key;
  };
  struct Page {
    std::string id;
    GtkWidget* page = nullptr;  // in a notebook (we hold a reference)
    GtkWidget* label = nullptr;
    std::unique_ptr<ViewWidget> view;  // a plugin's or the hub's view
    Region region = Region::main;
    mp::Value seen;
    bool seen_loading = false;
  };

  void activate_app();
  void closing();
  void build_window();
  GtkWidget* make_notebook(Region r);
  void install_css();
  void measure();

  // views and regions
  void queue_sync();
  void sync();
  Page& add_page(const ViewData& v);
  void remove_page(const std::string& id);
  void move_page(Page& p, Region r);
  Page* page_of(GtkWidget* w);  // the page a widget is in
  GtkNotebook* notebook_of(GtkWidget* page) const;
  const ViewData* active_view() const;
  void activate(const std::string& id);  // show it and give it the focus
  void focus_later();
  void set_focus_view(const std::string& id);
  void focus_changed();
  void close_view(const std::string& id);
  void cycle_tab(int step);
  void cycle_region(int step);
  void toggle_maximize();
  void update_regions();
  void apply_sizes();
  void sizes_dragged();
  void update_selection();
  Place place_here();
  void go(bool back);

  // commands
  std::vector<CommandInfo> all_commands() const;
  std::vector<Entry> palette_entries() const;
  Entry entry(const std::string& id) const;
  std::vector<Entry> view_menu(const ViewData& v);
  std::vector<Entry> tab_menu(const ViewData& v);
  void show_menu(const std::vector<Entry>& items, GtkWidget* at, double x, double y);
  void run_command(const std::string& id);
  void sort_focused(bool next_column);
  bool key_pressed(guint keyval, GdkModifierType state);
  void go_to_anything();
  void update_menus();
  void update_jobs();

  // status
  void set_status(const std::string& text, std::string_view style, int seconds = 5);

  // terminal views
  std::string open_term(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                        std::function<void(bool)> done, Region region = Region::main);
  void term_ended(const std::string& id);

  Hub hub_;
  EditorLinks links_;
  std::vector<CommandInfo> builtins_;
  GtkApplication* app_ = nullptr;
  GtkWidget* window_ = nullptr;
  GtkWidget* hpaned_ = nullptr;  // main | side
  GtkWidget* vpaned_ = nullptr;  // (main | side) / bottom
  std::array<GtkWidget*, 3> notebooks_{};
  GtkWidget* status_label_ = nullptr;
  GtkWidget* jobs_box_ = nullptr;
  GtkWidget* jobs_bar_ = nullptr;
  GtkWidget* jobs_label_ = nullptr;
  GtkWidget* back_button_ = nullptr;
  GtkWidget* forward_button_ = nullptr;
  GtkWidget* perspective_button_ = nullptr;
  GtkWidget* main_menu_button_ = nullptr;
  std::string menus_seen_;  // what the menus were built from
  std::map<std::string, Page> pages_;
  std::array<std::string, 3> want_shown_;  // what an arrangement wants each region to show
  std::string want_focus_;
  std::string focus_id_;
  Region focus_region_ = Region::main;
  bool maximized_ = false;
  bool syncing_ = false;
  bool closing_ = false;  // the window is being destroyed
  Arrangement final_;     // what it showed then
  guint sync_source_ = 0;
  guint focus_source_ = 0;
  bool applying_sizes_ = false;
  guint sizes_timer_ = 0;
  NavHistory history_;
  bool going_ = false;
  std::string sel_view_;
  mp::Value sel_items_;
  guint status_timer_ = 0;
  std::unique_ptr<QuickPick> pick_;

  GSource* poll_source_ = nullptr;
  std::vector<GPollFD> gfds_;
  std::vector<Hub::PollToken> tokens_;
  size_t links_at_ = 0;

  struct Term {
    GtkWidget* widget = nullptr;  // the VteTerminal
    std::function<void(bool)> done;  // an editor's request, answered when it exits
    bool own_title = false;
  };
  std::map<std::string, Term> terms_;  // by view id "term:<n>"
  int next_term_ = 1;
  int exit_code_ = 0;
};

}  // namespace rcp::gtk
