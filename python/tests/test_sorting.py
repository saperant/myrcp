"""Sortable tables in the terminal: a click on a title, Alt+S and Alt+R; the cursor stays
on its row (the demo plugin's Folder table)."""

import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")


def order(lines):
    names = ("big.txt", "mid.txt", "small.txt", "sub")
    seen = [n for line in lines for n in names if line.lstrip("│ ").startswith(n)]
    return seen


def test_sort_by_clicking_a_title_and_by_keys(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "big.txt").write_text("x" * 5000)
    (work / "mid.txt").write_text("x" * 900)
    (work / "small.txt").write_text("x" * 10)
    (work / "sub").mkdir()
    opened = tmp_path / "opened"
    editor = tmp_path / "ed"
    editor.write_text(f'#!/bin/sh\nbasename "$1" >> {opened}\n')
    editor.chmod(0o755)
    t = Pty([str(BINARY)], work, env_for(tmp_path, VISUAL=str(editor)), cols=120, rows=36)
    s = Screen(t, 120, 36)
    try:
        s.wait(lambda ls: "Files" in ls[0])
        t.type("\x10")
        s.wait(lambda ls: any("Commands" in line for line in ls))
        t.type("open folder table")
        time.sleep(0.3)
        t.type("\r")
        lines = s.wait(lambda ls: any("Modified" in line for line in ls) and len(order(ls)) == 4)
        assert order(lines) == ["big.txt", "mid.txt", "small.txt", "sub"]  # the plugin's order
        t.type("\x1b[B")  # the cursor on mid.txt

        header = next(i for i, line in enumerate(lines) if "Name" in line and "Modified" in line)
        x = lines[header].index("Size") + 1
        t.type(f"\x1b[<0;{x};{header + 1}M\x1b[<0;{x};{header + 1}m")  # a click on Size
        lines = s.wait(lambda ls: order(ls) == ["small.txt", "mid.txt", "big.txt", "sub"])
        assert "▲" in lines[header]

        t.type("\x1br")  # Alt+R: the other way; folders (no size) stay last
        lines = s.wait(lambda ls: order(ls) == ["big.txt", "mid.txt", "small.txt", "sub"] and "▼" in ls[header])
        t.type("\r")  # the cursor followed mid.txt through both sorts
        deadline = time.monotonic() + 5
        while not opened.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        assert opened.read_text().split() == ["mid.txt"]

        t.type("\x1bs")  # Alt+S: the next column, Modified
        s.wait(lambda ls: "Sorted by Modified" in ls[-1] and "▲" in ls[header][ls[header].index("Modified") :])
        t.type("\x1bs")  # past the last: the plugin's order again
        s.wait(lambda ls: "In the plugin's order" in ls[-1] and "▲" not in ls[header] and "▼" not in ls[header])
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
