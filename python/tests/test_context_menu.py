"""The context menu in the terminal: a right click on a row (the cursor goes there, its
commands act on it), on a tab, and Shift+F10."""

import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")


def right_click(t, x, y):
    t.type(f"\x1b[<2;{x + 1};{y + 1}M\x1b[<2;{x + 1};{y + 1}m")


def left_click(t, x, y):
    t.type(f"\x1b[<0;{x + 1};{y + 1}M\x1b[<0;{x + 1};{y + 1}m")


def at(lines, text):
    y = next(i for i, line in enumerate(lines) if text in line)
    return lines[y].index(text), y


def test_right_click_menus(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "a.txt").write_text("a\n")
    (work / "b.txt").write_text("b\n")
    opened = tmp_path / "opened"
    editor = tmp_path / "ed"
    editor.write_text(f'#!/bin/sh\nbasename "$1" >> {opened}\n')
    editor.chmod(0o755)
    t = Pty([str(BINARY)], work, env_for(tmp_path, VISUAL=str(editor)), cols=160, rows=44)
    s = Screen(t, 160, 44)  # room for the side
    try:
        lines = s.wait(lambda ls: "b.txt" in "\n".join(ls) and "Jobs" in "\n".join(ls))
        right_click(t, *at(lines, "b.txt"))  # the cursor was on ..: it moves to b.txt
        lines = s.wait(lambda ls: any("Open in editor" in line for line in ls))
        menu = "\n".join(lines)
        assert "Find TODOs" in menu and "Properties: Show the selection" in menu and "View: Close" in menu
        left_click(t, *at(lines, "Open in editor"))
        deadline = time.monotonic() + 5
        while not opened.exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        assert opened.read_text().split() == ["b.txt"]

        lines = s.wait(lambda ls: "Open in editor" not in "\n".join(ls))
        x, y = at(lines, "Jobs")  # the bottom region's tab
        right_click(t, x + 1, y)
        lines = s.wait(lambda ls: any("View: Move to the side" in line for line in ls))
        assert "View: Close" in "\n".join(lines)
        left_click(t, *at(lines, "View: Move to the side"))
        s.wait(lambda ls: "│" in ls[0] and "Jobs" in ls[0][ls[0].index("│") :])  # its tab is at the side now

        t.type("\x1b1\x1b[21;2~")  # Files, Shift+F10: the menu at the cursor row
        s.wait(lambda ls: any("Parent directory" in line for line in ls))
        t.type("\x1b")
        s.wait(lambda ls: not any("Parent directory" in line for line in ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
