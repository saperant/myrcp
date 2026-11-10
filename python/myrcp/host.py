"""The plugin host: ``python -m myrcp.host``, started by the workbench.

Reads plugin manifests (no plugin code runs), reports their contributions, and imports
a plugin when one of its views opens or commands runs. Protocol: msgpack-RPC on the
original stdin/stdout (see ARCHITECTURE.md). Plugin output to stdout goes to stderr,
which the workbench writes to its log.
"""

from __future__ import annotations

import importlib
import os
import queue
import sys
import threading
import tomllib
import traceback
from collections.abc import Callable
from dataclasses import dataclass, field
from importlib import metadata, resources
from pathlib import Path
from typing import Any, BinaryIO, ClassVar

from . import ui
from ._msgpack import Unpacker, packb
from .plugin import Cancelled, Context, Job, Plugin, View
from .selection import Item, Selection

PROTOCOL = 1


def log(msg: str) -> None:
    print(f"myrcp.host: {msg}", file=sys.stderr, flush=True)


@dataclass
class Manifest:
    id: str
    name: str
    module: str
    root: str | None  # added to sys.path on activation
    source: str
    python: str | None = None  # the interpreter it needs, when not this one
    views: list[dict[str, Any]] = field(default_factory=list)
    commands: list[dict[str, Any]] = field(default_factory=list)
    perspectives: list[dict[str, Any]] = field(default_factory=list)
    preferences: list[dict[str, Any]] = field(default_factory=list)


class ManifestError(ValueError):
    pass


def parse_manifest(text: str, source: str, root: str | None) -> Manifest:
    try:
        data = tomllib.loads(text)
    except tomllib.TOMLDecodeError as e:
        raise ManifestError(f"{source}: {e}") from None
    p = data.get("plugin")
    if not isinstance(p, dict) or not p.get("id") or not p.get("module"):
        raise ManifestError(f"{source}: [plugin] needs id and module")
    m = Manifest(str(p["id"]), str(p.get("name", p["id"])), str(p["module"]), root, source)
    m.python = _find_python(p.get("python"), root)
    for v in data.get("views", []):
        if not v.get("id"):
            raise ManifestError(f"{source}: a view without id")
        region = str(v.get("region", "main"))
        if region not in ("main", "side", "bottom"):
            raise ManifestError(f"{source}: view {v['id']}: region is main, side or bottom, not {region!r}")
        m.views.append(
            {
                "id": str(v["id"]),
                "title": str(v.get("title", v["id"])),
                "startup": bool(v.get("startup")),
                "region": region,
            }
        )
    for c in data.get("commands", []):
        if not c.get("id"):
            raise ManifestError(f"{source}: a command without id")
        key = c.get("key", [])
        keys = [key] if isinstance(key, str) else [str(k) for k in key]
        m.commands.append(
            {"id": str(c["id"]), "title": str(c.get("title", c["id"])), "keys": keys, "view": str(c.get("view", ""))}
        )
    m.perspectives = parse_perspectives(data, source)
    m.preferences = parse_preferences(data, source, m.id)
    return m


PREF_TYPES = {"string": str, "bool": bool, "int": int, "choice": str, "paths": list}


def parse_preferences(data: dict[str, Any], source: str, plugin: str) -> list[dict[str, Any]]:
    """``[[preferences]]`` tables: a setting ``<plugin>.<id>`` of a ``type`` (string,
    bool, int, choice with ``choices``, paths) with a ``default``, a ``title``, a
    ``description`` and optionally an ``env`` variable that wins while it is set."""
    out = []
    for p in data.get("preferences", []):
        key = str(p.get("id", "")) if isinstance(p, dict) else ""
        if not key or not all(c.isalnum() or c in "_-" for c in key):
            raise ManifestError(f"{source}: a preference needs an id of letters, digits, _ and -")
        kind = str(p.get("type", "string"))
        if kind not in PREF_TYPES:
            raise ManifestError(f"{source}: preference {key}: type is one of {', '.join(PREF_TYPES)}, not {kind!r}")
        choices = [str(c) for c in p.get("choices", [])]
        if kind == "choice" and not choices:
            raise ManifestError(f"{source}: preference {key}: a choice needs choices")
        empty: Any = choices[0] if kind == "choice" else {"string": "", "bool": False, "int": 0, "paths": []}[kind]
        default = p.get("default", empty)
        ok = isinstance(default, PREF_TYPES[kind]) and not (kind == "int" and isinstance(default, bool))
        if kind == "paths":
            ok = ok and all(isinstance(x, str) for x in default)
        if kind == "choice":
            ok = ok and default in choices
        if not ok:
            raise ManifestError(f"{source}: preference {key}: the default {default!r} does not fit the type {kind}")
        out.append(
            {
                "id": f"{plugin}.{key}",
                "title": str(p.get("title", key)),
                "description": str(p.get("description", "")),
                "type": kind,
                "default": default,
                "choices": choices,
                "env": str(p.get("env", "")),
            }
        )
    return out


REGIONS = ("main", "side", "bottom")


def parse_perspectives(data: dict[str, Any], source: str) -> list[dict[str, Any]]:
    """``[[perspectives]]`` tables: id, title, the views of each region (``main``,
    ``side``, ``bottom``: view ids in tab order) and ``side_size``/``bottom_size`` in
    percent of the width and the height."""
    out = []
    for p in data.get("perspectives", []):
        if not isinstance(p, dict) or not p.get("id"):
            raise ManifestError(f"{source}: a perspective without id")
        views: list[list[str]] = []
        for region in REGIONS:
            ids = p.get(region, [])
            if isinstance(ids, str):
                ids = [ids]
            views += [[str(v), region] for v in ids]
        if not views:
            raise ManifestError(f"{source}: perspective {p['id']}: no views (main, side, bottom)")
        entry: dict[str, Any] = {"id": str(p["id"]), "title": str(p.get("title", p["id"])), "views": views}
        for key in ("side_size", "bottom_size"):
            if key in p:
                size = p[key]
                if not isinstance(size, int) or not 15 <= size <= 75:
                    raise ManifestError(f"{source}: perspective {p['id']}: {key} is a percentage, 15 to 75")
                entry[key] = size
        out.append(entry)
    return out


def user_perspectives() -> tuple[list[dict[str, Any]], list[str]]:
    """The user's own perspectives: $XDG_CONFIG_HOME/myrcp/perspectives.toml."""
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    path = Path(base) / "myrcp" / "perspectives.toml"
    if not path.is_file():
        return [], []
    try:
        return parse_perspectives(tomllib.loads(path.read_text(encoding="utf-8")), str(path)), []
    except (ManifestError, OSError, tomllib.TOMLDecodeError) as e:
        return [], [f"{path}: {e}" if isinstance(e, tomllib.TOMLDecodeError) else str(e)]


def _find_python(spec: Any, root: str | None) -> str | None:
    """The first existing interpreter of ``python = "..."`` or ``["$VAR", "../.venv/bin/python"]``.

    Paths are relative to the plugin directory; ~ and $VARS expand. None: none exists
    (the plugin then runs in whatever host finds it)."""
    candidates = [spec] if isinstance(spec, str) else list(spec or [])
    for c in candidates:
        path = os.path.expanduser(os.path.expandvars(str(c)))
        if "$" in path:  # an unset variable
            continue
        if root and not os.path.isabs(path):
            path = os.path.join(root, path)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return os.path.normpath(os.path.abspath(path))
    return None


def _same_python(path: str) -> bool:
    # Compare without resolving symlinks: a venv's python links to the base one.
    return os.path.normpath(os.path.abspath(path)) == os.path.normpath(os.path.abspath(sys.executable))


def discover(plugin_path: str) -> tuple[list[Manifest], list[str]]:
    """Manifests from MYRCP_PLUGIN_PATH directories and the myrcp.plugins entry points."""
    found: list[Manifest] = []
    errors: list[str] = []
    for d in filter(None, plugin_path.split(os.pathsep)):
        base = Path(d)
        if not base.is_dir():
            continue
        for child in sorted(base.iterdir()):
            toml = child / "myrcp.toml"
            if toml.is_file():
                try:
                    found.append(parse_manifest(toml.read_text(encoding="utf-8"), str(toml), str(child)))
                except (ManifestError, OSError) as e:
                    errors.append(str(e))
    for ep in metadata.entry_points(group="myrcp.plugins"):
        # The value names the package holding myrcp.toml; importing it should be cheap.
        try:
            text = (resources.files(ep.value) / "myrcp.toml").read_text(encoding="utf-8")
            found.append(parse_manifest(text, f"{ep.value} (entry point {ep.name})", None))
        except Exception as e:  # noqa: BLE001 - a broken plugin must not stop the others
            errors.append(f"entry point {ep.name}: {e}")
    unique: dict[str, Manifest] = {}
    for m in found:
        if m.id in unique:
            errors.append(f"plugin id {m.id!r} from {m.source} already provided by {unique[m.id].source}")
        else:
            unique[m.id] = m
    return list(unique.values()), errors


@dataclass
class OpenView:
    instance: View
    handlers: dict[tuple[str, str], Callable[..., Any]] = field(default_factory=dict)
    windows: dict[str, tuple[int, int]] = field(default_factory=dict)  # table id → rows sent
    sorts: dict[str, ui.SortState] = field(default_factory=dict)  # table id → its sort
    last: bytes = b""


class Host:
    def __init__(self, rfd: BinaryIO, wfd: BinaryIO, plugin_path: str) -> None:
        self._in = rfd
        self._out = wfd
        self._wlock = threading.Lock()
        self._queue: queue.Queue[tuple[str, Any] | None] = queue.Queue()
        self.ctx = Context(self)
        manifests, self.errors = discover(plugin_path)
        only = [p for p in os.environ.get("MYRCP_HOST_PLUGINS", "").split(",") if p]
        self.delegates: dict[str, list[str]] = {}  # python → plugin ids for other hosts
        self.user_perspectives: list[dict[str, Any]] = []
        if only:  # a host started for plugins that need this interpreter
            self.manifests = [m for m in manifests if m.id in only]
        else:
            self.user_perspectives, errors = user_perspectives()
            self.errors += errors
            self.manifests = []
            for m in manifests:
                if m.python and not _same_python(m.python):
                    self.delegates.setdefault(m.python, []).append(m.id)
                else:
                    self.manifests.append(m)
        self.view_owner = {v["id"]: m for m in self.manifests for v in m.views}
        self.command_owner = {c["id"]: m for m in self.manifests for c in m.commands}
        self.plugins: dict[str, Plugin] = {}
        self.views: dict[str, OpenView] = {}
        self.jobs: dict[str, Job] = {}
        self._dirty: set[str] = set()
        self._running = True
        self._next_request = 1
        self._pending: dict[int, Callable[[Any], Any]] = {}

    # ---- wire ----------------------------------------------------------------------

    def _send(self, msg: list[Any]) -> None:
        data = packb(msg)
        with self._wlock:
            view = memoryview(data)
            while view:
                n = self._out.write(view)
                view = view[n or 0 :]
            self._out.flush()

    def notify(self, method: str, params: dict[str, Any]) -> None:
        try:
            self._send([2, method, [params]])
        except (BrokenPipeError, OSError):
            self._running = False

    def request(self, method: str, params: dict[str, Any], on_result: Callable[[Any], Any]) -> None:
        """Ask the workbench; on_result(result) runs on the main thread when it answers."""
        msgid = self._next_request
        self._next_request += 1
        self._pending[msgid] = on_result
        try:
            self._send([0, msgid, method, [params]])
        except (BrokenPipeError, OSError):
            self._running = False

    def close_view(self, view_id: str) -> None:
        self.notify("view.close", {"view": view_id})
        self._view_close({"view": view_id})

    def invalidate(self, view_id: str) -> None:
        self._queue.put(("invalidate", view_id))

    def call_soon(self, fn: Callable[[], Any]) -> None:
        self._queue.put(("call", fn))

    def _reader(self) -> None:
        unpacker = Unpacker()
        fd = self._in.fileno()
        while True:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if not data:
                self._queue.put(None)
                return
            unpacker.feed(data)
            for msg in unpacker:
                self._queue.put(("rpc", msg))

    def serve(self) -> None:
        threading.Thread(target=self._reader, name="myrcp-reader", daemon=True).start()
        while self._running:
            item = self._queue.get()
            if item is None:
                break
            self._handle(item)
            # Drain what is queued before re-rendering, so bursts coalesce.
            while self._running:
                try:
                    item = self._queue.get_nowait()
                except queue.Empty:
                    break
                if item is None:
                    self._running = False
                    break
                self._handle(item)
            self._flush()
        self._shutdown()

    def _handle(self, item: tuple[str, Any]) -> None:
        kind, payload = item
        if kind == "rpc":
            self._dispatch(payload)
        elif kind == "invalidate":
            self._dirty.add(payload)
        elif kind == "call":
            self._guard("callback", payload)

    def _dispatch(self, msg: Any) -> None:
        if not isinstance(msg, list) or not msg:
            return
        if msg[0] == 0 and len(msg) == 4:
            _, msgid, method, params = msg
            handler = self._requests.get(method)
            try:
                if handler is None:
                    raise ValueError(f"unknown method {method}")
                result = handler(self, params[0] if params else {})
                self._send([1, msgid, None, result])
            except Exception as e:  # noqa: BLE001
                log(f"{method} failed:\n{traceback.format_exc()}")
                self._send([1, msgid, str(e) or type(e).__name__, None])
        elif msg[0] == 1 and len(msg) == 4:
            _, msgid, error, result = msg
            callback = self._pending.pop(msgid, None)
            if error is not None:
                log(f"request {msgid} failed: {error}")
            elif callback is not None:
                self._guard("callback", callback, result if isinstance(result, dict) else {})
                self._dirty.update(self.views)
        elif msg[0] == 2 and len(msg) == 3:
            _, method, params = msg
            handler = self._notifications.get(method)
            if handler is None:
                log(f"unknown notification {method}")
                return
            try:
                handler(self, params[0] if params else {})
            except Exception as e:  # noqa: BLE001
                log(f"{method} failed:\n{traceback.format_exc()}")
                self.ctx.status(f"{method}: {e}", "error")

    def _guard(self, what: str, fn: Callable[..., Any], *args: Any) -> Any:
        """Runs plugin code; an exception becomes a status message, not a dead host."""
        try:
            return fn(*args)
        except Exception as e:  # noqa: BLE001
            log(f"{what} failed:\n{traceback.format_exc()}")
            self.ctx.status(f"{what}: {type(e).__name__}: {e}", "error")
            return None

    # ---- plugins and views ---------------------------------------------------------

    def activate(self, m: Manifest) -> Plugin:
        if m.id in self.plugins:
            return self.plugins[m.id]
        if m.root and m.root not in sys.path:
            sys.path.insert(0, m.root)
        module = importlib.import_module(m.module)
        plugin = getattr(module, "plugin", None)
        if not isinstance(plugin, Plugin):
            raise TypeError(f"{m.module} does not define `plugin = myrcp.Plugin()`")
        declared = {v["id"] for v in m.views} | {c["id"] for c in m.commands}
        implemented = set(plugin.views) | set(plugin.commands)
        for cls in plugin.views.values():
            implemented |= set(cls._myrcp_commands)
        for missing in sorted(declared - implemented):
            log(f"plugin {m.id}: {missing} is declared in myrcp.toml but not implemented")
        for extra in sorted(implemented - declared):
            log(f"plugin {m.id}: {extra} is implemented but not declared in myrcp.toml")
        self.plugins[m.id] = plugin
        log(f"activated {m.id} ({m.module})")
        return plugin

    def _render(self, view_id: str) -> dict[str, Any]:
        ov = self.views[view_id]
        try:
            node = ov.instance.render()
        except Exception as e:  # noqa: BLE001
            log(f"render of {view_id} failed:\n{traceback.format_exc()}")
            node = ui.text(f"{view_id} failed to render: {type(e).__name__}: {e}", "error", wrap=True)
        tree, ov.handlers = ui.serialize(node, ov.windows, ov.sorts)
        return tree

    def _title(self, view_id: str) -> str | None:
        title = self._guard(f"{view_id}: title", self.views[view_id].instance.title)
        return title if isinstance(title, str) else None

    def _flush(self) -> None:
        dirty, self._dirty = self._dirty, set()
        for view_id in sorted(dirty):
            if view_id not in self.views:
                continue
            tree = self._render(view_id)
            packed = packb(tree)
            ov = self.views[view_id]
            title = self._title(view_id)
            if title:
                packed += title.encode()
            if packed != ov.last:  # unchanged trees are not resent
                ov.last = packed
                params: dict[str, Any] = {"view": view_id, "tree": tree}
                if title:
                    params["title"] = title
                self.notify("view.update", params)

    def start_job(self, title: str, fn: Callable[[Job], Any], on_done: Callable[[Job], Any] | None) -> Job:
        job = Job(self, title)
        self.jobs[job.id] = job
        self.notify("job.begin", {"job": job.id, "title": title})

        def run() -> None:
            text = ""
            try:
                job.result = fn(job)
                job.ok = True
            except Cancelled:
                job.ok, text = False, "cancelled"
            except Exception as e:  # noqa: BLE001
                log(f"job {title!r} failed:\n{traceback.format_exc()}")
                job.ok, job.error, text = False, e, f"{type(e).__name__}: {e}"
            self.notify("job.end", {"job": job.id, "ok": bool(job.ok), "text": text})

            def finish() -> None:
                self.jobs.pop(job.id, None)
                if on_done:
                    self._guard(f"{title} (on_done)", on_done, job)
                    self._dirty.update(self.views)

            self.call_soon(finish)

        threading.Thread(target=run, name=f"myrcp-{job.id}", daemon=True).start()
        return job

    # ---- requests ------------------------------------------------------------------

    def _initialize(self, params: dict[str, Any]) -> dict[str, Any]:
        for e in self.errors:
            log(e)
        return {
            "protocol": PROTOCOL,
            "plugins": [
                {
                    "id": m.id,
                    "name": m.name,
                    "views": m.views,
                    "commands": m.commands,
                    "perspectives": m.perspectives,
                    "preferences": m.preferences,
                }
                for m in self.manifests
            ],
            "perspectives": self.user_perspectives,
            "delegates": [{"python": py, "plugins": ids} for py, ids in self.delegates.items()],
            "errors": self.errors,
        }

    def _view_open(self, params: dict[str, Any]) -> dict[str, Any]:
        view_id = str(params["view"])
        if view_id not in self.views:
            base, _, arg = view_id.partition("#")  # "tool.job#photos": an instance
            m = self.view_owner.get(base)
            if m is None:
                raise KeyError(f"no plugin provides view {base}")
            cls = self.activate(m).views.get(base)
            if cls is None:
                raise KeyError(f"plugin {m.id} does not implement view {base}")
            instance = cls(self.ctx, view_id, arg or None)
            self.views[view_id] = OpenView(instance)
            try:
                instance.setup()
            except Exception:
                del self.views[view_id]
                raise
        ov = self.views[view_id]
        tree = self._render(view_id)
        title = self._title(view_id)
        ov.last = packb(tree) + (title.encode() if title else b"")
        return {"tree": tree, "title": title}

    # ---- notifications -------------------------------------------------------------

    def _view_event(self, params: dict[str, Any]) -> None:
        view_id = params.get("view")
        ov = self.views.get(view_id)
        if ov is None:
            return
        widget, kind, value = str(params.get("widget")), str(params.get("kind")), params.get("value")
        if kind == "window":  # the workbench scrolled a table past the rows it has
            first, count = value
            ov.windows[widget] = (int(first), int(count))
            self._dirty.add(view_id)
            return
        if kind == "sort":  # [column, descending, the key at the cursor]
            column, descending, key = (list(value or []) + [-1, False, None])[:3]
            old = ov.sorts.get(widget, ui.SortState())
            ov.sorts[widget] = ui.SortState(int(column), bool(descending), key, old.nonce + 1)
            ov.windows.pop(widget, None)  # from the top again; the front-end asks for more
            fn = ov.handlers.get((widget, "sort"))
            if fn is not None:
                self._guard(f"{view_id}: {widget}", fn, int(column), bool(descending))
            self._dirty.add(view_id)
            return
        fn = ov.handlers.get((widget, kind))
        if fn is not None:
            what = f"{view_id}: {widget}"
            if kind == "press":
                ov.instance.form = dict((value or {}).get("form") or {})
                ov.instance.focus_widget = widget
                self._guard(what, fn)
            elif kind == "dismiss":
                self._guard(what, fn)
            else:
                if kind == "submit":
                    ov.instance.form[widget] = value
                self._guard(what, fn, value)
        self._dirty.add(view_id)

    def _view_close(self, params: dict[str, Any]) -> None:
        ov = self.views.pop(params.get("view"), None)
        if ov is not None:
            self._guard("dispose", ov.instance.dispose)

    def _command_run(self, params: dict[str, Any]) -> None:
        cid = params.get("command")
        m = self.command_owner.get(cid)
        if m is None:
            raise KeyError(f"no plugin provides command {cid}")
        plugin = self.activate(m)
        view_id = params.get("view")
        ov = self.views.get(view_id) if view_id else None
        if ov is not None:
            inst = ov.instance
            inst.focus_widget = params.get("widget") or None
            inst.focus_key = params.get("key")
            inst.focus_index = params.get("index")
            method = type(inst)._myrcp_commands.get(cid)
            if method:
                self._guard(cid, getattr(ov.instance, method))
                self._dirty.add(view_id)
                return
        fn = plugin.commands.get(cid)
        if fn is None:
            self.ctx.status(f"{cid} is not available here", "error")
            return
        self._guard(cid, fn, self.ctx)
        self._dirty.update(self.views)

    def _selection_changed(self, params: dict[str, Any]) -> None:
        sel = Selection(
            str(params.get("view") or ""),
            str(params.get("title") or ""),
            tuple(Item.from_wire(d) for d in params.get("items") or () if isinstance(d, dict)),
        )
        self.ctx._selection = sel
        for view_id, ov in list(self.views.items()):
            if type(ov.instance).on_selection is not View.on_selection:
                self._guard(f"{view_id}: on_selection", ov.instance.on_selection, sel)
                self._dirty.add(view_id)

    def _preferences_changed(self, params: dict[str, Any]) -> None:
        values = params.get("values") or {}
        if not isinstance(values, dict) or not values:
            return
        self.ctx._prefs.update(values)
        changed = frozenset(values)
        for view_id, ov in list(self.views.items()):
            if type(ov.instance).on_preferences is not View.on_preferences:
                self._guard(f"{view_id}: on_preferences", ov.instance.on_preferences, changed)
            self._dirty.add(view_id)  # a view may read any of them while rendering

    def _job_cancel(self, params: dict[str, Any]) -> None:
        job = self.jobs.get(params.get("job"))
        if job:
            job.cancel()

    def _shutdown_request(self, params: dict[str, Any]) -> None:
        self._running = False

    def _shutdown(self) -> None:
        for job in self.jobs.values():
            job.cancel()
        for ov in self.views.values():
            self._guard("dispose", ov.instance.dispose)
        self.views.clear()

    _requests: ClassVar[dict[str, Callable[[Host, dict[str, Any]], Any]]] = {
        "initialize": _initialize,
        "view.open": _view_open,
    }
    _notifications: ClassVar[dict[str, Callable[[Host, dict[str, Any]], None]]] = {
        "view.event": _view_event,
        "view.close": _view_close,
        "command.run": _command_run,
        "job.cancel": _job_cancel,
        "selection.changed": _selection_changed,
        "preferences.changed": _preferences_changed,
        "shutdown": _shutdown_request,
    }


def main() -> int:
    # Keep the protocol on private fds: stray print()s go to stderr, and a plugin
    # reading stdin gets EOF instead of protocol bytes.
    proto_in = os.fdopen(os.dup(0), "rb", buffering=0)
    proto_out = os.fdopen(os.dup(1), "wb", buffering=0)
    null = os.open(os.devnull, os.O_RDONLY)
    os.dup2(null, 0)
    os.close(null)
    os.dup2(2, 1)
    host = Host(proto_in, proto_out, os.environ.get("MYRCP_PLUGIN_PATH", ""))
    log(f"started (python {sys.version.split()[0]}, {len(host.manifests)} plugins)")
    host.serve()
    return 0


if __name__ == "__main__":
    sys.exit(main())
