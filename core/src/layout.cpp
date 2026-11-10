#include "rcp/layout.hpp"

#include <algorithm>
#include <cmath>

#include "rcp/utf8.hpp"

namespace rcp {

namespace {

using mp::Value;

std::string_view type_of(const Value& n) { return n["t"].str(); }
const mp::Array& children_of(const Value& n) { return n["children"].array(); }

Style style_of(const Value& v, Style def) {
  auto s = style_from_name(v.str());
  return s ? *s : def;
}

bool is_focusable(std::string_view t) {
  return t == "list" || t == "button" || t == "field" || t == "table" || t == "tree" || t == "checkbox" ||
         t == "log";
}

void collect_focusables(const Value& n, std::vector<std::string>& out) {
  if (type_of(n) == "modal") return collect_focusables(n["dialog"], out);  // the base is inert
  if (is_focusable(type_of(n)) && !n["id"].str().empty()) out.emplace_back(n["id"].str());
  for (const auto& c : children_of(n)) collect_focusables(c, out);
  if (!n["child"].is_nil()) collect_focusables(n["child"], out);
}

// The first focusable widget marked autofocus (inside the dialog when there is a modal).
std::string_view autofocus_id(const Value& n) {
  if (type_of(n) == "modal") return autofocus_id(n["dialog"]);
  if (is_focusable(type_of(n)) && n["autofocus"].boolean() && !n["id"].str().empty()) return n["id"].str();
  for (const auto& c : children_of(n)) {
    if (auto id = autofocus_id(c); !id.empty()) return id;
  }
  return n["child"].is_nil() ? std::string_view() : autofocus_id(n["child"]);
}

bool contains_id(const Value& n, std::string_view id) {
  if (id.empty()) return false;
  if (n["id"].str() == id && is_focusable(type_of(n))) return true;
  for (const auto& c : children_of(n)) {
    if (contains_id(c, id)) return true;
  }
  if (!n["dialog"].is_nil() && contains_id(n["dialog"], id)) return true;
  return !n["child"].is_nil() && contains_id(n["child"], id);
}

// Columns a cell takes.
int cell_width(const Value& c) {
  if (c.is_str()) return utf8::width(c.str());
  if (c.is_map()) return utf8::width(c["text"].str());
  int w = 0;
  for (const auto& s : c.array()) w += utf8::width(s.is_str() ? s.str() : s.at(0).str());
  return w;
}

// A table cell or log line: a string, {text, style}, or spans [[text, style], ...].
void draw_cell(Grid& g, const Value& c, int x, int y, int w, Style base, bool force, bool right) {
  if (w <= 0) return;
  std::vector<std::pair<std::string_view, Style>> spans;
  if (c.is_str()) {
    spans.emplace_back(c.str(), base);
  } else if (c.is_map()) {
    spans.emplace_back(c["text"].str(), force ? base : style_of(c["style"], base));
  } else {
    for (const auto& s : c.array()) {
      if (s.is_str()) spans.emplace_back(s.str(), base);
      else spans.emplace_back(s.at(0).str(), force ? base : style_of(s.at(1), base));
    }
  }
  int cx = x;
  if (right) {
    int total = 0;
    for (const auto& [t, st] : spans) total += utf8::width(t);
    cx = x + std::max(0, w - total);
  }
  for (const auto& [t, st] : spans) cx = g.text(cx, y, t, st, x + w);
}

// ---- text ----------------------------------------------------------------------

struct Span {
  std::string text;
  Style style;
};
using Line = std::vector<Span>;

// Splits s into chunks of at most w columns.
void hard_split(std::string_view s, int w, std::vector<std::string>& out) {
  std::string cur;
  int cw = 0;
  for (size_t i = 0; i < s.size();) {
    const size_t start = i;
    const int chw = utf8::width(utf8::decode(s, i));
    if (cw + chw > w && !cur.empty()) {
      out.push_back(std::move(cur));
      cur.clear();
      cw = 0;
    }
    cur.append(s.substr(start, i - start));
    cw += chw;
  }
  out.push_back(std::move(cur));
}

std::vector<std::string> wrap(std::string_view para, int w) {
  std::vector<std::string> lines;
  if (w <= 0) return {std::string(para)};
  std::string line;
  int lw = 0;
  size_t i = 0;
  while (i <= para.size()) {
    size_t e = para.find(' ', i);
    if (e == std::string_view::npos) e = para.size();
    std::string_view word = para.substr(i, e - i);
    const int ww = utf8::width(word);
    if (lw > 0 && lw + 1 + ww <= w) {
      line += ' ';
      line += word;
      lw += 1 + ww;
    } else {
      if (lw > 0 || !line.empty()) lines.push_back(std::move(line));
      line.clear();
      lw = 0;
      if (ww > w) {
        std::vector<std::string> parts;
        hard_split(word, w, parts);
        for (size_t k = 0; k + 1 < parts.size(); ++k) lines.push_back(std::move(parts[k]));
        line = std::move(parts.back());
        lw = utf8::width(line);
      } else {
        line = std::string(word);
        lw = ww;
      }
    }
    i = e + 1;
  }
  lines.push_back(std::move(line));
  return lines;
}

// Wraps a line of styled spans at spaces, each word keeping its style; a word wider
// than the line is split.
std::vector<Line> wrap_spans(const Line& line, int w) {
  struct Word {
    Line pieces;  // a word can change style midway ("(value" + "1.7 V")
    int width = 0;
  };
  std::vector<Word> words(1);
  for (const auto& span : line) {
    size_t i = 0;
    while (i <= span.text.size()) {
      size_t e = span.text.find(' ', i);
      if (e == std::string::npos) e = span.text.size();
      if (e > i) {
        std::string_view piece = std::string_view(span.text).substr(i, e - i);
        words.back().pieces.push_back({std::string(piece), span.style});
        words.back().width += utf8::width(piece);
      }
      if (e == span.text.size()) break;
      if (!words.back().pieces.empty()) words.emplace_back();
      i = e + 1;
    }
  }
  std::vector<Line> out(1);
  int lw = 0;
  for (auto& word : words) {
    if (word.pieces.empty()) continue;
    if (word.width > w) {  // wider than any line: from a line of its own, cut to width
      if (lw > 0) out.emplace_back();
      std::string all;
      for (const auto& piece : word.pieces) all += piece.text;
      std::vector<std::string> parts;
      hard_split(all, w, parts);
      for (size_t k = 0; k < parts.size(); ++k) {
        if (k > 0) out.emplace_back();
        out.back().push_back({std::move(parts[k]), word.pieces.front().style});
      }
      lw = utf8::width(out.back().back().text);
      continue;
    }
    if (lw > 0 && lw + 1 + word.width > w) {
      out.emplace_back();
      lw = 0;
    }
    if (lw > 0) {
      out.back().push_back({" ", word.pieces.front().style});
      lw += 1;
    }
    for (auto& piece : word.pieces) out.back().push_back(std::move(piece));
    lw += word.width;
  }
  return out;
}

std::vector<Line> text_lines(const Value& n, int w) {
  const Style base = style_of(n["style"], Style::normal);
  std::vector<Line> lines(1);
  const Value& content = n["text"];
  if (content.is_array()) {  // spans: [[text, style], ...]
    for (const auto& span : content.array()) {
      std::string_view s = span.is_str() ? span.str() : span.at(0).str();
      const Style st = span.is_str() ? base : style_of(span.at(1), base);
      size_t i = 0;
      while (true) {
        const size_t e = s.find('\n', i);
        lines.back().push_back({std::string(s.substr(i, e == std::string_view::npos ? s.npos : e - i)), st});
        if (e == std::string_view::npos) break;
        lines.emplace_back();
        i = e + 1;
      }
    }
    if (!n["wrap"].boolean() || w <= 0) return lines;
    std::vector<Line> wrapped;
    for (const auto& l : lines) {
      for (auto& wl : wrap_spans(l, w)) wrapped.push_back(std::move(wl));
    }
    return wrapped;
  }
  lines.clear();
  std::string_view s = content.str();
  const bool do_wrap = n["wrap"].boolean();
  size_t i = 0;
  while (true) {
    const size_t e = s.find('\n', i);
    std::string_view para = s.substr(i, e == std::string_view::npos ? s.npos : e - i);
    if (do_wrap) {
      for (auto& l : wrap(para, w)) lines.push_back({{std::move(l), base}});
    } else {
      lines.push_back({{std::string(para), base}});
    }
    if (e == std::string_view::npos) break;
    i = e + 1;
  }
  return lines;
}

int line_width(const Line& l) {
  int w = 0;
  for (const auto& s : l) w += utf8::width(s.text);
  return w;
}

// ---- measuring -----------------------------------------------------------------

int grow_of(const Value& n, bool in_hbox);

bool any_child_grows(const Value& n) {
  for (const auto& c : children_of(n)) {
    if (grow_of(c, false) > 0) return true;
  }
  return false;
}

// Flex weight. Lists (and containers holding one) grow in a vbox; in an hbox everything
// except text and buttons shares the width.
int grow_of(const Value& n, bool in_hbox) {
  const Value& g = n["grow"];
  if (!g.is_nil()) return int(std::max<int64_t>(0, g.integer()));
  const auto t = type_of(n);
  if (in_hbox) return (t == "text" || t == "button" || t == "checkbox") ? 0 : 1;
  if (t == "list" || t == "table" || t == "tree" || t == "log") return 1;
  if (t == "panel" || t == "modal") return grow_of(n["child"], false);
  if (t == "vbox" || t == "hbox") return any_child_grows(n) ? 1 : 0;
  return 0;
}

int natural_w(const Value& n) {
  if (!n["width"].is_nil()) return int(n["width"].integer());
  const auto t = type_of(n);
  if (t == "text") {
    int w = 0;
    for (const auto& l : text_lines(n, 0)) w = std::max(w, line_width(l));
    return w;
  }
  if (t == "button" || t == "checkbox") return utf8::width(n["label"].str()) + 4;
  return 0;
}

std::vector<int> hbox_widths(const Value& n, int w);

int natural_h(const Value& n, int w) {
  if (!n["height"].is_nil()) return int(std::max<int64_t>(0, n["height"].integer()));
  const auto t = type_of(n);
  if (t == "text") return int(text_lines(n, w).size());
  if (t == "list") return std::max<int>(1, int(n["items"].array().size()));
  if (t == "table") return 1 + std::max(1, row_count(n));
  if (t == "tree") return std::max(1, row_count(n));
  if (t == "log") return std::max<int>(1, int(n["lines"].array().size()));
  if (t == "panel") return natural_h(n["child"], w - 2) + 2;
  if (t == "modal") return natural_h(n["child"], w);
  if (t == "vbox") {
    const auto& kids = children_of(n);
    int h = int(n["gap"].integer(0)) * std::max(0, int(kids.size()) - 1);
    for (const auto& c : kids) h += natural_h(c, w);
    return h;
  }
  if (t == "hbox") {
    const auto widths = hbox_widths(n, w);
    int h = 0;
    const auto& kids = children_of(n);
    for (size_t i = 0; i < kids.size(); ++i) h = std::max(h, natural_h(kids[i], widths[i]));
    return h;
  }
  return 1;
}

// Splits `total` among weights; the last weighted entry gets the rounding remainder.
void distribute(std::vector<int>& sizes, const std::vector<int>& weights, int total) {
  int sum = 0;
  for (int wgt : weights) sum += wgt;
  if (sum == 0) return;
  int left = std::max(0, total), last = -1;
  const int avail = left;
  for (size_t i = 0; i < weights.size(); ++i) {
    if (weights[i] == 0) continue;
    sizes[i] = avail * weights[i] / sum;
    left -= sizes[i];
    last = int(i);
  }
  if (last >= 0) sizes[size_t(last)] += left;
}

std::vector<int> hbox_widths(const Value& n, int w) {
  const auto& kids = children_of(n);
  const int gap = int(n["gap"].integer(1));
  std::vector<int> widths(kids.size(), 0), weights(kids.size(), 0);
  int fixed = gap * std::max(0, int(kids.size()) - 1);
  for (size_t i = 0; i < kids.size(); ++i) {
    const int g = kids[i]["width"].is_nil() ? grow_of(kids[i], true) : 0;
    if (g > 0) {
      weights[i] = g;
    } else {
      widths[i] = natural_w(kids[i]);
      fixed += widths[i];
    }
  }
  distribute(widths, weights, w - fixed);
  return widths;
}

// ---- drawing -------------------------------------------------------------------

struct Painter {
  Grid& g;
  ViewState& vs;
  bool active;
  bool inert = false;  // drawing a modal's base: no focus, no click targets

  void hit(const Value& n, Rect r, int first = 0, std::vector<int> col_x = {}) {
    hit_as(n, type_of(n), r, std::move(col_x), first);
  }
  void hit_as(const Value& n, std::string_view type, Rect r, std::vector<int> col_x = {}, int first = 0) {
    if (inert || r.empty() || n["id"].str().empty()) return;
    vs.hits.push_back({std::string(n["id"].str()), std::string(type), r, first, std::move(col_x)});
  }

  bool focused(const Value& n) const { return active && !vs.focus.empty() && n["id"].str() == vs.focus; }

  void draw(const Value& n, Rect r) {
    if (r.empty()) return;
    const auto t = type_of(n);
    if (t == "text") text(n, r);
    else if (t == "vbox") vbox(n, r);
    else if (t == "hbox") hbox(n, r);
    else if (t == "panel") panel(n, r);
    else if (t == "list") list(n, r);
    else if (t == "button") button(n, r);
    else if (t == "field") field(n, r);
    else if (t == "progress") progress(n, r);
    else if (t == "table") table(n, r);
    else if (t == "tree") tree(n, r);
    else if (t == "checkbox") checkbox(n, r);
    else if (t == "log") log(n, r);
    else if (t == "modal") modal(n, r);
    else g.text(r.x, r.y, "<unknown node '" + std::string(t) + "'>", Style::error, r.x + r.w);
  }

  void text(const Value& n, Rect r) {
    const auto lines = text_lines(n, r.w);
    for (int i = 0; i < r.h && i < int(lines.size()); ++i) {
      int x = r.x;
      for (const auto& s : lines[size_t(i)]) x = g.text(x, r.y + i, s.text, s.style, r.x + r.w);
    }
  }

  void vbox(const Value& n, Rect r) {
    const auto& kids = children_of(n);
    const int gap = int(n["gap"].integer(0));
    std::vector<int> heights(kids.size(), 0), weights(kids.size(), 0);
    int fixed = gap * std::max(0, int(kids.size()) - 1);
    for (size_t i = 0; i < kids.size(); ++i) {
      const int gr = kids[i]["height"].is_nil() ? grow_of(kids[i], false) : 0;
      if (gr > 0) {
        weights[i] = gr;
      } else {
        heights[i] = natural_h(kids[i], r.w);
        fixed += heights[i];
      }
    }
    distribute(heights, weights, r.h - fixed);
    int y = r.y;
    for (size_t i = 0; i < kids.size() && y < r.y + r.h; ++i) {
      const int h = std::min(heights[i], r.y + r.h - y);
      draw(kids[i], {r.x, y, r.w, h});
      y += heights[i] + gap;
    }
  }

  void hbox(const Value& n, Rect r) {
    const auto& kids = children_of(n);
    const auto widths = hbox_widths(n, r.w);
    const int gap = int(n["gap"].integer(1));
    int x = r.x;
    for (size_t i = 0; i < kids.size() && x < r.x + r.w; ++i) {
      const int w = std::min(widths[i], r.x + r.w - x);
      draw(kids[i], {x, r.y, w, r.h});
      x += widths[i] + gap;
    }
  }

  void panel(const Value& n, Rect r) {
    if (r.w < 3 || r.h < 3) return draw(n["child"], r);
    const bool inside = active && contains_id(n["child"], vs.focus);
    draw_box(g, r, n["title"].str(), inside ? Style::border_focus : Style::border,
             inside ? Style::border_focus : Style::title);
    draw(n["child"], {r.x + 1, r.y + 1, r.w - 2, r.h - 2});
  }

  void list(const Value& n, Rect r) {
    const auto& items = n["items"].array();
    WidgetState& ws = vs.widgets[std::string(n["id"].str())];
    sync_widget(n, ws);
    const int count = int(items.size());
    ws.page = r.h;
    if (count == 0) {
      ws.cursor = ws.scroll = 0;
      g.text(r.x + 1, r.y, n["empty"].str(), Style::dim, r.x + r.w);
      return;
    }
    ws.cursor = std::clamp(ws.cursor, 0, count - 1);
    if (ws.cursor < ws.scroll) ws.scroll = ws.cursor;
    if (ws.cursor >= ws.scroll + r.h) ws.scroll = ws.cursor - r.h + 1;
    ws.scroll = std::clamp(ws.scroll, 0, std::max(0, count - r.h));
    const bool foc = focused(n);
    hit(n, r, ws.scroll);
    for (int row = 0; row < r.h && ws.scroll + row < count; ++row) {
      const int idx = ws.scroll + row;
      const Value& item = items[size_t(idx)];
      const int y = r.y + row;
      const bool cur = idx == ws.cursor;
      const Style row_style = foc ? Style::selected : Style::selected_inactive;
      const Style st = cur ? row_style : style_of(item["style"], Style::normal);
      if (cur) g.fill({r.x, y, r.w, 1}, st);
      std::string_view label = item.is_str() ? item.str() : item["text"].str();
      std::string_view detail = item["detail"].str();
      const int end = g.text(r.x + 1, y, label, st, r.x + r.w);
      if (!detail.empty()) {
        const int dx = r.x + r.w - 1 - utf8::width(detail);
        if (dx > end) g.text(dx, y, detail, cur ? st : Style::dim, r.x + r.w);
      }
    }
    if (count > r.h && r.w > 2) {  // scroll position marker
      const int y = r.y + int(std::lround(double(ws.scroll) / std::max(1, count - r.h) * (r.h - 1)));
      g.put(r.x + r.w - 1, y, U'▐', Style::dim, r.x + r.w);
    }
  }

  // Cursor and scroll over `count` rows with `h` visible.
  static void clamp_cursor(WidgetState& ws, int count, int h) {
    ws.cursor = std::clamp(ws.cursor, 0, std::max(0, count - 1));
    if (ws.cursor < ws.scroll) ws.scroll = ws.cursor;
    if (ws.cursor >= ws.scroll + h) ws.scroll = ws.cursor - h + 1;
    ws.scroll = std::clamp(ws.scroll, 0, std::max(0, count - h));
  }

  void scroll_marker(Rect r, int scroll, int count) {
    if (count > r.h && r.w > 2 && r.h > 0) {
      const int y = r.y + int(std::lround(double(scroll) / std::max(1, count - r.h) * (r.h - 1)));
      g.put(r.x + r.w - 1, y, U'▐', Style::dim, r.x + r.w);
    }
  }

  void table(const Value& n, Rect r) {
    WidgetState& ws = vs.widgets[std::string(n["id"].str())];
    sync_widget(n, ws);
    const auto& cols = n["columns"].array();
    const auto& rows = n["rows"].array();
    const int total = row_count(n);
    const int offset = int(n["offset"].integer(0));
    const int tree_col = n["tree"].is_nil() ? -1 : int(n["tree"].integer(-1));

    std::vector<int> widths(cols.size(), 0), weights(cols.size(), 0);
    int fixed = std::max(0, int(cols.size()) - 1);
    for (size_t i = 0; i < cols.size(); ++i) {
      if (!cols[i]["width"].is_nil()) {
        widths[i] = int(std::max<int64_t>(0, cols[i]["width"].integer()));
        fixed += widths[i];
      } else {
        weights[i] = int(std::max<int64_t>(0, cols[i]["grow"].integer(1)));
      }
    }
    distribute(widths, weights, r.w - fixed);
    auto right = [&](size_t i) { return cols[i]["align"].str() == "right"; };

    std::vector<int> col_x;
    for (size_t i = 0, x = size_t(r.x); i < cols.size(); x += size_t(widths[i]) + 1, ++i) col_x.push_back(int(x));
    int y = r.y;
    if (n["header"].boolean(true) && r.h >= 2) {
      const Value& sorted = n["sorted"];
      const int sorted_col = sorted.is_array() ? int(sorted.at(0).integer(-1)) : -1;
      int x = r.x;
      for (size_t i = 0; i < cols.size(); ++i) {
        const int w = std::min(widths[i], r.x + r.w - x);
        draw_cell(g, cols[i]["title"], x, y, w, Style::title, false, right(i));
        if (int(i) == sorted_col && w >= 2) {
          // ▲ (ascending) or ▼ just after the title; before it when right-aligned.
          const int tw = cell_width(cols[i]["title"]);
          const int ax = std::clamp(right(i) ? x + w - tw - 2 : x + tw + 1, x, x + w - 1);  // a full title: over its end
          g.text(ax, y, sorted.at(1).boolean() ? "▼" : "▲", Style::title, x + w);
        }
        x += widths[i] + 1;
      }
      if (n["sortable"].boolean()) hit_as(n, "header", {r.x, y, r.w, 1}, col_x);
      ++y;
    }
    const Rect body{r.x, y, r.w, r.y + r.h - y};
    ws.page = std::max(1, body.h);
    if (total > 0) clamp_cursor(ws, total, body.h);
    hit(n, body, ws.scroll, col_x);
    if (total == 0) {
      ws.cursor = ws.scroll = 0;
      ws.want_first = -1;
      g.text(body.x + 1, body.y, n["empty"].str(), Style::dim, body.x + body.w);
      return;
    }
    clamp_cursor(ws, total, body.h);
    const bool foc = focused(n);
    bool missing = false;
    for (int row = 0; row < body.h && ws.scroll + row < total; ++row) {
      const int idx = ws.scroll + row;
      const int yy = body.y + row;
      const bool cur = idx == ws.cursor;  // shown in an unfocused region too, inactive
      const Style rs = foc ? Style::selected : Style::selected_inactive;
      if (cur) g.fill({body.x, yy, body.w, 1}, rs);
      const int wi = idx - offset;
      if (wi < 0 || wi >= int(rows.size())) {
        g.text(body.x, yy, "…", cur ? rs : Style::dim, body.x + body.w);
        missing = true;
        continue;
      }
      const Value& rowv = rows[size_t(wi)];
      const Style base = cur ? rs : style_of(rowv["style"], Style::normal);
      int x = body.x;
      for (size_t i = 0; i < cols.size() && x < body.x + body.w; ++i) {
        const int w = std::min(widths[i], body.x + body.w - x);
        if (int(i) == tree_col) {  // indent, ▶/▼, then the cell
          const int ind = std::min(w, 2 * int(std::max<int64_t>(0, rowv["depth"].integer())));
          if (rowv["expandable"].boolean() && ind < w)
            g.put(x + ind, yy, rowv["expanded"].boolean() ? U'▼' : U'▶', cur ? rs : Style::dim, x + w);
          const int skip = std::min(w, ind + 2);
          draw_cell(g, rowv["cells"].at(i), x + skip, yy, w - skip, base, cur, right(i));
        } else {
          draw_cell(g, rowv["cells"].at(i), x, yy, w, base, cur, right(i));
        }
        x += widths[i] + 1;
      }
    }
    if (missing) {
      ws.want_first = std::max(0, ws.scroll - body.h);
      ws.want_count = body.h * 3 + 50;
    } else {
      ws.want_first = -1;
    }
    scroll_marker(body, ws.scroll, total);
  }

  // A tree: rows {key, text, detail, depth, expandable, expanded, check, style}, flattened
  // by the plugin (it owns which folders are open), windowed like a table.
  void tree(const Value& n, Rect r) {
    WidgetState& ws = vs.widgets[std::string(n["id"].str())];
    sync_widget(n, ws);
    const auto& rows = n["rows"].array();
    const int total = row_count(n);
    const int offset = int(n["offset"].integer(0));
    ws.page = std::max(1, r.h);
    if (total == 0) {
      ws.cursor = ws.scroll = 0;
      ws.want_first = -1;
      g.text(r.x + 1, r.y, n["empty"].str(), Style::dim, r.x + r.w);
      return;
    }
    clamp_cursor(ws, total, r.h);
    hit(n, r, ws.scroll);
    const bool foc = focused(n);
    bool missing = false;
    const int xe = r.x + r.w - (total > r.h ? 1 : 0);
    for (int row = 0; row < r.h && ws.scroll + row < total; ++row) {
      const int idx = ws.scroll + row;
      const int y = r.y + row;
      const bool cur = idx == ws.cursor;  // shown in an unfocused region too, inactive
      const Style rs = foc ? Style::selected : Style::selected_inactive;
      if (cur) g.fill({r.x, y, xe - r.x, 1}, rs);
      const int wi = idx - offset;
      if (wi < 0 || wi >= int(rows.size())) {
        g.text(r.x, y, "…", cur ? rs : Style::dim, xe);
        missing = true;
        continue;
      }
      const Value& rv = rows[size_t(wi)];
      const Style base = cur ? rs : style_of(rv["style"], Style::normal);
      int x = r.x + 2 * int(std::max<int64_t>(0, rv["depth"].integer()));
      if (rv["expandable"].boolean()) g.put(x, y, rv["expanded"].boolean() ? U'▼' : U'▶', cur ? rs : Style::dim, xe);
      x += 2;
      const auto check = rv["check"].str();
      if (!check.empty()) {
        const bool all = check == "all", some = check == "some";
        const Style cs = cur ? rs : all ? Style::ok : some ? Style::warning : Style::dim;
        x = g.text(x, y, all ? "[x]" : some ? "[-]" : "[ ]", cs, xe) + 1;
      }
      draw_cell(g, rv["text"], x, y, xe - x, base, cur, false);
      const int dx = x + cell_width(rv["text"]) + 2;
      if (!rv["detail"].is_nil() && dx < xe) draw_cell(g, rv["detail"], dx, y, xe - dx, cur ? rs : Style::dim, cur, false);
    }
    if (missing) {
      ws.want_first = std::max(0, ws.scroll - r.h);
      ws.want_count = r.h * 3 + 50;
    } else {
      ws.want_first = -1;
    }
    scroll_marker(r, ws.scroll, total);
  }

  void checkbox(const Value& n, Rect r) {
    hit(n, {r.x, r.y, std::min(r.w, natural_w(n)), 1});
    const bool on = n["checked"].boolean();
    const bool foc = focused(n);
    const int x = g.text(r.x, r.y, on ? "[x] " : "[ ] ", foc ? Style::selected : Style::normal, r.x + r.w);
    if (on && !foc) g.put(r.x + 1, r.y, U'x', Style::accent, r.x + r.w);
    g.text(x, r.y, n["label"].str(), foc ? Style::selected : Style::normal, r.x + r.w);
  }

  void log(const Value& n, Rect r) {
    const auto& lines = n["lines"].array();
    const int count = int(lines.size());
    const int max_top = std::max(0, count - r.h);
    int top = max_top;
    if (!n["id"].str().empty()) {
      WidgetState& ws = vs.widgets[std::string(n["id"].str())];
      ws.page = r.h;
      if (ws.follow) ws.scroll = max_top;
      ws.scroll = std::clamp(ws.scroll, 0, max_top);
      top = ws.scroll;
    }
    hit(n, r, top);
    for (int row = 0; row < r.h && top + row < count; ++row)
      draw_cell(g, lines[size_t(top + row)], r.x, r.y + row, r.w - (count > r.h ? 1 : 0), Style::normal, false, false);
    if (focused(n)) scroll_marker(r, top, count);
  }

  void modal(const Value& n, Rect r) {
    const bool was_active = active, was_inert = inert;
    active = false;  // the base shows no focus and takes no clicks
    inert = true;
    draw(n["child"], r);
    active = was_active;
    inert = was_inert;
    g.restyle(r, Style::dim);
    if (r.w < 10 || r.h < 5) return;
    const Value& d = n["dialog"];
    const int w = std::min(r.w - 2, int(n["dialog_width"].integer(std::min(76, r.w - 4))));
    const int h = n["dialog_height"].is_nil() ? std::min(r.h - 2, natural_h(d, w - 2) + 2)
                                              : std::min(r.h - 2, int(n["dialog_height"].integer()));
    const Rect box{r.x + (r.w - w) / 2, r.y + std::max(1, (r.h - h) / 3), w, h};
    g.fill(box, Style::normal);
    draw_box(g, box, n["title"].str(), Style::border_focus, Style::border_focus);
    draw(d, {box.x + 1, box.y + 1, box.w - 2, box.h - 2});
    g.set_layer(box, Layer::dialog);
    g.shadow(box);
  }

  void button(const Value& n, Rect r) {
    const std::string label = "[ " + std::string(n["label"].str()) + " ]";
    hit(n, {r.x, r.y, std::min(r.w, utf8::width(label)), 1});
    g.text(r.x, r.y, label, focused(n) ? Style::button_focus : Style::button, r.x + r.w);
  }

  void field(const Value& n, Rect r) {
    WidgetState& ws = vs.widgets[std::string(n["id"].str())];
    sync_widget(n, ws);
    hit(n, {r.x, r.y, r.w, 1});
    const bool foc = focused(n);
    g.fill({r.x, r.y, r.w, 1}, Style::input);
    if (ws.text.empty()) {
      g.text(r.x, r.y, n["placeholder"].str(), Style::dim, r.x + r.w);
      if (foc) g.put(r.x, r.y, U' ', Style::selected, r.x + r.w);
      return;
    }
    // Scroll horizontally so the caret stays visible.
    const int caret_col = utf8::width(std::string_view(ws.text).substr(0, ws.caret));
    if (caret_col - ws.scroll >= r.w) ws.scroll = caret_col - r.w + 1;
    if (caret_col < ws.scroll) ws.scroll = caret_col;
    int col = 0;
    for (size_t i = 0; i < ws.text.size();) {
      const size_t at = i;
      const char32_t c = utf8::decode(ws.text, i);
      const int cw = utf8::width(c);
      if (col >= ws.scroll) {
        const Style st = foc && at == ws.caret ? Style::selected : Style::input;
        g.put(r.x + col - ws.scroll, r.y, c, st, r.x + r.w);
      }
      col += cw;
    }
    if (foc && ws.caret >= ws.text.size()) g.put(r.x + caret_col - ws.scroll, r.y, U' ', Style::selected, r.x + r.w);
  }

  void progress(const Value& n, Rect r) {
    const double v = std::clamp(n["value"].number(), 0.0, 1.0);
    int x = r.x;
    const auto label = n["label"].str();
    if (!label.empty()) x = g.text(x, r.y, label, Style::normal, r.x + r.w) + 1;
    const int pct_w = 5;
    const int bar = r.x + r.w - x - pct_w;
    if (bar >= 1) {
      const int filled = int(std::lround(v * bar));
      for (int i = 0; i < bar; ++i) g.put(x + i, r.y, i < filled ? U'█' : U'░', i < filled ? Style::accent : Style::dim, r.x + r.w);
      x += bar;
    }
    std::string pct = std::to_string(int(std::lround(v * 100))) + "%";
    pct.insert(0, size_t(std::max(0, pct_w - int(pct.size()))), ' ');
    g.text(x, r.y, pct, Style::normal, r.x + r.w);
  }
};

}  // namespace

void sync_widget(const Value& node, WidgetState& ws) {
  const auto t = type_of(node);
  if (t == "field") {
    const auto value = node["value"].str();
    if (!ws.seen || value != ws.seen_value) {
      ws.text = std::string(value);
      ws.caret = ws.text.size();
      ws.seen_value = ws.text;
      ws.seen = true;
    }
  } else if (t == "table" || t == "tree") {
    // [index, nonce]: where the cursor's row went after a sort, applied once.
    if (const Value& f = node["follow"]; f.is_array()) {
      const std::string want = std::to_string(f.at(0).integer()) + ":" + std::to_string(f.at(1).integer());
      if (want != ws.seen_follow) {
        ws.seen_follow = want;
        ws.cursor = int(f.at(0).integer());
      }
    }
    // [index, seq]: applied when seq changes (each move bumps it; the index alone may
    // change when the rows are sorted, and the cursor then follows its row instead).
    const Value& c = node["cursor"];
    if (!c.is_array()) return;
    const std::string want = std::to_string(c.at(1).integer());
    if (!ws.seen || want != ws.seen_value) {
      ws.seen = true;
      ws.seen_value = want;
      ws.cursor = int(c.at(0).integer());
    }
  } else if (t == "list") {
    const Value& sel = node["selected"];
    if (sel.is_nil()) return;
    const std::string want = sel.is_str() ? std::string(sel.str()) : std::to_string(sel.integer());
    if (ws.seen && want == ws.seen_value) return;
    ws.seen = true;
    ws.seen_value = want;
    const auto& items = node["items"].array();
    for (size_t i = 0; i < items.size(); ++i) {
      if (item_key(items[i], i) == want) {
        ws.cursor = int(i);
        break;
      }
    }
  }
}

std::string item_key(const Value& item, size_t index) {
  if (item.is_str()) return std::string(item.str());
  const Value& k = item["key"];
  if (k.is_str()) return std::string(k.str());
  if (k.is_int()) return std::to_string(k.integer());
  return std::to_string(index);
}

void draw_box(Grid& g, Rect r, std::string_view title, Style border, Style title_style) {
  if (r.w < 2 || r.h < 2) return;
  const int x1 = r.x + r.w - 1, y1 = r.y + r.h - 1;
  for (int x = r.x + 1; x < x1; ++x) {
    g.put(x, r.y, U'─', border, x1);
    g.put(x, y1, U'─', border, x1);
  }
  for (int y = r.y + 1; y < y1; ++y) {
    g.put(r.x, y, U'│', border, r.x + 1);
    g.put(x1, y, U'│', border, x1 + 1);
  }
  g.put(r.x, r.y, U'┌', border, r.x + 1);
  g.put(x1, r.y, U'┐', border, x1 + 1);
  g.put(r.x, y1, U'└', border, r.x + 1);
  g.put(x1, y1, U'┘', border, x1 + 1);
  if (!title.empty() && r.w > 6) {
    const int x = g.text(r.x + 2, r.y, " ", border, x1 - 1);
    const int e = g.text(x, r.y, title, title_style, x1 - 2);
    g.text(e, r.y, " ", border, x1 - 1);
  }
}

void render_view(Grid& g, Rect r, const Value& tree, ViewState& vs, bool active) {
  vs.focus_order.clear();
  vs.hits.clear();
  collect_focusables(tree, vs.focus_order);
  const bool gone = std::find(vs.focus_order.begin(), vs.focus_order.end(), vs.focus) == vs.focus_order.end();
  if (gone || vs.focus_fallback) {
    const auto preferred = autofocus_id(tree);
    if (!preferred.empty()) {
      vs.focus = std::string(preferred);
      vs.focus_fallback = false;
    } else if (gone) {
      vs.focus = vs.focus_order.empty() ? "" : vs.focus_order.front();
      vs.focus_fallback = true;
    }
  }
  Painter p{g, vs, active};
  p.draw(tree, r);
}

const Value* find_widget(const Value& tree, std::string_view id) {
  if (is_focusable(type_of(tree)) && tree["id"].str() == id) return &tree;
  if (!tree["dialog"].is_nil()) {
    if (auto f = find_widget(tree["dialog"], id)) return f;
  }
  for (const auto& c : children_of(tree)) {
    if (auto f = find_widget(c, id)) return f;
  }
  if (!tree["child"].is_nil()) return find_widget(tree["child"], id);
  return nullptr;
}

const Value* find_modal(const Value& tree) {
  if (type_of(tree) == "modal") {
    if (auto inner = find_modal(tree["dialog"])) return inner;
    return &tree;
  }
  for (const auto& c : children_of(tree)) {
    if (auto m = find_modal(c)) return m;
  }
  if (!tree["child"].is_nil()) return find_modal(tree["child"]);
  return nullptr;
}

void for_each_node(const Value& tree, const std::function<void(const Value&)>& fn) {
  if (!tree.is_map()) return;
  fn(tree);
  for (const auto& c : children_of(tree)) for_each_node(c, fn);
  if (!tree["child"].is_nil()) for_each_node(tree["child"], fn);
  if (!tree["dialog"].is_nil()) for_each_node(tree["dialog"], fn);
}

int row_count(const Value& node) {
  if (!node["total"].is_nil()) return int(std::max<int64_t>(0, node["total"].integer()));
  if (type_of(node) == "table" || type_of(node) == "tree") return int(node["rows"].array().size());
  return int(node["items"].array().size());
}

const Value* row_at(const Value& node, int i) {
  if (type_of(node) == "table" || type_of(node) == "tree") {
    const auto& rows = node["rows"].array();
    const int wi = i - int(node["offset"].integer(0));
    return wi >= 0 && wi < int(rows.size()) ? &rows[size_t(wi)] : nullptr;
  }
  const auto& items = node["items"].array();
  return i >= 0 && i < int(items.size()) ? &items[size_t(i)] : nullptr;
}

namespace {
bool is_rows_node(const Value& n) {
  const auto t = n["t"].str();
  return t == "list" || t == "table" || t == "tree";
}

bool rows_carry_sel(const Value& n) {
  for (const auto& r : n[n["t"].str() == "list" ? "items" : "rows"].array()) {
    if (r.is_map() && !r["sel"].is_nil()) return true;
  }
  return false;
}
}  // namespace

std::optional<Value> view_selection(const Value& tree, std::string_view focus,
                                    const std::function<int(std::string_view)>& cursor_of) {
  const Value* rows = nullptr;
  if (const Value* f = focus.empty() ? nullptr : find_widget(tree, focus); f && is_rows_node(*f) && rows_carry_sel(*f)) {
    rows = f;
  } else {
    for_each_node(tree, [&](const Value& n) {
      if (!rows && is_rows_node(n) && rows_carry_sel(n)) rows = &n;
    });
  }
  if (rows) {
    const Value* row = row_at(*rows, cursor_of((*rows)["id"].str()));
    if (row && row->is_map() && (*row)["sel"].is_array()) return (*row)["sel"];
    if (tree["sel"].is_array()) return tree["sel"];
    return Value(mp::Array{});
  }
  if (tree["sel"].is_array()) return tree["sel"];
  return std::nullopt;
}

}  // namespace rcp
