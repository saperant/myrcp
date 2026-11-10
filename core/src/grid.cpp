#include "rcp/grid.hpp"

#include <algorithm>

#include "rcp/utf8.hpp"

namespace rcp {

std::optional<Style> style_from_name(std::string_view name) {
  static constexpr std::pair<std::string_view, Style> names[] = {
      {"normal", Style::normal}, {"dim", Style::dim},         {"bold", Style::bold},
      {"title", Style::title},   {"accent", Style::accent},   {"error", Style::error},
      {"warning", Style::warning}, {"ok", Style::ok},     {"info", Style::info},
      {"special", Style::special},
  };
  for (const auto& [n, s] : names) {
    if (n == name) return s;
  }
  return std::nullopt;
}

Grid::Grid(int w, int h) : w_(std::max(0, w)), h_(std::max(0, h)), cells_(size_t(w_ * h_)) {}

void Grid::fill(Rect r, Style s, char32_t ch) {
  const int x0 = std::max(0, r.x), y0 = std::max(0, r.y);
  const int x1 = std::min(w_, r.x + r.w), y1 = std::min(h_, r.y + r.h);
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) put(x, y, ch, s, x1);
  }
}

int Grid::put_raw(int x, int y, char32_t ch, uint32_t fg, uint32_t bg, uint8_t attrs, int x_end) {
  const int end = put(x, y, ch, Style::raw, x_end);
  for (int cx = std::max(0, x); cx < end && cx < w_; ++cx) {
    if (y < 0 || y >= h_) break;
    Cell& c = cell(cx, y);
    c.fg = fg;
    c.bg = bg;
    c.attrs = attrs;
  }
  return end;
}

void Grid::set_layer(Rect r, Layer layer) {
  for (int y = std::max(0, r.y); y < std::min(h_, r.y + r.h); ++y) {
    for (int x = std::max(0, r.x); x < std::min(w_, r.x + r.w); ++x) cell(x, y).layer = layer;
  }
}

void Grid::shadow(Rect r) {
  auto shade = [&](int x, int y) {
    if (!contains(x, y)) return;
    cell(x, y).style = Style::shadow;
    cell(x, y).layer = Layer::window;
  };
  for (int y = r.y + 1; y <= r.y + r.h; ++y) {
    shade(r.x + r.w, y);
    shade(r.x + r.w + 1, y);
  }
  for (int x = r.x + 2; x < r.x + r.w; ++x) shade(x, r.y + r.h);
}

void Grid::restyle(Rect r, Style s) {
  for (int y = std::max(0, r.y); y < std::min(h_, r.y + r.h); ++y) {
    for (int x = std::max(0, r.x); x < std::min(w_, r.x + r.w); ++x) cell(x, y).style = s;
  }
}

int Grid::put(int x, int y, char32_t ch, Style s, int x_end) {
  x_end = std::min(x_end, w_);
  if (y < 0 || y >= h_ || x >= x_end) return x;
  int cw = utf8::width(ch);
  if (cw == 0) {  // controls become '?', combining marks are dropped for now
    if (ch >= 0x20 && ch != 0x7f) return x;
    ch = U'?';
    cw = 1;
  }
  if (cw == 2 && x + 1 >= x_end) {
    ch = U' ';
    cw = 1;
  }
  if (x < 0) return x + cw;
  // Never leave half of a wide character behind.
  // Drawing keeps a cell's layer: the layer belongs to the region (a dialog), not to
  // what is drawn there.
  auto set_cell = [&](int cx, char32_t c, Style st, bool cont) { cell(cx, y) = Cell{c, st, cont, cell(cx, y).layer}; };
  if (cell(x, y).cont && x > 0) set_cell(x - 1, U' ', cell(x - 1, y).style, false);
  const int last = x + cw - 1;
  if (last + 1 < w_ && cell(last + 1, y).cont) set_cell(last + 1, U' ', s, false);
  set_cell(x, ch, s, false);
  if (cw == 2) set_cell(x + 1, U' ', s, true);
  return x + cw;
}

int Grid::text(int x, int y, std::string_view s, Style st, int x_end) {
  for (size_t i = 0; i < s.size() && x < x_end;) x = put(x, y, utf8::decode(s, i), st, x_end);
  return x;
}

std::string Grid::row_text(int y) const {
  std::string out;
  for (int x = 0; x < w_; ++x) {
    if (!at(x, y).cont) utf8::append(out, at(x, y).ch);
  }
  return out;
}

namespace {

using Palette = std::array<std::string, size_t(Style::count_)>;

void set(Palette& p, Style s, const char* sgr) { p[size_t(s)] = sgr; }

}  // namespace

Theme default_theme() {
  // The 16 ANSI colours on the default background: the terminal's palette decides.
  Theme t{"terminal", "Terminal colours", {}, false};
  for (auto& p : t.sgr) {
    set(p, Style::normal, "0");
    set(p, Style::dim, "0;90");
    set(p, Style::bold, "0;1");
    set(p, Style::title, "0;1");
    set(p, Style::accent, "0;36");
    set(p, Style::error, "0;31");
    set(p, Style::warning, "0;33");
    set(p, Style::ok, "0;32");
    set(p, Style::info, "0;34");
    set(p, Style::special, "0;35");
    set(p, Style::selected, "0;30;46");
    set(p, Style::selected_inactive, "0;7");
    set(p, Style::border, "0;90");
    set(p, Style::border_focus, "0;36");
    set(p, Style::input, "0;4");
    set(p, Style::button, "0;36");
    set(p, Style::button_focus, "0;30;46");
    set(p, Style::shadow, "0;90");
    set(p, Style::bar, "0;7");
    set(p, Style::bar_active, "0;1");
    set(p, Style::bar_accent, "0;1;7;36");
    set(p, Style::bar_error, "0;1;97;41");
    set(p, Style::raw, "0");
  }
  return t;
}

Theme classic_theme() {
  Theme t{"classic", "Classic (blue windows, grey dialogs)", {}, true};
  Palette& w = t.sgr[size_t(Layer::window)];
  set(w, Style::normal, "0;97;44");
  set(w, Style::dim, "0;37;44");
  set(w, Style::bold, "0;1;97;44");
  set(w, Style::title, "0;1;93;44");
  set(w, Style::accent, "0;96;44");
  set(w, Style::error, "0;1;91;44");
  set(w, Style::warning, "0;93;44");
  set(w, Style::ok, "0;92;44");
  set(w, Style::info, "0;36;44");
  set(w, Style::special, "0;95;44");
  set(w, Style::selected, "0;30;46");
  set(w, Style::selected_inactive, "0;30;47");
  set(w, Style::border, "0;37;44");
  set(w, Style::border_focus, "0;1;97;44");
  set(w, Style::input, "0;30;46");
  set(w, Style::button, "0;30;42");
  set(w, Style::button_focus, "0;1;97;42");
  set(w, Style::shadow, "0;90;40");
  set(w, Style::bar, "0;30;47");
  set(w, Style::bar_active, "0;30;42");
  set(w, Style::bar_accent, "0;31;47");
  set(w, Style::bar_error, "0;1;97;41");
  set(w, Style::raw, "0;97;44");  // a program's default colours: the window's
  Palette& d = t.sgr[size_t(Layer::dialog)];
  d = w;
  set(d, Style::normal, "0;30;47");
  set(d, Style::dim, "0;90;47");
  set(d, Style::bold, "0;1;30;47");
  set(d, Style::title, "0;1;30;47");
  set(d, Style::accent, "0;34;47");
  set(d, Style::error, "0;31;47");
  set(d, Style::warning, "0;30;43");
  set(d, Style::ok, "0;32;47");
  set(d, Style::info, "0;34;47");
  set(d, Style::special, "0;35;47");
  set(d, Style::selected, "0;97;46");
  set(d, Style::selected_inactive, "0;30;46");
  set(d, Style::border, "0;97;47");
  set(d, Style::border_focus, "0;1;97;47");
  set(d, Style::input, "0;97;44");
  return t;
}

const std::vector<Theme>& themes() {
  static const std::vector<Theme> all{default_theme(), classic_theme()};
  return all;
}

const Theme& theme_by_name(std::string_view name) {
  for (const auto& t : themes()) {
    if (t.name == name) return t;
  }
  return themes().front();
}

namespace {

char32_t double_line(char32_t c) {
  switch (c) {
    case U'─': return U'═';
    case U'│': return U'║';
    case U'┌': return U'╔';
    case U'┐': return U'╗';
    case U'└': return U'╚';
    case U'┘': return U'╝';
    default: return c;
  }
}

}  // namespace

// SGR for a raw cell: the theme's default, then the program's attributes and colours.
std::string raw_sgr(const Theme& theme, const Cell& c) {
  std::string s = theme.at(c.layer, Style::raw);
  if (c.attrs & attr_bold) s += ";1";
  if (c.attrs & attr_italic) s += ";3";
  if (c.attrs & attr_underline) s += ";4";
  if (c.attrs & attr_reverse) s += ";7";
  if (c.attrs & attr_strike) s += ";9";
  auto color = [&](uint32_t col, int base, int bright) {
    if ((col & 0xff000000u) == kColorIndexed) {
      const uint32_t i = col & 0xff;
      if (i < 8) s += ";" + std::to_string(base + int(i));
      else if (i < 16) s += ";" + std::to_string(bright + int(i) - 8);
      else s += ";" + std::to_string(base + 8) + ";5;" + std::to_string(i);
    } else if ((col & 0xff000000u) == kColorRgb) {
      s += ";" + std::to_string(base + 8) + ";2;" + std::to_string((col >> 16) & 0xff) + ";" +
           std::to_string((col >> 8) & 0xff) + ";" + std::to_string(col & 0xff);
    }
  };
  color(c.fg, 30, 90);
  color(c.bg, 40, 100);
  return s;
}

std::string diff(const Grid& prev, const Grid& next, const Theme& theme) {
  const bool full = prev.width() != next.width() || prev.height() != next.height();
  std::string out = "\x1b[?2026h";  // synchronized update; ignored where unsupported
  if (full) out += "\x1b[0m\x1b[2J";
  std::string sgr;  // the SGR in effect
  bool have_sgr = false;
  int cx = -1, cy = -1;
  for (int y = 0; y < next.height(); ++y) {
    for (int x = 0; x < next.width(); ++x) {
      const Cell& c = next.at(x, y);
      if (c.cont) continue;
      const bool wide = x + 1 < next.width() && next.at(x + 1, y).cont;
      if (!full && prev.at(x, y) == c && (!wide || prev.at(x + 1, y) == next.at(x + 1, y))) continue;
      if (cx != x || cy != y) out += "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H";
      std::string want = c.style == Style::raw ? raw_sgr(theme, c) : theme.at(c.layer, c.style);
      if (!have_sgr || want != sgr) {
        out += "\x1b[" + want + "m";
        sgr = std::move(want);
        have_sgr = true;
      }
      utf8::append(out, theme.double_focus && c.style == Style::border_focus ? double_line(c.ch) : c.ch);
      cx = x + (wide ? 2 : 1);
      cy = y;
    }
  }
  out += "\x1b[0m\x1b[?2026l";
  return out;
}

}  // namespace rcp
