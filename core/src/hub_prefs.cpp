// The hub's preferences: declared settings, their values (environment, file, default),
// the hosts' copies, and the Preferences view with its edit dialog.
#include <algorithm>
#include <cstdlib>

#include "hub.hpp"
#include "rcp/grid.hpp"

namespace rcp {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

Value typed_default(const PrefInfo& p) {
  if (pref_fits(p, p.def)) return p.def;
  if (p.type == "bool") return false;
  if (p.type == "int") return 0;
  if (p.type == "paths") return Array{};
  if (p.type == "choice") return p.choices.empty() ? Value("") : Value(p.choices.front());
  return "";
}

const char* env_value(const PrefInfo& p) {
  const char* e = p.env.empty() ? nullptr : std::getenv(p.env.c_str());
  return e && *e ? e : nullptr;
}

std::string shown(const PrefInfo& p, const Value& v) {
  if (p.type == "bool") return v.boolean() ? "on" : "off";
  const std::string t = pref_to_text(v);
  return t.empty() ? "—" : t;
}

Value text(Value t, std::string_view style = "", bool wrap = false) {
  Map n{{"t", "text"}, {"text", std::move(t)}};
  if (!style.empty()) n.emplace_back("style", style);
  if (wrap) n.emplace_back("wrap", true);
  return n;
}

Value button(std::string_view id, std::string_view label) { return Map{{"t", "button"}, {"id", id}, {"label", label}}; }

}  // namespace

void Hub::declare_pref(PrefInfo p) {
  std::erase_if(front_prefs_, [&](const PrefInfo& q) { return q.id == p.id; });
  std::erase_if(prefs_, [&](const PrefInfo& q) { return q.id == p.id; });
  front_prefs_.push_back(p);
  prefs_.push_back(std::move(p));
}

const PrefInfo* Hub::find_pref(std::string_view id) const {
  for (const auto& p : prefs_) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

Value Hub::pref_of(const PrefInfo& p) const {
  if (const char* e = env_value(p)) {
    std::string err;
    if (Value v = pref_from_text(p, e, err); !v.is_nil()) return v;
  }
  if (const Value& v = prefs_file_.get(p.id); !v.is_nil() && pref_fits(p, v)) return v;
  return typed_default(p);
}

Value Hub::pref(std::string_view id) const {
  const PrefInfo* p = find_pref(id);
  return p ? pref_of(*p) : Value();
}

std::string Hub::pref_source(const PrefInfo& p) const {
  if (const char* e = env_value(p)) {
    std::string err;
    if (!pref_from_text(p, e, err).is_nil()) return "$" + p.env;
  }
  if (const Value& v = prefs_file_.get(p.id); !v.is_nil() && pref_fits(p, v)) return "file";
  return "default";
}

std::map<std::string, Value> Hub::pref_values() const {
  std::map<std::string, Value> out;
  for (const auto& p : prefs_) out[p.id] = pref_of(p);
  return out;
}

void Hub::send_prefs(Host& h, const std::vector<std::string>* only) {
  Map values;
  for (const auto& p : prefs_) {
    if (!only || std::find(only->begin(), only->end(), p.id) != only->end()) values.emplace_back(p.id, pref_of(p));
  }
  if (!values.empty() && h.running()) h.rpc().notify("preferences.changed", Map{{"values", std::move(values)}});
}

void Hub::prefs_moved(const std::map<std::string, Value>& before) {
  std::vector<std::string> changed;
  for (const auto& [id, v] : pref_values()) {
    const auto it = before.find(id);
    if (it == before.end() || !(it->second == v)) changed.push_back(id);
  }
  refresh_preferences();
  if (changed.empty()) return;
  for (auto& slot : hosts_) {
    if (slot.ready) send_prefs(*slot.host, &changed);
  }
  for (const auto& svc : services_) svc->prefs_changed();
  listener_.prefs_changed(changed);
  listener_.changed();
}

void Hub::reload_prefs() {
  const auto before = pref_values();
  std::string err;
  if (options_.prefs_path.empty() || prefs_file_.load(options_.prefs_path, err)) {
    prefs_error_.clear();
  } else {
    prefs_error_ = err;
    status("Preferences: " + err, "error", 15);
  }
  prefs_moved(before);
}

std::string Hub::set_pref(const std::string& id, Value v) {
  const PrefInfo* p = find_pref(id);
  if (!p) return "No setting " + id;
  if (!prefs_error_.empty()) return "Fix the preferences file first: " + prefs_error_;
  if (!v.is_nil() && !pref_fits(*p, v)) return "Not a value for " + p->title;
  if (!v.is_nil() && v == typed_default(*p)) v = Value();  // the default needs no line
  const auto before = pref_values();
  prefs_file_.set(id, v);
  if (std::string err; !options_.prefs_path.empty() && !prefs_file_.save(options_.prefs_path, err)) return err;
  prefs_moved(before);
  return "";
}

void Hub::refresh_preferences() {
  if (ViewData* v = find_view(kPreferences)) v->tree = preferences_tree();
}

Value Hub::preferences_tree() {
  const PrefInfo* cur = find_pref(pref_cursor_);
  if (!cur && !prefs_.empty()) cur = &prefs_.front();
  Array rows;
  for (const auto& p : prefs_) {
    const Value v = pref_of(p);
    const std::string src = pref_source(p);
    const std::string def = shown(p, typed_default(p));
    Array props{Array{"Id", p.id}, Array{"Type", p.type}, Array{"Default", def}, Array{"From", src}};
    if (!p.env.empty()) props.push_back(Array{"Environment", "$" + p.env + (env_value(p) ? " (set)" : " (not set)")});
    if (!p.description.empty()) props.push_back(Array{"About", p.description});
    rows.push_back(Map{{"key", p.id},
                       {"cells", Array{Map{{"text", p.plugin.empty() ? "workbench" : p.plugin}, {"style", "dim"}}, p.title,
                                       Map{{"text", shown(p, v)}, {"style", src == "default" ? "normal" : "accent"}},
                                       Map{{"text", src}, {"style", src.starts_with("$") ? "warning" : "dim"}}}},
                       {"sel", Array{Map{{"type", "preference"}, {"label", p.title}, {"props", props}}}}});
  }
  Array kids;
  const std::string where = options_.prefs_path.empty() ? "(not kept)" : options_.prefs_path;
  kids.push_back(text(Array{"Kept in ", Array{where, "accent"}, "; an environment variable that is set wins."}, "dim"));
  if (!prefs_error_.empty()) kids.push_back(text("The file has an error, so nothing is written to it: " + prefs_error_, "error", true));
  Map table{{"t", "table"},
            {"id", "prefs"},
            {"autofocus", true},
            {"grow", 1},
            {"empty", "Nothing to set."},
            {"columns", Array{Map{{"title", "Of"}, {"width", 10}}, Map{{"title", "Setting"}, {"grow", 2}},
                              Map{{"title", "Value"}, {"grow", 3}}, Map{{"title", "From"}, {"width", 18}}}},
            {"rows", rows}};
  kids.push_back(table);
  if (cur) {
    Array about;
    if (!cur->description.empty()) about.push_back(text(cur->description, "", true));
    std::string facts = cur->id + " · " + cur->type + " · default " + shown(*cur, typed_default(*cur));
    if (!cur->env.empty()) facts += " · $" + cur->env;
    about.push_back(text(facts, "dim", true));
    kids.push_back(Map{{"t", "panel"}, {"title", cur->title}, {"child", Map{{"t", "vbox"}, {"children", about}}}});
  }
  kids.push_back(text(Array{Array{"Enter", "accent"}, " change   ", Array{"Preferences: Edit the file", "accent"},
                            " (Ctrl+P) to edit it by hand"},
                      "dim"));
  Value base = Map{{"t", "vbox"}, {"children", kids}};

  const PrefInfo* e = find_pref(pref_edit_);
  if (!e) return base;
  // The dialog: a list of the choices, else a field.
  Array dlg;
  if (!e->description.empty()) dlg.push_back(text(e->description, "", true));
  const Value v = pref_of(*e);
  if (e->type == "choice") {
    Array items;
    for (const auto& c : e->choices) items.push_back(Map{{"key", c}, {"text", c}});
    dlg.push_back(Map{{"t", "list"}, {"id", "choices"}, {"items", items}, {"selected", v.str()}, {"autofocus", true},
                      {"height", int(e->choices.size())}});
  } else {
    dlg.push_back(Map{{"t", "field"}, {"id", "value"}, {"value", pref_to_text(v)}, {"autofocus", true}});
    if (e->type == "paths") dlg.push_back(text("Paths separated by “:”; ~ is your home.", "dim"));
  }
  if (env_value(*e))
    dlg.push_back(text("$" + e->env + " is set, and wins over what you set here while it is.", "warning", true));
  if (!pref_problem_.empty()) dlg.push_back(text(pref_problem_, "error", true));
  Array buttons;
  if (e->type != "choice") buttons.push_back(button("ok", "OK"));
  buttons.push_back(button("default", "Default (" + shown(*e, typed_default(*e)) + ")"));
  buttons.push_back(button("cancel", "Cancel"));
  dlg.push_back(Map{{"t", "hbox"}, {"children", buttons}});
  return Map{{"t", "modal"}, {"id", "edit"}, {"title", e->title}, {"child", base},
             {"dialog", Map{{"t", "vbox"}, {"children", dlg}}}, {"dialog_width", 64}};
}

void Hub::preferences_event(std::string_view widget, std::string_view kind, const Value& value) {
  auto done = [&](std::string err) {
    pref_problem_ = std::move(err);
    if (pref_problem_.empty()) pref_edit_.clear();
    refresh_preferences();
    listener_.changed();
  };
  if (widget == "prefs" && kind == "select") {
    pref_cursor_ = std::string(value.str());
    refresh_preferences();
    return listener_.changed();
  }
  if (widget == "prefs" && kind == "activate") {
    const PrefInfo* p = find_pref(value.str());
    if (!p) return;
    pref_cursor_ = p->id;
    if (p->type == "bool") {  // nothing to ask
      const std::string err = set_pref(p->id, !pref_of(*p).boolean());
      if (!err.empty()) status(err, "error");
      else if (env_value(*p)) status("$" + p->env + " is set and wins over the file", "normal");
      return done("");
    }
    pref_edit_ = p->id;
    pref_problem_.clear();
    refresh_preferences();
    return listener_.changed();
  }
  const PrefInfo* e = find_pref(pref_edit_);
  if (!e) return;
  if ((widget == "edit" && kind == "dismiss") || widget == "cancel") return done("");
  if (widget == "default") return done(set_pref(e->id, Value()));
  if (widget == "choices" && kind == "activate") return done(set_pref(e->id, Value(value.str())));
  if ((widget == "value" && kind == "submit") || widget == "ok") {
    const std::string t(widget == "ok" ? value["form"]["value"].str() : value.str());
    std::string err;
    Value v = pref_from_text(*e, t, err);
    return done(v.is_nil() ? err : set_pref(e->id, std::move(v)));
  }
}

void declare_workbench_prefs(Hub& hub, bool terminal) {
  std::vector<std::string> names;
  for (const auto& t : themes()) names.push_back(t.name);
  hub.declare_pref({"workbench.theme", "Theme", "The colours: terminal (your terminal's palette) or classic (blue "
                    "windows, grey dialogs). Theme: … in the palette sets it too.", "", "choice", "terminal", names,
                    "MYRCP_THEME"});
  // Editors linked to the workbench (the front-end links the ones in its terminal views).
  hub.declare_pref({"workbench.nvim_server", "Your Neovim",
                    "The socket of a Neovim you started yourself (nvim --listen /tmp/nvim.sock), say in a tmux pane: "
                    "files open there, markers become its diagnostics, its cursor is the selection and its own "
                    "diagnostics are in Problems. Empty: myrcp starts its own (see Editor mode).",
                    "", "string", "", {}, "MYRCP_NVIM_SERVER"});
  hub.declare_pref({"workbench.vim_socket", "Your Vim",
                    "Where a Vim of your own links itself to myrcp: save the script myrcp --vim-script prints as "
                    "~/.vim/plugin/myrcp.vim, and any Vim (with +channel) links while myrcp runs, like Neovim does. "
                    "Empty: $XDG_RUNTIME_DIR/myrcp-vim.sock. Set elsewhere, export it to Vim as $MYRCP_VIM_SOCKET.",
                    "", "string", "", {}, ""});
  hub.declare_pref({"workbench.diff", "Compare with",
                    "The program two files are compared with (they come after it), run where the editor runs (see "
                    "Editor mode). A linked Vim or Neovim of yours compares them in a tab of its own instead.",
                    "", "string", "vimdiff", {}, "MYRCP_DIFF"});
  if (!terminal) return;  // the rest is the terminal workbench's
  hub.declare_pref({"workbench.editor_mode", "Editor mode",
                    "Where a file opens in $VISUAL/$EDITOR: suspend (the editor takes the terminal until it exits), "
                    "tab (in a terminal view of the workbench), tmux-window or tmux-split (beside it, inside tmux).",
                    "", "choice", "suspend", {"suspend", "tab", "tmux-window", "tmux-split"}, "MYRCP_EDITOR_MODE"});
  hub.declare_pref({"workbench.mouse", "Mouse", "Clicks, the wheel and drags. Off leaves the mouse to the terminal "
                    "(selecting text without Shift).", "", "bool", true, {}, "MYRCP_MOUSE"});
}

}  // namespace rcp
