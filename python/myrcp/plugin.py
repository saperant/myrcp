"""The plugin API: Plugin (what a module registers), View, Context and Job."""

from __future__ import annotations

import itertools
import os
import threading
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, ClassVar, TypeVar

from .selection import Item, Selection

if TYPE_CHECKING:
    from .host import Host
    from .ui import Node

__all__ = ["Cancelled", "Context", "Item", "Job", "Marker", "Plugin", "Selection", "View"]

F = TypeVar("F", bound=Callable[..., Any])
V = TypeVar("V", bound="type[View]")


class Plugin:
    """What a plugin module registers. The module named in myrcp.toml defines ``plugin``.

    ``@plugin.command`` on a View method makes it a command of that view (it runs on the
    active instance); on a module-level function, a command taking the Context.
    """

    def __init__(self) -> None:
        self.views: dict[str, type[View]] = {}
        self.commands: dict[str, Callable[[Context], Any]] = {}

    def view(self, view_id: str) -> Callable[[V], V]:
        def deco(cls: V) -> V:
            if not (isinstance(cls, type) and issubclass(cls, View)):
                raise TypeError(f"@plugin.view({view_id!r}) needs a View subclass")
            methods: dict[str, str] = {}
            for name in dir(cls):
                cid = getattr(getattr(cls, name, None), "_myrcp_command", None)
                if cid:
                    methods[cid] = name
                    self.commands.pop(cid, None)  # it is the view's, not module-level
            cls._myrcp_commands = methods
            self.views[view_id] = cls
            return cls

        return deco

    def command(self, command_id: str) -> Callable[[F], F]:
        def deco(fn: F) -> F:
            fn._myrcp_command = command_id  # type: ignore[attr-defined]
            self.commands[command_id] = fn
            return fn

        return deco


class View:
    """One open view. ``render()`` is re-run after every event and ``invalidate()``.

    ``arg`` is the instance argument (``rcp.open_view("tool.job", "photos")``) or None.
    Before a command or button handler runs, ``focus_widget``, ``focus_key`` and
    ``focus_index`` say which widget and row the cursor is on, and ``form`` holds the
    text of every field in the view (button presses only).
    """

    _myrcp_commands: ClassVar[dict[str, str]] = {}

    def __init__(self, rcp: Context, view_id: str, arg: str | None = None) -> None:
        self.rcp = rcp
        self.view_id = view_id
        self.arg = arg
        self.focus_widget: str | None = None
        self.focus_key: str | None = None
        self.focus_index: int | None = None
        self.form: dict[str, str] = {}

    def setup(self) -> None:
        """Called once after construction."""

    def title(self) -> str | None:
        """The tab title; None keeps the manifest's."""
        return None

    def render(self) -> Node:
        raise NotImplementedError

    def dispose(self) -> None:
        """Called when the view closes."""

    def on_preferences(self, changed: frozenset[str]) -> None:
        """These preferences (ids, e.g. ``"demo.hidden"``) changed value: read them
        with ``self.rcp.pref(id)``. Every open view is rendered again afterwards."""

    def on_selection(self, selection: Selection) -> None:
        """The workbench selection changed (made in any view, this one included: see
        ``selection.view``). The view is rendered again afterwards."""

    def invalidate(self) -> None:
        """Schedule a re-render. Safe from any thread."""
        self.rcp._host.invalidate(self.view_id)

    def close(self) -> None:
        """Close this view's tab."""
        self.rcp._host.close_view(self.view_id)


@dataclass(frozen=True)
class Marker:
    """A problem to show in the Problems view (and as a diagnostic in an editor).

    ``path`` and ``line`` (1-based) place it in a file; ``view`` names a view that shows
    it, e.g. ``"tool.job#photos"``. Either or both."""

    message: str
    path: str | Path | None = None
    line: int = 0
    col: int = 0
    severity: str = "error"  # "error", "warning" or "info"
    source: str = ""
    view: str | None = None

    def to_wire(self) -> dict[str, Any]:
        return {
            "message": self.message,
            "path": str(Path(self.path).resolve()) if self.path else "",
            "line": int(self.line),
            "col": int(self.col),
            "severity": self.severity,
            "source": self.source,
            "view": self.view or "",
        }


class Cancelled(Exception):
    """Raised by Job.check() when the job was cancelled."""


class Job:
    """A background job. Its function runs in a thread; report with progress()."""

    _ids = itertools.count(1)

    def __init__(self, host: Host, title: str) -> None:
        self.id = f"{os.getpid()}.{next(self._ids)}"  # unique across plugin hosts
        self.title = title
        self.value = 0.0
        self.label = ""
        self.ok: bool | None = None  # None while running
        self.result: Any = None
        self.error: BaseException | None = None
        self._cancel = threading.Event()
        self._host = host
        self._last_sent = 0.0

    @property
    def cancelled(self) -> bool:
        return self._cancel.is_set()

    @property
    def cancel_event(self) -> threading.Event:
        """Set when the job is cancelled; hand it to code that polls an Event."""
        return self._cancel

    def cancel(self) -> None:
        self._cancel.set()

    def check(self) -> None:
        if self._cancel.is_set():
            raise Cancelled

    def progress(self, value: float, label: str | None = None) -> None:
        self.value = max(0.0, min(1.0, float(value)))
        if label is not None:
            self.label = label
        now = time.monotonic()
        if now - self._last_sent >= 0.05 or self.value >= 1.0:  # at most 20 updates/s
            self._last_sent = now
            self._host.notify("job.progress", {"job": self.id, "value": self.value, "label": self.label})


class Context:
    """The workbench as plugins see it (``self.rcp`` in a View)."""

    def __init__(self, host: Host) -> None:
        self._host = host
        self._selection = Selection()
        self._prefs: dict[str, Any] = {}

    @property
    def selection(self) -> Selection:
        """The workbench selection as last heard: what the focused view's cursor is on."""
        return self._selection

    def pref(self, pref_id: str, default: Any = None) -> Any:
        """A preference's value (``"<plugin>.<id>"`` as the manifest declares it, or the
        workbench's, e.g. ``"workbench.theme"``): its environment variable's while that
        is set, else the preferences file's, else the default. Known before any view
        opens; ``View.on_preferences`` hears changes."""
        return self._prefs.get(pref_id, default)

    def status(self, text: str, style: str = "normal") -> None:
        """Show a message in the status bar (style: normal, ok, error)."""
        self._host.notify("status", {"text": text, "style": style})

    def open_in_editor(
        self, path: str | Path, line: int | None = None, *, on_close: Callable[[bool], Any] | None = None
    ) -> None:
        """Open a file in the user's editor ($VISUAL/$EDITOR; see MYRCP_EDITOR_MODE).

        on_close(waited) runs when the workbench is back: waited is True when the editor
        ran in the foreground and has exited, False when it opened in a tmux window and
        may still be running."""
        params = {"path": str(Path(path).resolve()), "line": int(line or 0)}
        if on_close is None:
            self._host.notify("editor.open", params)
        else:
            self._host.request("editor.open", params, lambda result: on_close(bool(result.get("waited"))))

    def compare(
        self,
        left: str | Path,
        right: str | Path,
        *,
        title: str | None = None,
        on_close: Callable[[bool], Any] | None = None,
    ) -> None:
        """Compare two files side by side: in the user's linked Vim or Neovim (a diff tab
        of its own), else with the workbench.diff program (vimdiff by default), run
        where the editor runs. on_close(waited) as for open_in_editor."""
        params = {"left": str(Path(left).resolve()), "right": str(Path(right).resolve()), "title": title or ""}
        if on_close is None:
            self._host.notify("compare.open", params)
        else:
            self._host.request("compare.open", params, lambda result: on_close(bool(result.get("waited"))))

    def set_markers(self, name: str, markers: list[Marker]) -> None:
        """Replace this plugin's problem markers named ``name`` (an empty list clears them)."""
        self._host.notify("markers.set", {"set": name, "markers": [m.to_wire() for m in markers]})

    def open_terminal(self, argv: list[str], *, cwd: str | Path | None = None, title: str | None = None) -> None:
        """Run a program in a terminal view (a tab of its own in the terminal workbench).
        The tab closes when the program exits."""
        params: dict[str, Any] = {"argv": [str(a) for a in argv], "cwd": str(cwd or ""), "title": title or ""}
        self._host.notify("terminal.open", params)

    def run_in_console(
        self,
        argv: list[str],
        *,
        cwd: str | Path | None = None,
        title: str | None = None,
        name: str | None = None,
    ) -> None:
        """Run a program for its output, in a Console view of its own (Eclipse's
        Console, NetBeans' Output): the output stays after it exits, a line naming
        path:line opens there, Ctrl+C stops it and r runs it again. Runs with the same
        ``name`` reuse that console."""
        params = {"argv": [str(a) for a in argv], "cwd": str(cwd or ""), "title": title or "", "name": name or ""}
        self._host.notify("console.run", params)

    def open_view(self, view_id: str, arg: str | None = None) -> None:
        """Open (or switch to) a view; with arg, the instance of the view for that arg."""
        self._host.notify("view.show", {"view": view_id if arg is None else f"{view_id}#{arg}"})

    def run_job(
        self,
        title: str,
        fn: Callable[[Job], Any],
        *,
        on_done: Callable[[Job], Any] | None = None,
    ) -> Job:
        """Run fn(job) in a thread. on_done(job) runs on the main thread afterwards,
        with job.ok, job.result and job.error set."""
        return self._host.start_job(title, fn, on_done)

    def call_soon(self, fn: Callable[[], Any]) -> None:
        """Run fn on the main thread (safe from any thread)."""
        self._host.call_soon(fn)
