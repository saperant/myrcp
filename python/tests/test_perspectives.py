"""Perspectives: named arrangements of views, remembered as left and across sessions,
with the regions' sizes (keys and mouse drags resize them)."""

import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen, bar_with, command

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")

PREFIX, SHIFT_LEFT = "\x1c", "\x1b[1;2D"

REVIEW = """
[[perspectives]]
id = "review"
title = "Review"
main = ["demo.files"]
side = ["properties", "demo.jobs"]
bottom = ["problems"]
side_size = 50
"""


def separator(lines):
    """The side's separator column, from the top bar; None when there is no side."""
    return lines[0].index("│") if "│" in lines[0] else None


def start(tmp_path, work):
    config = tmp_path / "config" / "myrcp"
    config.mkdir(parents=True, exist_ok=True)
    (config / "perspectives.toml").write_text(REVIEW)
    env = env_for(tmp_path, XDG_CONFIG_HOME=str(tmp_path / "config"), SHELL="/bin/sh")
    t = Pty([str(BINARY)], work, env, cols=160, rows=44)
    return t, Screen(t, 160, 44)


def quit_(t):
    t.type("\x11")
    assert t.proc.wait(timeout=10) == 0


def test_perspectives_switch_remember_and_resize(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "notes.txt").write_text("hello\n")

    # Default: the startup views. Jobs moved to the side is remembered across sessions.
    t, s = start(tmp_path, work)
    try:
        s.wait(lambda ls: bar_with(ls, "Files") == 0 and (bar_with(ls, "Jobs") or 0) > 20)
        t.type("\x1b[17~")  # F6: the bottom's Jobs
        time.sleep(0.3)
        command(t, s, "move to the side")
        s.wait(lambda ls: bar_with(ls, "Jobs") == 0 and ls[0].index("Jobs") > (separator(ls) or 999))
        quit_(t)
    finally:
        if t.proc.poll() is None:
            t.proc.kill()

    t, s = start(tmp_path, work)
    try:
        lines = s.wait(lambda ls: separator(ls) and bar_with(ls, "Jobs") == 0 and "notes.txt" in "\n".join(ls))
        assert ls_index(lines, "Jobs") > separator(lines)

        # Review: Files; Properties and Jobs at the side (half the width), Problems below;
        # the other startup views closed.
        command(t, s, "perspective review")
        lines = s.wait(lambda ls: "Review" in ls[0] and bar_with(ls, "Properties") == 0)
        assert (bar_with(lines, "Problems") or 0) > 20
        assert separator(lines) == 80
        # Ctrl+\ Shift+← widens the side.
        t.type(PREFIX + SHIFT_LEFT)
        lines = s.wait(lambda ls: (separator(ls) or 999) < 80)
        widened = separator(lines)
        # Dragging the bottom's tab bar up raises it.
        y = bar_with(lines, "Problems")
        t.type(f"\x1b[<0;60;{y + 1}M\x1b[<32;60;{y - 4}M\x1b[<0;60;{y - 4}m")
        lines = s.wait(lambda ls: (bar_with(ls, "Problems") or 99) < y)
        raised = bar_with(lines, "Problems")

        # Back to Default: as it was left, Jobs at the side.
        command(t, s, "perspective default")
        lines = s.wait(lambda ls: "Review" not in ls[0] and bar_with(ls, "Jobs") == 0 and bar_with(ls, "Files") == 0)
        assert bar_with(lines, "Properties") is None and bar_with(lines, "Problems") is None
        quit_(t)
    finally:
        if t.proc.poll() is None:
            t.proc.kill()

    # Review remembers its sizes across sessions too.
    t, s = start(tmp_path, work)
    try:
        s.wait(lambda ls: bar_with(ls, "Jobs") == 0 and "notes.txt" in "\n".join(ls))
        command(t, s, "perspective review")
        lines = s.wait(lambda ls: "Review" in ls[0] and bar_with(ls, "Problems") is not None)
        assert separator(lines) == widened and bar_with(lines, "Problems") == raised
        # Reset: back to its definition's sizes.
        command(t, s, "perspective reset")
        s.wait(lambda ls: separator(ls) == 80)
        quit_(t)
    finally:
        if t.proc.poll() is None:
            t.proc.kill()

    # A session starts in the perspective the last one ended in, its tabs in its order
    # (the hub's own views open at once, the plugins' as their hosts come up).
    t, s = start(tmp_path, work)
    try:
        lines = s.wait(lambda ls: "Review" in ls[0] and bar_with(ls, "Jobs") == 0 and bar_with(ls, "Problems"))
        assert "1 Files" in lines[0] and "2 Properties" in lines[0] and "3 Jobs" in lines[0]
        assert "4 Problems" in lines[bar_with(lines, "Problems")]
        quit_(t)
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


def ls_index(lines, title):
    return lines[0].index(title)
