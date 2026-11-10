// The UI IR as GTK widgets. Each IR node becomes the GTK widget for its concept (a list
// or tree a GtkListView, a table a GtkColumnView, a panel a frame, a modal a dialog
// window) and the tree is reconciled in place as the plugin sends new ones, so focus,
// scrolling and selections stay. What the user does comes back as the same semantic
// events the terminal workbench sends.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <gtk/gtk.h>

#include "rcp/msgpack.hpp"

namespace rcp::gtk {

// What a view's widgets ask of the workbench.
struct ViewHost {
  virtual ~ViewHost() = default;
  virtual void event(const std::string& view, std::string_view widget, std::string_view kind, mp::Value value) = 0;
  virtual void cursor_moved(const std::string& view) = 0;  // the selection follows
  virtual void focused(const std::string& view) = 0;       // keyboard focus went into it
  // A right click (or the menu key) on a row: the view's context menu, at x, y in `at`.
  virtual void context_menu(const std::string& view, GtkWidget* at, double x, double y) = 0;
};

// Semantic styles (normal dim bold title accent error warning ok) as Pango markup
// colours; picked to read on light or dark backgrounds.
void set_dark(bool dark);
// The width of a character and the height of a line of the UI font, for the IR's
// width/height hints (given in characters and lines).
void set_metrics(int char_px, int line_px);
// Pango markup of a cell, log line or text: a string, {text, style} or [[text, style], ...].
std::string markup(const mp::Value& c, std::string_view base = "normal");

struct Node;

class ViewWidget {
 public:
  ViewWidget(ViewHost& host, std::string view);
  ~ViewWidget();
  ViewWidget(const ViewWidget&) = delete;
  ViewWidget& operator=(const ViewWidget&) = delete;

  GtkWidget* widget() const { return root_; }
  const std::string& view() const { return view_; }
  void update(const mp::Value& tree, bool loading);

  // The widget commands act on: the rows widget (or field) that last had the focus,
  // else the first rows widget.
  std::string focus() const;
  int cursor(std::string_view widget) const;  // a rows widget's cursor row; 0 when none
  // Moves a rows widget's cursor (Back/Forward), telling the plugin.
  void set_cursor(const std::string& widget, int index);
  mp::Value form() const;  // every field's text, by id
  void grab_focus();       // into the focused widget, else the first focusable one
  void focus_widget(const std::string& id);  // an autofocus widget, once drawn
  // The focused rows widget and the screen point of its cursor row, for a context menu.
  GtkWidget* cursor_widget(double& x, double& y) const;

  // Per widget id: what survives a widget being built again.
  struct State {
    int cursor = 0;
    std::string cursor_seen, follow_seen, value_seen;
    bool autofocused = false;
  };
  ViewHost& host() { return host_; }
  State& state(const std::string& id) { return state_[id]; }
  void set_focus(const std::string& id);
  const mp::Value& tree() const { return tree_; }
  std::weak_ptr<ViewWidget*> alive() const { return alive_; }  // empty once it is gone
  std::map<std::string, Node*>& widgets() { return widgets_; }

 private:
  ViewHost& host_;
  std::string view_;
  GtkWidget* root_;  // a box holding the tree's widget (we hold a reference)
  std::unique_ptr<Node> node_;
  mp::Value tree_;
  std::map<std::string, State> state_;
  std::map<std::string, Node*> widgets_;  // the interactive nodes now built, by id
  std::string focus_;
  std::shared_ptr<ViewWidget*> alive_ = std::make_shared<ViewWidget*>(this);
};

}  // namespace rcp::gtk
