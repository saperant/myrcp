// A grid of character cells with semantic styles, and the ANSI diff between two grids.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace rcp {

// Semantic styles: plugins name what a thing is, the theme decides how it looks.
enum class Style : uint8_t {
  normal,
  dim,
  bold,
  title,
  accent,
  error,
  warning,
  ok,
  info,     // blue
  special,  // magenta
  selected,           // cursor row / focused widget
  selected_inactive,  // cursor row of an unfocused list
  border,
  border_focus,
  input,
  button,
  button_focus,
  shadow,      // cast by dialogs
  bar,         // top and bottom bars
  bar_active,  // the active tab
  bar_accent,
  bar_error,
  raw,  // a program's own colours (a terminal view): Cell::fg/bg/attrs
  count_,
};

// Style names usable from plugins ("normal", "dim", ...). Chrome-only styles are excluded.
std::optional<Style> style_from_name(std::string_view name);

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

// Cells belong to a layer, and each layer has a palette of its own: windows, or
// dialogs drawn over them (as Turbo Vision had blue windows and grey dialogs).
enum class Layer : uint8_t { window, dialog, count_ };

// Colours of raw cells: 0 is the theme's default, else palette index or RGB.
constexpr uint32_t kColorIndexed = 1u << 24;  // | 0..255
constexpr uint32_t kColorRgb = 2u << 24;      // | 0xRRGGBB
enum CellAttr : uint8_t { attr_bold = 1, attr_underline = 2, attr_italic = 4, attr_reverse = 8, attr_strike = 16 };

struct Cell {
  char32_t ch = U' ';
  Style style = Style::normal;
  bool cont = false;  // right half of a wide character
  Layer layer = Layer::window;
  uint8_t attrs = 0;  // raw cells
  uint32_t fg = 0, bg = 0;
  bool operator==(const Cell&) const = default;
};

class Grid {
 public:
  Grid() = default;
  Grid(int w, int h);

  int width() const { return w_; }
  int height() const { return h_; }
  bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
  const Cell& at(int x, int y) const { return cells_[size_t(y * w_ + x)]; }

  void fill(Rect r, Style s, char32_t ch = U' ');
  // Changes the style of the cells in r, keeping their characters.
  void restyle(Rect r, Style s);
  // Moves the cells in r to a layer.
  void set_layer(Rect r, Layer layer);
  // The shadow a box at r casts: two columns to its right, one row below.
  void shadow(Rect r);
  // Puts one code point; wide characters take two cells (a space if only one fits
  // before x_end). Returns the column after it.
  int put(int x, int y, char32_t ch, Style s, int x_end);
  // A cell in a program's own colours (fg/bg: 0, kColorIndexed | i or kColorRgb | rgb).
  int put_raw(int x, int y, char32_t ch, uint32_t fg, uint32_t bg, uint8_t attrs, int x_end);
  // Writes UTF-8 text clipped to [x, x_end). Returns the column after the text.
  int text(int x, int y, std::string_view s, Style st, int x_end);
  int text(int x, int y, std::string_view s, Style st) { return text(x, y, s, st, w_); }

  std::string row_text(int y) const;  // for tests

 private:
  Cell& cell(int x, int y) { return cells_[size_t(y * w_ + x)]; }
  int w_ = 0, h_ = 0;
  std::vector<Cell> cells_;
};

// SGR parameters per layer and style, e.g. "0;36".
struct Theme {
  std::string name, title;
  std::array<std::array<std::string, size_t(Style::count_)>, size_t(Layer::count_)> sgr;
  bool double_focus = false;  // focused frames in double lines (╔═╗)

  const std::string& at(Layer l, Style s) const { return sgr[size_t(l)][size_t(s)]; }
};

// "terminal": the terminal's own 16 colours on its own background (the default).
Theme default_theme();
// "classic": blue windows, grey dialogs with shadows, grey bars; after the 1990s
// text-mode IDEs.
Theme classic_theme();
const std::vector<Theme>& themes();
// The theme with this name, or the default.
const Theme& theme_by_name(std::string_view name);

// Escape sequences that turn the screen showing `prev` into `next`. A size mismatch
// (including an empty prev) redraws everything.
std::string diff(const Grid& prev, const Grid& next, const Theme& theme);

}  // namespace rcp
