# myrcp

A terminal-first rich client platform: one workbench (views, command palette,
keybindings, background jobs, a uniform look) for applications whose tools are Python
plugins. The core is a C++20 library with no required dependencies; plugins run in
their own process and describe their UI as a semantic tree, which the workbench draws,
in a terminal or as a GTK 4 application. Editors are Vim/Neovim: myrcp hands files to
`$VISUAL`/`$EDITOR`, or links to the one you run, instead of having an editor of its own.

Built in, in every application: Problems, Properties, Preferences, Consoles, Search
(`Ctrl+F`, ripgrep / git grep / grep), Tasks (TODO, FIXME, …), Outline, perspectives,
regions, terminal views, and the Neovim and Vim links.

Design, protocol and roadmap: [ARCHITECTURE.md](ARCHITECTURE.md).

## Install

```sh
pipx install myrcp        # or pip install myrcp, in an application's venv
myrcp --help
```

The wheel (Linux, x86_64 and aarch64) has the terminal workbench built in: the plugin
SDK (`import myrcp`), `libmyrcp` and the `myrcp` command, which runs plugin hosts in the
Python it was installed with. An application in Python depends on it (`myrcp>=0.9`, say
as an extra) and starts its product directory with `myrcp.launch.run_product(dir)` from
an entry point of its own. The GTK workbench, the C++ library for programs of their own
(`find_package(myrcp)`) and other platforms come from building it from source.

## Build and run

```sh
sudo dnf install cmake ninja-build gcc-c++ libvterm-devel   # or your system's equivalents
./build.sh               # build/: libmyrcp, the myrcp launcher, and the unit tests
./build.sh demo          # … with the demo application (examples/demo), and start it
./build.sh install       # a stripped release build into ~/.local (or ./build.sh install DIR)
./build.sh help          # also: test, release, asan, gtk, size, clean
```

libvterm is optional (without it there are no terminal views). An install has
`bin/myrcp`, `lib/libmyrcp.so`, `include/rcp/app.hpp`, the CMake package
(`find_package(myrcp)`), `myrcp.pc`, and the plugin SDK in `share/myrcp/python`.

## An application on myrcp

An application is a name and its plugins. Written in Python, it needs no program of its
own: a product directory, started by the launcher,

```
songs/myrcp/
├── product.toml          # [product] id = "songs", name = "Songs", plugins = ["plugins"]
└── plugins/songs/
    ├── myrcp.toml        # what the plugin contributes; python = ["../../../.venv/bin/python"]
    └── myrcp_songs/plugin.py
```

```sh
myrcp --product songs/myrcp          # or MYRCP_PRODUCT=songs/myrcp myrcp
myrcp --product songs/myrcp --list   # headless: what its plugins contribute
```

or a program of its own on the library (`examples/demo/main.cpp`):

```cpp
#include <rcp/app.hpp>

int main(int argc, char** argv) {
  rcp::AppInfo app;
  app.id = "songs";                       // its state: ~/.local/state/songs/
  app.name = "Songs";
  app.plugin_dirs = {"/usr/share/songs/plugins"};
  return rcp::run(argc, argv, app);       // rcp::run_gtk for the GTK workbench
}
```

```cmake
find_package(myrcp REQUIRED)
target_link_libraries(songs PRIVATE myrcp::myrcp)
```

The plugin manifest's `python` names the interpreter its plugin runs under (the
application's venv), so the application's own packages are importable there; the
workbench gives that interpreter the SDK. `myrcp` without `--product` runs the plugins
on `$MYRCP_PLUGIN_PATH`.

**The demo** (`examples/demo`, `-DMYRCP_DEMO=ON`, `./build.sh demo`) is such an
application, for developing myrcp itself; it is not built or installed by default. Its
plugin shows a file browser, background jobs, a sortable table, problem markers,
preferences, and in "Perspective: Gallery" every widget of the UI IR (fields, buttons,
check boxes, progress, lists, trees with check boxes, tables, logs, a modal dialog), a
view with instances, consoles, comparing and terminal views.

**GTK.** The same workbench as a GTK 4 application, built from the same plugins and
views with native widgets (lists and tables, frames, dialogs, popover menus, panes of
tabs you drag between), is a build option:

```sh
sudo dnf install gtk4-devel vte291-gtk4-devel   # Debian/Ubuntu: libgtk-4-dev libvte-2.91-gtk4-dev
./build.sh gtk                                  # build-gtk/: libmyrcp-gtk, myrcp-gtk; starts the demo
MYRCP_GTK=ON ./build.sh install                 # installs them next to libmyrcp and myrcp
```

VTE is optional there too: it gives shells and editors in tabs. See "The GTK workbench"
in [ARCHITECTURE.md](ARCHITECTURE.md).

| Key | |
|---|---|
| `Ctrl+P` | command palette (every command, fuzzy) |
| `Alt+1`…`Alt+9`, `Ctrl+PgUp/PgDn` | switch views (Alt+N any tab, Ctrl+PgUp/PgDn within the region) |
| `Ctrl+O` | Go to Anything: fzf over the files below the current directory (`rg`/`fd` when there, else `find`), the open views and the problems; the one chosen opens (in a terminal view: `Ctrl+\` `Ctrl+O`) |
| `Alt+←` / `Alt+→` | Back to where you were (the view, its focused list, the row) and Forward again |
| `F6` / `Shift+F6`, `Ctrl+\` + arrows | focus the next region / the one in that direction; a click focuses one too |
| `Alt+Z` (`Ctrl+\ z`) | maximize: all tabs in one bar, one view at a time; again to restore |
| `Ctrl+\` + `Shift`+arrows | resize: widen/narrow the side, raise/lower the bottom (or drag the separator, the bottom's tab bar) |
| `Tab` / `Shift+Tab` | move focus between widgets |
| `Ctrl+R` | restart the plugin host (after editing a plugin) |
| `Ctrl+W` / `Ctrl+Z` / `Ctrl+Q` | close view / suspend / quit |
| right click, `Shift+F10` | the context menu: on a row (the cursor goes there), the view's commands, Properties, sorting, moving and closing the view; on a tab, the tab's (a terminal view's program gets the click when it uses the mouse) |
| `Alt+S`, `Alt+R` | in a table: sort by the next column, the other way (a click on a column's title too: by it, the other way, not) |
| `Alt+Enter` | Properties: what the cursor is on in the focused view (a file, a problem, a match, a symbol, what a plugin's rows describe), with the source around its line |
| `F8` | Problems: what plugins report (the demo's TODOs, say) and a linked editor's diagnostics; Enter opens the place |
| `Ctrl+F` | Search: text in the files below the working directory, as a tree of files and lines (ripgrep, else git grep, else grep); `F5` again, `*`/`/` open/close all |
| Tasks: Show, Outline: Show | Tasks: the TODO, FIXME, XXX and HACK comments there, most urgent first (`r` scans again). Outline: the classes and functions, headings or sections of the file the selection is in (a linked editor's cursor, a match, a problem), following its line; `f` stops it following |
| Console: Run a command… | a program's output in a Console view: `path:line` lines open on Enter, `Ctrl+C` stops it, `r` runs it again (plugins: `rcp.run_in_console(argv)`) |
| `Ctrl+T` | a shell in a tab, at the bottom (in a terminal view, `Ctrl+\` first: the next key is the workbench's) |
| mouse | click tabs, buttons, rows and tree arrows/boxes; the wheel scrolls (`MYRCP_MOUSE=0` turns it off) |

**Regions.** Views sit in the main area, at the side (right) or at the bottom, each a
stack of tabs: Jobs, Problems and shells open at the bottom, a manifest says where its
views go (`region = "side"`), and "View: Move to the side / bottom / main area" in the
palette moves the focused one. The side needs 140 columns and the bottom 34 rows; below
that, or while maximized, they fold into the main area's tabs, which is all a small
terminal shows.

**Perspectives.** A perspective is a named arrangement of views for a task: which are
open, in which region, and how big the regions are. "Perspective: …" in the palette switches: the views the next one lacks close, those it
has open where it puts them; shells stay. Each remembers how you left it — views opened,
moved or closed, sizes, what each region shows, the focus — across switches and
sessions, and a session starts in the one the last ended in ("Perspective: Reset"
returns to its definition). Default is the plugins' startup views; plugins bring their
own (the demo's Gallery: its widgets, Properties and the Outline at the side, Problems,
Search and Tasks below). Your own go
in `~/.config/myrcp/perspectives.toml`, as plugins declare theirs:

```toml
[[perspectives]]
id = "review"
title = "Review"
main = ["demo.files", "demo.table"]   # view ids, in tab order
side = ["properties"]
bottom = ["problems", "demo.jobs"]
side_size = 40     # percent of the width (15–75)
bottom_size = 25   # percent of the height
```

**Neovim.** With `$VISUAL` (or `$EDITOR`) a Neovim, myrcp links to it over its socket
(`--listen`), whether it runs in a tab of the workbench (Editor mode `tab`), in a tmux
window or split beside it (`tmux-window`, `tmux-split`), or is your own: start one with
`nvim --listen /tmp/nvim.sock` in any pane and set the preference "Your Neovim"
(`$MYRCP_NVIM_SERVER`) to that path. Then, both ways:

- files open in that Neovim, and problems jump there, instead of starting another editor;
- plugins' problems are its diagnostics (signs, virtual text);
- where its cursor rests is the workbench selection: Properties shows the line, and
  the Outline follows it to the function or section around it;
- its own diagnostics (LSP servers, linters) are in Problems, next to the plugins'.

**Vim.** A Vim with `+channel` (`vim --version`) is linked the same way: myrcp listens
on a socket and the Vim connects with a script of myrcp's (`myrcp --vim-script` prints
it). The Vims myrcp starts — in a tab, a tmux window or a split — load it themselves. For
your own, save it as a plugin once:

```sh
myrcp --vim-script > ~/.vim/plugin/myrcp.vim
```

and any Vim you start links while myrcp runs (it tries every two seconds, quietly;
`:MyrcpLink` tries now). Files and problems open there; plugins' problems are signs with
the message after the line; its cursor is the selection; and its quickfix list (`:make`,
`:grep`, `:cexpr`) is in Problems. The socket is `$XDG_RUNTIME_DIR/myrcp-vim.sock`, or the
preference "Your Vim" (export it to Vim as `$MYRCP_VIM_SOCKET`; myrcp's own terminals
have it).

**Comparing.** Plugins compare two files with `rcp.compare(left, right)` ("Demo: Compare
two files"). A linked Vim or Neovim
shows them in a diff tab of its own; otherwise the preference "Compare with"
(`$MYRCP_DIFF`, default `vimdiff`) runs where the editor would (Editor mode).

**Preferences.** "Preferences: Show" lists every
setting: the workbench's (theme, editor mode, mouse, the task tags) and the plugins'
(the demo's dotfiles and start folder), with
where each value comes from. Enter changes one: a bool flips, a choice or a text opens a
dialog. They are kept in `~/.config/myrcp/preferences.toml`, a table per plugin, which
you can also edit by hand ("Preferences: Edit the file" reads it again when the editor
exits); myrcp changes only the lines of what it sets, so your comments stay. An
environment variable that is set still wins (`MYRCP_THEME`, `MYRCP_EDITOR_MODE`,
`MYRCP_MOUSE`, `MYRCP_NVIM_SERVER`, `MYRCP_DIFF`, and the plugins' own), and the view
says so. A plugin declares its own:

```toml
[[preferences]]
id = "folders"            # songs.folders; rcp.pref("songs.folders") reads it
title = "Song folders"
type = "paths"            # string, bool, int, choice (with choices = [...]), paths
default = []
env = "SONGS_PATH"        # optional: wins while it is set
description = "Where to look for songs."
```

Environment: `MYRCP_PRODUCT` (the launcher's `--product`), `MYRCP_PLUGIN_PATH` (more
plugin directories), `MYRCP_PYTHON` (the Python of the main plugin host), `MYRCP_SDK`
(the SDK, when not next to the library), and the preferences' variables above. Startup
tabs keep the plugins' order whichever host starts first. The plugin host logs to
`~/.local/state/<application>/host.log`; the perspectives as left are in
`workbench.state` next to it.

## A plugin

`<plugin directory>/<name>/myrcp.toml` declares what the plugin contributes; the module
is imported only when one of its views opens or commands runs. See
`examples/demo/plugins/demo/` and ARCHITECTURE.md.

```python
from myrcp import Plugin, View, ui

plugin = Plugin()

@plugin.view("hello.main")
class Hello(View):
    def setup(self):
        self.count = 0

    def render(self):
        return ui.panel("Hello", ui.vbox(
            ui.text(f"Pressed {self.count} times"),
            ui.button("inc", "Press", on_press=self.press),
        ))

    def press(self):
        self.count += 1
```

Rows can say what they are, which makes them the workbench selection when the cursor is
on them; any view can follow it:

```python
ui.row(path, cells, selection=Item("file", path.name, path, line, props={"Size": size}))

def on_selection(self, selection):          # a View method, called in every plugin
    self.files = selection.of_type("file")
```

## Tests

```sh
./build.sh test          # build/ with the demo, the unit tests, then the Python tests (.venv made with uv)
./build.sh asan          # the unit tests under AddressSanitizer and UBSan, in build-asan/
```

The Python tests drive the plugin host over the real protocol and run the demo
application in a pseudo-terminal (skipped when it is not built), and the GTK one under
Xvfb (skipped without `./build.sh gtk`, Xvfb and xdotool).

## Licence

MIT; see [LICENSE](LICENSE).
