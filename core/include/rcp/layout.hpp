// The UI IR (see ARCHITECTURE.md) laid out into a Grid. Transient widget state (list
// cursor, field text, focus) is kept here, in the host, not in the plugin.
#pragma once

#include <functional>
#include <optional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "rcp/grid.hpp"
#include "rcp/msgpack.hpp"

namespace rcp {

struct WidgetState {
  int cursor = 0;  // list/table: row index
  int scroll = 0;  // list/table/log: first visible row; field: first visible column
  int page = 1;    // list/table/log: rows visible at the last render
  bool follow = true;  // log: stick to the end
  std::string text;  // field: current text
  size_t caret = 0;  // field: byte offset into text
  bool seen = false;
  std::string seen_value;  // last list.selected / table.cursor / field.value applied
  std::string seen_follow;  // table.follow (the cursor's row after a sort) last applied
  // A sort asked for and the table.sorted it was asked from: until the tree changes
  // from that, the request is how it is sorted (keys come faster than the plugin).
  std::string sort_from;
  int sort_col = -1;
  bool sort_desc = false;
  // table: rows the window lacks (first < 0: none), and the last such request sent
  int want_first = -1, want_count = 0;
  int sent_first = -1, sent_count = 0;
};

// Where a widget was drawn, for mouse clicks. Recorded at every render; widgets under
// a modal's dialog are not recorded.
struct Hit {
  std::string id, type;
  Rect rect;              // list/table/tree: the rows (without a table's header; a
                          // sortable table's header is a hit of its own, type "header")
  int first = 0;          // list/table/tree/log: the row shown at rect.y
  std::vector<int> col_x;  // table: where each column starts
};

struct ViewState {
  std::map<std::string, WidgetState, std::less<>> widgets;
  std::string focus;  // widget id
  // The focus fell back to the first widget (nothing focused, nothing marked autofocus):
  // a widget marked autofocus takes it when it appears. Tab and clicks clear this.
  bool focus_fallback = false;
  std::vector<std::string> focus_order;
  std::vector<Hit> hits;  // in drawing order: a later one is on top
};

// Draws tree into r. `active`: this view has keyboard focus (styles the focused widget).
void render_view(Grid& g, Rect r, const mp::Value& tree, ViewState& vs, bool active);

// Draws a single-line box with an optional title in the top border.
void draw_box(Grid& g, Rect r, std::string_view title, Style border, Style title_style);

// The interactive node with this id, or nullptr.
const mp::Value* find_widget(const mp::Value& tree, std::string_view id);

// The innermost modal in the tree, or nullptr. While one is shown only its dialog
// takes focus.
const mp::Value* find_modal(const mp::Value& tree);

// Calls fn for every node (children, panel/modal child, modal dialog).
void for_each_node(const mp::Value& tree, const std::function<void(const mp::Value&)>& fn);

// Rows of a table/list: its "total", else the number of items/rows sent.
int row_count(const mp::Value& node);
// The row at absolute index i of a (windowed) table or list, or nullptr when not sent.
const mp::Value* row_at(const mp::Value& node, int i);

// The selection a view offers (the workbench's selection service): the `sel` items of
// the cursor row of its focused rows widget (else of the first rows widget whose rows
// carry any), else the tree's own `sel`. An empty array: it offers selections, but not
// on this row. nullopt: it offers none, so focusing it leaves the selection as it was.
std::optional<mp::Value> view_selection(const mp::Value& tree, std::string_view focus,
                                        const std::function<int(std::string_view)>& cursor_of);

// The key of a list item: its "key", else its text when it is a plain string, else its index.
std::string item_key(const mp::Value& item, size_t index);

// Applies list.selected / field.value when they changed since last seen.
void sync_widget(const mp::Value& node, WidgetState& ws);

}  // namespace rcp
