#include "hub.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <tuple>

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

std::string describe(const Value& v) {
  if (v.is_str()) return std::string(v.str());
  if (v.is_array() && v.at(1).is_str()) return std::string(v.at(1).str());  // [code, message]
  return "error";
}

PerspectiveInfo perspective_from(const Value& p, const std::string& plugin) {
  PerspectiveInfo pi{std::string(p["id"].str()), std::string(p["title"].str()), plugin, {}, {}};
  for (const auto& v : p["views"].array()) pi.views.emplace_back(v.at(0).str(), region_from_name(v.at(1).str()));
  pi.sizes = clamp_sizes({int(p["side_size"].integer(Sizes{}.side)), int(p["bottom_size"].integer(Sizes{}.bottom))});
  return pi;
}

Value error_tree(std::string_view msg) {
  return Map{{"t", "text"}, {"text", msg}, {"style", "error"}, {"wrap", true}};
}

}  // namespace

std::string base_view(std::string_view id) { return std::string(id.substr(0, id.find('#'))); }

Hub::Hub(HostOptions options, Listener& listener) : options_(std::move(options)), listener_(listener) {
  services_.push_back(make_search_service(*this));
  services_.push_back(make_outline_service(*this));
}

bool Service::owns_view(std::string_view id) const {
  const std::string base = base_view(id);
  for (const auto& v : views())
    if (v.id == base) return true;
  return false;
}

bool Service::owns_command(std::string_view id) const {
  for (const auto& c : commands())
    if (c.id == id) return true;
  return false;
}

Service* Hub::service_of_view(std::string_view id) const {
  for (const auto& s : services_)
    if (s->owns_view(id)) return s.get();
  return nullptr;
}

Service* Hub::service_of_command(std::string_view id) const {
  for (const auto& s : services_)
    if (s->owns_command(id)) return s.get();
  return nullptr;
}

void Hub::service_changed(const std::string& view) {
  ViewData* v = find_view(view);
  Service* s = service_of_view(view);
  if (!v || !s) return;
  v->tree = s->tree(view);
  v->title = s->title(view);
  listener_.changed();
}

void Hub::show_view(const std::string& id) {
  open_view(id);
  listener_.view_shown(id);
}

Hub::~Hub() { stop(); }

// ---- hosts ------------------------------------------------------------------------

void Hub::start() {
  hosts_.clear();
  plugin_host_.clear();
  commands_.clear();
  commands_.push_back({"console.stop", "Console: Stop (again: harder)", {"ctrl+c"}, "console", "hub"});
  commands_.push_back({"console.rerun", "Console: Run again", {"r", "f5"}, "console", "hub"});
  commands_.push_back({"console.new", "Console: Run a command…", {}, "", "hub"});
  view_infos_.clear();
  view_infos_.push_back({kProblems, "Problems", "", false, Region::bottom});
  view_infos_.push_back({kProperties, "Properties", "", false, Region::side});
  view_infos_.push_back({kPreferences, "Preferences", "", false, Region::main});
  prefs_ = front_prefs_;  // the plugins' come with their manifests
  for (const auto& svc : services_) {
    for (auto c : svc->commands()) c.plugin = "hub", commands_.push_back(std::move(c));
    for (auto v : svc->views()) view_infos_.push_back(std::move(v));
    for (auto p : svc->prefs())
      if (!find_pref(p.id)) prefs_.push_back(std::move(p));
  }
  reload_prefs();
  perspectives_.clear();
  perspectives_.push_back({kDefault, "Default", "", {}, {}});
  if (!restarting_) {
    load_state();
    // The hub's own views of the saved arrangement need no host.
    quiet_ = true;
    for (size_t k = 0; k < pending_.size(); ++k) {
      auto& [id, region] = pending_[k];
      if (builtin_view(id)) open_at(id, region, int(k)), id.clear();  // done: keeps the indices
    }
    quiet_ = false;
  }
  jobs_.clear();
  if (!markers_.empty()) {
    markers_.clear();
    marker_slot_.clear();
    listener_.markers_changed();
  }
  hosts_.push_back(Slot{std::make_unique<Host>(options_), 0, false, {}});
  start_slot(0);
}

void Hub::stop() {
  for (auto& s : hosts_) s.host->stop();
}

void Hub::restart() {
  stop();
  restarting_ = true;
  status("Restarting plugin hosts…", "normal");
  start();
}

void Hub::add_fds(std::vector<pollfd>& fds, std::vector<PollToken>& tokens) const {
  for (size_t i = 0; i < hosts_.size(); ++i) {
    if (!hosts_[i].host->running()) continue;
    fds.push_back({hosts_[i].host->fd(), POLLIN, 0});
    tokens.push_back({i, hosts_[i].gen});
  }
  std::vector<std::string> ids;
  const size_t at = fds.size();
  consoles_.add_fds(fds, ids);
  for (size_t k = 0; k < ids.size(); ++k) tokens.push_back({SIZE_MAX, 0, ids[k], fds[at + k].fd});
  for (const auto& svc : services_) {
    const size_t from = fds.size();
    svc->add_fds(fds);
    for (size_t k = from; k < fds.size(); ++k) tokens.push_back({SIZE_MAX, 0, {}, fds[k].fd, svc.get()});
  }
}

void Hub::on_ready(const PollToken& t) {
  if (t.service) {
    for (const auto& svc : services_)
      if (svc.get() == t.service) svc->on_readable(t.fd);
    return;
  }
  if (!t.console.empty()) {
    if (consoles_.on_readable(t.console, t.fd)) console_changed(t.console);
    return;
  }
  // The host may have been replaced since the fds were collected.
  if (t.slot >= hosts_.size() || hosts_[t.slot].gen != t.gen || !hosts_[t.slot].host->running()) return;
  if (!hosts_[t.slot].host->rpc().on_readable()) host_gone(t.slot);
}

void Hub::start_slot(size_t i) {
  std::string err;
  hosts_[i].ready = false;
  hosts_[i].plugins.clear();
  if (!hosts_[i].host->start(err)) {
    status(err, "error", 30);
    return;
  }
  const int gen = ++next_gen_;
  hosts_[i].gen = gen;
  Rpc& rpc = hosts_[i].host->rpc();
  rpc.on_notify = [this, i](std::string_view m, const Value& p) { on_notify(i, m, p); };
  rpc.on_request = [this, i](std::string_view m, const Value& p) { return on_request(i, m, p); };
  rpc.on_request_later = [this, i, gen](std::string_view m, const Value& p, const Value& msgid) {
    if (m != "editor.open" && m != "compare.open") return false;
    // Answered when the editor is done, if the host that asked is still the same one.
    auto done = [this, i, gen, msgid](bool waited) {
      if (i < hosts_.size() && hosts_[i].gen == gen && hosts_[i].host->running())
        hosts_[i].host->rpc().respond(msgid, Value(), Map{{"waited", waited}});
    };
    if (m == "compare.open")
      listener_.open_compare(std::string(p["left"].str()), std::string(p["right"].str()),
                             std::string(p["title"].str()), done);
    else
      listener_.open_editor_later(std::string(p["path"].str()), int(p["line"].integer()), done);
    return true;
  };
  rpc.request("initialize", Map{{"protocol", 1}}, [this, i, gen](const Value& err, const Value& res) {
    if (i >= hosts_.size() || hosts_[i].gen != gen) return;
    if (!err.is_nil()) {
      status("Plugin host failed to start: " + describe(err), "error", 30);
      return;
    }
    hosts_[i].ready = true;
    register_contributions(i, res);
    send_prefs(*hosts_[i].host);  // before any of its views opens
    if (!selection_.items.array().empty()) send_selection(*hosts_[i].host);
    // Plugins that want another interpreter get a host of their own.
    for (const auto& d : res["delegates"].array()) {
      HostOptions o = options_;
      o.python = std::string(d["python"].str());
      for (const auto& p : d["plugins"].array()) o.only += (o.only.empty() ? "" : ",") + std::string(p.str());
      hosts_.push_back(Slot{std::make_unique<Host>(o), 0, false, {}});
      start_slot(hosts_.size() - 1);
    }
    const auto served = hosts_[i].plugins;
    auto mine = [&](std::string_view view_id) {
      return std::find(served.begin(), served.end(), plugin_of_view(view_id)) != served.end();
    };
    for (const auto& v : views_) {
      if (!v.native && !v.hub_owned && mine(v.id)) request_tree(v.id);  // after a restart
    }
    if (!restarting_) {
      // Hosts answer in any order; the tabs keep the arrangement's (or the manifests').
      quiet_ = true;
      if (from_saved_) {
        for (size_t k = 0; k < pending_.size(); ++k) {
          auto& [id, region] = pending_[k];
          if (!id.empty() && mine(id)) open_at(id, region, int(k)), id.clear();  // done: keeps the indices
        }
        // Views the perspective has gained since it was saved (a new plugin's, say).
        const auto& known = want_.known;
        int n = 0;
        for (const auto& [id, region] : defined(active_)) {
          if (mine(id) && !find_view(id) && std::find(known.begin(), known.end(), id) == known.end())
            open_at(id, region, 1'000'000 + n++);
        }
      } else {
        int n = 0;
        for (const auto& vi : std::vector<ViewInfo>(view_infos_)) {
          if (vi.startup && mine(vi.id) && !find_view(vi.id)) open_at(vi.id, vi.region, int(i) * 10'000 + n++);
        }
      }
      quiet_ = false;
      if (std::all_of(hosts_.begin(), hosts_.end(), [](const Slot& s) { return s.ready; }))
        pending_.clear();  // views of plugins no host has
      // Show what the arrangement shows as those views open, each once: what the user
      // did meanwhile stays.
      Arrangement now = want_;
      for (size_t r = 0; r < now.shown.size(); ++r) {
        if (find_view(want_.shown[r])) want_.shown[r].clear();
        else now.shown[r].clear();
      }
      if (find_view(want_.focus)) want_.focus.clear();
      else now.focus.clear();
      listener_.arranged(now);
    }
    listener_.changed();
  });
}

void Hub::host_gone(size_t i) {
  hosts_[i].host->exited();
  hosts_[i].ready = false;
  clear_markers_of(i);
  std::erase_if(jobs_, [i](const auto& kv) { return kv.second.host == i; });
  for (auto& v : views_) {
    if (v.native || v.hub_owned) continue;  // no plugin's
    const auto it = plugin_host_.find(plugin_of_view(v.id));
    if ((it == plugin_host_.end() ? 0 : it->second) != i) continue;
    v.tree = error_tree("The plugin host exited. Restart the hosts; the log is at " + options_.log_path);
    v.loading = false;
  }
  status("A plugin host exited (Ctrl+R restarts it)", "error", 30);
  listener_.changed();
}

Host* Hub::host_for_plugin(std::string_view plugin) {
  const auto it = plugin_host_.find(plugin);
  const size_t i = it == plugin_host_.end() ? 0 : it->second;
  if (i >= hosts_.size() || !hosts_[i].host->running()) return nullptr;
  return hosts_[i].host.get();
}

std::string Hub::plugin_of_view(std::string_view view_id) const {
  const std::string base = base_view(view_id);
  for (const auto& vi : view_infos_) {
    if (vi.id == base) return vi.plugin;
  }
  return "";
}

Host* Hub::host_for_view(std::string_view view_id) { return host_for_plugin(plugin_of_view(view_id)); }

void Hub::register_contributions(size_t slot, const Value& res) {
  for (const auto& pl : res["plugins"].array()) {
    const std::string pid(pl["id"].str());
    plugin_host_[pid] = slot;
    hosts_[slot].plugins.push_back(pid);
    std::erase_if(commands_, [&](const CommandInfo& c) { return c.plugin == pid; });
    std::erase_if(view_infos_, [&](const ViewInfo& v) { return v.plugin == pid; });
    std::erase_if(perspectives_, [&](const PerspectiveInfo& p) { return p.plugin == pid; });
    for (const auto& v : pl["views"].array())
      view_infos_.push_back({std::string(v["id"].str()), std::string(v["title"].str()), pid, v["startup"].boolean(),
                             region_from_name(v["region"].str())});
    for (const auto& c : pl["commands"].array()) {
      CommandInfo ci{std::string(c["id"].str()), std::string(c["title"].str()), {}, std::string(c["view"].str()), pid};
      for (const auto& key : c["keys"].array()) ci.keys.emplace_back(key.str());
      commands_.push_back(std::move(ci));
    }
    std::erase_if(prefs_, [&](const PrefInfo& p) { return p.plugin == pid; });
    for (const auto& p : pl["preferences"].array()) {
      PrefInfo pi{std::string(p["id"].str()), std::string(p["title"].str()), std::string(p["description"].str()), pid,
                  std::string(p["type"].str("string")), p["default"], {}, std::string(p["env"].str())};
      for (const auto& c : p["choices"].array()) pi.choices.emplace_back(c.str());
      if (!pi.id.empty() && !find_pref(pi.id)) prefs_.push_back(std::move(pi));
    }
    for (const auto& p : pl["perspectives"].array()) {
      PerspectiveInfo pi = perspective_from(p, pid);
      if (!pi.id.empty() && !find_perspective(pi.id)) perspectives_.push_back(std::move(pi));
    }
  }
  for (const auto& p : res["perspectives"].array()) {  // the user's own (perspectives.toml)
    PerspectiveInfo pi = perspective_from(p, "");
    std::erase_if(perspectives_, [&](const PerspectiveInfo& q) { return q.id == pi.id; });  // the user's wins
    if (!pi.id.empty() && pi.id != kDefault) perspectives_.push_back(std::move(pi));
  }
  refresh_preferences();
  const auto& errors = res["errors"].array();
  if (!errors.empty()) {
    std::string msg(errors.front().str());
    if (errors.size() > 1) msg += " (+" + std::to_string(errors.size() - 1) + " more in the log)";
    status(msg, "error", 15);
  }
}

// ---- views --------------------------------------------------------------------------

ViewData* Hub::find_view(std::string_view id) {
  for (auto& v : views_) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

void Hub::open_view(const std::string& id) {
  if (find_view(id)) return;
  if (id == kProblems) {
    views_.push_back(ViewData{id, "Problems", problems_tree(), false, false, true, -1, Region::bottom});
    listener_.changed();
    save_state();
    return;
  }
  if (id == kProperties) {
    views_.push_back(ViewData{id, "Properties", properties_tree(), false, false, true, -1, Region::side});
    listener_.changed();
    save_state();
    return;
  }
  if (id == kPreferences) {
    views_.push_back(ViewData{id, "Preferences", preferences_tree(), false, false, true, -1, Region::main});
    listener_.changed();
    save_state();
    return;
  }
  if (Service* svc = service_of_view(id)) {
    Region region = Region::main;
    for (const auto& vi : svc->views())
      if (vi.id == base_view(id)) region = vi.region;
    views_.push_back(ViewData{id, svc->title(id), Value(), false, false, true, -1, region});
    svc->opened(id);
    service_changed(id);
    save_state();
    return;
  }
  const std::string base = base_view(id);
  std::string title = base;
  Region region = Region::main;
  for (const auto& vi : view_infos_) {
    if (vi.id == base) title = vi.title, region = vi.region;
  }
  if (base.size() < id.size()) title += " " + id.substr(base.size() + 1);
  views_.push_back(ViewData{id, title, Value(), true, false, false, -1, region});
  request_tree(id);
  listener_.changed();
  save_state();
}

void Hub::request_tree(const std::string& id) {
  Host* h = host_for_view(id);
  if (!h) {
    if (auto v = find_view(id)) {
      v->tree = error_tree("The plugin host of " + id + " is not running. Restart the hosts.");
      v->loading = false;
    }
    return;
  }
  h->rpc().request("view.open", Map{{"view", id}}, [this, id](const Value& err, const Value& res) {
    ViewData* v = find_view(id);
    if (!v) return;
    v->tree = err.is_nil() ? res["tree"] : error_tree("Cannot open " + id + ": " + describe(err));
    if (res["title"].is_str()) v->title = std::string(res["title"].str());
    v->loading = false;
    listener_.changed();
  });
}

void Hub::open_native(const std::string& id, const std::string& title, Region region) {
  if (find_view(id)) return;
  views_.push_back(ViewData{id, title, Value(), false, true, false, -1, region});
  listener_.changed();
}

void Hub::set_region(const std::string& id, Region region) {
  if (ViewData* v = find_view(id); v && v->region != region) {
    v->region = region;
    listener_.changed();
    if (v->kept()) save_state();
  }
}

Region region_from_name(std::string_view name) {
  if (name == "side") return Region::side;
  if (name == "bottom") return Region::bottom;
  return Region::main;
}

const char* region_name(Region r) {
  switch (r) {
    case Region::side: return "side";
    case Region::bottom: return "bottom";
    default: return "main";
  }
}

void Hub::set_title(const std::string& id, const std::string& title) {
  if (ViewData* v = find_view(id); v && v->title != title) {
    v->title = title;
    listener_.changed();
  }
}

void Hub::close_view(const std::string& id) {
  const auto it = std::find_if(views_.begin(), views_.end(), [&](const ViewData& v) { return v.id == id; });
  if (it == views_.end()) return;
  if (Host* h = it->native || it->hub_owned ? nullptr : host_for_view(id))
    h->rpc().notify("view.close", Map{{"view", id}});
  const bool kept = it->kept();
  if (id.starts_with("console#")) consoles_.close(id);
  views_.erase(it);
  if (Service* svc = service_of_view(id)) svc->closed(id);
  listener_.changed();
  if (kept) save_state();
}

// ---- consoles ---------------------------------------------------------------------------

std::string Hub::run_console(const std::vector<std::string>& argv, const std::string& cwd, const std::string& title,
                             const std::string& name) {
  std::string error;
  const std::string id = consoles_.run(argv, cwd, title, name, error);
  if (id.empty()) {
    status("Console: " + error, "error", 10);
    return "";
  }
  const Console* c = consoles_.find(id);
  if (ViewData* v = find_view(id)) {
    v->title = c->title;
  } else {
    views_.push_back(ViewData{id, c->title, Value(), false, false, true, -1, Region::bottom});
  }
  console_changed(id);
  listener_.view_shown(id);
  return id;
}

void Hub::console_changed(const std::string& id) {
  ViewData* v = find_view(id);
  const Console* c = consoles_.find(id);
  if (!v || !c) return;
  v->tree = consoles_.tree(*c);
  v->title = (c->running ? "⟳ " : c->status == 0 ? "" : "✘ ") + c->title;
  listener_.changed();
}

bool Hub::console_command(const std::string& id, const Map& params) {
  std::string view;
  for (const auto& [k, v] : params) {
    if (k.is_str() && k.str() == "view") view = std::string(v.str());
  }
  if (id == "console.new") return !run_console({}, "", "Console").empty();
  if (!view.starts_with("console#")) return false;
  if (id == "console.stop") {
    consoles_.stop(view);
  } else if (id == "console.rerun") {
    std::string error;
    if (!consoles_.rerun(view, error) && !error.empty()) status("Console: " + error, "error", 5);
  } else {
    return false;
  }
  console_changed(view);
  return true;
}

void Hub::send_event(const std::string& view, std::string_view widget, std::string_view kind, Value value) {
  if (view.starts_with("console#")) {
    if (consoles_.event(view, widget, kind, value,
                        [this](const std::string& path, int line) { listener_.open_location(path, line); }))
      console_changed(view);
    return;
  }
  if (view == kProblems) return problems_event(widget, kind, value);
  if (view == kProperties) return properties_event(widget, kind, value);
  if (view == kPreferences) return preferences_event(widget, kind, value);
  if (Service* svc = service_of_view(view)) return svc->event(view, widget, kind, value);
  if (Host* h = host_for_view(view))
    h->rpc().notify("view.event", Map{{"view", view}, {"widget", widget}, {"kind", kind}, {"value", std::move(value)}});
}

std::string Hub::run_command(const std::string& id, Map params) {
  const auto it = std::find_if(commands_.begin(), commands_.end(), [&](const CommandInfo& c) { return c.id == id; });
  if (it == commands_.end()) return "Unknown command " + id;
  if (it->plugin == "hub") {
    if (Service* svc = service_of_command(id)) {
      std::string view;
      for (const auto& [k, v] : params)
        if (k.is_str() && k.str() == "view") view = std::string(v.str());
      return svc->command(id, view) ? "" : "Not available here";
    }
    return console_command(id, params) ? "" : "Not available here";
  }
  Host* h = host_for_plugin(it->plugin);
  if (!h) return "The plugin host is not running (restart the hosts)";
  params.insert(params.begin(), {Value("command"), Value(id)});
  h->rpc().notify("command.run", std::move(params));
  return "";
}

void Hub::cancel_jobs() {
  for (const auto& [job, info] : jobs_) {
    if (info.host < hosts_.size() && hosts_[info.host].host->running())
      hosts_[info.host].host->rpc().notify("job.cancel", Map{{"job", job}});
  }
}

// ---- from the hosts -----------------------------------------------------------------

// ---- problem markers ----------------------------------------------------------------

std::vector<Marker> Hub::markers() const {
  std::vector<Marker> all;
  for (const auto& [set, ms] : markers_) all.insert(all.end(), ms.begin(), ms.end());
  return all;
}

std::vector<Marker> Hub::markers_without(std::string_view set_prefix) const {
  std::vector<Marker> out;
  for (const auto& [set, ms] : markers_) {
    if (!set.starts_with(set_prefix)) out.insert(out.end(), ms.begin(), ms.end());
  }
  return out;
}

void Hub::set_markers(const std::string& set, std::vector<Marker> markers) {
  const auto it = markers_.find(set);
  if (it == markers_.end() ? markers.empty() : it->second == markers) return;  // nothing new
  if (markers.empty()) markers_.erase(set);
  else markers_[set] = std::move(markers);
  if (ViewData* v = find_view(kProblems)) v->tree = problems_tree();
  listener_.markers_changed();
  listener_.changed();
}

void Hub::clear_markers_of(size_t slot) {
  bool any = false;
  for (auto it = marker_slot_.begin(); it != marker_slot_.end();) {
    if (it->second == slot) {
      any |= markers_.erase(it->first) > 0;
      it = marker_slot_.erase(it);
    } else {
      ++it;
    }
  }
  if (!any) return;
  if (ViewData* v = find_view(kProblems)) v->tree = problems_tree();
  listener_.markers_changed();
}

Value Hub::problems_tree() {
  // Errors first, then warnings, then the rest; by place within each.
  auto rank = [](const std::string& s) { return s == "error" ? 0 : s == "warning" ? 1 : 2; };
  auto& shown = shown_;
  shown = markers();
  std::stable_sort(shown.begin(), shown.end(), [&](const Marker& a, const Marker& b) {
    return std::tuple(rank(a.severity), a.path, a.line) < std::tuple(rank(b.severity), b.path, b.line);
  });
  int errors = 0, warnings = 0;
  mp::Array rows;
  for (size_t i = 0; i < shown.size(); ++i) {
    const Marker& m = shown[i];
    errors += m.severity == "error";
    warnings += m.severity == "warning";
    // Under the working directory a path is shown relative to it: the line stays in view.
    std::string where = m.path;
    std::error_code ec;
    if (const auto cwd = std::filesystem::current_path(ec); !ec && !m.path.empty()) {
      const auto rel = std::filesystem::path(m.path).lexically_relative(cwd);
      if (!rel.empty() && !rel.string().starts_with("..")) where = rel.string();
    }
    if (m.line > 0) where += ":" + std::to_string(m.line);
    if (where.empty()) where = m.view;
    const std::string sev = m.severity == "error" ? "error" : m.severity == "warning" ? "warning" : "info";
    mp::Array props{mp::Array{"Severity", sev}, mp::Array{"Message", m.message}};
    if (!m.source.empty()) props.push_back(mp::Array{"From", m.source});
    if (!m.view.empty()) props.push_back(mp::Array{"View", m.view});
    rows.push_back(Map{{"key", std::to_string(i)},
                       {"cells", mp::Array{Map{{"text", sev}, {"style", sev}}, where, m.message,
                                           Map{{"text", m.source}, {"style", "dim"}}}},
                       {"sel", mp::Array{Map{{"type", "problem"},
                                             {"label", m.message},
                                             {"path", m.path},
                                             {"line", m.line},
                                             {"props", props}}}}});
  }
  const std::string summary = std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings, " +
                              std::to_string(shown.size() - size_t(errors + warnings)) + " other";
  return Map{{"t", "vbox"},
             {"children",
              mp::Array{Map{{"t", "text"}, {"text", summary}, {"style", "dim"}},
                        Map{{"t", "table"},
                            {"id", "problems"},
                            {"autofocus", true},
                            {"empty", "No problems reported."},
                            {"columns", mp::Array{Map{{"title", ""}, {"width", 7}}, Map{{"title", "Where"}, {"grow", 2}},
                                                  Map{{"title", "Problem"}, {"grow", 3}}, Map{{"title", "From"}, {"width", 10}}}},
                            {"rows", rows}},
                        Map{{"t", "text"}, {"text", "Enter opens the place (the editor, or the view that shows it)"}, {"style", "dim"}}}}};
}

void Hub::problems_event(std::string_view widget, std::string_view kind, const Value& value) {
  if (widget != "problems" || kind != "activate") return;
  const size_t i = size_t(std::atoi(std::string(value.str()).c_str()));
  if (i >= shown_.size()) return;
  const Marker m = shown_[i];
  if (!m.view.empty()) {
    open_view(m.view);
    listener_.view_shown(m.view);
  }
  if (!m.path.empty()) listener_.open_location(m.path, m.line);
}

// ---- the selection ------------------------------------------------------------------

void Hub::set_selection(const std::string& view, Value items) {
  if (!items.is_array()) items = mp::Array{};
  if (view == selection_.view && items == selection_.items) return;
  selection_ = {view, std::move(items)};
  for (auto& slot : hosts_) {
    if (slot.ready && slot.host->running()) send_selection(*slot.host);
  }
  if (ViewData* v = find_view(kProperties)) v->tree = properties_tree();
  for (const auto& svc : services_) svc->selection_changed(selection_.view, selection_.items);
  listener_.changed();
}

void Hub::send_selection(Host& h) {
  const ViewData* v = find_view(selection_.view);
  h.rpc().notify("selection.changed", Map{{"view", selection_.view},
                                          {"title", v ? v->title : selection_.view},
                                          {"items", selection_.items}});
}

namespace {

// Up to `count` lines of a text file around `line` (1-based), as [first line, lines];
// nothing for a binary file, a big one, or one that cannot be read.
std::pair<int, std::vector<std::string>> source_around(const std::string& path, int line, int count) {
  std::error_code ec;
  if (path.empty() || !std::filesystem::is_regular_file(path, ec) || std::filesystem::file_size(path, ec) > (8u << 20))
    return {0, {}};
  std::ifstream in(path, std::ios::binary);
  const int first = std::max(1, line - count / 3);
  std::vector<std::string> out;
  std::string s;
  for (int n = 1; std::getline(in, s) && int(out.size()) < count; ++n) {
    if (s.find('\0') != std::string::npos) return {0, {}};
    if (!s.empty() && s.back() == '\r') s.pop_back();
    if (n >= first) out.push_back(s);
  }
  return {first, out};
}

std::string where_of(const Value& item) {
  std::string w(item["path"].str());
  std::error_code ec;
  if (const auto cwd = std::filesystem::current_path(ec); !ec && !w.empty()) {
    const auto rel = std::filesystem::path(w).lexically_relative(cwd);
    if (!rel.empty() && !rel.string().starts_with("..")) w = rel.string();
  }
  if (!w.empty() && item["line"].integer() > 0) w += ":" + std::to_string(item["line"].integer());
  return w;
}

Value text_node(Value text, std::string_view style = "") {
  Map n{{"t", "text"}, {"text", std::move(text)}};
  if (!style.empty()) n.emplace_back("style", style);
  return n;
}

}  // namespace

Value Hub::properties_tree() {
  const auto& items = selection_.items.array();
  if (items.empty()) {
    return Map{{"t", "vbox"},
               {"children", mp::Array{text_node("Nothing selected.", "dim"),
                                      Map{{"t", "text"},
                                          {"wrap", true},
                                          {"style", "dim"},
                                          {"text", "Move the cursor in a view whose rows describe what they are "
                                                   "(Files, Problems, Search, Outline): this shows it, and "
                                                   "the source around its line."}}}}};
  }
  const ViewData* from = find_view(selection_.view);
  mp::Array kids;
  kids.push_back(text_node(mp::Array{"From ", mp::Array{from ? from->title : selection_.view, "accent"}}, "dim"));
  // One item: its properties. More: a row each.
  mp::Array rows;
  if (items.size() == 1) {
    const Value& it = items.front();
    kids.push_back(text_node(mp::Array{mp::Array{std::string(it["label"].str()), "title"},
                                       mp::Array{"  " + std::string(it["type"].str()), "dim"}}));
    if (const std::string w = where_of(it); !w.empty()) rows.push_back(Map{{"key", "where"}, {"cells", mp::Array{"Where", w}}});
    size_t i = 0;
    for (const auto& kv : it["props"].array()) {
      rows.push_back(Map{{"key", "p" + std::to_string(i++)},
                         {"cells", mp::Array{std::string(kv.at(0).str()), std::string(kv.at(1).str())}}});
    }
    kids.push_back(Map{{"t", "table"},
                       {"id", "props"},
                       {"columns", mp::Array{Map{{"title", "Property"}, {"width", 14}}, Map{{"title", "Value"}, {"grow", 1}}}},
                       {"rows", rows},
                       {"height", int(rows.size()) + 1}});
  } else {
    size_t i = 0;
    for (const auto& it : items) {
      rows.push_back(Map{{"key", std::to_string(i++)},
                         {"cells", mp::Array{std::string(it["label"].str()), Map{{"text", std::string(it["type"].str())}, {"style", "dim"}},
                                             where_of(it)}}});
    }
    kids.push_back(text_node(std::to_string(items.size()) + " items"));
    kids.push_back(Map{{"t", "table"},
                       {"id", "items"},
                       {"columns", mp::Array{Map{{"title", "Item"}, {"grow", 2}}, Map{{"title", "Type"}, {"width", 12}},
                                             Map{{"title", "Where"}, {"grow", 2}}}},
                       {"rows", rows},
                       {"height", int(std::min<size_t>(rows.size(), 8)) + 1}});
  }
  // The source around the first item that has a place.
  for (const auto& it : items) {
    const std::string path(it["path"].str());
    if (path.empty()) continue;
    const int line = int(it["line"].integer());
    const auto [first, lines] = source_around(path, line, 40);
    if (lines.empty()) break;
    const int width = int(std::to_string(first + int(lines.size())).size());
    mp::Array src;
    for (size_t k = 0; k < lines.size(); ++k) {
      const int n = first + int(k);
      std::string num = std::to_string(n);
      num.insert(0, size_t(width) - num.size(), ' ');
      src.push_back(Map{{"key", std::to_string(n)},
                        {"text", num + " " + lines[k]},
                        {"style", n == line ? "accent" : "normal"}});
    }
    Map list{{"t", "list"}, {"id", "source"}, {"items", src}, {"grow", 1}, {"autofocus", true}};
    if (line > 0) list.emplace_back("selected", std::to_string(line));
    kids.push_back(Map{{"t", "panel"}, {"title", std::filesystem::path(path).filename().string()}, {"grow", 1}, {"child", list}});
    kids.push_back(text_node("Enter opens the line in the editor", "dim"));
    break;
  }
  return Map{{"t", "vbox"}, {"children", kids}};
}

void Hub::properties_event(std::string_view widget, std::string_view kind, const Value& value) {
  const auto& items = selection_.items.array();
  if (kind != "activate" || items.empty()) return;
  if (widget == "source") {
    for (const auto& it : items) {
      if (it["path"].str().empty()) continue;
      return listener_.open_location(std::string(it["path"].str()), std::atoi(std::string(value.str()).c_str()));
    }
  } else if (widget == "items" || widget == "props") {
    const size_t i = widget == "items" ? size_t(std::atoi(std::string(value.str()).c_str())) : 0;
    if (i < items.size() && !items[i]["path"].str().empty())
      listener_.open_location(std::string(items[i]["path"].str()), int(items[i]["line"].integer()));
  }
}

Value Hub::on_request(size_t, std::string_view method, const Value& p) {
  if (method == "editor.open") {
    const bool waited = listener_.open_editor(std::string(p["path"].str()), int(p["line"].integer()));
    return Map{{"waited", waited}};
  }
  throw std::runtime_error("unknown method " + std::string(method));
}

void Hub::on_notify(size_t slot, std::string_view method, const Value& p) {
  if (method == "view.update") {
    if (ViewData* v = find_view(p["view"].str())) {
      v->tree = p["tree"];
      if (p["title"].is_str()) v->title = std::string(p["title"].str());
      v->loading = false;
    }
  } else if (method == "view.show") {
    const std::string id(p["view"].str());
    open_view(id);
    listener_.view_shown(id);
  } else if (method == "view.close") {
    const std::string id(p["view"].str());
    const auto it = std::find_if(views_.begin(), views_.end(), [&](const ViewData& v) { return v.id == id; });
    if (it != views_.end()) {
      views_.erase(it);
      listener_.view_closed(id);
    }
  } else if (method == "status") {
    const auto style = p["style"].str();
    status(std::string(p["text"].str()), style, style == "error" ? 10 : 5);
  } else if (method == "editor.open") {
    listener_.open_editor_later(std::string(p["path"].str()), int(p["line"].integer()), [](bool) {});
  } else if (method == "compare.open") {
    listener_.open_compare(std::string(p["left"].str()), std::string(p["right"].str()), std::string(p["title"].str()),
                           [](bool) {});
  } else if (method == "console.run") {
    std::vector<std::string> argv;
    for (const auto& a : p["argv"].array()) argv.emplace_back(a.str());
    run_console(argv, std::string(p["cwd"].str()), std::string(p["title"].str()), std::string(p["name"].str()));
  } else if (method == "terminal.open") {
    std::vector<std::string> argv;
    for (const auto& a : p["argv"].array()) argv.emplace_back(a.str());
    if (!argv.empty()) listener_.open_terminal(argv, std::string(p["cwd"].str()), std::string(p["title"].str()));
  } else if (method == "markers.set") {
    std::vector<Marker> markers;
    for (const auto& m : p["markers"].array()) {
      markers.push_back({std::string(m["path"].str()), int(m["line"].integer()), int(m["col"].integer()),
                         std::string(m["severity"].str("error")), std::string(m["message"].str()),
                         std::string(m["source"].str()), std::string(m["view"].str())});
    }
    const std::string set(p["set"].str());
    marker_slot_[set] = slot;
    set_markers(set, std::move(markers));
  } else if (method == "job.begin") {
    jobs_[std::string(p["job"].str())] = JobInfo{std::string(p["title"].str()), "", 0, slot};
  } else if (method == "job.progress") {
    auto it = jobs_.find(std::string(p["job"].str()));
    if (it != jobs_.end()) {
      it->second.value = p["value"].number();
      it->second.label = std::string(p["label"].str());
    }
  } else if (method == "job.end") {
    auto it = jobs_.find(std::string(p["job"].str()));
    const std::string title = it != jobs_.end() ? it->second.title : "Job";
    if (it != jobs_.end()) jobs_.erase(it);
    const std::string text(p["text"].str());
    if (!p["ok"].boolean()) status(title + ": " + (text.empty() ? "failed" : text), "error", 10);
    else if (!text.empty()) status(title + ": " + text, "ok");
  }
  listener_.changed();
}

// ---- perspectives -------------------------------------------------------------------

Sizes clamp_sizes(Sizes s) { return {std::clamp(s.side, 15, 75), std::clamp(s.bottom, 15, 75)}; }

const PerspectiveInfo* Hub::find_perspective(std::string_view id) const {
  for (const auto& p : perspectives_) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

Placement Hub::defined(const std::string& perspective) const {
  Placement out;
  if (perspective == kDefault) {
    for (const auto& vi : view_infos_) {
      if (vi.startup) out.emplace_back(vi.id, vi.region);
    }
  } else if (const PerspectiveInfo* p = find_perspective(perspective)) {
    out = p->views;
  }
  return out;
}

Arrangement Hub::current() {
  Arrangement a;
  auto has = [&](const std::string& id) {
    return std::any_of(a.views.begin(), a.views.end(), [&](const auto& p) { return p.first == id; });
  };
  for (const auto& v : views_) {
    if (v.kept()) a.views.emplace_back(v.id, v.region);
  }
  for (const auto& p : pending_) {  // not open yet: their host is still starting
    if (!p.first.empty() && !has(p.first)) a.views.push_back(p);
  }
  a.sizes = sizes_;
  if (const auto it = saved_.find(active_); it != saved_.end()) {
    a.shown = it->second.shown;
    a.focus = it->second.focus;
    a.known = it->second.known;
  }
  for (const auto& [id, region] : defined(active_)) {
    if (std::find(a.known.begin(), a.known.end(), id) == a.known.end()) a.known.push_back(id);
  }
  listener_.front_state(a);
  return a;
}

bool Hub::can_open(const std::string& id) const {
  if (builtin_view(id)) return true;
  const std::string plugin = plugin_of_view(id);
  const auto it = plugin.empty() ? plugin_host_.end() : plugin_host_.find(plugin);
  return it != plugin_host_.end() && it->second < hosts_.size() && hosts_[it->second].ready;
}

void Hub::open_at(const std::string& id, Region region, int order) {
  if (find_view(id)) return;
  open_view(id);
  if (views_.empty() || views_.back().id != id) return;
  ViewData v = std::move(views_.back());
  views_.pop_back();
  v.region = region;
  v.order = order;
  const auto at = std::find_if(views_.begin(), views_.end(), [&](const ViewData& w) { return w.order < 0 || w.order > order; });
  views_.insert(at, std::move(v));
}

void Hub::apply(const Arrangement& a) {
  quiet_ = true;
  auto index_of = [&](std::string_view id) -> int {
    for (size_t k = 0; k < a.views.size(); ++k) {
      if (a.views[k].first == id) return int(k);
    }
    return -1;
  };
  std::vector<std::string> gone;
  for (const auto& v : views_) {
    if (v.kept() && index_of(v.id) < 0) gone.push_back(v.id);
  }
  for (const auto& id : gone) {
    close_view(id);
    listener_.view_closed(id);
  }
  pending_ = a.views;  // what cannot open yet stays, at its index (its tab order)
  for (size_t k = 0; k < a.views.size(); ++k) {
    const auto& [id, region] = a.views[k];
    if (!find_view(id) && can_open(id)) open_view(id);
    if (ViewData* v = find_view(id)) {
      v->region = region;
      v->order = int(k);
      pending_[k].first.clear();
    }
  }
  // The arrangement's order; terminal views keep theirs, after it.
  std::stable_sort(views_.begin(), views_.end(), [&](const ViewData& x, const ViewData& y) {
    const int i = !x.kept() ? INT32_MAX : index_of(x.id), j = !y.kept() ? INT32_MAX : index_of(y.id);
    return i < j;
  });
  sizes_ = clamp_sizes(a.sizes);
  want_ = a;
  from_saved_ = true;  // hosts still starting open the arrangement's views, not the startup ones
  quiet_ = false;
  listener_.changed();
  listener_.arranged(a);
}

void Hub::switch_perspective(const std::string& id) {
  if (id == active_ || (id != kDefault && !find_perspective(id))) return;
  saved_[active_] = current();
  active_ = id;
  if (const auto it = saved_.find(id); it != saved_.end()) {
    apply(it->second);
  } else {
    Arrangement a;
    a.views = defined(id);
    if (const PerspectiveInfo* p = find_perspective(id)) a.sizes = p->sizes;
    for (const auto& v : a.views) a.known.push_back(v.first);
    apply(a);
  }
  save_state();
}

void Hub::reset_perspective() {
  saved_.erase(active_);
  Arrangement a;
  a.views = defined(active_);
  if (const PerspectiveInfo* p = find_perspective(active_)) a.sizes = p->sizes;
  for (const auto& v : a.views) a.known.push_back(v.first);
  apply(a);
  save_state();
}

void Hub::set_sizes(Sizes s) {
  s = clamp_sizes(s);
  if (s == sizes_) return;
  sizes_ = s;
  listener_.changed();
  save_state();
}

namespace {

Value strings(const std::vector<std::string>& v) {
  Array out;
  for (const auto& s : v) out.emplace_back(s);
  return out;
}

}  // namespace

void Hub::save_state() {
  if (quiet_) return;
  saved_[active_] = current();
  if (options_.state_path.empty()) return;
  Map perspectives;
  for (const auto& [id, a] : saved_) {
    Array views;
    for (const auto& [view, region] : a.views) views.emplace_back(Array{view, region_name(region)});
    perspectives.emplace_back(id, Map{{"views", std::move(views)},
                                      {"side", a.sizes.side},
                                      {"bottom", a.sizes.bottom},
                                      {"shown", strings({a.shown.begin(), a.shown.end()})},
                                      {"focus", a.focus},
                                      {"known", strings(a.known)}});
  }
  const std::string data = mp::encode(Map{{"version", 1}, {"active", active_}, {"perspectives", std::move(perspectives)}});
  const std::string tmp = options_.state_path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, options_.state_path, ec);
}

void Hub::load_state() {
  saved_.clear();
  pending_.clear();
  want_ = {};
  sizes_ = {};
  from_saved_ = false;
  active_ = kDefault;
  if (options_.state_path.empty()) return;
  std::ifstream in(options_.state_path, std::ios::binary);
  const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  Value root;
  try {
    size_t pos = 0;
    auto v = mp::decode(data, pos);
    if (!v) return;
    root = std::move(*v);
  } catch (const mp::DecodeError&) {
    return;  // a damaged file: start as if there were none
  }
  if (root["version"].integer() != 1) return;
  for (const auto& [k, p] : root["perspectives"].map()) {
    Arrangement a;
    for (const auto& v : p["views"].array()) {
      if (v.at(0).is_str()) a.views.emplace_back(v.at(0).str(), region_from_name(v.at(1).str()));
    }
    a.sizes = clamp_sizes({int(p["side"].integer(Sizes{}.side)), int(p["bottom"].integer(Sizes{}.bottom))});
    for (size_t r = 0; r < a.shown.size(); ++r) a.shown[r] = std::string(p["shown"].at(r).str());
    a.focus = std::string(p["focus"].str());
    for (const auto& id : p["known"].array()) a.known.emplace_back(id.str());
    saved_[std::string(k.str())] = std::move(a);
  }
  active_ = std::string(root["active"].str(kDefault));
  auto it = saved_.find(active_);
  if (it == saved_.end()) {
    active_ = kDefault;
    it = saved_.find(active_);
  }
  if (it == saved_.end()) return;
  want_ = it->second;
  pending_ = want_.views;
  sizes_ = want_.sizes;
  from_saved_ = true;
}

}  // namespace rcp
