# myrcp architecture

A terminal-first rich client platform: one workbench (views, commands, keybindings,
jobs, a uniform look), tools as plugins, their views rendered in a terminal. myrcp is a
framework: applications are built on it (libmyrcp, or a product directory the `myrcp`
launcher starts) and bring their own plugins.

## Principles

1. **Views describe, hosts render.** A plugin view returns a *semantic* UI tree (the UI IR):
   panels, lists, fields, buttons, progress bars. It never draws cells. A host lays the
   tree out its own way, as SWT used native widgets per platform.
2. **Plugins run out of process.** A plugin host process per language/venv speaks
   msgpack-RPC to the core. A crashing plugin, a slow import or the GIL never freezes the
   workbench, and `Ctrl+R` restarts the host while the workbench keeps its state.
3. **Lazy activation.** What a plugin contributes (views, commands, keys) is a static
   `myrcp.toml`. The host reads manifests without importing plugin code, and imports a
   plugin only when one of its views opens or one of its commands runs.
4. **Integrate, don't reimplement.** Editors are Vim/Neovim; tmux stays the multiplexer.
   myrcp runs inside tmux and can open things in tmux windows and splits.
5. **Everything stays usable without myrcp.** Plugins adapt existing tools, which keep
   their CLIs; an application's plugins live with the application, not in myrcp.
6. **What every workbench needs is built in.** Problems, Properties, Preferences,
   Consoles, Search, Tasks and Outline are the hub's own (C++, no plugin host needed),
   in every application.

## Processes

```
┌──────────────────────────── myrcp (C++20) ─────────────────────────────┐
│ Workbench: views · command registry + palette · keybindings · jobs ·   │
│            status · editor handoff                                     │
│ UI IR renderer (layout → cell grid) · grid diff → ANSI · key decoder   │
└──────────────▲─────────────────────────────────────────────────────────┘
               │ msgpack-RPC over pipes (stdin/stdout)
┌──────────────┴────────────────┐
│ python -m myrcp.host          │
│  manifests → contributions    │
│  lazy import, View instances  │
└───────────────────────────────┘
```

Source layout:

| Path | What |
|---|---|
| `core/include/rcp/msgpack.hpp` | `mp::Value`, encoder, streaming decoder (also what Neovim `--embed` speaks) |
| `core/include/rcp/grid.hpp` | `Grid` of cells with semantic `Style`s, `Theme`, `diff()` → ANSI |
| `core/include/rcp/keys.hpp` | `KeyDecoder`: bytes → key names (`ctrl+p`, `alt+1`, `pagedown`, `paste`) |
| `core/include/rcp/layout.hpp` | UI IR → grid: layout, widget state, focus order |
| `core/include/rcp/app.hpp` | the public API: `AppInfo`, `rcp::run()`, `rcp::run_gtk()`, `load_product()` |
| `core/src/app.cpp` | `rcp::run`: options, `--list`, the terminal workbench; `launcher.cpp` the `myrcp` program |
| `core/src/workbench.cpp` | the event loop, chrome, palette, key routing, host protocol |
| `core/src/hub.hpp` | `Hub`, the workbench model; `service.hpp` its built-in features with views (`search.cpp`, `outline.cpp`), `console.hpp` the consoles |
| `core/src/editor_link.hpp` | links to Neovim (`nvim.cpp`, msgpack-RPC) and Vim (`vim.cpp`, JSON channels; `json.cpp`) |
| `core/src/editor_links.hpp` | `EditorLinks`: a front-end's linked editors (start, poll, the live one, selection, markers) |
| `gtk/` | `libmyrcp-gtk` (`-DMYRCP_GTK=ON`): the workbench as a GTK 4 application; `render.cpp` the IR as GTK widgets, `workbench.cpp` the chrome |
| `python/myrcp/` | the SDK: `ui` builders, `Plugin`, `View`, `Context`, `Job`, the host |
| `examples/demo/` | the demo application (`-DMYRCP_DEMO=ON`): a `main.cpp` on libmyrcp and its plugin (files, jobs, a table, a gallery of every widget and service) |

`rcp_core` (msgpack, JSON, grid, keys, layout) does no I/O. `rcp_hub`
(`core/src/hub.hpp`) is the workbench model: plugin hosts and their delegation,
contributions, open views and their trees, jobs, plugin commands and events, and the
built-in services. The front-end (`core/src/workbench.cpp`, the IR into a cell grid)
owns presentation only (widget state, focus, drawing) and hears the hub through
`Hub::Listener`. A second front-end, the GTK one, is built on the same hub (below, "The
GTK workbench"). These are static libraries inside the shared ones, which export only
`rcp/app.hpp` (hidden visibility for the rest).

## Applications

An application is an `rcp::AppInfo`: an id (its state, `$XDG_STATE_HOME/<id>/`: the
perspectives as left, the host log), a name (the title bar, `--help`, `--version`), a
version and its plugin directories (searched after `$MYRCP_PLUGIN_PATH`). It starts the
workbench one of two ways:

- **A program on libmyrcp**, as `examples/demo/main.cpp` is: `find_package(myrcp)` and
  `myrcp::myrcp` (or `pkg-config myrcp`), then `return rcp::run(argc, argv, app);`
  (`rcp::run_gtk` with `myrcp::myrcp-gtk`).
- **A product directory** for the launcher, for an application in Python that wants no
  C++ build: `myrcp --product DIR` (or `$MYRCP_PRODUCT`) reads `DIR/product.toml`:

  ```toml
  [product]
  id = "songs"
  name = "Songs"
  version = "1.2.0"
  plugins = ["plugins"]   # relative to DIR; default: DIR itself
  ```

The SDK is found next to the library that runs (`<prefix>/share/myrcp/python` beside
`<prefix>/lib/libmyrcp.so`, by `dladdr`), else in the source tree of a build tree, and
`$MYRCP_SDK` wins over both. Each plugin's manifest may name the Python it runs under
(below, "One host per interpreter"), so an application's plugins run in its own venv
while the workbench stays the same program.

An install (`cmake --install`, `./build.sh install`) has `bin/myrcp` (and `myrcp-gtk`),
`lib/libmyrcp.so` (and `libmyrcp-gtk.so`), `include/rcp/app.hpp`,
`lib/cmake/myrcp/` (the CMake package), `lib/pkgconfig/myrcp.pc` and the SDK in
`share/myrcp/python`.

## UI IR

A node is a msgpack map with `t` (type) plus properties. Styles are semantic names
(`normal dim bold title accent error warning ok`), so every tool looks the same and each
host maps them to its own colours (terminal theme → 16 ANSI colours by default).

| `t` | Properties | Interaction |
|---|---|---|
| `text` | `text` (string, or `[[text, style], ...]` spans), `style`, `wrap` (spans wrap word by word, keeping their styles) | |
| `vbox`, `hbox` | `children`, `gap` | |
| `panel` | `title`, `child` | border highlights when focus is inside |
| `list` | `id`, `items` (`{key, text, detail, style}`), `empty`, `selected` | `select`, `activate` (value: item key) |
| `button` | `id`, `label` | `press` |
| `field` | `id`, `value`, `placeholder` | `submit` (value: text) |
| `progress` | `value` (0..1), `label` | |
| `table` | `id`, `columns` (`{title, width, grow, align, clickable, sortable}`), `rows` (`{key, cells, style, depth, expandable, expanded}`), `offset`, `total`, `cursor` (`[index, seq]`, applied when `seq` changes), `header`, `empty`, `sortable`, `sorted` (`[column, descending]`), `follow` (`[index, nonce]`), `tree` (a column index: the rows are a tree drawn in that column, indented by `depth`, with arrows) | `select`, `activate`, `window`, `cell` (clicks on `clickable` columns, Shift/Ctrl+clicks: `{key, column, shift, ctrl}`), `sort` (`[column, descending, key at the cursor]`; column -1: unsorted), `expand`/`collapse` (a `tree` table: →/←, arrow clicks) |
| `tree` | `id`, `rows` (`{key, text, detail, depth, expandable, expanded, check: all/some/none, style}`, flattened by the plugin), windowed like `table` | `select`, `activate`, `expand`, `collapse`, `toggle` |
| `checkbox` | `id`, `label`, `checked` | `toggle` (value: the new state) |
| `log` | `id` (optional: focusable, scrollable), `lines` | |
| `modal` | `id`, `title`, `child` (the base, dimmed), `dialog`, `dialog_width`, `dialog_height` | `dismiss` (Escape) |

Every node takes layout hints: `grow` (flex weight), `width`, `height`; focusable ones
take `autofocus` (the initial focus of the view). A view that shows only, say, a mode
button while it loads has that focus by default until the `autofocus` widget appears; a
focus chosen with Tab or a click stays.

**Windowed tables.** A table sends only rows `offset .. offset+len(rows)` of `total`.
When the cursor scrolls past them the workbench sends a `window` event `[first, count]`
and the SDK answers with those rows, so a plan of 100 000 items costs what a screenful
does (`ui.lazy_rows(count, make_row)` builds only the rows sent).

**Sortable tables.** `ui.table(..., sortable=True)`: a click on a column's title (or
Alt+S, the next column; Alt+R, the other way) sends a `sort` event, and the host keeps
how each table is sorted, as it keeps windows. The SDK sorts before it cuts the window,
so the front-ends never need all the rows: by the cells' text in natural order (numbers
by value, sizes with their units), stably, empty cells last. The event carries the key
at the cursor; the SDK answers with `follow`, that row's new index, which the front-end
applies once, so the cursor stays on its row. A plugin's own `cursor` (an index in its
order) is translated into the sorted one; front-ends apply it by its `seq`, so a sort
alone does not move it. A plugin with more rows than it wants to build sorts them itself
(`on_sort(column, descending)`), and its rows are shown as given. The workbench
remembers the sort it asked for until the tree catches up, so quick keys add up.

**Mouse.** The workbench reports presses and the wheel (SGR mode; `MYRCP_MOUSE=0` turns
it off; Shift+drag still selects text). Every render records where each widget was
drawn, and a click maps to the same semantic events as keys: tabs switch, buttons press,
checkboxes toggle, rows select (a click on the cursor row activates), tree arrows
expand/collapse and boxes toggle, the wheel moves the cursor or scrolls a log. Plugins
never see coordinates.

**Context menus.** A right click (or Shift+F10, at the focused row) opens a menu where
it was: on a row, the cursor goes there first and the menu lists the view's commands
(its manifest's, without their "View: " prefix, with their keys), Properties, sorting
for a sortable table, then the tab's items (move to another region, maximize, close); on
a tab or a terminal view whose program has no use for the mouse, the tab's items. The
commands run as from the palette, so they act on that row.

**Modals.** While a tree has a `modal`, only its dialog takes focus and the view's own
key bindings are off (workbench keys such as Ctrl+P still work).

**Button presses carry the form**: the press event's value is `{form: {field id: text}}`
for every field in the view, so a dialog reads what was typed without a round trip per key.
Defaults: in a `vbox`, lists (and containers holding one) grow; in an `hbox`, text and
buttons take their natural width and everything else shares the rest.

Transient widget state lives in the host, not the plugin: list cursor and scroll, field
text and caret, focus. The plugin hears only semantic events. Two props are applied only
when their value changes, so a plugin can steer without owning the state:
`field.value` (e.g. clearing a field after submit) and `list.selected` (move the cursor
to a key, e.g. the directory you came from).

## Protocol

msgpack-RPC framing, as Neovim uses it: `[0, id, method, [params]]` request,
`[1, id, error, result]` response, `[2, method, [params]]` notification. `params` is
always one map.

Core → host (and back: the host also sends requests, e.g. `editor.open`):

| Message | Params | Result |
|---|---|---|
| request `initialize` | `protocol` | `plugins: [{id, name, views, commands, perspectives}]`, `perspectives` (the user's), `delegates`, `errors` |
| request `view.open` | `view` | `tree` |
| notify `view.event` | `view, widget, kind, value` | (host answers with `view.update`) |
| notify `view.close` | `view` | |
| notify `command.run` | `command, view` (active view), `widget, key, index` (the focused row) | |
| notify `job.cancel` | `job` | |
| notify `preferences.changed` | `values: {id: value}` (all of them before a host's views open, then what changed) | |
| notify `selection.changed` | `view, title, items` (the workbench selection; sent to every host) | |
| notify `shutdown` | | |

Host → core: `view.update {view, tree, title}`, `view.show {view}` (open/switch to,
e.g. `tool.job#photos`), `view.close {view}`, `status {text, style}`,
request `editor.open {path, line}` → `{waited}` (true: the editor ran in the foreground
and has exited), `compare.open {left, right, title}` (notify, or a request → `{waited}`),
`console.run {argv, cwd, title, name}`, `terminal.open {argv, cwd, title}`,
`job.begin {job, title}`, `job.progress {job, value, label}`, `job.end {job, ok, text}`.

**Consoles** (`core/src/console.hpp`): programs run for their output, owned by the hub
so the workbench shows them and any plugin starts one (`rcp.run_in_console(argv)`) —
the output of an Ansible run, an ffmpeg encode, a build. Each is a view `console#<n>`
(not saved in the arrangement): the command in a field (edit it, Enter runs it), the
output as rows (stdout and stderr as they come, colour codes dropped, `\r` progress
lines kept as their last state, 20 000 lines at most), the cursor following the end
while it is there. A line naming an existing `path:line` (compilers, test runners,
Python's `File "x", line n`) is a link: Enter opens it, and it is the selection.
`Ctrl+C` stops it (SIGINT, then SIGTERM, then SIGKILL, to its process group), `r` runs
it again; a run with the same `name` reuses its console. "Console: Run a command…"
opens an empty one.

**View instances.** A view id with `#arg` is an instance of that view for `arg` (one tab
per job, say; the demo's `demo.note#<colour>`). The plugin's View gets `self.arg`; key bindings scoped to the view
apply to all its instances.

**One host per interpreter.** A manifest may name the Python it needs:
`python = ["$TOOL_PYTHON", "../../.venv/bin/python"]` (first existing wins, relative to
the plugin directory). The primary host does not serve such a plugin; it
lists it under `delegates` in its `initialize` answer, and the workbench starts a host
with that interpreter and `MYRCP_HOST_PLUGINS=<ids>`. Each tool keeps its own venv.

The host coalesces re-renders (a job reporting progress 1000 times a second causes at
most one render per batch) and skips `view.update` when the tree did not change.

## Plugins

```toml
# examples/demo/plugins/demo/myrcp.toml
[plugin]
id = "demo"
name = "Demo"
module = "myrcp_demo.plugin"     # imported on first use, must define `plugin`

[[views]]
id = "demo.files"
title = "Files"
startup = true
# region = "bottom"              # main (default), side or bottom: where it opens

[[commands]]
id = "demo.files.up"
title = "Files: Parent directory"
key = "backspace"
view = "demo.files"              # binding and palette entry only while this view is active

[[perspectives]]                 # a named arrangement: Perspective: Files in the palette
id = "files"
title = "Files"
main = ["demo.files"]            # view ids per region, in tab order
bottom = ["demo.jobs", "problems"]
bottom_size = 25                 # percent of the height (side_size: of the width)

[[preferences]]                  # a setting: demo.hidden, in the Preferences view
id = "hidden"
title = "Show dotfiles"
type = "bool"                    # string, bool, int, choice (choices = [...]), paths
description = "Whether Files lists names starting with a dot when it opens."
```

Discovery: every directory in `MYRCP_PLUGIN_PATH` whose subdirectories contain a
`myrcp.toml`, and installed packages registering the entry point group `myrcp.plugins`
(value: the package holding `myrcp.toml`).

```python
from myrcp import Plugin, View, ui

plugin = Plugin()

@plugin.view("demo.files")
class Files(View):
    def setup(self): self.cwd = Path.cwd()
    def render(self):
        return ui.panel(str(self.cwd), ui.listbox("entries", items, on_activate=self.open))
    @plugin.command("demo.files.up")
    def up(self): self.cwd = self.cwd.parent
```

All plugin code (setup, render, handlers, commands, job `on_done`) runs on the host's
main thread. `rcp.run_job(title, fn, on_done=...)` runs `fn(job)` in a thread
(`job.progress()`, `job.check()` for cancellation); `on_done` applies the result back on
the main thread.

## Regions

The workbench has three regions, after Eclipse's editor area and its view stacks: main
(top left), side (its right) and bottom (below both). Each is a stack of tabs. Which
region a view is in belongs to the hub (`ViewData::region`, from the manifest's `region`,
changed with `Hub::set_region`); which view each region shows
and which region has the focus belong to the front-end.

- The terminal shows the side from 140 columns and the bottom from 34 rows. Below that,
  or while maximized (Alt+Z), a region folds into main: its views become main's tabs, so
  a small terminal works exactly as before regions. Tabs keep the hub's numbering
  (Alt+N), whichever region they are in; Alt+←/→ stays within the focused region.
- Main's tabs are in the top bar, the side's there too past the separator, the bottom's
  in a bar of their own. The focused region's active tab is highlighted; an unfocused
  view still shows its cursor row, inactive. Each visible view asks for its table
  windows and is drawn at its own size (a terminal view is resized to its region).
- F6 / Shift+F6 cycle the focus; Ctrl+\ then an arrow moves it in that direction, also
  out of a terminal view; a click focuses the region it lands in, the wheel scrolls one
  without focusing it.
- Sizes are the hub's (`Hub::sizes`, percent of the width and the height): `Ctrl+\`
  Shift+arrows or a drag of the separator or the bottom's tab bar (SGR button-event
  mouse mode, 1002; saved on release).

## Preferences

Settings with a declared type, after Eclipse's preference store, kept where a user
expects configuration: `$XDG_CONFIG_HOME/myrcp/preferences.toml`, a table per plugin
(`[workbench]`, `[search]`, `[demo]`, ...).

- Declared by the front-end (`declare_workbench_prefs`: the theme, the editor mode and
  the mouse) and by plugins (`[[preferences]]`: `id`, `title`, `type`
  string/bool/int/choice/paths, `default`, `choices`, `env`, `description`; the id
  becomes `<plugin>.<id>`). The hub owns the values, so they are there before any host
  is: the terminal's theme and mouse apply before the first draw.
- A value is its `env` variable's while that is set (the variables myrcp read before
  preferences keep working, and win), else the file's if it fits the type, else the
  default. The front-end hears changes (`Listener::prefs_changed`); every host gets all
  values (`preferences.changed`) right after it registers, before its views open, then
  each change; `Context.pref(id)` reads them and `View.on_preferences(changed)` hears
  them, after which every view renders again.
- The file is read and written by a TOML subset of the hub's own (`prefs.cpp`: tables,
  strings, bools, integers, arrays, comments), which rewrites only the lines of what it
  sets, so hand edits and comments survive; setting the default removes the line. A
  file with a syntax error is not written to until it is fixed (the view says where).
  Ctrl+R (restart the hosts) and "Preferences: Edit the file" read it again.
- The Preferences view is the hub's own, in the IR like Problems: a row per setting
  with where its value comes from, its description below, a dialog to change it (a
  field, or the list of choices; Default; a note when an environment variable wins).
  Its rows carry selection items, so Properties shows a setting too.

## Perspectives

A named arrangement of views for a task, after Eclipse's: which views are open, in which
region and tab order, and the regions' sizes. The hub owns them.

- Defined by plugins (`[[perspectives]]` in `myrcp.toml`: `id`, `title`, `main`/`side`/
  `bottom` view ids, `side_size`/`bottom_size`) and by the user
  (`$XDG_CONFIG_HOME/myrcp/perspectives.toml`, read by the main host, the same tables;
  the user's win on an id). "default" is the manifests' startup views.
- An `Arrangement` is a perspective as left: views with regions in tab order, sizes,
  what each region shows and the focus (the front-end's part, asked for with
  `Listener::front_state`), and the definition's views at the time (`known`: a view
  added to the definition since, say by a new plugin, opens next time).
- Every change of the arrangement (opening, closing, moving a view; sizes) is saved at
  once, msgpack in `$XDG_STATE_HOME/myrcp/workbench.state`, written to a temporary file
  and renamed; the active perspective's entry is replaced, the others kept. Terminal
  views are not part of it and stay through switches.
- Switching saves the current one, closes the plugin and hub views the next one lacks,
  opens the rest where it puts them (`Hub::apply`) and tells the front-end what to show
  (`Listener::arranged`). Views whose host is not up yet wait in `pending_` at their
  index, so tabs keep the arrangement's order whichever host answers first; the same
  path opens the saved arrangement at startup, the hub's own views at once.
- A plugin view closed by a switch is disposed in its host; switching back opens it
  again (its `arg` included), not its scroll position or its widgets' state.

## The selection service

What the cursor of the focused view is on is the workbench selection, after Eclipse's
selection service and NetBeans' Lookup, made of data rather than objects since plugins
live in separate processes.

- A row says what it is: `ui.row(key, cells, selection=Item("demo.colour", "blue",
  path, line, props={...}))` (also `ui.item`, `ui.tree_row`) puts `sel: [items]` on the
  row in the IR. An item has a `type` ("file", "problem", "search.match",
  "outline.function", ...), a `label`, an optional `path`/`line` and ordered
  `props`; a tree's own `sel` stands for the whole view.
- The front-end picks it (`view_selection` in `layout.cpp`): the cursor row of the
  focused rows widget, else of the first one whose rows carry any, else the tree's
  `sel`. A row without one makes an empty selection; a view without any (a terminal,
  Properties itself) leaves the selection as it was, so focusing Properties keeps what
  it shows. The terminal sets it when what the focused view offers changed (or the
  focus moved), so a linked Neovim outside the workbench keeps the selection it made
  until something here moves; a Neovim tab offers its cursor.
- The hub keeps it (`Hub::set_selection`) and tells every host (`selection.changed`):
  `View.on_selection(selection)` runs in every view that defines it, and
  `rcp.selection` is the last one heard (`selection.of_type("demo")`). The hub's services
  hear it too: Outline follows the file and line it names, so a linked editor's cursor
  moves the Outline's.
- **Properties** (`properties`, a hub-owned view at the side, Alt+Enter) shows it: where
  it is from, the item's properties (a table of items when several), and the source
  around `path:line` with the line marked; Enter opens a line in the editor.
- Producers: Problems (each problem at its place), Search and Tasks (each match at its
  line), Outline (each symbol at its line), Consoles (`path:line` lines), a linked
  editor's cursor, and the demo's Files, table and gallery.

## Built-in services: Search, Tasks, Outline

Features every application wants are the hub's own, as `Service`s
(`core/src/service.hpp`): a service declares views, commands (with keys) and preferences
the way a manifest does, builds its views' trees as UI IR itself (so both front-ends show
them with no code of their own), runs programs whose output the hub's event loop reads,
and hears the selection and preference changes. `Hub::builtin_view` covers them and the
hub's other views, which need no plugin host and open from a saved arrangement at once.

- **Search** (`search`, at the bottom; `Ctrl+F`, "Search: Find in files"): text in the
  files below the working directory, as a tree of files and their matching lines, by
  ripgrep when it is installed, else `git grep` in a work tree, else `grep` (each leaves
  out what version control ignores as well as it can; `core/src/search.cpp`). Regex and
  match case are check boxes; `F5` searches again, `*` and `/` open and close every
  file; Enter on a line opens it in the editor. At most 10 000 matches are kept.
- **Tasks** (`tasks`, at the bottom; "Tasks: Show"): the comments tagged with the
  preference `search.tags` (default `FIXME, XXX, TODO, HACK`, the first ones the most
  urgent), most urgent first; `r` scans again.
- **Outline** (`outline`, at the side; "Outline: Show"): the structure of the file the
  selection is in, following its line (the symbol around it is the cursor's): Python by
  its indentation outside strings and brackets, Markdown headings, TOML/INI sections,
  YAML keys, and C/C++, JavaScript/TypeScript, Rust, Go and shell by the shapes of their
  lines (`core/src/outline.cpp`). `f` stops it following; Enter goes to a symbol.

## Problem markers

A plugin reports problems as named sets (`rcp.set_markers(name, [Marker(...)])`,
`markers.set` on the wire): a message, a severity, a place in a file (path, 1-based line)
and/or a view that shows it. A set replaces the plugin's previous one of that name; a
host that exits takes its sets along.

The hub lists them in a **Problems** view (F8, or View: Open Problems) whose tree the hub
builds itself, as UI IR, so the front-end shows it with no code of their own. Errors come
first; paths under the working directory are relative. Enter opens the place: the view
that shows it, and the file at its line (`Listener::open_location`: in a linked Neovim or Vim when
there is one, else as an editor request). An editor that understands diagnostics gets the
markers too (Neovim and Vim, above).

The demo reports some for trying it out: "Files: Find TODOs" (TODO/FIXME lines at their
place) and "Demo: Problems that name views" (markers whose place is a view, one of them
a view instance).

## Terminal views

A terminal view is a native view: the hub lists it among the tabs (`Hub::open_native`), the
terminal front-end owns its process and screen. Keys go to the program, except Alt+1…9 and
Alt+←/→ (tabs); Ctrl+\ makes the next key the workbench's (Ctrl+\ Ctrl+P: commands,
Ctrl+\ Ctrl+W: close, Ctrl+\ ↑: the region above; Ctrl+\ twice sends one). Shells open
at the bottom, editors in the main area. Mouse presses go to programs that asked for
the mouse. Cells keep the program's colours (`Style::raw`); its default colours are the
theme's, so vim sits on blue in the classic theme. The tab closes when the program exits.

`editor.open` is a request the workbench may answer later (`Rpc::on_request_later`): an
editor in a tab answers `{waited: true}` when it exits, so `on_close` runs at the right time.

## Themes

Plugins name semantic styles only; a theme maps them to colours. Cells belong to a
layer (window or dialog) and each layer has its own palette, so dialogs can look
different from the windows under them. Modals and the command palette are dialog
layer and cast a shadow.

| Theme | Look |
|---|---|
| `terminal` (default) | the terminal's 16 colours on its own background |
| `classic` | blue windows, grey dialogs with drop shadows, grey bars with red hotkeys, green buttons, focused frames in double lines; after the 1990s text-mode IDEs |

`MYRCP_THEME=classic`, or "Theme: …" in the command palette.

## The GTK workbench

`libmyrcp-gtk` and `myrcp-gtk` (`cmake -DMYRCP_GTK=ON`, or `./build.sh gtk`; GTK 4.12
or later, VTE for GTK 4 optional; an application calls `rcp::run_gtk`) runs the same plugin hosts, views, commands, perspectives, preferences,
markers and editor links as the terminal workbench, through the same `Hub`. Plugins
need nothing new: their UI IR becomes GTK widgets. It translates the workbench's
concepts into GTK's own ways rather than imitating the terminal's look:

| Concept | In GTK |
|---|---|
| regions | three notebooks in panes: main and side above, the bottom below; empty ones hide. Tabs drag between them (the view's region changes), the panes' handles set the sizes kept with the perspective; maximize shows the focused region alone |
| `text` | a label; styles as Pango colours chosen for a light or dark theme; a line too long is cut with an ellipsis |
| `vbox` / `hbox`, `panel` | boxes (`grow` as expand; `width`/`height` in the UI font's characters and lines), a frame |
| `list`, `tree` | a `GtkListView`: the cursor is the selection, double click or Enter activates, tree rows have expander arrows and check buttons (`some`: inconsistent) |
| `table` | a `GtkColumnView`; a sortable one's column titles send `sort` (the plugin sorts; GTK shows the arrow). Windowed tables are a list model of `total` rows: a row not sent yet shows `…` and asks for its window |
| `field`, `button`, `checkbox`, `progress` | an entry (Enter submits), a button (the press carries the form), a check button, a progress bar |
| `log` | a monospace text view that stays at the end while it is there |
| `modal` | a modal dialog window over the view, whose base is insensitive meanwhile; closing it or Escape dismisses |
| palette, Go to Anything | a search window (Ctrl+P; Ctrl+O lists views, problems and the files from `rg --files`, else `git ls-files`, else `find`) |
| context menus | popover menus on a right click or Shift+F10/Menu, the commands' keys shown as accelerators |
| perspectives, views, plugins | the header bar: Back/Forward, a perspective menu, the palette button and the main menu |
| status, jobs | a status bar; the running jobs' progress and a cancel button at its right |
| terminal views | VTE tabs: shells (Ctrl+T), and files open in `$VISUAL`/`$EDITOR` there, a Vim or Neovim linked both ways as in the terminal's `tab` mode |

The tree is reconciled into the widgets as plugins send new ones (a node keeps its
widget while its type and id stay), so focus, scrolling, cursors and typed text survive
updates, and a dialog opening over a view keeps the view's widgets as its base. Keys
are bound as in the terminal workbench, except that navigation keys stay with the
widgets, plain keys are not commands while a field has the focus, and in a terminal
view the program has the keys but for Alt+1…9, Alt+←/→, Ctrl+PgUp/PgDn and
Ctrl+Shift+<key> (the workbench's Ctrl+<key>). The theme preference is the terminal's;
GTK follows the desktop's theme. Without VTE there are no terminal views, so files have
nowhere to open but a linked editor of your own.

## Editors

| Level | How | Status |
|---|---|---|
| 0 | Hand off to `$VISUAL`/`$EDITOR` (`+line path`). `MYRCP_EDITOR_MODE`: `suspend` (default; the TUI steps aside like git does), `tmux-window`, `tmux-split` | done |
| 1 | Terminal views: a pty + libvterm (the emulator Neovim uses) as a native view (`core/src/term.cpp`). `MYRCP_EDITOR_MODE=tab` opens files in a tab of their own, answered to the plugin when the editor exits; Ctrl+T opens a shell; plugins run programs with `rcp.open_terminal(argv)` | done |
| 2 | Neovim in a terminal view, also listening on a socket of ours (`--listen`): problem markers become `vim.diagnostic` entries (signs, virtual text) for every buffer, and opening a problem jumps in the open Neovim instead of starting another editor. Recognised by the editor command's name (`nvim…`) | done |
| 3 | The link both ways, in every editor mode (`core/src/editor_link.hpp`, `NvimLink` in `nvim.cpp`): Neovim in a tab, in a tmux window or split we open (`--listen` on our socket; the pane id from `tmux … -P` brings it forward), or the user's own (the `workbench.nvim_server` preference). Files and problems open in a linked Neovim rather than a new editor. Lua we send on connect (`nvim_get_api_info` gives our channel) notifies us where the cursor rests (debounced, only when the line or file changes) and its own diagnostics (all namespaces but ours, only when they change): the first become the workbench selection (from a tab only while it has the focus), the second a marker set `editor:<socket>` in Problems, never pushed back to it | done |
| compare | `rcp.compare(left, right)` (`compare.open`, a request when the plugin waits): `EditorLink::diff` in the linked editor (Neovim: `tabedit`, `diffthis`, `vertical diffsplit`; Vim: the same as a channel `ex` command), else `workbench.diff` (`vimdiff`) run as the editor would be: a tab, tmux, or in the foreground | done |
| 2+ | `nvim --embed` with `ext_linegrid` (Neovim's grid drawn straight into ours, without a pty) | planned |
| 2' + 3 | Vim, the same both ways (`VimLink` in `core/src/vim.cpp`). Vim has no server we can reach without `+clientserver`, so we are the server: a unix socket of ours per Vim we start (`--cmd "let g:myrcp_socket=…" -S <script>`), and a shared one (`VimListener`, `$XDG_RUNTIME_DIR/myrcp-vim.sock` or `workbench.vim_socket`) that the user's own Vims connect to with the same script installed as a plugin (`myrcp --vim-script`; it retries every two seconds). JSON channel messages, a line each (`core/src/json.cpp`): we send `["call", "MyrcpMarkers"/"MyrcpGoto", args]`; it sends `[n, {kind: hello/cursor/diagnostics}]`. Markers are signs plus virtual text (`prop_add(…, {text, text_align: 'after'})`); its own diagnostics are the quickfix list (`QuickFixCmdPost`). `EditorLink` is what the workbench sees of either kind | done |

## Roadmap

1. **Core + protocol + SDK**: terminal host, msgpack-RPC, the IR above, the Python host,
   editor handoff, a demo plugin. Done.
2. **Widgets from a real port**: `table`, `checkbox`, `modal`, `log`, `tree`, mouse,
   view instances, per-venv hosts, tables sorted by column. Done.
3. **Workbench services**: problem markers, terminal views, regions, the selection
   service with a Properties view, perspectives, preferences, consoles, the Neovim and
   Vim links both ways (editor levels 1–3), Search, Tasks and Outline built in, and the
   GTK front-end. Done.
4. **A framework**: libmyrcp with a public API, the launcher and product directories,
   the demo as an application on the library. Done. Next: `nvim --embed` (editor level
   2+), Nodes/explorer views (NetBeans), a stable protocol version for plugins written
   outside this repository.

## Ideas taken from Eclipse and NetBeans

- Declarative extensions with lazy activation (Eclipse `plugin.xml`, VS Code `contributes`).
- Commands separate from handlers and keybindings (Eclipse).
- Jobs with progress and cancel (Eclipse Jobs API).
- Selection service (Eclipse) and Lookup (NetBeans), as typed data items rather than
  adapters; the Properties view (Eclipse); problem markers (Eclipse).
- Perspectives (Eclipse), as data the hub saves rather than layouts per window; the
  preference store (Eclipse), as a TOML file the user may edit too.
- Planned: Nodes/explorer views (NetBeans).
- Products (Eclipse RCP): an application is a name and its plugins, on the same
  workbench.
- Left out: OSGi class loading. Process isolation and a versioned protocol cover the
  same need.
