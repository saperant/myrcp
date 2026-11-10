// Tests of the I/O-free core: msgpack, keys, grid, layout.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <poll.h>
#include <unistd.h>
#include <string>

#include "rcp/grid.hpp"
#include "rcp/keys.hpp"
#include "rcp/layout.hpp"
#include "rcp/msgpack.hpp"
#include "rcp/utf8.hpp"
#include "console.hpp"
#include "history.hpp"
#include "json.hpp"
#include "prefs.hpp"

namespace {

int failures = 0, checks = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    ++checks;                                                          \
    if (!(cond)) {                                                     \
      ++failures;                                                      \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                  \
  } while (0)

#define CHECK_EQ(a, b)                                                                  \
  do {                                                                                  \
    ++checks;                                                                           \
    const auto va = (a);                                                                \
    const auto vb = (b);                                                                \
    if (!(va == vb)) {                                                                  \
      ++failures;                                                                       \
      std::printf("%s:%d: CHECK_EQ failed: %s == %s\n", __FILE__, __LINE__, #a, #b);    \
    }                                                                                   \
  } while (0)

using namespace rcp;
using mp::Array;
using mp::Map;
using mp::Value;

Value roundtrip(const Value& v) {
  const std::string bytes = mp::encode(v);
  size_t pos = 0;
  auto out = mp::decode(bytes, pos);
  CHECK(out.has_value());
  CHECK_EQ(pos, bytes.size());
  return out ? *out : Value();
}

void test_msgpack() {
  for (int64_t i : {0L, 1L, 127L, 128L, 255L, 256L, 65535L, 65536L, -1L, -32L, -33L, -128L, -129L, -32768L,
                    -32769L, 4294967296L, -4294967296L}) {
    CHECK_EQ(roundtrip(Value(i)).integer(), i);
  }
  CHECK_EQ(roundtrip(Value(18446744073709551615UL)), Value(18446744073709551615UL));
  CHECK_EQ(roundtrip(Value(3.25)).number(), 3.25);
  CHECK_EQ(roundtrip(Value(true)).boolean(), true);
  CHECK(roundtrip(Value()).is_nil());
  CHECK_EQ(roundtrip(Value(std::string(40, 'x'))), Value(std::string(40, 'x')));
  CHECK_EQ(roundtrip(Value(std::string(300, 'y'))), Value(std::string(300, 'y')));
  CHECK_EQ(roundtrip(Value(std::string(70000, 'z'))).str().size(), 70000u);
  CHECK_EQ(roundtrip(Value(mp::Ext{1, "ab"})), Value(mp::Ext{1, "ab"}));
  CHECK_EQ(roundtrip(Value(mp::Bin{"\x00\x01"})), Value(mp::Bin{"\x00\x01"}));

  const Value tree = Map{{"t", "vbox"}, {"children", Array{Map{{"t", "text"}, {"text", "hä"}}}}};
  const Value back = roundtrip(tree);
  CHECK_EQ(back, tree);
  CHECK_EQ(back["children"].at(0)["text"].str(), "hä");
  CHECK(back["missing"].is_nil());
  CHECK(back["children"].at(5).is_nil());

  // Known encodings.
  CHECK_EQ(mp::encode(Value(Array{1, "a"})), std::string("\x92\x01\xa1" "a"));
  CHECK_EQ(mp::encode(Value(-1)), std::string("\xff"));

  // Streaming: values split across feeds.
  const std::string a = mp::encode(Value(Array{0, 1, "initialize", Array{Map{{"protocol", 1}}}}));
  const std::string b = mp::encode(Value("second"));
  const std::string all = a + b;
  mp::Decoder d;
  int got = 0;
  for (char c : all) {
    d.feed(std::string_view(&c, 1));
    while (auto v = d.next()) {
      ++got;
      if (got == 1) CHECK_EQ((*v).at(2).str(), "initialize");
      if (got == 2) CHECK_EQ((*v).str(), "second");
    }
  }
  CHECK_EQ(got, 2);
  CHECK_EQ(d.buffered(), 0u);

  bool threw = false;
  try {
    size_t pos = 0;
    mp::decode("\xc1", pos);
  } catch (const mp::DecodeError&) {
    threw = true;
  }
  CHECK(threw);
}

std::vector<Key> keys_of(std::string_view bytes, bool flush = false) {
  KeyDecoder d;
  d.feed(bytes);
  if (flush) d.flush();
  std::vector<Key> out;
  while (auto k = d.next()) out.push_back(*k);
  return out;
}

std::string first_name(std::string_view bytes, bool flush = false) {
  auto ks = keys_of(bytes, flush);
  return ks.empty() ? "<none>" : ks.front().name;
}

void test_keys() {
  CHECK_EQ(first_name("a"), "a");
  CHECK_EQ(keys_of("a").front().text, "a");
  CHECK_EQ(first_name("\r"), "enter");
  CHECK_EQ(first_name("\x10"), "ctrl+p");
  CHECK_EQ(first_name("\x11"), "ctrl+q");
  CHECK_EQ(first_name("\x7f"), "backspace");
  CHECK_EQ(first_name("\x1b[A"), "up");
  CHECK_EQ(first_name("\x1b[1;5C"), "ctrl+right");
  CHECK_EQ(first_name("\x1b[1;3D"), "alt+left");
  CHECK_EQ(first_name("\x1b[6~"), "pagedown");
  CHECK_EQ(first_name("\x1b[3;2~"), "shift+delete");
  CHECK_EQ(first_name("\x1b[15~"), "f5");
  CHECK_EQ(first_name("\x1bOP"), "f1");
  CHECK_EQ(first_name("\x1b[Z"), "shift+tab");
  CHECK_EQ(first_name("\x1b" "1"), "alt+1");
  CHECK_EQ(first_name("\x1b[112;5u"), "ctrl+p");
  CHECK_EQ(first_name("ä"), "ä");
  // A lone ESC waits until flushed.
  CHECK_EQ(first_name("\x1b"), "<none>");
  CHECK_EQ(first_name("\x1b", true), "escape");
  // Incomplete UTF-8 waits.
  CHECK_EQ(first_name("\xc3"), "<none>");
  // Several keys in one read.
  auto ks = keys_of("ab\x1b[B\r");
  CHECK_EQ(ks.size(), 4u);
  CHECK_EQ(ks[2].name, "down");
  // Bracketed paste, split across feeds.
  KeyDecoder d;
  d.feed("\x1b[200~hello\nwor");
  auto k1 = d.next();
  CHECK(!k1 || k1->name == "unknown");
  d.feed("ld\x1b[201~x");
  auto k2 = d.next();
  CHECK(k2 && k2->name == "paste" && k2->text == "hello\nworld");
  auto k3 = d.next();
  CHECK(k3 && k3->name == "x");

  // SGR mouse: presses, wheel, releases and left-button drags; other motion is dropped.
  auto mk = keys_of("\x1b[<0;10;5M");
  CHECK(mk.size() == 1 && mk[0].name == "mouse" && mk[0].mouse.x == 9 && mk[0].mouse.y == 4 &&
        mk[0].mouse.button == "left");
  CHECK_EQ(keys_of("\x1b[<0;10;5m").front().mouse.button, "release");
  CHECK_EQ(keys_of("\x1b[<32;12;5M").front().mouse.button, "drag");
  CHECK_EQ(keys_of("\x1b[<35;12;5M").front().name, "unknown");
  CHECK_EQ(keys_of("\x1b[<65;1;1M").front().mouse.button, "wheeldown");
  auto ctrl = keys_of("\x1b[<16;2;3M").front().mouse;
  CHECK(ctrl.ctrl && !ctrl.shift && ctrl.button == "left");
  CHECK_EQ(keys_of("\x1b[<4;2;3M").front().mouse.shift, true);
}

void test_grid() {
  CHECK_EQ(utf8::width("abc"), 3);
  CHECK_EQ(utf8::width("日本"), 4);
  Grid g(10, 2);
  CHECK_EQ(g.text(0, 0, "hello world", Style::normal), 10);
  CHECK_EQ(g.row_text(0), "hello worl");
  g.text(0, 1, "日本語", Style::normal, 5);  // the third wide char does not fit
  CHECK_EQ(g.row_text(1), "日本      ");
  CHECK(g.at(1, 1).cont);
  g.put(1, 1, U'x', Style::normal, 10);  // overwriting half of a wide char clears the other half
  CHECK_EQ(g.row_text(1), " x本      ");

  // An unchanged grid diffs to nothing but the frame bracket.
  Grid a(5, 1), b(5, 1);
  a.text(0, 0, "abc", Style::normal);
  b.text(0, 0, "abc", Style::normal);
  const Theme t = default_theme();
  CHECK_EQ(diff(a, b, t), std::string("\x1b[?2026h\x1b[0m\x1b[?2026l"));
  b.text(1, 0, "X", Style::error);
  const std::string d = diff(a, b, t);
  CHECK(d.find("\x1b[1;2H") != std::string::npos);
  CHECK(d.find("\x1b[0;31mX") != std::string::npos);
  CHECK(d.find('a') == std::string::npos);

  // Layers: the classic theme paints dialog cells with the dialog palette, keeps the
  // layer when drawing into it, casts shadows, and doubles focused frames.
  const Theme& classic = theme_by_name("classic");
  CHECK_EQ(classic.name, "classic");
  CHECK_EQ(theme_by_name("nope").name, "terminal");
  Grid w(10, 4), dlg(10, 4);
  dlg.set_layer({0, 0, 5, 2}, Layer::dialog);
  dlg.text(0, 0, "hi", Style::normal);
  CHECK(dlg.at(0, 0).layer == Layer::dialog);
  dlg.shadow({0, 0, 5, 2});
  CHECK_EQ(dlg.at(5, 1).style, Style::shadow);
  CHECK(dlg.at(5, 1).layer == Layer::window);
  const std::string dd = diff(w, dlg, classic);
  CHECK(dd.find("\x1b[" + classic.at(Layer::dialog, Style::normal) + "mhi") != std::string::npos);
  Grid frame(3, 1);
  frame.put(0, 0, U'─', Style::border_focus, 3);
  CHECK(diff(Grid(), frame, classic).find("═") != std::string::npos);
  CHECK(diff(Grid(), frame, default_theme()).find("─") != std::string::npos);
}

Value text(std::string_view s) { return Map{{"t", "text"}, {"text", s}}; }

Value list(std::string_view id, int n) {
  Array items;
  for (int i = 0; i < n; ++i) items.push_back(Map{{"key", "k" + std::to_string(i)}, {"text", "item " + std::to_string(i)}});
  return Map{{"t", "list"}, {"id", id}, {"items", items}};
}

void test_layout() {
  // vbox: text keeps its height, the list takes the rest.
  const Value tree = Map{{"t", "vbox"}, {"children", Array{text("header"), list("l", 20), text("footer")}}};
  Grid g(20, 6);
  ViewState vs;
  render_view(g, {0, 0, 20, 6}, tree, vs, true);
  CHECK_EQ(vs.focus, "l");
  CHECK(g.row_text(0).starts_with("header"));
  CHECK(g.row_text(1).starts_with(" item 0"));
  CHECK(g.row_text(4).starts_with(" item 3"));
  CHECK(g.row_text(5).starts_with("footer"));
  CHECK_EQ(g.at(0, 1).style, Style::selected);  // cursor row of the focused list
  CHECK_EQ(vs.widgets["l"].page, 4);

  // The cursor scrolls into view.
  vs.widgets["l"].cursor = 10;
  render_view(g, {0, 0, 20, 6}, tree, vs, true);
  CHECK(g.row_text(4).starts_with(" item 10"));

  // list.selected moves the cursor once, when it changes.
  Value sel = list("l", 20);
  std::get<Map>(sel.v).emplace_back("selected", "k2");
  WidgetState ws;
  sync_widget(sel, ws);
  CHECK_EQ(ws.cursor, 2);
  ws.cursor = 5;
  sync_widget(sel, ws);
  CHECK_EQ(ws.cursor, 5);

  // hbox: buttons take their natural width, a field the rest.
  const Value row = Map{{"t", "hbox"},
                        {"children", Array{Map{{"t", "button"}, {"id", "b"}, {"label", "Go"}},
                                           Map{{"t", "field"}, {"id", "f"}, {"value", "abc"}}}}};
  Grid h(20, 1);
  ViewState vs2;
  render_view(h, {0, 0, 20, 1}, row, vs2, true);
  CHECK(h.row_text(0).starts_with("[ Go ] abc"));
  CHECK_EQ(vs2.focus_order.size(), 2u);
  CHECK_EQ(vs2.widgets["f"].text, "abc");

  // panel border and title.
  const Value panel = Map{{"t", "panel"}, {"title", "Files"}, {"child", text("x")}};
  Grid p(12, 3);
  ViewState vs3;
  render_view(p, {0, 0, 12, 3}, panel, vs3, true);
  CHECK_EQ(p.row_text(0), "┌─ Files ──┐");
  CHECK_EQ(p.row_text(1), "│x         │");

  // wrapped text and progress
  const Value wrapped = Map{{"t", "text"}, {"text", "one two three four"}, {"wrap", true}};
  Grid w(9, 3);
  ViewState vs4;
  render_view(w, {0, 0, 9, 3}, wrapped, vs4, true);
  CHECK_EQ(w.row_text(0), "one two  ");
  CHECK_EQ(w.row_text(1), "three    ");
  CHECK_EQ(w.row_text(2), "four     ");

  // Styled spans wrap too, each word keeping its style; a word wider than the line is cut.
  const Value spans = Map{{"t", "text"},
                          {"text", Array{"held by ", Array{"Status LED", "warning"}, "; raise it to IDEAL abcdefghijkl"}},
                          {"wrap", true}};
  Grid sw(10, 6);
  ViewState vs5;
  render_view(sw, {0, 0, 10, 6}, spans, vs5, true);
  CHECK_EQ(sw.row_text(0), "held by   ");
  CHECK_EQ(sw.row_text(1), "Status    ");
  CHECK_EQ(sw.row_text(2), "LED; raise");
  CHECK_EQ(sw.row_text(3), "it to     ");
  CHECK_EQ(sw.row_text(4), "IDEAL     ");
  CHECK_EQ(sw.row_text(5), "abcdefghij");
  CHECK_EQ(sw.at(0, 1).style, Style::warning);
  CHECK_EQ(sw.at(3, 2).style, Style::normal);  // the ";" after "LED"
  CHECK_EQ(sw.at(0, 0).style, Style::normal);

  const Value prog = Map{{"t", "progress"}, {"value", 0.5}};
  Grid pr(15, 1);
  render_view(pr, {0, 0, 15, 1}, prog, vs4, true);
  CHECK_EQ(pr.row_text(0), "█████░░░░░  50%");

  CHECK(find_widget(tree, "l") != nullptr);
  CHECK(find_widget(tree, "nope") == nullptr);

  // A fallback focus (loading: only a button) gives way to the autofocus table when it
  // appears; a focus the user chose stays.
  const Value btn = Map{{"t", "button"}, {"id", "mode"}, {"label", "1 Blocks"}};
  const Value loading = Map{{"t", "vbox"}, {"children", Array{btn, text("Evaluating…")}}};
  const Value loaded = Map{{"t", "vbox"},
                           {"children", Array{btn, Map{{"t", "table"}, {"id", "blocks"}, {"autofocus", true},
                                                       {"columns", Array{Map{{"title", "A"}}}}, {"rows", Array{}}}}}};
  Grid fg(20, 4);
  ViewState fvs;
  render_view(fg, {0, 0, 20, 4}, loading, fvs, true);
  CHECK_EQ(fvs.focus, "mode");
  render_view(fg, {0, 0, 20, 4}, loaded, fvs, true);
  CHECK_EQ(fvs.focus, "blocks");
  ViewState chosen;
  render_view(fg, {0, 0, 20, 4}, loading, chosen, true);
  chosen.focus_fallback = false;  // as Tab or a click does
  render_view(fg, {0, 0, 20, 4}, loaded, chosen, true);
  CHECK_EQ(chosen.focus, "mode");
}

void test_widgets() {
  // A windowed table: 1000 rows, rows 0..9 sent. Header, then rows; missing rows ask for more.
  Array rows;
  for (int i = 0; i < 10; ++i) rows.push_back(Map{{"key", "r" + std::to_string(i)}, {"cells", Array{"a" + std::to_string(i), Map{{"text", "x"}, {"style", "error"}}}}});
  const Array cols{Map{{"title", "A"}, {"width", 4}}, Map{{"title", "B"}}};
  Value tbl = Map{{"t", "table"}, {"id", "t"}, {"columns", cols}, {"rows", rows}, {"offset", 0}, {"total", 1000}};
  Grid g(12, 5);
  ViewState vs;
  render_view(g, {0, 0, 12, 5}, tbl, vs, true);
  CHECK(g.row_text(0).starts_with("A    B"));
  CHECK(g.row_text(1).starts_with("a0   x"));
  CHECK_EQ(g.at(5, 2).style, Style::error);
  CHECK_EQ(vs.widgets["t"].want_first, -1);
  vs.widgets["t"].cursor = 500;  // far outside the window
  render_view(g, {0, 0, 12, 5}, tbl, vs, true);
  CHECK(g.row_text(4).starts_with("…"));
  CHECK(vs.widgets["t"].want_first >= 0 && vs.widgets["t"].want_first <= 500);
  CHECK_EQ(row_count(tbl), 1000);
  CHECK(row_at(tbl, 3) != nullptr);
  CHECK(row_at(tbl, 500) == nullptr);

  // table.cursor [index, seq] applies once per seq.
  std::get<Map>(tbl.v).emplace_back("cursor", Array{7, 1});
  WidgetState ws;
  sync_widget(tbl, ws);
  CHECK_EQ(ws.cursor, 7);
  ws.cursor = 2;
  sync_widget(tbl, ws);
  CHECK_EQ(ws.cursor, 2);

  // A modal: only the dialog takes focus; the base is dimmed.
  const Value modal = Map{{"t", "modal"}, {"id", "m"}, {"title", "Q"},
                          {"child", Map{{"t", "vbox"}, {"children", Array{Map{{"t", "button"}, {"id", "under"}, {"label", "U"}}}}}},
                          {"dialog", Map{{"t", "hbox"}, {"children", Array{Map{{"t", "button"}, {"id", "ok"}, {"label", "OK"}},
                                                                       Map{{"t", "checkbox"}, {"id", "c"}, {"label", "on"}, {"checked", true}}}}}}};
  Grid mg(40, 10);
  ViewState mvs;
  render_view(mg, {0, 0, 40, 10}, modal, mvs, true);
  CHECK_EQ(mvs.focus_order.size(), 2u);
  CHECK_EQ(mvs.focus, "ok");
  CHECK_EQ(mg.at(0, 0).style, Style::dim);
  CHECK(find_modal(modal) != nullptr);
  CHECK(find_widget(modal, "c") != nullptr);
  bool found_box = false;
  for (int y = 0; y < 10; ++y) found_box |= mg.row_text(y).find("[x] on") != std::string::npos;
  CHECK(found_box);

  // A tree: indentation, arrows, tri-state boxes, detail; click targets recorded.
  const Value tr = Map{{"t", "tree"}, {"id", "tr"},
                       {"rows", Array{Map{{"key", ""}, {"text", "/"}, {"depth", 0}, {"expandable", true}, {"expanded", true}, {"check", "some"}},
                                      Map{{"key", "a"}, {"text", "a/"}, {"depth", 1}, {"expandable", true}, {"check", "all"}, {"detail", "1 KiB"}},
                                      Map{{"key", "b"}, {"text", "b"}, {"depth", 1}, {"check", "none"}}}}};
  Grid tg(30, 4);
  ViewState tvs;
  render_view(tg, {0, 0, 30, 4}, tr, tvs, true);
  CHECK(tg.row_text(0).starts_with("▼ [-] /"));
  CHECK(tg.row_text(1).starts_with("  ▶ [x] a/  1 KiB"));
  CHECK(tg.row_text(2).starts_with("    [ ] b"));
  CHECK_EQ(tvs.hits.size(), 1u);
  CHECK_EQ(tvs.hits[0].type, "tree");
  CHECK_EQ(row_count(tr), 3);

  // A table whose second column is a tree: that cell indented, with its arrow.
  const Value tt = Map{{"t", "table"}, {"id", "tt"}, {"tree", 1},
                       {"columns", Array{Map{{"title", "M"}, {"width", 1}}, Map{{"title", "Path"}}}},
                       {"rows", Array{Map{{"key", "d"}, {"cells", Array{"·", "d/"}}, {"depth", 0}, {"expandable", true}, {"expanded", true}},
                                      Map{{"key", "d/e"}, {"cells", Array{"·", "e/"}}, {"depth", 1}, {"expandable", true}, {"expanded", false}},
                                      Map{{"key", "d/f"}, {"cells", Array{"·", "f"}}, {"depth", 1}}}}};
  Grid ttg(20, 4);
  ViewState ttvs;
  render_view(ttg, {0, 0, 20, 4}, tt, ttvs, true);
  CHECK(ttg.row_text(1).starts_with("· ▼ d/"));
  CHECK(ttg.row_text(2).starts_with("·   ▶ e/"));
  CHECK(ttg.row_text(3).starts_with("·     f"));

  // Hits: buttons and table columns; nothing under a modal's dialog but the dialog.
  CHECK_EQ(mvs.hits.size(), 2u);  // the dialog's button and checkbox, not the base's
  CHECK_EQ(mvs.hits[0].id, "ok");
  CHECK_EQ(vs.hits.size(), 1u);
  CHECK_EQ(vs.hits[0].col_x.size(), 2u);
  CHECK_EQ(vs.hits[0].col_x[1], 5);

  // autofocus picks the initial focus; without it the first focusable wins.
  const Value af = Map{{"t", "vbox"}, {"children", Array{Map{{"t", "checkbox"}, {"id", "c"}, {"label", "x"}},
                                                         Map{{"t", "list"}, {"id", "l"}, {"autofocus", true}, {"items", Array{"a"}}}}}};
  Grid ag(10, 3);
  ViewState avs;
  render_view(ag, {0, 0, 10, 3}, af, avs, true);
  CHECK_EQ(avs.focus, "l");

  // A log follows its end.
  Array lines;
  for (int i = 0; i < 20; ++i) lines.push_back("line " + std::to_string(i));
  const Value lg = Map{{"t", "log"}, {"id", "l"}, {"lines", lines}};
  Grid lgrid(10, 3);
  ViewState lvs;
  render_view(lgrid, {0, 0, 10, 3}, lg, lvs, true);
  CHECK(lgrid.row_text(2).starts_with("line 19"));
}

void test_selection() {
  auto item = [](std::string label) { return Array{Map{{"type", "file"}, {"label", std::move(label)}}}; };
  const Value tree = Map{
      {"t", "vbox"},
      {"children",
       Array{Map{{"t", "button"}, {"id", "b"}, {"label", "x"}},
             Map{{"t", "table"},
                 {"id", "plain"},
                 {"columns", Array{Map{{"title", "A"}}}},
                 {"rows", Array{Map{{"key", "p"}, {"cells", Array{"p"}}}}}},
             Map{{"t", "table"},
                 {"id", "t"},
                 {"columns", Array{Map{{"title", "A"}}}},
                 {"offset", 10},
                 {"rows", Array{Map{{"key", "a"}, {"cells", Array{"a"}}, {"sel", item("a")}},
                                Map{{"key", "b"}, {"cells", Array{"b"}}}}}}}}};
  int cursor = 10;
  auto cur = [&](std::string_view w) { return w == "t" ? cursor : 0; };
  // Focus on a button: the first table whose rows carry selections, at its cursor.
  auto sel = view_selection(tree, "b", cur);
  CHECK(sel && sel->array().size() == 1 && (*sel).at(0)["label"].str() == "a");
  // A row without one: an empty selection, not "none offered".
  cursor = 11;
  sel = view_selection(tree, "t", cur);
  CHECK(sel && sel->array().empty());
  // The focused table carries none: the other one still speaks for the view.
  cursor = 10;
  sel = view_selection(tree, "plain", cur);
  CHECK(sel && sel->array().size() == 1);
  // Nothing anywhere: no selection offered; a tree-level `sel` is the fallback.
  const Value bare = Map{{"t", "text"}, {"text", "x"}};
  CHECK(!view_selection(bare, "", cur));
  const Value whole = Map{{"t", "text"}, {"text", "x"}, {"sel", item("whole")}};
  sel = view_selection(whole, "", cur);
  CHECK(sel && (*sel).at(0)["label"].str() == "whole");
}

}  // namespace

#ifdef MYRCP_HAVE_VTERM
int term_tests();
#endif

void test_prefs() {
  using rcp::PrefsFile;
  using rcp::mp::Array;
  using rcp::mp::Value;
  PrefsFile f;
  std::string err;
  const char* text =
      "# my settings\n"
      "[workbench]\n"
      "theme = \"classic\"  # blue\n"
      "mouse = false\n"
      "\n"
      "[tool]\n"
      "designs = [\n"
      "  \"~/a\",  # first\n"
      "  '/b',\n"
      "]\n"
      "count = 1_000\n";
  CHECK(f.parse(text, err));
  CHECK_EQ(f.get("workbench.theme").str(), "classic");
  CHECK_EQ(f.get("workbench.mouse").boolean(true), false);
  CHECK(f.get("tool.designs") == Value(Array{"~/a", "/b"}));
  CHECK_EQ(f.get("tool.count").integer(), 1000);
  CHECK(f.get("tool.none").is_nil());
  // Writes touch only their own lines: comments and order stay.
  f.set("tool.designs", Array{"/c"});
  f.set("workbench.theme", "terminal");
  f.set("workbench.editor_mode", "tab");
  f.set("other.projects", Array{"/p"});
  f.set("workbench.mouse", Value());
  CHECK_EQ(f.text(),
           "# my settings\n[workbench]\ntheme = \"terminal\"\neditor_mode = \"tab\"\n\n[tool]\n"
           "designs = [\"/c\"]\ncount = 1_000\n\n[other]\nprojects = [\"/p\"]\n");
  PrefsFile again;
  CHECK(again.parse(f.text(), err) && again.get("tool.count").integer() == 1000 &&
        again.get("workbench.editor_mode").str() == "tab");
  CHECK(!again.parse("[x]\nkey = \"open\n", err) && err.starts_with("2:"));
  CHECK(!again.parse("a = [1, 2\n", err));
  CHECK(!again.parse("just words\n", err));

  rcp::PrefInfo p{"x.y", "", "", "", "paths", Array{}, {}, ""};
  CHECK(rcp::pref_from_text(p, " /a : ~/b ::", err) == Value(Array{"/a", "~/b"}));
  p.type = "bool";
  CHECK_EQ(rcp::pref_from_text(p, "yes", err).boolean(), true);
  CHECK(rcp::pref_from_text(p, "maybe", err).is_nil());
  p.type = "choice";
  p.choices = {"a", "b"};
  CHECK(rcp::pref_fits(p, "b") && !rcp::pref_fits(p, "c") && !rcp::pref_fits(p, 1));
}

void test_json() {
  using rcp::from_json;
  using rcp::to_json;
  using rcp::mp::Array;
  using rcp::mp::Map;
  using rcp::mp::Value;
  // What Vim sends on a channel, and back.
  const auto v = from_json(R"( [3, {"kind": "cursor", "line": 12, "col": -1.5e1, "ok": true, "x": null,
    "text": "a\"b\\c\n\u00e4\ud83d\ude00", "items": []}] )");
  CHECK(v.has_value());
  CHECK_EQ(v->at(0).integer(), 3);
  const Value& m = v->at(1);
  CHECK_EQ(m["kind"].str(), "cursor");
  CHECK_EQ(m["line"].integer(), 12);
  CHECK(m["col"].number() == -15.0 && m["ok"].boolean() && m["x"].is_nil());
  CHECK_EQ(m["text"].str(), "a\"b\\c\n\xc3\xa4\xf0\x9f\x98\x80");
  CHECK(m["items"].is_array() && m["items"].array().empty());
  CHECK_EQ(to_json(Array{"call", "MyrcpGoto", Array{"/a \"b\".txt", 3}}), R"(["call","MyrcpGoto",["/a \"b\".txt",3]])");
  CHECK_EQ(to_json(Map{{"a", Array{}}, {"b", Map{}}, {"c", false}, {"d", "\x01"}}),
           R"({"a":[],"b":{},"c":false,"d":"\u0001"})");
  CHECK(from_json(to_json(*v)) == v);
  CHECK(!from_json("[1, 2"));
  CHECK(!from_json("{\"a\" 1}"));
  CHECK(!from_json("[1] x"));
  CHECK(!from_json("\"open"));
  CHECK(!from_json(""));
}

void test_history() {
  NavHistory h;
  const auto all = [](const Place&) { return true; };
  CHECK(!h.back({"a"}, all));
  h.left({"files", "list", "x.txt", 3});
  h.left({"problems", "list", "p1", 0});
  h.left({"problems", "list", "p2", 1});  // the same view again: its latest row
  CHECK(h.can_back() && !h.can_forward());
  auto p = h.back({"props"}, all);
  CHECK(p && p->view == "problems" && p->key == "p2");
  p = h.back({"problems", "list", "p2", 1}, all);
  CHECK(p && p->view == "files" && p->cursor == 3);
  CHECK(h.can_forward());
  p = h.forward({"files"}, all);
  CHECK(p && p->view == "problems");
  p = h.forward({"problems"}, all);
  CHECK(p && p->view == "props");
  // Closed views are passed over; leaving somewhere new forgets Forward.
  h.back({"props"}, all);
  h.left({"problems"});
  CHECK(!h.can_forward());
  p = h.back({"new"}, [](const Place& q) { return q.view != "problems"; });
  CHECK(p && p->view == "files");
}

void test_console() {
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / ("rcp-console-test-" + std::to_string(getpid()));
  fs::create_directories(dir / "src");
  { std::ofstream(dir / "src" / "a.cpp") << "int x;\n"; }
  // Places in output lines: compilers', Python's, only for files that are there.
  const auto p = Consoles::place_in("src/a.cpp:12:3: error: no", dir.string());
  CHECK(p && p->first == (dir / "src" / "a.cpp").string() && p->second == 12);
  CHECK(Consoles::place_in("  File \"src/a.cpp\", line 7, in f", dir.string())->second == 7);
  CHECK(Consoles::place_in("FAILED (src/a.cpp:4)", dir.string())->second == 4);
  CHECK(!Consoles::place_in("src/nope.cpp:3: x", dir.string()));
  CHECK(!Consoles::place_in("time 12:30", dir.string()));

  Consoles cs;
  std::string err;
  const std::string id = cs.run({"/bin/sh", "-c", "printf 'one\\nsrc/a.cpp:1: here\\n'; echo oops >&2; printf 'part'; exit 2"},
                                dir.string(), "t", "build", err);
  CHECK(!id.empty());
  for (int i = 0; i < 200 && cs.find(id)->running; ++i) {
    std::vector<pollfd> fds;
    std::vector<std::string> ids;
    cs.add_fds(fds, ids);
    poll(fds.data(), fds.size(), 50);
    for (size_t k = 0; k < fds.size(); ++k) {
      if (fds[k].revents) cs.on_readable(ids[k], fds[k].fd);
    }
  }
  const Console* c = cs.find(id);
  CHECK(!c->running && c->status == 2);
  CHECK_EQ(c->lines.size(), 4u);  // stdout and stderr in the order they came; the unfinished line too
  const Value tree = cs.tree(*c);
  const Value* out = find_widget(tree, "out");
  CHECK(out && (*out)["rows"].array().size() == 4);
  bool linked = false;
  for (const auto& r : (*out)["rows"].array()) linked |= !r["sel"].is_nil() && r["sel"].at(0)["line"].integer() == 1;
  CHECK(linked);
  // The same name runs there again; Enter on the line opens the place.
  std::string opened;
  cs.event(id, "out", "activate", Value("1"), [&](const std::string& path, int line) { opened = path + ":" + std::to_string(line); });
  CHECK_EQ(opened, (dir / "src" / "a.cpp").string() + ":1");
  CHECK_EQ(cs.run({"true"}, dir.string(), "t", "build", err), id);
  cs.close(id);
  CHECK(cs.find(id) == nullptr);
  fs::remove_all(dir);
}

int service_tests();

int main() {
  test_msgpack();
  test_console();
  test_history();
  test_keys();
  test_grid();
  test_layout();
  test_widgets();
  test_selection();
  test_prefs();
  test_json();
  std::printf("%d checks, %d failures\n", checks, failures);
  if (service_tests() != 0) return 1;
#ifdef MYRCP_HAVE_VTERM
  if (term_tests() != 0) return 1;
#endif
  return failures == 0 ? 0 : 1;
}
