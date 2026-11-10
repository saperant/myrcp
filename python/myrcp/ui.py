"""Builders for the UI IR (see ARCHITECTURE.md).

Views return a tree of plain dicts. Event handlers passed here (``on_activate=...``)
stay in the plugin host; only the data goes to the workbench.

    ui.panel("Files", ui.listbox("entries", items, on_activate=self.open))
"""

from __future__ import annotations

import re
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from typing import Any

from .selection import Item, wire_items

Node = dict[str, Any]
Spans = Sequence[str | tuple[str, str]]

__all__ = [
    "CursorRequest",
    "LazyRows",
    "Node",
    "button",
    "checkbox",
    "column",
    "field",
    "hbox",
    "item",
    "lazy_rows",
    "listbox",
    "log",
    "modal",
    "panel",
    "progress",
    "row",
    "table",
    "text",
    "tree",
    "tree_row",
    "vbox",
]

STYLES = ("normal", "dim", "bold", "title", "accent", "error", "warning", "ok", "info", "special")
WINDOW = 200  # table rows sent at a time; the workbench asks for others as it scrolls


def _layout(node: Node, grow: int | None, width: int | None, height: int | None) -> Node:
    if grow is not None:
        node["grow"] = grow
    if width is not None:
        node["width"] = width
    if height is not None:
        node["height"] = height
    return node


def _handlers(node: Node, **handlers: Callable[..., Any] | None) -> Node:
    on = {kind: fn for kind, fn in handlers.items() if fn is not None}
    if on:
        node["_on"] = on
    return node


def _kids(children: Iterable[Node | None | bool]) -> list[Node]:
    # Falsy children are skipped, so `cond and ui.text(...)` works.
    return [c for c in children if c]


def text(
    content: str | Spans,
    style: str | None = None,
    *,
    wrap: bool = False,
    grow: int | None = None,
    width: int | None = None,
    height: int | None = None,
) -> Node:
    """Text: a string, or spans ``["plain", ("bold part", "bold"), ...]``."""
    node: Node = {"t": "text"}
    if isinstance(content, str):
        node["text"] = content
    else:
        node["text"] = [[s, style or "normal"] if isinstance(s, str) else [s[0], s[1]] for s in content]
    if style:
        node["style"] = style
    if wrap:
        node["wrap"] = True
    return _layout(node, grow, width, height)


def vbox(
    *children: Node | None | bool,
    gap: int = 0,
    grow: int | None = None,
    width: int | None = None,
    height: int | None = None,
) -> Node:
    return _layout({"t": "vbox", "children": _kids(children), "gap": gap}, grow, width, height)


def hbox(
    *children: Node | None | bool,
    gap: int = 1,
    grow: int | None = None,
    width: int | None = None,
    height: int | None = None,
) -> Node:
    return _layout({"t": "hbox", "children": _kids(children), "gap": gap}, grow, width, height)


def panel(
    title: str,
    child: Node,
    *,
    grow: int | None = None,
    width: int | None = None,
    height: int | None = None,
) -> Node:
    return _layout({"t": "panel", "title": title, "child": child}, grow, width, height)


def item(
    key: str | int,
    text: str,
    detail: str | None = None,
    style: str | None = None,
    *,
    selection: Item | Iterable[Item] | None = None,
) -> Node:
    """A list item. ``selection``: what it is, for the workbench selection (see Item)."""
    node: Node = {"key": key, "text": text}
    if detail:
        node["detail"] = detail
    if style:
        node["style"] = style
    if (sel := wire_items(selection)) is not None:
        node["sel"] = sel
    return node


def listbox(
    id: str,
    items: Sequence[Node | str],
    *,
    on_select: Callable[[str], Any] | None = None,
    on_activate: Callable[[str], Any] | None = None,
    selected: str | int | None = None,
    empty: str | None = None,
    grow: int | None = None,
    width: int | None = None,
    height: int | None = None,
) -> Node:
    """A list; the workbench keeps the cursor. ``selected`` moves it when it changes."""
    node: Node = {"t": "list", "id": id, "items": list(items)}
    if selected is not None:
        node["selected"] = selected
    if empty:
        node["empty"] = empty
    return _handlers(_layout(node, grow, width, height), select=on_select, activate=on_activate)


def button(id: str, label: str, *, on_press: Callable[[], Any] | None = None, width: int | None = None) -> Node:
    return _handlers(_layout({"t": "button", "id": id, "label": label}, None, width, None), press=on_press)


def field(
    id: str,
    value: str = "",
    *,
    placeholder: str | None = None,
    on_submit: Callable[[str], Any] | None = None,
    grow: int | None = None,
    width: int | None = None,
) -> Node:
    """A one-line text field. ``value`` replaces what was typed when it changes."""
    node: Node = {"t": "field", "id": id, "value": value}
    if placeholder:
        node["placeholder"] = placeholder
    return _handlers(_layout(node, grow, width, None), submit=on_submit)


def progress(value: float, label: str | None = None, *, width: int | None = None) -> Node:
    node: Node = {"t": "progress", "value": float(max(0.0, min(1.0, value)))}
    if label:
        node["label"] = label
    return _layout(node, None, width, None)


def checkbox(id: str, label: str, checked: bool, *, on_toggle: Callable[[bool], Any] | None = None) -> Node:
    """A checkbox; on_toggle gets the new value. The plugin owns ``checked``."""
    return _handlers({"t": "checkbox", "id": id, "label": label, "checked": bool(checked)}, toggle=on_toggle)


def column(
    title: str = "",
    *,
    width: int | None = None,
    grow: int | None = None,
    align: str | None = None,
    clickable: bool = False,
    sortable: bool = True,
) -> Node:
    """A table column: a fixed ``width``, or a share of the rest (``grow``, default 1).

    A click on a ``clickable`` column (or a Shift/Ctrl+click anywhere) is reported to
    the table's on_cell instead of selecting or opening the row. In a sortable table,
    ``sortable=False`` keeps a column out of it (a column of checkboxes, say)."""
    col: Node = {"title": title}
    if clickable:
        col["clickable"] = True
    if not sortable:
        col["sortable"] = False
    if width is not None:
        col["width"] = width
    if grow is not None:
        col["grow"] = grow
    if align:
        col["align"] = align
    return col


Cell = str | Spans | Node


def row(
    key: str | int,
    cells: Sequence[Cell],
    style: str | None = None,
    *,
    selection: Item | Iterable[Item] | None = None,
    depth: int | None = None,
    expandable: bool = False,
    expanded: bool = False,
) -> Node:
    """A table row. A cell is a string, spans ``[(text, style), ...]`` or ``{"text", "style"}``.
    ``selection``: what the row is, for the workbench selection (see Item).
    In a table with a ``tree_column``: ``depth`` indents that cell, ``expandable`` gives
    it an arrow, open (▼) when ``expanded``."""
    out: Node = {"key": key, "cells": [_cell(c) for c in cells]}
    if style:
        out["style"] = style
    if depth is not None:
        out["depth"] = depth
    if expandable:
        out["expandable"] = True
        out["expanded"] = bool(expanded)
    if (sel := wire_items(selection)) is not None:
        out["sel"] = sel
    return out


def _cell(c: Cell) -> Any:
    if isinstance(c, (str, dict)):
        return c
    return [[s, "normal"] if isinstance(s, str) else [s[0], s[1]] for s in c]


class LazyRows:
    """Rows built on demand: ``LazyRows(len(items), lambda i: ui.row(...))``.

    Only the rows the workbench shows are built and sent, so a table of 100 000
    items costs what a screenful does."""

    def __init__(self, count: int, make: Callable[[int], Node]) -> None:
        self.count = count
        self.make = make

    def __len__(self) -> int:
        return self.count

    def __getitem__(self, i: int) -> Node:
        return self.make(i)


def lazy_rows(count: int, make: Callable[[int], Node]) -> LazyRows:
    return LazyRows(count, make)


class CursorRequest:
    """Moves a table's cursor: keep one per table, call ``move(i)``, pass it as ``cursor=``.

    The workbench owns the cursor; a request applies once, when it changes."""

    def __init__(self) -> None:
        self.index = 0
        self.seq = 0

    def move(self, index: int) -> None:
        self.index = max(0, int(index))
        self.seq += 1


def table(
    id: str,
    columns: Sequence[Node],
    rows: Sequence[Node] | LazyRows,
    *,
    cursor: CursorRequest | None = None,
    header: bool = True,
    autofocus: bool = False,
    empty: str | None = None,
    on_select: Callable[[str], Any] | None = None,
    on_activate: Callable[[str], Any] | None = None,
    on_cell: Callable[[dict[str, Any]], Any] | None = None,
    sortable: bool = False,
    on_sort: Callable[[int, bool], Any] | None = None,
    tree_column: int | None = None,
    on_expand: Callable[[str], Any] | None = None,
    on_collapse: Callable[[str], Any] | None = None,
    grow: int | None = None,
    height: int | None = None,
) -> Node:
    """A table with a header row; the workbench keeps the cursor and scrolls.

    ``tree_column``: the rows are a tree the plugin flattened (it owns which nodes are
    open), drawn in that column with the rows' ``depth`` and arrows. → / a click on
    the arrow: on_expand; ←: on_collapse (go to the parent when the node is closed).
    A tree sorts its own way: give it on_sort, if anything.

    Events carry the row key; on_cell gets ``{"key", "column", "shift", "ctrl"}`` for
    clicks on clickable columns and modified clicks. Only a window of rows goes over
    the wire.

    ``sortable``: a click on a column's title (or Alt+S, Alt+R) sorts the rows by it:
    by the cells' text, numbers in it by value ("9 kB" before "10 kB"), stably, and
    the cursor stays on its row. That builds every row of LazyRows; a plugin with
    more than it wants to build sorts itself with ``on_sort(column, descending)``
    (column -1: its own order again), and the rows it gives are shown as given."""
    node: Node = {"t": "table", "id": id, "columns": list(columns), "_rows": rows}
    if sortable or on_sort is not None:
        node["sortable"] = True
    if cursor is not None and cursor.seq:
        node["cursor"] = [cursor.index, cursor.seq]
    if not header:
        node["header"] = False
    if tree_column is not None:
        node["tree"] = tree_column
    if autofocus:
        node["autofocus"] = True
    if empty:
        node["empty"] = empty
    return _handlers(
        _layout(node, grow, None, height),
        select=on_select,
        activate=on_activate,
        cell=on_cell,
        sort=on_sort,
        expand=on_expand,
        collapse=on_collapse,
    )


def tree_row(
    key: str,
    text: Cell,
    *,
    depth: int = 0,
    expandable: bool = False,
    expanded: bool = False,
    check: str | None = None,
    detail: Cell | None = None,
    style: str | None = None,
    selection: Item | Iterable[Item] | None = None,
) -> Node:
    """A row of a tree: ``check`` is "all", "some" or "none" for a tri-state box.
    ``selection``: what the row is, for the workbench selection (see Item)."""
    out: Node = {"key": key, "text": _cell(text), "depth": depth}
    if expandable:
        out["expandable"] = True
        out["expanded"] = bool(expanded)
    if check:
        out["check"] = check
    if detail is not None:
        out["detail"] = _cell(detail)
    if style:
        out["style"] = style
    if (sel := wire_items(selection)) is not None:
        out["sel"] = sel
    return out


def tree(
    id: str,
    rows: Sequence[Node] | LazyRows,
    *,
    cursor: CursorRequest | None = None,
    autofocus: bool = False,
    empty: str | None = None,
    on_select: Callable[[str], Any] | None = None,
    on_activate: Callable[[str], Any] | None = None,
    on_expand: Callable[[str], Any] | None = None,
    on_collapse: Callable[[str], Any] | None = None,
    on_toggle: Callable[[str], Any] | None = None,
    grow: int | None = None,
    height: int | None = None,
) -> Node:
    """A tree of the rows the plugin flattened: it owns which nodes are open.

    → / a click on the arrow: on_expand; ←: on_collapse (go to the parent when the
    node is closed); Space / a click on the box: on_toggle; Enter: on_activate.
    Windowed like a table."""
    node: Node = {"t": "tree", "id": id, "_rows": rows}
    if cursor is not None and cursor.seq:
        node["cursor"] = [cursor.index, cursor.seq]
    if autofocus:
        node["autofocus"] = True
    if empty:
        node["empty"] = empty
    return _handlers(
        _layout(node, grow, None, height),
        select=on_select,
        activate=on_activate,
        expand=on_expand,
        collapse=on_collapse,
        toggle=on_toggle,
    )


def log(id: str | None, lines: Sequence[Cell], *, height: int | None = None, grow: int | None = None) -> Node:
    """Lines that stick to the end; with an id the user can focus and scroll them."""
    node: Node = {"t": "log", "lines": [_cell(line) for line in lines]}
    if id:
        node["id"] = id
    return _layout(node, grow, None, height)


def modal(
    id: str,
    base: Node,
    dialog: Node,
    *,
    title: str = "",
    on_dismiss: Callable[[], Any] | None = None,
    width: int | None = None,
    height: int | None = None,
) -> Node:
    """``dialog`` over ``base`` (dimmed). Only the dialog takes focus; Escape calls
    on_dismiss. Return it as the root of render()."""
    node: Node = {"t": "modal", "id": id, "title": title, "child": base, "dialog": dialog}
    if width is not None:
        node["dialog_width"] = width
    if height is not None:
        node["dialog_height"] = height
    return _handlers(node, dismiss=on_dismiss)


@dataclass
class SortState:
    """How the workbench asked a table to be sorted (the host keeps one per table)."""

    column: int = -1  # -1: the plugin's order
    descending: bool = False
    key: Any = None  # the row the cursor was on, to follow
    nonce: int = 0  # bumped by each request: the front-end moves the cursor once


def _cell_text(cell: Any) -> str:
    if isinstance(cell, str):
        return cell
    if isinstance(cell, dict):
        return str(cell.get("text", ""))
    if isinstance(cell, list):
        return "".join(str(s[0]) for s in cell if isinstance(s, (list, tuple)) and s)
    return ""


_NUMBER = re.compile(r"(-?\d+(?:\.\d+)?)")


_SIZE = re.compile(r"^\s*(-?\d+(?:\.\d+)?)\s*([kKMGTP]?)(i?)B?\s*$")  # "12", "1.5K", "3 MB"
_SCALE = {"": 0, "k": 1, "K": 1, "M": 2, "G": 3, "T": 4, "P": 5}


def sort_key(text: str) -> tuple[Any, ...]:
    """Natural order: numbers in the text by value ("9 kB" before "10 kB" before
    "2 MB"), the rest without case; empty cells last."""
    if m := _SIZE.match(text):
        base = 1024 if m.group(3) else 1000
        return (False, (0, float(m.group(1)) * base ** _SCALE[m.group(2)], ""))
    parts = _NUMBER.split(text.strip().casefold())
    out: list[Any] = [not text.strip()]
    for i, p in enumerate(parts):
        out.append((0, float(p), "") if i % 2 else (1, 0.0, p))
    return tuple(out)


def sort_order(rows: Sequence[Node], column: int, descending: bool) -> list[int]:
    """The rows' indices in sorted order (stable; empty cells last either way)."""

    def text(i: int) -> str:
        cells = rows[i].get("cells") or []
        return _cell_text(cells[column]) if column < len(cells) else ""

    keys = [sort_key(text(i)) for i in range(len(rows))]
    order = sorted(range(len(rows)), key=lambda i: keys[i], reverse=descending)
    if descending:
        order.sort(key=lambda i: keys[i][0])
    return order


class _Reordered:
    def __init__(self, rows: Sequence[Node], order: list[int]) -> None:
        self.rows, self.order = rows, order

    def __len__(self) -> int:
        return len(self.order)

    def __getitem__(self, i: int) -> Node:
        return self.rows[self.order[i]]


def sorted_rows(rows: Sequence[Node], column: int, descending: bool) -> list[Node]:
    return [rows[i] for i in sort_order(rows, column, descending)]


def serialize(
    node: Any, windows: dict[str, tuple[int, int]] | None = None, sorts: dict[str, SortState] | None = None
) -> tuple[Node, dict[tuple[str, str], Callable[..., Any]]]:
    """Strips handlers from a tree, sorts sortable tables and cuts them to their window.

    Returns (wire tree, {(widget id, kind): handler}). ``windows`` maps a table id to
    the (first, count) rows to send, ``sorts`` to how it is sorted."""
    handlers: dict[tuple[str, str], Callable[..., Any]] = {}
    windows = windows or {}
    sorts = sorts or {}

    def walk(n: Any) -> Node:
        if not isinstance(n, dict) or "t" not in n:
            return {"t": "text", "text": f"<not a node: {type(n).__name__}>", "style": "error"}
        out = {k: v for k, v in n.items() if k not in ("_on", "_rows")}
        for kind, fn in n.get("_on", {}).items():
            handlers[(str(n.get("id", "")), kind)] = fn
        if "_rows" in n:
            rows = n["_rows"]
            st = sorts.get(str(n.get("id", ""))) if n.get("sortable") else None
            if st is not None and st.column >= 0:
                out["sorted"] = [st.column, st.descending]
                if "sort" not in n.get("_on", {}):  # the plugin sorts itself otherwise
                    built = [rows[i] for i in range(len(rows))]  # LazyRows: built once here
                    order = sort_order(built, st.column, st.descending)
                    rows = _Reordered(built, order)
                    if "cursor" in out:  # the plugin's cursor names a row in its own order
                        where = {old: new for new, old in enumerate(order)}
                        out["cursor"] = [where.get(int(out["cursor"][0]), 0), out["cursor"][1]]
            if st is not None and st.key is not None:
                at = next((i for i in range(len(rows)) if rows[i].get("key") == st.key), None)
                if at is not None:
                    out["follow"] = [at, st.nonce]
            total = len(rows)
            first, count = windows.get(str(n.get("id", "")), (0, WINDOW))
            first = max(0, min(first, max(0, total - 1)))
            end = min(total, first + max(count, 1))
            out["rows"] = [rows[i] for i in range(first, end)]
            out["offset"] = first
            out["total"] = total
        if "children" in out:
            out["children"] = [walk(c) for c in out["children"]]
        if "child" in out:
            out["child"] = walk(out["child"])
        if "dialog" in out:
            out["dialog"] = walk(out["dialog"])
        return out

    return walk(node), handlers
