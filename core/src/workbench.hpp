// The terminal front-end: draws the hub's views into a cell grid and turns keys and
// mouse clicks into events and commands.
#pragma once

#include <array>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hub.hpp"
#include "rpc.hpp"
#include "rcp/grid.hpp"
#include "rcp/keys.hpp"
#include "rcp/layout.hpp"
#include "rcp/msgpack.hpp"
#include "editor_links.hpp"
#include "history.hpp"
#include "terminal.hpp"

namespace rcp {

class TermSession;

class Workbench : public Hub::Listener {
 public:
  explicit Workbench(HostOptions o);
  ~Workbench() override;
  int run();

  // Hub::Listener
  void changed() override { dirty_ = true; }
  void status(const std::string& text, std::string_view style, int seconds) override;
  bool open_editor(const std::string& path, int line) override;
  void open_editor_later(const std::string& path, int line, std::function<void(bool)> done) override;
  void open_terminal(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title) override;
  void open_compare(const std::string& left, const std::string& right, const std::string& title,
                    std::function<void(bool)> done) override;
  void open_location(const std::string& path, int line) override;
  void markers_changed() override;
  void view_shown(const std::string& id) override { activate(id); }
  void view_closed(const std::string& id) override;
  void front_state(Arrangement& a) override;
  void arranged(const Arrangement& a) override;
  void prefs_changed(const std::vector<std::string>& ids) override;

 private:
  struct PaletteEntry {
    std::string id, title, key;
  };

  std::string editor_mode() const;
  // input
  void drain_keys();
  void handle_key(const Key& k);
  bool widget_key(const ViewData& v, ViewState& vs, const Key& k);
  void handle_mouse(const Mouse& m);
  void palette_mouse(const Mouse& m);
  void palette_key(const Key& k);
  // The context menu (a right click, Shift+F10): what can be done with what is under it.
  void open_menu(int x, int y, std::vector<PaletteEntry> items);
  std::vector<PaletteEntry> view_menu(const ViewData& v);  // its commands, then the workbench's
  std::vector<PaletteEntry> tab_menu(const ViewData& v);
  void menu_key(const Key& k);
  void menu_mouse(const Mouse& m);
  void draw_menu(Grid& g);
  void context_menu_here();  // Shift+F10: at the focused row
  PaletteEntry entry(const std::string& id);  // a command as a menu item, its keys too
  void run_command(const std::string& id);
  // Asks a sortable table's plugin to sort it (column -1: its own order), with the key at
  // the cursor for the cursor to follow its row.
  void sort_table(const ViewData& v, ViewState& vs, const std::string& id, const mp::Value& node, int column,
                  bool descending);
  void sort_focused(bool next_column);
  std::pair<int, bool> sort_state(const WidgetState& ws, const mp::Value& node);  // column, descending  // Alt+S: the next column; Alt+R: the other way
  void move_cursor(const ViewData& v, ViewState& vs, const std::string& id, const mp::Value& node, int to);
  // views
  const ViewData* active_view();  // the focused one
  void activate(const std::string& id);
  void activate_index(size_t i);
  void close_active();
  // regions: main, side and bottom, each a stack of tabs; folded into main when the
  // terminal is too small for one, or while maximized.
  bool region_shown(Region r) const;
  Region where(const ViewData& v) const;  // the region it is drawn in now
  std::vector<const ViewData*> tabs_of(Region r) const;
  const ViewData* region_view(Region r);  // the one it shows
  void focus_region(Region r);
  void cycle_region(int step);
  void focus_direction(std::string_view dir);
  void move_active(Region r);
  void update_selection();  // the focused view's, to the hub
  void send_event(const ViewData& v, std::string_view widget, std::string_view kind, mp::Value value);
  void request_windows(const ViewData& v, ViewState& vs);
  mp::Value form_values(const ViewData& v, const ViewState& vs) const;
  std::vector<CommandInfo> all_commands() const;
  // output
  void draw();
  void draw_palette(Grid& g);
  int draw_tabs(Grid& g, int x, int y, int xe, Region r, const ViewData* shown);
  void draw_region(Grid& g, Region r, const ViewData* v);
  void set_status(std::string text, Style style, int seconds = 5);
  void expire_status();
  int status_timeout_ms() const;
  // palette
  std::vector<PaletteEntry> palette_entries();
  void refilter();
  void suspend();
  void redraw_all();
  // terminal views
  TermSession* active_term();
  // Returns the new view's id, "" when the program could not start.
  std::string open_term(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                        std::function<void(bool)> done, Region region = Region::main);
  void term_ended(const std::string& id);
  void send_term_key(TermSession* t, const Key& k);
  void send_term_mouse(TermSession* t, const Mouse& m, int col, int row);
  void draw_term(Grid& g, Rect r, const std::string& id, bool focused);

  Terminal term_;
  KeyDecoder keys_;
  Hub hub_;
  const Theme* theme_;
  Grid front_;
  std::vector<CommandInfo> builtins_;

  std::map<std::string, ViewState, std::less<>> vstate_;  // per open view
  struct RegionState {
    std::string active;  // the view it shows
    size_t hint = 0;     // where that was among its tabs, for when it closes
    Rect rect;           // where it was drawn; empty when not shown
  };
  std::array<RegionState, 3> regions_;
  std::string focus_id_;  // the focused view
  NavHistory history_;    // Back and Forward (history.hpp)
  bool going_ = false;    // moving through it: not a place left
  Place place_here();     // the focused view, widget and row
  void go(bool back);
  std::string sel_view_;  // the selection as this front-end last made it
  mp::Value sel_items_;
  Region focus_region_ = Region::main;
  bool maximized_ = false;
  int side_sep_x_ = -1, bottom_bar_y_ = -1;  // as drawn: where a drag resizes; -1: none
  enum class Drag : uint8_t { none, side, bottom } drag_ = Drag::none;
  Sizes drag_sizes_;  // while dragging; the hub's on release
  Size size_{80, 24};  // the terminal's, as last drawn

  struct {
    bool open = false;
    std::string text;
    int cursor = 0;
    int scroll = 0;
    std::vector<PaletteEntry> matches;
    Rect box;        // where it was drawn, for clicks
    int rows_y = 0;  // first entry row
  } palette_;
  struct {
    bool open = false;
    int x = 0, y = 0;  // where it was asked for
    std::vector<PaletteEntry> items;  // an empty id: a separator
    int cursor = 0;
    Rect box;
  } menu_;
  struct TabHit {
    int x0, x1, y;
    std::string id;
  };
  std::vector<TabHit> tab_hits_;  // the tabs as drawn, for clicks

  struct Term {
    std::unique_ptr<TermSession> session;
    std::function<void(bool)> done;  // an editor's request, answered when it exits
    bool own_title = false;           // the program's title (a shell's) names the tab
  };
  std::map<std::string, Term> terms_;  // by view id "term:<n>"
  // A program over the workbench, in a box of its own (fzf for Go to Anything): it has
  // the keys until it exits, then done(exit status).
  struct Popup {
    std::unique_ptr<TermSession> session;
    std::function<void(int status)> done;
    std::string title;
  };
  std::unique_ptr<Popup> popup_;
  bool open_popup(const std::vector<std::string>& argv, const std::string& title, std::function<void(int)> done);
  void popup_ended();
  void draw_popup(Grid& g);
  void go_to_anything();
  int next_term_ = 1;
  EditorLinks links_;  // Neovim and Vim, linked both ways (editor_links.hpp)
  bool prefix_ = false;  // Ctrl+\ was pressed: the next key is the workbench's

  std::string status_;
  Style status_style_ = Style::bar;
  std::chrono::steady_clock::time_point status_until_;

  bool quit_ = false;
  bool dirty_ = true;
};

}  // namespace rcp
