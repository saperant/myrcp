"""Demo views: a file browser that opens files in your editor, and background jobs."""

from __future__ import annotations

import time
from pathlib import Path

from myrcp import Context, Item, Job, Marker, Plugin, View, ui

plugin = Plugin()


def human_size(n: int) -> str:
    size = float(n)
    for unit in ("B", "K", "M", "G"):
        if size < 1024 or unit == "G":
            return f"{size:.0f}{unit}" if unit == "B" else f"{size:.1f}{unit}"
        size /= 1024
    return f"{size:.1f}T"


@plugin.view("demo.files")
class Files(View):
    def setup(self) -> None:
        start = Path(self.rcp.pref("demo.start") or ".").expanduser()
        self.cwd = start.resolve() if start.is_dir() else Path.cwd()
        self.show_hidden = bool(self.rcp.pref("demo.hidden", False))
        self.filter = ""
        self.selected: str | None = None

    def entries(self) -> list[Path]:
        try:
            paths = list(self.cwd.iterdir())
        except OSError as e:
            self.rcp.status(f"{self.cwd}: {e.strerror}", "error")
            return []
        if not self.show_hidden:
            paths = [p for p in paths if not p.name.startswith(".")]
        if self.filter:
            paths = [p for p in paths if self.filter.lower() in p.name.lower()]
        return sorted(paths, key=lambda p: (not p.is_dir(), p.name.lower()))

    def render(self) -> ui.Node:
        items = (
            [] if self.cwd.parent == self.cwd else [ui.item("..", "../", style="dim")]
        )
        for p in self.entries():
            try:
                st = p.stat()
                modified = time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))
            except OSError:
                st, modified = None, "?"
            if p.is_dir():
                sel = Item("directory", p.name + "/", p, props={"Modified": modified})
                items.append(
                    ui.item(p.name, p.name + "/", style="accent", selection=sel)
                )
            else:
                size = human_size(st.st_size) if st else "?"
                sel = Item(
                    "file", p.name, p, props={"Size": size, "Modified": modified}
                )
                items.append(ui.item(p.name, p.name, size, selection=sel))
        hidden = "shown" if self.show_hidden else "hidden"
        return ui.vbox(
            ui.panel(
                str(self.cwd),
                ui.listbox(
                    "entries",
                    items,
                    selected=self.selected,
                    on_select=self.select,
                    on_activate=self.activate,
                    empty="(nothing here)",
                ),
            ),
            ui.hbox(
                ui.text("Filter", "dim"),
                ui.field(
                    "filter",
                    self.filter,
                    placeholder="type, then Enter",
                    on_submit=self.set_filter,
                ),
            ),
            ui.text(
                [
                    ("Enter", "accent"),
                    " open  ",
                    ("e", "accent"),
                    " edit  ",
                    ("Backspace", "accent"),
                    " parent  ",
                    (".", "accent"),
                    f" dotfiles ({hidden})",
                ],
                "dim",
            ),
        )

    def select(self, key: str) -> None:
        self.selected = key

    def activate(self, key: str) -> None:
        if key == "..":
            return self.up()
        path = self.cwd / key
        if path.is_dir():
            self.cwd = path.resolve()
            self.selected = ".."
        else:
            self.rcp.open_in_editor(path)

    def set_filter(self, text: str) -> None:
        self.filter = text.strip()

    @plugin.command("demo.files.up")
    def up(self) -> None:
        came_from = self.cwd.name
        self.cwd = self.cwd.parent
        self.selected = came_from

    def on_preferences(self, changed: frozenset[str]) -> None:
        if "demo.hidden" in changed:
            self.show_hidden = bool(self.rcp.pref("demo.hidden"))
        if "demo.start" in changed:
            start = Path(self.rcp.pref("demo.start") or ".").expanduser()
            if start.is_dir():
                self.cwd, self.selected = start.resolve(), None

    @plugin.command("demo.files.hidden")
    def toggle_hidden(self) -> None:
        self.show_hidden = not self.show_hidden

    @plugin.command("demo.files.todos")
    def todos(self) -> None:
        """TODO/FIXME lines of the files here, as problem markers."""
        markers = []
        for p in self.entries():
            if not p.is_file() or p.stat().st_size > 1 << 20:
                continue
            try:
                lines = p.read_text(encoding="utf-8").splitlines()
            except (OSError, UnicodeDecodeError):
                continue
            for n, line in enumerate(lines, 1):
                for word, severity in (("FIXME", "error"), ("TODO", "warning")):
                    if (col := line.find(word)) >= 0:
                        markers.append(
                            Marker(line.strip(), p, n, col, severity, "demo")
                        )
                        break
        self.rcp.set_markers("todos", markers)
        self.rcp.status(
            f"{len(markers)} TODO/FIXME line(s); see View: Open Problems", "ok"
        )

    @plugin.command("demo.files.edit")
    def edit(self) -> None:
        if self.selected and self.selected != "..":
            path = self.cwd / self.selected
            self.rcp.open_in_editor(path if path.is_file() else self.cwd)


@plugin.view("demo.table")
class Table(View):
    """The start folder as a sortable table: a click on a title (or Alt+S, Alt+R) sorts."""

    def setup(self) -> None:
        start = Path(self.rcp.pref("demo.start") or ".").expanduser()
        self.cwd = start.resolve() if start.is_dir() else Path.cwd()

    def title(self) -> str:
        return f"Table: {self.cwd.name or self.cwd}"

    def render(self) -> ui.Node:
        rows = []
        for p in sorted(self.cwd.iterdir(), key=lambda p: p.name.lower()):
            try:
                st = p.stat()
            except OSError:
                continue
            kind = "folder" if p.is_dir() else (p.suffix[1:] or "file")
            modified = time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime))
            size = "" if p.is_dir() else human_size(st.st_size)
            item = Item("directory" if p.is_dir() else "file", p.name, p)
            rows.append(ui.row(p.name, [p.name, kind, size, modified], selection=item))
        return ui.panel(
            str(self.cwd),
            ui.table(
                "entries",
                [
                    ui.column("Name", grow=2),
                    ui.column("Kind", width=8),
                    ui.column("Size", width=8, align="right"),
                    ui.column("Modified", width=16),
                ],
                rows,
                autofocus=True,
                sortable=True,
                empty="An empty folder.",
                on_activate=self.activate,
            ),
        )

    def activate(self, key: str) -> None:
        path = self.cwd / key
        if path.is_file():
            self.rcp.open_in_editor(path)


@plugin.view("demo.jobs")
class Jobs(View):
    def setup(self) -> None:
        self.running: dict[str, Job] = {}
        self.history: list[ui.Node] = []
        self.count = 0

    def render(self) -> ui.Node:
        return ui.vbox(
            ui.hbox(
                ui.button("start", "Start job", on_press=self.start),
                ui.button(
                    "fail",
                    "Start a failing job",
                    on_press=lambda: self.start(fail=True),
                ),
                ui.button("cancel", "Cancel all", on_press=self.cancel),
            ),
            ui.text(""),
            *[
                ui.progress(job.value, f"{job.title:<12}")
                for job in self.running.values()
            ],
            not self.running
            and ui.text("No jobs running. Press s or a button.", "dim"),
            ui.text(""),
            ui.panel(
                "History",
                ui.listbox("history", self.history, empty="(no finished jobs yet)"),
            ),
            gap=0,
        )

    @plugin.command("demo.jobs.start")
    def start(self, fail: bool = False) -> None:
        self.count += 1
        title = f"Job {self.count}"

        def work(job: Job) -> str:
            steps = 40
            for i in range(steps):
                job.check()
                time.sleep(0.05)
                job.progress((i + 1) / steps, f"step {i + 1}/{steps}")
                self.invalidate()
                if fail and i == steps // 2:
                    raise RuntimeError("simulated failure halfway")
            return f"{steps} steps"

        job = self.rcp.run_job(title, work, on_done=self.done)
        self.running[job.id] = job

    @plugin.command("demo.jobs.cancel")
    def cancel(self) -> None:
        for job in self.running.values():
            job.cancel()

    def done(self, job: Job) -> None:
        self.running.pop(job.id, None)
        if job.ok:
            entry = ui.item(job.id, job.title, f"ok, {job.result}", style="ok")
        elif job.error is None:
            entry = ui.item(job.id, job.title, "cancelled", style="warning")
        else:
            entry = ui.item(job.id, job.title, f"failed: {job.error}", style="error")
        self.history.insert(0, entry)


@plugin.command("demo.hello")
def hello(rcp: Context) -> None:
    rcp.status("Hello from the demo plugin", "ok")


# The gallery (every widget, a view with instances, the workbench's services) registers
# on the same plugin.
from . import gallery  # noqa: F401
