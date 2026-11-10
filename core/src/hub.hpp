// The workbench model, shared by front-ends: plugin hosts and
// their contributions, open views with their trees, jobs, and the plugin side of
// commands and events. Front-ends own presentation: widget state, focus, drawing.
// Not thread-safe; a front-end with threads guards it with one mutex.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <poll.h>
#include <string>
#include <string_view>
#include <vector>

#include "console.hpp"
#include "host.hpp"
#include "prefs.hpp"
#include "service.hpp"
#include "rcp/msgpack.hpp"

namespace rcp {

struct CommandInfo {
  std::string id, title;
  std::vector<std::string> keys;
  std::string view;    // binding/palette entry only while this view is active
  std::string plugin;  // empty: built into the front-end
};

// Where a view sits in the workbench. The front-end lays the regions out (Eclipse's
// editor area with a side and a bottom stack) and may fold them into one when small.
enum class Region : uint8_t { main, side, bottom };
inline constexpr Region kRegions[] = {Region::main, Region::side, Region::bottom};
Region region_from_name(std::string_view name);  // main when unknown
const char* region_name(Region r);

struct ViewInfo {
  std::string id, title, plugin;
  bool startup = false;
  Region region = Region::main;  // where it opens (the manifest's `region`)
};

// An open view: `id` is the view id, or "<view id>#<arg>" for an instance of a
// view with an argument (one tab per job, say).
struct ViewData {
  std::string id, title;
  mp::Value tree;
  bool loading = true;
  bool native = false;     // drawn by the front-end itself (a terminal view), no plugin
  bool hub_owned = false;  // its tree comes from the hub (Problems), its events go there
  int order = -1;  // its place in the arrangement it was opened for; -1: opened since
  Region region = Region::main;
  // Part of the saved arrangement: not a terminal or a console (what ran there is gone).
  bool kept() const { return !native && !id.starts_with("console#"); }
};

// A problem a plugin reports: a place in a file and/or a view that shows it.
struct Marker {
  std::string path;  // absolute; empty: no file
  int line = 0, col = 0;  // 1-based line; 0: none
  std::string severity;   // "error", "warning", "info"
  std::string message, source;
  std::string view;  // open this view to see it (e.g. "tool.job#photos")
  bool operator==(const Marker&) const = default;
};

// The regions' sizes in percent: the side's share of the width, the bottom's of the height.
struct Sizes {
  int side = 36, bottom = 30;
  bool operator==(const Sizes&) const = default;
};
Sizes clamp_sizes(Sizes s);

using Placement = std::vector<std::pair<std::string, Region>>;  // view ids, in tab order

// A named arrangement of views for a task (Eclipse's perspectives). Plugins contribute
// them in their manifests (`[[perspectives]]`), users in perspectives.toml; "default"
// is the views the manifests open at startup.
struct PerspectiveInfo {
  std::string id, title, plugin;
  Placement views;
  Sizes sizes;
};

// A perspective as the user left it: kept across switches and sessions.
struct Arrangement {
  Placement views;
  Sizes sizes;
  std::array<std::string, 3> shown;  // what each region shows (the front-end's part)
  std::string focus;                 // the focused view (the front-end's part)
  std::vector<std::string> known;    // the definition's views when saved: others are new
};

struct JobInfo {
  std::string title, label;
  double value = 0;
  size_t host = 0;
};

// The base view id of an open view id ("tool.job#photos" → "tool.job").
std::string base_view(std::string_view id);

class Hub {
 public:
  // What front-ends hear. Called from inside Hub calls (on_ready, open_view, ...).
  struct Listener {
    virtual ~Listener() = default;
    virtual void changed() {}  // trees, titles, jobs, views: draw again
    // style: "normal", "ok" or "error"
    virtual void status(const std::string& text, std::string_view style, int seconds) {
      (void)text, (void)style, (void)seconds;
    }
    // A plugin wants a file edited. Returns true when the editor ran and has exited.
    virtual bool open_editor(const std::string& path, int line) {
      (void)path, (void)line;
      return false;
    }
    // The same, answered when the editor is done: done(true) once it has exited,
    // done(false) when it keeps running elsewhere. An editor in a tab answers late.
    virtual void open_editor_later(const std::string& path, int line, std::function<void(bool waited)> done) {
      done(open_editor(path, line));
    }
    // A plugin wants two files compared (Hub::diff_command, or in the linked editor).
    // done(true) once the tool has exited, done(false) when it keeps running elsewhere.
    virtual void open_compare(const std::string& left, const std::string& right, const std::string& title,
                              std::function<void(bool waited)> done) {
      (void)left, (void)right, (void)title;
      status("Comparing needs a front-end that runs programs", "normal", 5);
      done(false);
    }
    // A plugin wants a program run in a terminal view.
    virtual void open_terminal(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title) {
      (void)argv, (void)cwd, (void)title;
      status("Terminal views need the terminal workbench", "normal", 5);
    }
    // Show this place in an editor: by default an editor request nobody waits for.
    virtual void open_location(const std::string& path, int line) {
      open_editor_later(path, line, [](bool) {});
    }
    virtual void markers_changed() {}  // e.g. for diagnostics in an editor
    virtual void view_shown(const std::string& id) { (void)id; }   // a plugin asked to show it
    virtual void view_closed(const std::string& id) { (void)id; }  // a plugin or a perspective closed it
    // Fill in the front-end's part of the arrangement (shown views, focus), to save it.
    virtual void front_state(Arrangement& a) { (void)a; }
    // A perspective was applied (or its views opened at startup): show what it shows,
    // where those views are open.
    virtual void arranged(const Arrangement& a) { (void)a; }
    // These preferences changed value (set, the file read again, an env var aside).
    virtual void prefs_changed(const std::vector<std::string>& ids) { (void)ids; }
  };

  Hub(HostOptions options, Listener& listener);
  ~Hub();
  Hub(const Hub&) = delete;
  Hub& operator=(const Hub&) = delete;

  void start();
  void stop();
  void restart();  // open views stay open and are asked for again
  bool ready() const { return !hosts_.empty() && hosts_[0].ready; }
  const HostOptions& options() const { return options_; }

  // Event loop integration: add the hosts' fds, then hand back the readable ones.
  struct PollToken {
    size_t slot;
    int gen;
    std::string console = {};  // a console's output instead of a host
    int fd = -1;
    Service* service = nullptr;  // or a service's program
  };
  void add_fds(std::vector<pollfd>& fds, std::vector<PollToken>& tokens) const;
  void on_ready(const PollToken& token);

  // Views, in the order they were opened.
  const std::vector<ViewData>& views() const { return views_; }
  ViewData* find_view(std::string_view id);
  void open_view(const std::string& id);  // no-op when already open
  void close_view(const std::string& id);
  // A front-end's own view (no plugin draws it), e.g. a terminal.
  void open_native(const std::string& id, const std::string& title, Region region = Region::main);
  void set_region(const std::string& id, Region region);
  void set_title(const std::string& id, const std::string& title);

  const std::vector<CommandInfo>& commands() const { return commands_; }
  const std::vector<ViewInfo>& view_infos() const { return view_infos_; }
  std::string plugin_of_view(std::string_view view_id) const;

  void send_event(const std::string& view, std::string_view widget, std::string_view kind, mp::Value value);
  // Runs a plugin command. params: view, and the focused widget/key/index if any.
  // Returns an error message, or "" when sent.
  std::string run_command(const std::string& id, mp::Map params);
  const std::map<std::string, JobInfo>& jobs() const { return jobs_; }
  void cancel_jobs();

  // Problem markers, by the set a plugin reported them in.
  std::vector<Marker> markers() const;
  std::vector<Marker> markers_without(std::string_view set_prefix) const;  // but those sets
  // Replaces a set (empty: removes it). Plugins' sets come over the protocol; a
  // front-end reports its own, e.g. an editor's diagnostics ("editor:…").
  void set_markers(const std::string& set, std::vector<Marker> markers);
  static constexpr const char* kProblems = "problems";  // the view listing them

  // The workbench selection: what the cursor of the focused view is on, as that view's
  // rows describe it (`sel`: items with type, label, path, line, props). Front-ends set
  // it as the focus and cursors move; every plugin host hears of it, and the
  // Properties view shows it. A view that offers no selection leaves it as it was.
  struct Selection {
    std::string view;  // where it was made
    mp::Value items = mp::Array{};
  };
  const Selection& selection() const { return selection_; }
  void set_selection(const std::string& view, mp::Value items);
  static constexpr const char* kProperties = "properties";  // the view showing it

  // Perspectives. Every change to the arrangement (views opened, closed or moved, sizes)
  // is remembered for the active one and saved (HostOptions::state_path), so a session
  // starts as the last one ended; switching closes the views the next one lacks and
  // opens those it has. Terminal views stay through switches and are not saved.
  const std::vector<PerspectiveInfo>& perspectives() const { return perspectives_; }
  const PerspectiveInfo* find_perspective(std::string_view id) const;
  const std::string& perspective() const { return active_; }
  void switch_perspective(const std::string& id);
  void reset_perspective();  // back to its definition
  const Sizes& sizes() const { return sizes_; }
  void set_sizes(Sizes s);
  void save_state();
  static constexpr const char* kDefault = "default";

  // Preferences: declared by the front-ends (declare_pref, before start) and by plugins
  // (`[[preferences]]`), kept in HostOptions::prefs_path. A value is its environment
  // variable's while that is set, else the file's, else the default. Every host hears
  // all of them (`preferences.changed`) before its views open, and each change after.
  void declare_pref(PrefInfo p);
  const std::vector<PrefInfo>& prefs() const { return prefs_; }
  const PrefInfo* find_pref(std::string_view id) const;
  mp::Value pref(std::string_view id) const;  // nil when not declared
  std::string pref_source(const PrefInfo& p) const;  // "default", "file" or "$VAR"
  // Sets one in the file (nil: back to the default). Returns an error, or "".
  std::string set_pref(const std::string& id, mp::Value v);
  void reload_prefs();  // the file may have changed (edited by hand)
  const std::string& prefs_error() const { return prefs_error_; }  // the file's, if broken
  static constexpr const char* kPreferences = "preferences";  // the view listing them
  static bool is_hub_view(std::string_view id) { return id == kProblems || id == kProperties || id == kPreferences; }
  // The hub's own views: those, and the services' (Search, Tasks, Outline).
  bool builtin_view(std::string_view id) const { return is_hub_view(id) || service_of_view(id); }

  // For services (service.hpp): their view's tree and title again; a view opened and
  // shown; a place shown in the editor; a message in the status bar.
  void service_changed(const std::string& view);
  void show_view(const std::string& id);
  void show_location(const std::string& path, int line) { listener_.open_location(path, line); }
  void notify_status(const std::string& text, std::string_view style, int seconds) { status(text, style, seconds); }

  // Consoles (console.hpp): a program's output in a view of its own. Returns the view
  // id (shown), or empty with the reason in the status bar.
  std::string run_console(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                          const std::string& name = "");
  Consoles& consoles() { return consoles_; }

 private:
  // One plugin host process. hosts_[0] serves every plugin that does not name its
  // own Python; it delegates the others to further hosts, one per interpreter.
  struct Slot {
    std::unique_ptr<Host> host;
    int gen = 0;
    bool ready = false;
    std::vector<std::string> plugins;  // the plugins it serves (known after initialize)
  };

  void start_slot(size_t i);
  void host_gone(size_t i);
  Host* host_for_plugin(std::string_view plugin);
  Host* host_for_view(std::string_view view_id);
  void request_tree(const std::string& id);
  void register_contributions(size_t slot, const mp::Value& result);
  void on_notify(size_t slot, std::string_view method, const mp::Value& p);
  mp::Value on_request(size_t slot, std::string_view method, const mp::Value& p);
  void status(const std::string& text, std::string_view style, int seconds = 5) { listener_.status(text, style, seconds); }
  void clear_markers_of(size_t slot);
  mp::Value problems_tree();  // also remembers its rows, for events
  void problems_event(std::string_view widget, std::string_view kind, const mp::Value& value);
  mp::Value properties_tree();
  void properties_event(std::string_view widget, std::string_view kind, const mp::Value& value);
  void send_selection(Host& h);
  // preferences
  mp::Value pref_of(const PrefInfo& p) const;
  void prefs_moved(const std::map<std::string, mp::Value>& before);  // tell who cares
  std::map<std::string, mp::Value> pref_values() const;
  void send_prefs(Host& h, const std::vector<std::string>* only = nullptr);
  mp::Value preferences_tree();
  void preferences_event(std::string_view widget, std::string_view kind, const mp::Value& value);
  void refresh_preferences();
  // perspectives
  void load_state();
  Placement defined(const std::string& perspective) const;
  Arrangement current();
  void apply(const Arrangement& a);
  bool can_open(const std::string& id) const;  // its plugin's host is up (or the hub's own)
  void open_at(const std::string& id, Region region, int order);  // in tab order

  Service* service_of_view(std::string_view id) const;
  Service* service_of_command(std::string_view id) const;

  HostOptions options_;
  Listener& listener_;
  std::vector<std::unique_ptr<Service>> services_;
  Consoles consoles_;
  void console_changed(const std::string& id);
  bool console_command(const std::string& id, const mp::Map& params);
  std::vector<Slot> hosts_;
  std::map<std::string, size_t, std::less<>> plugin_host_;  // plugin id → hosts_ index
  bool restarting_ = false;
  int next_gen_ = 0;
  std::vector<CommandInfo> commands_;  // plugin commands
  std::vector<ViewInfo> view_infos_;
  std::vector<ViewData> views_;
  std::map<std::string, JobInfo> jobs_;
  std::map<std::string, std::vector<Marker>> markers_;  // by set
  std::map<std::string, size_t> marker_slot_;           // which host reported a set
  std::vector<Marker> shown_;                           // the Problems view's rows
  Selection selection_;
  std::vector<PerspectiveInfo> perspectives_;
  std::string active_ = kDefault;
  std::map<std::string, Arrangement> saved_;  // by perspective
  Sizes sizes_;
  Placement pending_;           // to open as their hosts come up
  bool from_saved_ = false;     // this session opens the saved arrangement, not the startup views
  Arrangement want_;            // what the startup arrangement shows
  bool quiet_ = false;          // applying an arrangement: not a change of it
  std::vector<PrefInfo> front_prefs_, prefs_;  // the front-end's; with the plugins'
  PrefsFile prefs_file_;
  std::string prefs_error_;
  std::string pref_cursor_;              // the Preferences view's row
  std::string pref_edit_, pref_problem_;  // being edited in its dialog; what was wrong
};

// The workbench's own settings: the theme; with `terminal`, the editor mode and the mouse.
void declare_workbench_prefs(Hub& hub, bool terminal);

}  // namespace rcp
