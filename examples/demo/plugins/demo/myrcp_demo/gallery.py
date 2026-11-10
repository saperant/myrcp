"""The demo's gallery: every widget of the UI IR in one view, a view with instances, and
the workbench services a plugin can call (consoles, comparing, terminal views,
markers on a view). A reference for writing plugins as much as a test bed for myrcp."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path

from myrcp import Context, Item, Marker, View, ui

from .plugin import plugin

STYLES = [
    "normal",
    "dim",
    "bold",
    "accent",
    "ok",
    "info",
    "warning",
    "error",
    "special",
]
COLOURS = ["red", "orange", "yellow", "green", "blue", "violet"]


@plugin.view("demo.widgets")
class Widgets(View):
    """Text and styles, boxes and panels, form widgets, a list, a tree with check
    boxes, a sortable table, a log and a modal dialog."""

    def setup(self) -> None:
        self.name = ""
        self.loud = False
        self.level = 0.4
        self.picked = "blue"
        self.checked = {"red", "green"}
        self.open = {"warm"}
        self.lines = ["The log keeps its end in view; buttons below add to it."]
        self.dialog = False

    def title(self) -> str:
        return "Widgets"

    def render(self) -> ui.Node:
        form = ui.panel(
            "Form",
            ui.vbox(
                ui.hbox(
                    ui.text("Name", width=6),
                    ui.field(
                        "name",
                        self.name,
                        placeholder="type, then Enter",
                        on_submit=self.greet,
                        grow=1,
                    ),
                    grow=0,
                ),
                ui.checkbox("loud", "Shout it", self.loud, on_toggle=self.set_loud),
                ui.hbox(
                    ui.text("Level", width=6),
                    ui.button("less", "-", on_press=lambda: self.step(-0.1)),
                    ui.progress(self.level, width=24),
                    ui.button("more", "+", on_press=lambda: self.step(0.1)),
                ),
                ui.hbox(
                    ui.button("ask", "A dialog…", on_press=self.ask),
                    ui.button("note", "A note view", on_press=self.note),
                ),
            ),
            grow=1,
        )
        styles = ui.panel(
            "Text",
            ui.vbox(
                *(ui.text(s, s) for s in STYLES),
                ui.text(
                    [
                        "Spans: ",
                        ("bold", "bold"),
                        " and ",
                        ("accent", "accent"),
                        " in a line",
                    ]
                ),
                ui.text(
                    "A long line wraps when it is asked to, at the width of the box it is in.",
                    wrap=True,
                ),
            ),
            width=28,
        )
        colours = ui.panel(
            "List",
            ui.listbox(
                "colours",
                [
                    ui.item(
                        c,
                        c,
                        "picked" if c == self.picked else None,
                        selection=self._item(c),
                    )
                    for c in COLOURS
                ],
                on_activate=self.pick,
                selected=self.picked,
            ),
            width=20,
        )
        tree = ui.panel(
            "Tree",
            ui.tree(
                "groups",
                self._tree_rows(),
                on_expand=self.expand,
                on_collapse=self.collapse,
                on_toggle=self.toggle,
            ),
            grow=1,
        )
        table = ui.panel(
            "Table (Alt+S sorts)",
            ui.table(
                "sizes",
                [
                    ui.column("Colour", grow=1),
                    ui.column("Hue", width=6, align="right"),
                    ui.column("Warm", width=6),
                ],
                [
                    ui.row(
                        c, [c, str(h), "yes" if h < 70 else ""], selection=self._item(c)
                    )
                    for c, h in zip(COLOURS, (0, 30, 60, 120, 240, 280))
                ],
                sortable=True,
                on_activate=self.pick,
            ),
            grow=1,
        )
        base = ui.vbox(
            ui.hbox(form, styles, colours, grow=0),
            ui.hbox(tree, table, grow=1),
            ui.panel("Log", ui.log("log", self.lines[-50:], height=5)),
        )
        if not self.dialog:
            return base
        return ui.modal(
            "dialog",
            base,
            ui.vbox(
                ui.text(
                    "A modal dialog: only it takes the focus; Escape dismisses it.",
                    wrap=True,
                ),
                ui.hbox(
                    ui.button("yes", "Yes", on_press=lambda: self.answer("yes")),
                    ui.button("no", "No", on_press=lambda: self.answer("no")),
                ),
            ),
            title="A question",
            on_dismiss=lambda: self.answer("dismissed"),
            width=44,
        )

    # -- handlers -------------------------------------------------------------------------
    def _say(self, line: str) -> None:
        self.lines.append(line)

    def greet(self, name: str) -> None:
        self.name = name
        hello = f"Hello, {name or 'nobody'}"
        self._say(hello.upper() + "!" if self.loud else hello + ".")

    def set_loud(self, on: bool) -> None:
        self.loud = on

    def step(self, by: float) -> None:
        self.level = min(1.0, max(0.0, round(self.level + by, 2)))

    def ask(self) -> None:
        self.dialog = True

    def answer(self, what: str) -> None:
        self.dialog = False
        self._say(f"The dialog: {what}")

    def note(self) -> None:
        self.rcp.open_view("demo.note", self.picked)

    def pick(self, key: str) -> None:
        self.picked = key.removeprefix("c:")
        self._say(f"Picked {self.picked}")

    @staticmethod
    def _item(colour: str) -> Item:
        return Item(
            "demo.colour",
            colour,
            props={"Colour": colour, "Warm": colour in COLOURS[:3]},
        )

    def _tree_rows(self) -> list[ui.Node]:
        rows = []
        for group, members in (("warm", COLOURS[:3]), ("cool", COLOURS[3:])):
            n = sum(m in self.checked for m in members)
            check = "all" if n == len(members) else "some" if n else "none"
            rows.append(
                ui.tree_row(
                    group,
                    group,
                    expandable=True,
                    expanded=group in self.open,
                    check=check,
                    detail=f"{n}/{len(members)}",
                )
            )
            if group in self.open:
                rows += [
                    ui.tree_row(
                        f"c:{m}",
                        m,
                        depth=1,
                        check="all" if m in self.checked else "none",
                        selection=self._item(m),
                    )
                    for m in members
                ]
        return rows

    def expand(self, key: str) -> None:
        self.open.add(key)

    def collapse(self, key: str) -> None:
        self.open.discard(key)

    def toggle(self, key: str) -> None:
        members = (
            COLOURS[:3]
            if key == "warm"
            else COLOURS[3:]
            if key == "cool"
            else [key.removeprefix("c:")]
        )
        if all(m in self.checked for m in members):
            self.checked -= set(members)
        else:
            self.checked |= set(members)


@plugin.view("demo.note")
class Note(View):
    """A view with instances: demo.note#<colour>, one tab each."""

    def title(self) -> str:
        return f"Note: {self.arg}"

    def render(self) -> ui.Node:
        return ui.vbox(
            ui.text(
                f"The note of {self.arg}: a view opened with an argument, rcp.open_view('demo.note', '{self.arg}').",
                wrap=True,
            ),
            ui.text(
                "Each argument has a tab of its own; Ctrl+W closes this one.", "dim"
            ),
        )


# ---- services ---------------------------------------------------------------------------


def _scratch() -> Path:
    d = Path(tempfile.gettempdir()) / "myrcp-demo"
    d.mkdir(exist_ok=True)
    return d


@plugin.command("demo.console")
def console(rcp: Context) -> None:
    """A program's output in a Console: its path:line lines open in the editor."""
    src = _scratch() / "broken.py"
    src.write_text("def f():\n    return undefined_name\n\nf()\n")
    rcp.run_in_console(
        [sys.executable, str(src)],
        cwd=_scratch(),
        title="python broken.py",
        name="demo",
    )


@plugin.command("demo.compare")
def compare(rcp: Context) -> None:
    """Two files side by side: in a linked Vim/Neovim, else with workbench.diff."""
    left, right = _scratch() / "left.txt", _scratch() / "right.txt"
    left.write_text("one\ntwo\nthree\n")
    right.write_text("one\n2\nthree\nfour\n")
    rcp.compare(left, right, title="left ↔ right")


@plugin.command("demo.terminal")
def terminal(rcp: Context) -> None:
    """A program in a terminal view: top, else a shell."""
    import shutil

    rcp.open_terminal(["top"] if shutil.which("top") else ["sh"], title="top")


@plugin.command("demo.widgets.mark")
def mark(rcp: Context) -> None:
    """Problems that name a view instead of a file: Enter in Problems opens the view."""
    rcp.set_markers(
        "gallery",
        [
            Marker(
                "A problem shown by a view",
                severity="info",
                view="demo.widgets",
                source="demo",
            ),
            Marker(
                "And one by a view instance",
                severity="warning",
                view="demo.note#red",
                source="demo",
            ),
        ],
    )
    rcp.status("Two markers in Problems (F8)", "ok")
