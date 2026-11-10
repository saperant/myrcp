#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <memory>
#include <vector>

#include "gobj.hpp"
#include "rcp/layout.hpp"

// ---- a list model of `n` rows, each item only its index -------------------------------
// Tables send a window of their rows; the model has all `total`, and a row outside the
// window is drawn as a placeholder until the plugin sends it. get_item() makes a new item
// each time, so items-changed over a range binds those rows again.

G_BEGIN_DECLS
#define RCP_TYPE_ROW (rcp_row_get_type())
G_DECLARE_FINAL_TYPE(RcpRow, rcp_row, RCP, ROW, GObject)
#define RCP_TYPE_ROWS (rcp_rows_get_type())
G_DECLARE_FINAL_TYPE(RcpRows, rcp_rows, RCP, ROWS, GObject)
G_END_DECLS

struct _RcpRow {
  GObject parent_instance;
  guint index;
};
G_DEFINE_FINAL_TYPE(RcpRow, rcp_row, G_TYPE_OBJECT)
static void rcp_row_class_init(RcpRowClass*) {}
static void rcp_row_init(RcpRow* r) { r->index = 0; }

struct _RcpRows {
  GObject parent_instance;
  guint n;
};
static void rcp_rows_iface_init(GListModelInterface* iface) {
  iface->get_item_type = [](GListModel*) -> GType { return RCP_TYPE_ROW; };
  iface->get_n_items = [](GListModel* m) -> guint { return RCP_ROWS(m)->n; };
  iface->get_item = [](GListModel* m, guint i) -> gpointer {
    if (i >= RCP_ROWS(m)->n) return nullptr;
    auto* r = RCP_ROW(g_object_new(RCP_TYPE_ROW, nullptr));
    r->index = i;
    return r;
  };
}
G_DEFINE_FINAL_TYPE_WITH_CODE(RcpRows, rcp_rows, G_TYPE_OBJECT,
                              G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL, rcp_rows_iface_init))
static void rcp_rows_class_init(RcpRowsClass*) {}
static void rcp_rows_init(RcpRows* m) { m->n = 0; }

namespace rcp::gtk {

using mp::Array;
using mp::Map;
using mp::Value;

namespace {

bool g_dark = false;
int g_char_px = 8, g_line_px = 18;

std::string_view type_of(const Value& n) { return n["t"].str(); }
bool is_rows(std::string_view t) { return t == "list" || t == "table" || t == "tree"; }

// As the terminal layout (core/src/layout.cpp): lists, tables, trees and logs grow in a
// vbox; in an hbox everything but text, buttons and checkboxes.
int grow_of(const Value& n, bool in_hbox) {
  if (!n["grow"].is_nil()) return int(std::max<int64_t>(0, n["grow"].integer()));
  const auto t = type_of(n);
  if (in_hbox) return (t == "text" || t == "button" || t == "checkbox") ? 0 : 1;
  if (t == "list" || t == "table" || t == "tree" || t == "log") return 1;
  if (t == "panel" || t == "modal") return grow_of(n["child"], false);
  if (t == "vbox" || t == "hbox") {
    for (const auto& c : n["children"].array()) {
      if (grow_of(c, false) > 0) return 1;
    }
  }
  return 0;
}

std::string escape(std::string_view s) {
  char* e = g_markup_escape_text(s.data(), gssize(s.size()));
  std::string out(e);
  g_free(e);
  return out;
}

std::string styled(std::string_view text, std::string_view style) {
  const std::string t = escape(text);
  if (style == "bold" || style == "title") return "<b>" + t + "</b>";
  if (style == "dim") return "<span alpha=\"60%\">" + t + "</span>";
  const char* colour = nullptr;
  if (style == "accent") colour = g_dark ? "#78aeed" : "#1c71d8";
  else if (style == "error") colour = g_dark ? "#ff7b63" : "#c01c28";
  else if (style == "warning") colour = g_dark ? "#f8e45c" : "#9c6e03";
  else if (style == "ok") colour = g_dark ? "#8ff0a4" : "#26a269";
  if (!colour) return t;
  return std::format("<span foreground=\"{}\">{}</span>", colour, t);
}

// The text of a title, without its styles.
std::string plain(const Value& c) {
  if (c.is_str()) return std::string(c.str());
  if (c.is_map()) return std::string(c["text"].str());
  std::string out;
  for (const auto& s : c.array()) out += s.is_str() ? s.str() : s.at(0).str();
  return out;
}

void detach(GtkWidget* w) {
  GtkWidget* parent = gtk_widget_get_parent(w);
  if (!parent) return;
  if (GTK_IS_BOX(parent)) gtk_box_remove(GTK_BOX(parent), w);
  else if (GTK_IS_FRAME(parent)) gtk_frame_set_child(GTK_FRAME(parent), nullptr);
  else if (GTK_IS_WINDOW(parent)) gtk_window_set_child(GTK_WINDOW(parent), nullptr);
  else gtk_widget_unparent(w);
}

// Puts `kids` into `box` in this order, moving them in from elsewhere if need be, and
// takes out whatever else is there.
void place_children(GtkBox* box, const std::vector<GtkWidget*>& kids) {
  for (GtkWidget* c = gtk_widget_get_first_child(GTK_WIDGET(box)); c;) {
    GtkWidget* next = gtk_widget_get_next_sibling(c);
    if (std::find(kids.begin(), kids.end(), c) == kids.end()) gtk_box_remove(box, c);
    c = next;
  }
  GtkWidget* prev = nullptr;
  for (GtkWidget* k : kids) {
    if (gtk_widget_get_parent(k) != GTK_WIDGET(box)) {
      detach(k);
      gtk_box_insert_child_after(box, k, prev);
    } else {
      gtk_box_reorder_child_after(box, k, prev);
    }
    prev = k;
  }
}

// The IR's layout hints on a widget, in its parent's direction.
void apply_layout(GtkWidget* w, const Value& n, bool in_hbox, bool line) {
  const int g = grow_of(n, in_hbox);
  if (in_hbox) {
    gtk_widget_set_hexpand(w, g > 0);
    gtk_widget_set_valign(w, line ? GTK_ALIGN_CENTER : GTK_ALIGN_FILL);
  } else {
    gtk_widget_set_vexpand(w, g > 0);
  }
  const int width = n["width"].is_nil() ? -1 : int(n["width"].integer()) * g_char_px;
  const int height = n["height"].is_nil() ? -1 : int(n["height"].integer()) * g_line_px;
  gtk_widget_set_size_request(w, width, height);
  if (width >= 0 && in_hbox) gtk_widget_set_hexpand(w, false);
  if (height >= 0 && !in_hbox) gtk_widget_set_vexpand(w, false);
}

}  // namespace

void set_dark(bool dark) { g_dark = dark; }
void set_metrics(int char_px, int line_px) {
  g_char_px = std::max(4, char_px);
  g_line_px = std::max(8, line_px);
}

std::string markup(const Value& c, std::string_view base) {
  if (c.is_str()) return styled(c.str(), base);
  if (c.is_map()) return styled(c["text"].str(), c["style"].is_str() ? c["style"].str() : base);
  std::string out;
  for (const auto& s : c.array()) {
    if (s.is_str()) out += styled(s.str(), base);
    else out += styled(s.at(0).str(), s.at(1).is_str() ? s.at(1).str() : base);
  }
  return out;
}

// ---- nodes ------------------------------------------------------------------------------

struct Node {
  ViewWidget& vw;
  std::string type, id;
  GtkWidget* w = nullptr;  // what the parent holds; we keep a reference
  std::vector<std::pair<gpointer, gulong>> handlers;

  Node(ViewWidget& v, std::string_view t, std::string_view i) : vw(v), type(t), id(i) {
    if (!id.empty()) vw.widgets()[id] = this;
  }
  virtual ~Node() {
    disconnect();
    if (!id.empty()) {
      auto it = vw.widgets().find(id);
      if (it != vw.widgets().end() && it->second == this) vw.widgets().erase(it);
    }
    if (w) {
      detach(w);
      g_object_unref(w);
    }
  }
  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

  // Before what they are connected to may go (a subclass destroying a window of its own).
  void disconnect() {
    for (auto [obj, h] : handlers) {
      if (g_signal_handler_is_connected(obj, h)) g_signal_handler_disconnect(obj, h);
    }
    handlers.clear();
  }
  void own(GtkWidget* widget) { w = GTK_WIDGET(g_object_ref_sink(widget)); }
  template <typename Ret, typename... Args>
  void connect(gpointer obj, const char* signal, std::type_identity_t<std::function<Ret(Args...)>> fn, bool after = false) {
    handlers.emplace_back(obj, on<Ret, Args...>(obj, signal, std::move(fn), after));
  }
  // Keyboard focus coming in makes this the view's focused widget.
  void track_focus(GtkWidget* target) {
    GtkEventController* fc = gtk_event_controller_focus_new();
    gtk_widget_add_controller(target, fc);
    connect<void, GtkEventControllerFocus*>(fc, "enter", [this](GtkEventControllerFocus*) {
      vw.set_focus(id);
      vw.host().focused(vw.view());
    });
  }
  void emit(std::string_view kind, Value value) { vw.host().event(vw.view(), id, kind, std::move(value)); }

  virtual void update(const Value& n) = 0;
  virtual GtkWidget* focus_target() { return w; }
  virtual bool grab() { return gtk_widget_grab_focus(focus_target()); }
  // An autofocus widget takes the focus once, when it first appears.
  void autofocus(const Value& n) {
    if (id.empty() || !n["autofocus"].boolean()) return;
    auto& st = vw.state(id);
    if (st.autofocused) return;
    st.autofocused = true;
    // Once it is drawn; through the view, as the node (or the view) may be gone by then.
    struct Want {
      std::weak_ptr<ViewWidget*> vw;
      std::string id;
    };
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
      auto* want = static_cast<Want*>(d);
      if (auto v = want->vw.lock(); v && *v && gtk_widget_get_mapped((*v)->widget())) (*v)->focus_widget(want->id);
      return G_SOURCE_REMOVE;
    }, new Want{vw.alive(), id}, [](gpointer d) { delete static_cast<Want*>(d); });
  }
};

std::unique_ptr<Node> reconcile(ViewWidget& vw, std::unique_ptr<Node> old, const Value& n);

struct TextNode : Node {
  TextNode(ViewWidget& v) : Node(v, "text", "") {
    own(gtk_label_new(nullptr));
    gtk_widget_add_css_class(w, "rcp-text");
    gtk_label_set_xalign(GTK_LABEL(w), 0);
  }
  void update(const Value& n) override {
    gtk_label_set_markup(GTK_LABEL(w), markup(n["text"], n["style"].is_str() ? n["style"].str() : "normal").c_str());
    const bool wrap = n["wrap"].boolean();
    gtk_label_set_wrap(GTK_LABEL(w), wrap);
    gtk_label_set_wrap_mode(GTK_LABEL(w), PANGO_WRAP_WORD_CHAR);
    // A line too long is cut, as in the terminal, rather than widening the window.
    gtk_label_set_ellipsize(GTK_LABEL(w), wrap ? PANGO_ELLIPSIZE_NONE : PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable(GTK_LABEL(w), wrap);  // paragraphs are worth copying from
  }
};

struct BoxNode : Node {
  std::vector<std::unique_ptr<Node>> kids;
  BoxNode(ViewWidget& v, std::string_view t) : Node(v, t, "") {
    own(gtk_box_new(t == "hbox" ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL, 0));
  }
  void update(const Value& n) override {
    const bool h = type == "hbox";
    gtk_box_set_spacing(GTK_BOX(w), h ? 6 * int(n["gap"].integer(1)) : g_line_px * int(n["gap"].integer(0)) / 2 + 4);
    const auto& children = n["children"].array();
    // An hbox of text and buttons is a line: centred. One of panes shares the height.
    bool line = h;
    for (const auto& c : children) {
      const auto ct = type_of(c);
      line = line && (ct == "text" || ct == "button" || ct == "checkbox" || ct == "field" || ct == "progress");
    }
    std::vector<std::unique_ptr<Node>> next;
    std::vector<GtkWidget*> widgets;
    for (size_t i = 0; i < children.size(); ++i) {
      next.push_back(reconcile(vw, i < kids.size() ? std::move(kids[i]) : nullptr, children[i]));
      apply_layout(next.back()->w, children[i], h, line);
      widgets.push_back(next.back()->w);
    }
    place_children(GTK_BOX(w), widgets);
    kids = std::move(next);  // the rest of the old ones go (and leave the box)
  }
};

struct PanelNode : Node {
  std::unique_ptr<Node> child;
  GtkWidget* inner;
  PanelNode(ViewWidget& v) : Node(v, "panel", "") {
    own(gtk_frame_new(nullptr));
    gtk_widget_add_css_class(w, "rcp-panel");
    inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_frame_set_child(GTK_FRAME(w), inner);
  }
  void update(const Value& n) override {
    const std::string title = plain(n["title"]);
    gtk_frame_set_label(GTK_FRAME(w), title.empty() ? nullptr : title.c_str());
    child = reconcile(vw, std::move(child), n["child"]);
    gtk_widget_set_vexpand(child->w, true);
    // Rows sit flush in the frame; anything else gets some room.
    const int m = is_rows(type_of(n["child"])) || type_of(n["child"]) == "log" ? 0 : 6;
    gtk_widget_set_margin_start(child->w, m);
    gtk_widget_set_margin_end(child->w, m);
    gtk_widget_set_margin_top(child->w, m);
    gtk_widget_set_margin_bottom(child->w, m);
    place_children(GTK_BOX(inner), {child->w});
  }
};

struct ButtonNode : Node {
  ButtonNode(ViewWidget& v, std::string_view wid) : Node(v, "button", wid) {
    own(gtk_button_new());
    connect<void, GtkButton*>(w, "clicked", [this](GtkButton*) {
      vw.set_focus(id);
      emit("press", Map{{"form", vw.form()}});
    });
    track_focus(w);
  }
  void update(const Value& n) override {
    gtk_button_set_label(GTK_BUTTON(w), std::string(n["label"].str()).c_str());
    autofocus(n);
  }
};

struct FieldNode : Node {
  FieldNode(ViewWidget& v, std::string_view wid) : Node(v, "field", wid) {
    own(gtk_entry_new());
    connect<void, GtkEntry*>(w, "activate", [this](GtkEntry*) { emit("submit", text()); });
    track_focus(w);
  }
  std::string text() const { return gtk_editable_get_text(GTK_EDITABLE(w)); }
  void update(const Value& n) override {
    auto& st = vw.state(id);
    const std::string value(n["value"].str());
    if (st.value_seen != "=" + value) {  // the plugin set (or cleared) it
      st.value_seen = "=" + value;
      gtk_editable_set_text(GTK_EDITABLE(w), value.c_str());
      gtk_editable_set_position(GTK_EDITABLE(w), -1);
    }
    gtk_entry_set_placeholder_text(GTK_ENTRY(w), std::string(n["placeholder"].str()).c_str());
    autofocus(n);
  }
};

struct ProgressNode : Node {
  ProgressNode(ViewWidget& v) : Node(v, "progress", "") {
    own(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(w), true);
  }
  void update(const Value& n) override {
    const double v = std::clamp(n["value"].number(), 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(w), v);
    const std::string label(n["label"].str());
    const std::string pct = std::format("{}%", int(std::lround(v * 100)));
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(w), (label.empty() ? pct : label + " · " + pct).c_str());
  }
};

struct CheckNode : Node {
  bool updating = false;
  CheckNode(ViewWidget& v, std::string_view wid) : Node(v, "checkbox", wid) {
    own(gtk_check_button_new());
    connect<void, GtkCheckButton*>(w, "toggled", [this](GtkCheckButton* b) {
      if (!updating) emit("toggle", bool(gtk_check_button_get_active(b)));
    });
    track_focus(w);
  }
  void update(const Value& n) override {
    updating = true;
    gtk_check_button_set_label(GTK_CHECK_BUTTON(w), std::string(n["label"].str()).c_str());
    gtk_check_button_set_active(GTK_CHECK_BUTTON(w), n["checked"].boolean());
    updating = false;
    autofocus(n);
  }
};

struct LogNode : Node {
  GtkWidget* view;
  GtkTextMark* end;
  Value lines;
  LogNode(ViewWidget& v, std::string_view wid) : Node(v, "log", wid) {
    own(gtk_scrolled_window_new());
    view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), false);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), false);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), true);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 4);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(w), view);
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(buf, &it);
    end = gtk_text_buffer_create_mark(buf, nullptr, &it, false);
    if (!id.empty()) track_focus(view);
  }
  GtkWidget* focus_target() override { return view; }
  void update(const Value& n) override {
    if (n["lines"] == lines) return;
    lines = n["lines"];
    GtkAdjustment* adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(w));
    const bool at_end = gtk_adjustment_get_value(adj) + gtk_adjustment_get_page_size(adj) >= gtk_adjustment_get_upper(adj) - 2;
    GtkTextBuffer* buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(view));
    gtk_text_buffer_set_text(buf, "", 0);
    GtkTextIter it;
    gtk_text_buffer_get_end_iter(buf, &it);
    std::string text;
    bool first = true;
    for (const auto& l : lines.array()) {
      if (!first) text += "\n";
      first = false;
      text += markup(l);
    }
    gtk_text_buffer_insert_markup(buf, &it, text.c_str(), -1);
    if (at_end) gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(view), end, 0, false, 0, 1);  // stays at the end
  }
};

// ---- rows: list, tree, table -------------------------------------------------------

struct RowsNode : Node {
  Value n;  // the node as last given
  GtkWidget* stack;
  GtkWidget* scrolled;
  GtkWidget* empty;
  GtkWidget* lv;  // a GtkListView (list, tree) or a GtkColumnView (table)
  Ref<RcpRows> model;
  Ref<GtkSingleSelection> sel;
  std::vector<Ref<GtkColumnViewColumn>> columns;
  Value columns_seen;
  bool updating = false;
  std::shared_ptr<RowsNode*> alive = std::make_shared<RowsNode*>(this);  // for cells that outlive us
  // Rows bound while missing from the window, to ask the plugin for.
  int want_lo = -1, want_hi = -1;
  int sent_first = -1, sent_count = -1;
  bool flush_queued = false;

  RowsNode(ViewWidget& v, std::string_view t, std::string_view wid) : Node(v, t, wid) {
    own(gtk_stack_new());
    stack = w;
    model = adopt(RCP_ROWS(g_object_new(RCP_TYPE_ROWS, nullptr)));
    sel = adopt(gtk_single_selection_new(G_LIST_MODEL(g_object_ref(model.get()))));
    gtk_single_selection_set_autoselect(sel, false);
    gtk_single_selection_set_can_unselect(sel, false);
    if (t == "table") {
      lv = gtk_column_view_new(GTK_SELECTION_MODEL(g_object_ref(sel.get())));
      gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(lv), false);
      gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(lv), false);
      gtk_widget_add_css_class(lv, "data-table");
      if (GtkSorter* s = gtk_column_view_get_sorter(GTK_COLUMN_VIEW(lv))) {
        connect<void, GtkSorter*, GtkSorterChange>(s, "changed", [this](GtkSorter* s, GtkSorterChange) { sorted(s); });
      }
      connect<void, GtkColumnView*, guint>(lv, "activate", [this](GtkColumnView*, guint pos) { activate(int(pos)); });
    } else {
      GtkListItemFactory* f = gtk_signal_list_item_factory_new();
      connect<void, GtkSignalListItemFactory*, GObject*>(f, "setup", [this](GtkSignalListItemFactory*, GObject* li) { setup_item(li); });
      connect<void, GtkSignalListItemFactory*, GObject*>(f, "bind", [this](GtkSignalListItemFactory*, GObject* li) { bind_item(li); });
      lv = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(sel.get())), f);
      connect<void, GtkListView*, guint>(lv, "activate", [this](GtkListView*, guint pos) { activate(int(pos)); });
    }
    connect<void, GtkSelectionModel*, guint, guint>(sel.get(), "selection-changed",
                                                    [this](GtkSelectionModel*, guint, guint) { selection_changed(); });
    scrolled = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), lv);
    gtk_widget_set_vexpand(scrolled, true);
    empty = gtk_label_new(nullptr);
    gtk_widget_add_css_class(empty, "dim-label");
    gtk_widget_set_valign(empty, GTK_ALIGN_START);
    gtk_widget_set_halign(empty, GTK_ALIGN_START);
    gtk_widget_set_margin_start(empty, 8);
    gtk_widget_set_margin_top(empty, 6);
    gtk_stack_add_named(GTK_STACK(stack), scrolled, "rows");
    gtk_stack_add_named(GTK_STACK(stack), empty, "empty");
    track_focus(lv);
    keys();
  }
  ~RowsNode() override { *alive = nullptr; }

  GtkWidget* focus_target() override { return lv; }
  // Into the cursor row (a column view's own focus may land on its header).
  bool grab() override {
    // Now (a list's own focus goes to its focus row, else to itself), then onto the
    // cursor row once the list is laid out.
    const bool ok = gtk_widget_grab_focus(lv);
    if (row_count(n) == 0) return ok;
    const guint cur = guint(vw.state(id).cursor);
    if (type == "table") gtk_column_view_scroll_to(GTK_COLUMN_VIEW(lv), cur, nullptr, GTK_LIST_SCROLL_FOCUS, nullptr);
    else gtk_list_view_scroll_to(GTK_LIST_VIEW(lv), cur, GTK_LIST_SCROLL_FOCUS, nullptr);
    return true;
  }
  bool treeish() const { return type == "tree" || (type == "table" && !n["tree"].is_nil()); }

  // ---- cells
  static GObject* item_of(GObject* li) {
    return GTK_IS_COLUMN_VIEW_CELL(li) ? G_OBJECT(gtk_column_view_cell_get_item(GTK_COLUMN_VIEW_CELL(li)))
                                       : G_OBJECT(gtk_list_item_get_item(GTK_LIST_ITEM(li)));
  }
  static void set_child(GObject* li, GtkWidget* c) {
    if (GTK_IS_COLUMN_VIEW_CELL(li)) gtk_column_view_cell_set_child(GTK_COLUMN_VIEW_CELL(li), c);
    else gtk_list_item_set_child(GTK_LIST_ITEM(li), c);
  }
  static GtkWidget* child_of(GObject* li) {
    return GTK_IS_COLUMN_VIEW_CELL(li) ? gtk_column_view_cell_get_child(GTK_COLUMN_VIEW_CELL(li))
                                       : gtk_list_item_get_child(GTK_LIST_ITEM(li));
  }
  static int index_of(GtkWidget* cell) { return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(cell), "rcp-index")) - 1; }
  static GtkWidget* part(GtkWidget* cell, const char* name) { return GTK_WIDGET(g_object_get_data(G_OBJECT(cell), name)); }

  // A cell: [arrow] [check] text [detail]; `column` -1 for a list or tree row.
  GtkWidget* make_cell(int column, bool arrow, bool check, bool detail) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    std::weak_ptr<RowsNode*> self = alive;
    if (arrow) {
      GtkWidget* b = gtk_button_new_from_icon_name("pan-end-symbolic");
      gtk_widget_add_css_class(b, "flat");
      gtk_widget_add_css_class(b, "rcp-arrow");
      gtk_widget_set_focusable(b, false);
      gtk_box_append(GTK_BOX(box), b);
      g_object_set_data(G_OBJECT(box), "arrow", b);
      on<void, GtkButton*>(b, "clicked", [self, box](GtkButton*) {
        auto s = self.lock();
        if (!s || !*s) return;
        (*s)->arrow_clicked(index_of(box));
      });
    }
    if (check) {
      GtkWidget* c = gtk_check_button_new();
      gtk_widget_set_focusable(c, false);
      gtk_box_append(GTK_BOX(box), c);
      g_object_set_data(G_OBJECT(box), "check", c);
      on<void, GtkCheckButton*>(c, "toggled", [self, box](GtkCheckButton*) {
        auto s = self.lock();
        if (!s || !*s || g_object_get_data(G_OBJECT(box), "binding")) return;
        (*s)->check_clicked(index_of(box));
      });
    }
    GtkWidget* text = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(text), 0);
    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(text, true);
    gtk_box_append(GTK_BOX(box), text);
    g_object_set_data(G_OBJECT(box), "text", text);
    if (detail) {
      GtkWidget* d = gtk_label_new(nullptr);
      gtk_label_set_xalign(GTK_LABEL(d), 1);
      gtk_box_append(GTK_BOX(box), d);
      g_object_set_data(G_OBJECT(box), "detail", d);
    }
    // Right clicks open the context menu here; Shift/Ctrl+clicks and clicks on clickable
    // columns are the plugin's (`cell`).
    GtkGesture* g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
    gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(g));
    on<void, GtkGestureClick*, int, double, double>(g, "pressed", [self, box, column](GtkGestureClick* g, int, double x, double y) {
      auto s = self.lock();
      if (!s || !*s) return;
      if ((*s)->cell_pressed(g, box, column, x, y)) gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    });
    return box;
  }

  void setup_item(GObject* li) {
    const bool tree = type == "tree";
    set_child(li, make_cell(-1, tree, tree, true));
  }

  void bind_item(GObject* li) {
    GtkWidget* cell = child_of(li);
    const int i = int(RCP_ROW(item_of(li))->index);
    g_object_set_data(G_OBJECT(cell), "rcp-index", GINT_TO_POINTER(i + 1));
    const Value* r = row_at(n, i);
    GtkLabel* text = GTK_LABEL(part(cell, "text"));
    GtkLabel* detail = GTK_LABEL(part(cell, "detail"));
    if (!r) {
      gtk_label_set_markup(text, "<span alpha=\"40%\">…</span>");
      gtk_label_set_text(detail, "");
      want(i);
      return;
    }
    const std::string_view style = (*r)["style"].is_str() ? (*r)["style"].str() : "normal";
    gtk_label_set_markup(text, markup(r->is_str() ? *r : (*r)["text"], style).c_str());
    gtk_label_set_markup(detail, (*r)["detail"].is_nil() ? "" : markup((*r)["detail"], "dim").c_str());
    if (type == "tree") {
      bind_arrow(cell, *r);
      GtkWidget* check = part(cell, "check");
      const auto c = (*r)["check"].str();
      gtk_widget_set_visible(check, !c.empty());
      g_object_set_data(G_OBJECT(cell), "binding", GINT_TO_POINTER(1));
      gtk_check_button_set_active(GTK_CHECK_BUTTON(check), c == "all");
      gtk_check_button_set_inconsistent(GTK_CHECK_BUTTON(check), c == "some");
      g_object_set_data(G_OBJECT(cell), "binding", nullptr);
    }
  }

  static void bind_arrow(GtkWidget* cell, const Value& r) {
    GtkWidget* arrow = part(cell, "arrow");
    gtk_widget_set_margin_start(cell, 16 * int(r["depth"].integer()));
    const bool can = r["expandable"].boolean();
    gtk_widget_set_opacity(arrow, can ? 1 : 0);
    gtk_widget_set_sensitive(arrow, can);
    gtk_button_set_icon_name(GTK_BUTTON(arrow), r["expanded"].boolean() ? "pan-down-symbolic" : "pan-end-symbolic");
  }

  void setup_cell(GObject* li, int c) {
    const auto& col = n["columns"].at(size_t(c));
    GtkWidget* cell = make_cell(c, treeish() && c == int(n["tree"].integer(-1)), false, false);
    GtkLabel* text = GTK_LABEL(part(cell, "text"));
    if (col["align"].str() == "right") gtk_label_set_xalign(text, 1);
    if (col["clickable"].boolean()) gtk_widget_add_css_class(GTK_WIDGET(text), "rcp-clickable");
    set_child(li, cell);
  }

  void bind_cell(GObject* li, int c) {
    GtkWidget* cell = child_of(li);
    const int i = int(RCP_ROW(item_of(li))->index);
    g_object_set_data(G_OBJECT(cell), "rcp-index", GINT_TO_POINTER(i + 1));
    GtkLabel* text = GTK_LABEL(part(cell, "text"));
    const Value* r = row_at(n, i);
    if (!r) {
      gtk_label_set_markup(text, c == 0 ? "<span alpha=\"40%\">…</span>" : "");
      want(i);
      return;
    }
    const std::string_view style = (*r)["style"].is_str() ? (*r)["style"].str() : "normal";
    gtk_label_set_markup(text, markup((*r)["cells"].at(size_t(c)), style).c_str());
    if (part(cell, "arrow")) bind_arrow(cell, *r);
  }

  void make_columns() {
    GtkColumnView* cv = GTK_COLUMN_VIEW(lv);
    for (auto& c : columns) gtk_column_view_remove_column(cv, c);
    columns.clear();
    const auto& cols = n["columns"].array();
    for (size_t c = 0; c < cols.size(); ++c) {
      GtkListItemFactory* f = gtk_signal_list_item_factory_new();
      const int ci = int(c);
      connect<void, GtkSignalListItemFactory*, GObject*>(f, "setup", [this, ci](GtkSignalListItemFactory*, GObject* li) { setup_cell(li, ci); });
      connect<void, GtkSignalListItemFactory*, GObject*>(f, "bind", [this, ci](GtkSignalListItemFactory*, GObject* li) { bind_cell(li, ci); });
      auto col = adopt(gtk_column_view_column_new(plain(cols[c]["title"]).c_str(), f));
      gtk_column_view_column_set_resizable(col, true);
      if (!cols[c]["width"].is_nil()) gtk_column_view_column_set_fixed_width(col, int(cols[c]["width"].integer()) * g_char_px + 16);
      gtk_column_view_column_set_expand(col, cols[c]["grow"].integer(0) > 0 || cols[c]["width"].is_nil());
      if (n["sortable"].boolean() && cols[c]["sortable"].boolean(true)) {
        // The plugin sorts: the sorter only makes the title clickable and shows the order.
        gtk_column_view_column_set_sorter(col, GTK_SORTER(gtk_custom_sorter_new(nullptr, nullptr, nullptr)));
      }
      gtk_column_view_append_column(cv, col);
      columns.push_back(std::move(col));
    }
    columns_seen = n["columns"];
    // No header: hide the column view's own.
    for (GtkWidget* c = gtk_widget_get_first_child(lv); c; c = gtk_widget_get_next_sibling(c)) {
      if (std::string_view(gtk_widget_get_css_name(c)) == "header") gtk_widget_set_visible(c, n["header"].boolean(true));
    }
  }

  // ---- what the user does
  std::string key_at(int i) const {
    const Value* r = row_at(n, i);
    return r ? item_key(*r, size_t(i)) : "";
  }
  void move_to(int i) {  // the cursor goes to row i (by a click on its parts)
    auto& st = vw.state(id);
    if (st.cursor == i) return;
    st.cursor = i;
    updating = true;
    gtk_single_selection_set_selected(sel, guint(i));
    updating = false;
    if (const Value* r = row_at(n, i)) emit("select", item_key(*r, size_t(i)));
    vw.host().cursor_moved(vw.view());
  }
  void arrow_clicked(int i) {
    const Value* r = row_at(n, i);
    if (!r) return;
    vw.set_focus(id);
    move_to(i);
    emit((*r)["expanded"].boolean() ? "collapse" : "expand", item_key(*r, size_t(i)));
  }
  void check_clicked(int i) {
    if (!row_at(n, i)) return;
    vw.set_focus(id);
    move_to(i);
    emit("toggle", key_at(i));
  }
  bool cell_pressed(GtkGestureClick* g, GtkWidget* cell, int column, double x, double y) {
    const int i = index_of(cell);
    if (i < 0 || !row_at(n, i)) return false;
    const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    const GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    const bool shift = mods & GDK_SHIFT_MASK, ctrl = mods & GDK_CONTROL_MASK;
    if (button == GDK_BUTTON_SECONDARY) {
      vw.set_focus(id);
      move_to(i);
      graphene_point_t p{float(x), float(y)}, out{};
      if (!gtk_widget_compute_point(cell, lv, &p, &out)) out = p;
      vw.host().context_menu(vw.view(), lv, out.x, out.y);
      return true;
    }
    if (button != GDK_BUTTON_PRIMARY || type != "table") return false;
    const bool clickable = column >= 0 && n["columns"].at(size_t(column))["clickable"].boolean();
    if (!clickable && !shift && !ctrl) return false;
    vw.set_focus(id);
    gtk_widget_grab_focus(lv);
    move_to(i);
    emit("cell", Map{{"key", key_at(i)}, {"column", clickable && !shift && !ctrl ? column : -1}, {"shift", shift}, {"ctrl", ctrl}});
    return true;
  }
  void activate(int i) {
    if (const Value* r = row_at(n, i)) emit("activate", item_key(*r, size_t(i)));
  }
  void selection_changed() {
    if (updating) return;
    const guint s = gtk_single_selection_get_selected(sel);
    if (s == GTK_INVALID_LIST_POSITION) return;
    auto& st = vw.state(id);
    if (int(s) == st.cursor) return;
    st.cursor = int(s);
    vw.set_focus(id);
    if (const Value* r = row_at(n, int(s))) emit("select", item_key(*r, size_t(s)));
    else want(int(s));
    vw.host().cursor_moved(vw.view());
  }
  // A click on a column title: by that column, or the other way (GTK has no "unsorted").
  void sorted(GtkSorter* s) {
    if (updating) return;
    GtkColumnViewSorter* cs = GTK_COLUMN_VIEW_SORTER(s);
    GtkColumnViewColumn* col = gtk_column_view_sorter_get_primary_sort_column(cs);
    int c = -1;
    for (size_t k = 0; k < columns.size(); ++k) {
      if (columns[k].get() == col) c = int(k);
    }
    const bool desc = gtk_column_view_sorter_get_primary_sort_order(cs) == GTK_SORT_DESCENDING;
    const std::string key = key_at(vw.state(id).cursor);
    vw.set_focus(id);
    emit("sort", Array{c, desc, key.empty() ? Value() : Value(key)});
  }
  // →/← open and close tree rows (← on a closed one goes to its parent); Space toggles.
  void keys() {
    GtkEventController* kc = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(kc, GTK_PHASE_CAPTURE);
    gtk_widget_add_controller(lv, kc);
    connect<gboolean, GtkEventControllerKey*, guint, guint, GdkModifierType>(
        kc, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType mods) -> gboolean {
          if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SHIFT_MASK)) return false;
          const Value* r = row_at(n, vw.state(id).cursor);
          if (!r || !treeish()) return false;
          const std::string k = item_key(*r, size_t(vw.state(id).cursor));
          const bool open = (*r)["expandable"].boolean() && (*r)["expanded"].boolean();
          if (key == GDK_KEY_Right || key == GDK_KEY_KP_Right) {
            if (!(*r)["expandable"].boolean() || open) return false;
            emit("expand", k);
            return true;
          }
          if (key == GDK_KEY_Left || key == GDK_KEY_KP_Left) {
            if (!open && (*r)["depth"].integer() <= 0) return false;
            emit("collapse", k);
            return true;
          }
          if (key == GDK_KEY_space && type == "tree" && !(*r)["check"].is_nil()) {
            emit("toggle", k);
            return true;
          }
          return false;
        });
  }

  // ---- windows of rows
  void want(int i) {
    if (type == "list") return;
    want_lo = want_lo < 0 ? i : std::min(want_lo, i);
    want_hi = std::max(want_hi, i);
    if (flush_queued) return;
    flush_queued = true;
    std::weak_ptr<RowsNode*> self = alive;
    auto* holder = new std::weak_ptr<RowsNode*>(self);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, [](gpointer d) -> gboolean {
      if (auto s = static_cast<std::weak_ptr<RowsNode*>*>(d)->lock(); s && *s) (*s)->flush_window();
      return G_SOURCE_REMOVE;
    }, holder, [](gpointer d) { delete static_cast<std::weak_ptr<RowsNode*>*>(d); });
  }
  void flush_window() {
    flush_queued = false;
    if (want_lo < 0) return;
    const int total = row_count(n);
    const int first = std::clamp(want_lo - 50, 0, std::max(0, total - 1));
    const int count = std::min(500, want_hi - first + 100);
    want_lo = want_hi = -1;
    if (first == sent_first && count == sent_count) return;  // asked already: wait for it
    sent_first = first;
    sent_count = count;
    emit("window", Array{first, count});
  }

  // ---- the plugin's tree
  int apply_cursor() {
    auto& st = vw.state(id);
    const Value& c = n["cursor"];
    const Value& selp = n["selected"];
    bool moved = false;
    std::string wantk;
    if (c.is_array()) wantk = std::to_string(c.at(1).integer());  // applied when its seq changes
    else if (!selp.is_nil()) wantk = "s:" + (selp.is_str() ? std::string(selp.str()) : std::to_string(selp.integer()));
    if (const Value& f = n["follow"]; f.is_array()) {  // the cursor's row after a sort, once
      const std::string at = std::to_string(f.at(0).integer()) + ":" + std::to_string(f.at(1).integer());
      if (st.follow_seen != at) {
        st.follow_seen = at;
        st.cursor = int(f.at(0).integer());
        moved = true;
      }
    }
    if (!wantk.empty() && st.cursor_seen != wantk) {
      st.cursor_seen = wantk;
      if (c.is_array()) {
        st.cursor = int(c.at(0).integer());
        moved = true;
      } else {
        const int count = row_count(n);
        for (int i = 0; i < count; ++i) {
          if (const Value* r = row_at(n, i); r && item_key(*r, size_t(i)) == wantk.substr(2)) {
            st.cursor = i;
            moved = true;
          }
        }
      }
    }
    st.cursor = std::clamp(st.cursor, 0, std::max(0, row_count(n) - 1));
    return moved ? 1 : 0;
  }

  // Tells the list which rows changed, as few as can be: GTK builds those rows again,
  // and a row built again loses the keyboard focus and the selection.
  bool rebind(const Value& old, int old_total, int total) {
    const char* key = type == "list" ? "items" : "rows";
    const auto& orows = old[key].array();
    const auto& nrows = n[key].array();
    const int osz = int(orows.size()), nsz = int(nrows.size());
    const int o0 = type == "list" ? 0 : int(old["offset"].integer(0));
    const int o1 = type == "list" ? 0 : int(n["offset"].integer(0));
    auto changed = [&](int pos, int removed, int added) {
      if (removed > 0 || added > 0) g_list_model_items_changed(G_LIST_MODEL(model.get()), guint(pos), guint(removed), guint(added));
    };
    model.get()->n = guint(total);
    if (!(n["tree"] == old["tree"]) || !(n["columns"] == old["columns"])) {
      changed(0, old_total, total);
      sent_first = sent_count = -1;
      return true;
    }
    if (o0 == 0 && osz == old_total && o1 == 0 && nsz == total) {
      // All the rows, before and after: only those between the common ends changed.
      int p = 0;
      while (p < osz && p < nsz && orows[size_t(p)] == nrows[size_t(p)]) ++p;
      int q = 0;
      while (q < osz - p && q < nsz - p && orows[size_t(osz - 1 - q)] == nrows[size_t(nsz - 1 - q)]) ++q;
      changed(p, osz - p - q, nsz - p - q);
      if (osz - p - q || nsz - p - q) sent_first = sent_count = -1;
      return osz - p - q || nsz - p - q;
    }
    if (total != old_total) {
      changed(0, old_total, total);
      sent_first = sent_count = -1;
      return true;
    }
    if (o0 == o1 && orows == nrows) return false;
    // Windows of a table of the same size: the rows of each, bound again.
    int a0 = std::clamp(o0, 0, total), a1 = std::clamp(o0 + osz, a0, total);
    int b0 = std::clamp(o1, 0, total), b1 = std::clamp(o1 + nsz, b0, total);
    if (b0 <= a1 && a0 <= b1) {
      changed(std::min(a0, b0), std::max(a1, b1) - std::min(a0, b0), std::max(a1, b1) - std::min(a0, b0));
    } else {
      changed(a0, a1 - a0, a1 - a0);
      changed(b0, b1 - b0, b1 - b0);
    }
    sent_first = sent_count = -1;
    return true;
  }

  void update(const Value& nn) override {
    const Value old = std::move(n);
    n = nn;
    updating = true;
    const int total = row_count(n);
    const int old_total = int(model.get()->n);
    if (type == "table" && !(n["columns"] == columns_seen && n["sortable"] == old["sortable"] && n["header"] == old["header"] &&
                             n["tree"] == old["tree"])) {
      make_columns();
    }
    const bool focused = gtk_widget_get_state_flags(lv) & GTK_STATE_FLAG_FOCUS_WITHIN;
    const bool rebound = rebind(old, old_total, total);
    if (type == "table") {
      const Value& s = n["sorted"];
      const int sc = s.is_array() ? int(s.at(0).integer(-1)) : -1;
      gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(lv), nullptr, GTK_SORT_ASCENDING);  // one arrow, not a history
      if (sc >= 0 && size_t(sc) < columns.size()) {
        gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(lv), columns[size_t(sc)], s.at(1).boolean() ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
      }
    }
    const bool moved = apply_cursor();
    const int cur = vw.state(id).cursor;
    if (total > 0 && gtk_single_selection_get_selected(sel) != guint(cur)) gtk_single_selection_set_selected(sel, guint(cur));
    if ((moved || (rebound && focused)) && total > 0) {
      // Rows built again lose the keyboard focus: back to the cursor row.
      const GtkListScrollFlags f = focused ? GTK_LIST_SCROLL_FOCUS : GTK_LIST_SCROLL_NONE;
      if (type == "table") gtk_column_view_scroll_to(GTK_COLUMN_VIEW(lv), guint(cur), nullptr, f, nullptr);
      else gtk_list_view_scroll_to(GTK_LIST_VIEW(lv), guint(cur), f, nullptr);
    }
    if (total > 0 && !row_at(n, cur)) want(cur);
    gtk_label_set_text(GTK_LABEL(empty), std::string(n["empty"].str()).c_str());
    gtk_stack_set_visible_child_name(GTK_STACK(stack), total == 0 ? "empty" : "rows");
    updating = false;
    autofocus(n);
  }

  void set_cursor(int i) {
    const int total = row_count(n);
    if (total == 0) return;
    i = std::clamp(i, 0, total - 1);
    vw.state(id).cursor = i;
    updating = true;
    gtk_single_selection_set_selected(sel, guint(i));
    updating = false;
    if (type == "table") gtk_column_view_scroll_to(GTK_COLUMN_VIEW(lv), guint(i), nullptr, GTK_LIST_SCROLL_FOCUS, nullptr);
    else gtk_list_view_scroll_to(GTK_LIST_VIEW(lv), guint(i), GTK_LIST_SCROLL_FOCUS, nullptr);
    if (const Value* r = row_at(n, i)) emit("select", item_key(*r, size_t(i)));
    else want(i);
  }
};

// A modal: the base stays (insensitive) and the dialog is a window of its own over it,
// shown while the base is.
struct ModalNode : Node {
  std::unique_ptr<Node> base, dialog;
  GtkWidget* window;
  GtkWidget* dbox;
  ModalNode(ViewWidget& v, std::string_view wid, std::unique_ptr<Node> b) : Node(v, "modal", wid), base(std::move(b)) {
    own(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    window = gtk_window_new();
    gtk_window_set_modal(GTK_WINDOW(window), true);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window), true);
    dbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    for (auto* f : {gtk_widget_set_margin_start, gtk_widget_set_margin_end, gtk_widget_set_margin_top, gtk_widget_set_margin_bottom})
      f(dbox, 12);
    gtk_window_set_child(GTK_WINDOW(window), dbox);
    connect<gboolean, GtkWindow*>(window, "close-request", [this](GtkWindow*) -> gboolean {
      emit("dismiss", Value());
      return true;  // the plugin takes it away
    });
    GtkEventController* kc = gtk_event_controller_key_new();
    gtk_widget_add_controller(window, kc);
    connect<gboolean, GtkEventControllerKey*, guint, guint, GdkModifierType>(
        kc, "key-pressed", [this](GtkEventControllerKey*, guint key, guint, GdkModifierType) -> gboolean {
          if (key != GDK_KEY_Escape) return false;
          emit("dismiss", Value());
          return true;
        });
    connect<void, GtkWidget*>(w, "map", [this](GtkWidget*) { show(); });
    connect<void, GtkWidget*>(w, "unmap", [this](GtkWidget*) { gtk_widget_set_visible(window, false); });
  }
  ~ModalNode() override {
    disconnect();
    dialog.reset();
    base.reset();
    gtk_window_destroy(GTK_WINDOW(window));
  }
  void show() {
    if (!gtk_widget_get_mapped(w)) return;
    if (GtkRoot* root = gtk_widget_get_root(w); root && GTK_IS_WINDOW(root)) gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(root));
    gtk_window_present(GTK_WINDOW(window));
  }
  void update(const Value& n) override {
    base = reconcile(vw, std::move(base), n["child"]);
    gtk_widget_set_vexpand(base->w, true);
    gtk_widget_set_sensitive(base->w, false);
    place_children(GTK_BOX(w), {base->w});
    dialog = reconcile(vw, std::move(dialog), n["dialog"]);
    apply_layout(dialog->w, n["dialog"], false, false);
    gtk_widget_set_vexpand(dialog->w, true);
    place_children(GTK_BOX(dbox), {dialog->w});
    gtk_window_set_title(GTK_WINDOW(window), plain(n["title"]).c_str());
    const int dw = n["dialog_width"].is_nil() ? 60 : int(n["dialog_width"].integer());
    const int dh = n["dialog_height"].is_nil() ? -1 : int(n["dialog_height"].integer()) * g_line_px + 60;
    gtk_window_set_default_size(GTK_WINDOW(window), dw * g_char_px + 24, dh);
    if (!gtk_widget_get_visible(window)) show();
  }
};

std::unique_ptr<Node> build(ViewWidget& vw, const Value& n) {
  const auto t = type_of(n);
  const auto id = n["id"].str();
  if (t == "text") return std::make_unique<TextNode>(vw);
  if (t == "vbox" || t == "hbox") return std::make_unique<BoxNode>(vw, t);
  if (t == "panel") return std::make_unique<PanelNode>(vw);
  if (t == "button") return std::make_unique<ButtonNode>(vw, id);
  if (t == "field") return std::make_unique<FieldNode>(vw, id);
  if (t == "progress") return std::make_unique<ProgressNode>(vw);
  if (t == "checkbox") return std::make_unique<CheckNode>(vw, id);
  if (t == "log") return std::make_unique<LogNode>(vw, id);
  if (is_rows(t)) return std::make_unique<RowsNode>(vw, t, id);
  if (t == "modal") return std::make_unique<ModalNode>(vw, id, nullptr);
  auto unknown = std::make_unique<TextNode>(vw);
  unknown->type = std::string(t);  // never matches a known type: rebuilt when it becomes one
  return unknown;
}

std::unique_ptr<Node> reconcile(ViewWidget& vw, std::unique_ptr<Node> old, const Value& n) {
  const auto t = type_of(n);
  const auto id = n["id"].str();
  if (old && old->type == t && old->id == id) {
    old->update(n);
    return old;
  }
  // A dialog opening over what is there keeps it as its base, and closing gives it back,
  // so cursors and scrolling survive dialogs.
  if (t == "modal" && old && old->type != "modal") {
    auto m = std::make_unique<ModalNode>(vw, id, std::move(old));
    m->update(n);
    return m;
  }
  if (old && old->type == "modal") {
    std::unique_ptr<Node> b = std::move(static_cast<ModalNode&>(*old).base);
    old.reset();
    if (b) gtk_widget_set_sensitive(b->w, true);  // no longer behind a dialog
    return reconcile(vw, std::move(b), n);
  }
  std::unique_ptr<Node> fresh = build(vw, n);
  if (fresh->type == t || is_rows(t) || t == "modal") {
    fresh->update(n);
  } else {
    gtk_label_set_markup(GTK_LABEL(fresh->w), markup(Value(std::format("<unknown node '{}'>", t)), "error").c_str());
  }
  return fresh;
}

// ---- the view -----------------------------------------------------------------------

ViewWidget::ViewWidget(ViewHost& host, std::string view) : host_(host), view_(std::move(view)) {
  root_ = GTK_WIDGET(g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0)));
  gtk_widget_add_css_class(root_, "rcp-view");
  for (auto* f : {gtk_widget_set_margin_start, gtk_widget_set_margin_end, gtk_widget_set_margin_top, gtk_widget_set_margin_bottom})
    f(root_, 6);
}

ViewWidget::~ViewWidget() {
  *alive_ = nullptr;
  node_.reset();
  g_object_unref(root_);
}

void ViewWidget::update(const Value& tree, bool loading) {
  tree_ = loading && tree.is_nil() ? Value(Map{{"t", "text"}, {"text", "Loading…"}, {"style", "dim"}}) : tree;
  node_ = reconcile(*this, std::move(node_), tree_);
  gtk_widget_set_vexpand(node_->w, true);
  place_children(GTK_BOX(root_), {node_->w});
}

namespace {
const Value* first_rows(const Value& n) {
  if (!n.is_map()) return nullptr;
  const auto t = n["t"].str();
  if (is_rows(t) && !n["id"].str().empty()) return &n;
  if (t == "modal") return first_rows(n["dialog"]);  // the base is inert behind it
  for (const auto& c : n["children"].array()) {
    if (const Value* hit = first_rows(c)) return hit;
  }
  return n["child"].is_nil() ? nullptr : first_rows(n["child"]);
}
}  // namespace

std::string ViewWidget::focus() const {
  if (!focus_.empty() && widgets_.contains(focus_)) {
    // Not one hidden behind a dialog.
    const Value* m = find_modal(tree_);
    if (!m || find_widget((*m)["dialog"], focus_)) return focus_;
  }
  const Value* r = first_rows(tree_);
  return r ? std::string((*r)["id"].str()) : "";
}

void ViewWidget::set_focus(const std::string& id) {
  if (!id.empty()) focus_ = id;
}

int ViewWidget::cursor(std::string_view widget) const {
  const auto it = state_.find(std::string(widget));
  return it == state_.end() ? 0 : it->second.cursor;
}

void ViewWidget::set_cursor(const std::string& widget, int index) {
  const auto it = widgets_.find(widget);
  if (it == widgets_.end() || !is_rows(it->second->type)) return;
  focus_ = widget;
  static_cast<RowsNode*>(it->second)->set_cursor(index);
}

Value ViewWidget::form() const {
  Map out;
  for_each_node(tree_, [&](const Value& f) {
    if (f["t"].str() != "field") return;
    const std::string id(f["id"].str());
    const auto it = widgets_.find(id);
    if (it != widgets_.end() && it->second->type == "field") out.emplace_back(id, static_cast<FieldNode*>(it->second)->text());
    else out.emplace_back(id, f["value"]);
  });
  return out;
}

void ViewWidget::focus_widget(const std::string& id) {
  const auto it = widgets_.find(id);
  if (it == widgets_.end()) return;
  focus_ = id;
  it->second->grab();
}

void ViewWidget::grab_focus() {
  const auto it = widgets_.find(focus());
  if (it != widgets_.end() && it->second->grab()) return;
  gtk_widget_child_focus(root_, GTK_DIR_TAB_FORWARD);
}

GtkWidget* ViewWidget::cursor_widget(double& x, double& y) const {
  const auto it = widgets_.find(focus());
  if (it == widgets_.end()) return nullptr;
  GtkWidget* t = it->second->focus_target();
  x = 32;
  y = 16;
  // At the focused row when the focus is in there.
  if (GtkRoot* root = gtk_widget_get_root(t)) {
    if (GtkWidget* f = gtk_root_get_focus(root); f && gtk_widget_is_ancestor(f, t)) {
      graphene_rect_t b;
      if (gtk_widget_compute_bounds(f, t, &b)) {
        x = b.origin.x + 32;
        y = b.origin.y + b.size.height / 2;
      }
    }
  }
  return t;
}

}  // namespace rcp::gtk
