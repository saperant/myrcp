"""The workbench selection: what the cursor of the focused view is on.

A view says what its rows are: ``ui.row(key, cells, selection=Item(...))``. When the
cursor moves onto such a row in the focused view, the workbench makes its items the
selection, tells every plugin (``View.on_selection``, ``rcp.selection``) and shows them
in the Properties view, with the source around ``path:line``. Items are data, not
objects: any plugin can read another's, whatever host it runs in.
"""

from __future__ import annotations

from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

__all__ = ["Item", "Selection"]


@dataclass(frozen=True)
class Item:
    """One selected thing. ``type`` names its kind for consumers ("file", "problem",
    "demo.entry", ...); ``path``/``line`` (1-based) place it in a file; ``props`` are
    what the Properties view lists, in order."""

    type: str
    label: str
    path: str | Path | None = None
    line: int = 0
    props: Mapping[str, Any] | Iterable[tuple[str, Any]] = field(default_factory=dict)

    def prop(self, name: str, default: Any = None) -> Any:
        pairs = self.props.items() if isinstance(self.props, Mapping) else self.props
        for k, v in pairs:
            if k == name:
                return v
        return default

    def to_wire(self) -> dict[str, Any]:
        pairs = self.props.items() if isinstance(self.props, Mapping) else self.props
        return {
            "type": self.type,
            "label": self.label,
            "path": str(Path(self.path).resolve()) if self.path else "",
            "line": int(self.line),
            "props": [[str(k), "" if v is None else str(v)] for k, v in pairs],
        }

    @staticmethod
    def from_wire(d: Mapping[str, Any]) -> Item:
        return Item(
            str(d.get("type", "")),
            str(d.get("label", "")),
            str(d["path"]) if d.get("path") else None,
            int(d.get("line") or 0),
            tuple((str(k), str(v)) for k, v in d.get("props") or ()),
        )


@dataclass(frozen=True)
class Selection:
    """The workbench selection: the items, and the view (id and tab title) they are from."""

    view: str = ""
    title: str = ""
    items: tuple[Item, ...] = ()

    def of_type(self, type: str) -> list[Item]:
        """The items of this type, or of a type under it ("demo" matches "demo.entry")."""
        return [i for i in self.items if i.type == type or i.type.startswith(type + ".")]

    def __bool__(self) -> bool:
        return bool(self.items)


def wire_items(selection: Item | Iterable[Item] | None) -> list[dict[str, Any]] | None:
    if selection is None:
        return None
    if isinstance(selection, Item):
        return [selection.to_wire()]
    return [i.to_wire() for i in selection]
