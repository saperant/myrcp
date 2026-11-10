"""The workbench's regions: main, side and bottom, folded into one when the terminal is small."""

import fcntl
import os
import re
import signal
import struct
import termios
import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")

F6, PALETTE, ALT_Z, PREFIX, UP = "\x1b[17~", "\x10", "\x1bz", "\x1c", "\x1b[A"


class Screen:
    def __init__(self, pty, cols, rows):
        self.pty, self.cols, self.rows = pty, cols, rows

    def lines(self):
        screen = pyte.Screen(self.cols, self.rows)
        with self.pty.lock:
            pyte.ByteStream(screen).feed(bytes(self.pty.out))
        return screen.display

    def wait(self, pred, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            lines = self.lines()
            if pred(lines):
                return lines
            time.sleep(0.05)
        raise TimeoutError("not on screen:\n" + "\n".join(self.lines()))

    def resize(self, cols, rows):
        self.cols, self.rows = cols, rows
        fcntl.ioctl(self.pty.master, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        os.kill(self.pty.proc.pid, signal.SIGWINCH)  # no controlling terminal: tell it ourselves


def bar_with(lines, title):
    """The row whose tab bar has this tab (0: the top bar), else None."""
    for y, line in enumerate(lines):
        if re.search(rf"\d+ {re.escape(title)}\b", line):
            return y
    return None


def command(t, screen, text):
    t.type(PALETTE)
    screen.wait(lambda ls: any("Commands" in line for line in ls))
    t.type(text)
    time.sleep(0.3)
    t.type("\r")


def test_regions_split_move_focus_and_fold(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    t = Pty([str(BINARY)], work, env_for(tmp_path, SHELL="/bin/sh"), cols=160, rows=44)
    s = Screen(t, 160, 44)
    try:
        # Jobs' manifest puts it at the bottom: a bar of its own under the main area.
        s.wait(
            lambda ls: (
                bar_with(ls, "Files") == 0
                and (bar_with(ls, "Jobs") or 0) > 20
                and "No jobs running" in "\n".join(ls[bar_with(ls, "Jobs") :])
            )
        )
        lines = s.lines()
        assert "F6 region" in lines[-1]

        # F6 focuses the bottom: its keys reach Jobs, the Files list stays drawn.
        t.type(F6)
        time.sleep(0.3)
        t.type("s")  # Jobs: start a job
        s.wait(lambda ls: "No jobs running" not in "\n".join(ls) and "Filter" in "\n".join(ls))

        # Moved to the side, Jobs is drawn at the right of the main area, its tab in the
        # top bar beyond the separator.
        command(t, s, "move to the side")
        lines = s.wait(lambda ls: bar_with(ls, "Jobs") == 0 and ls[0].index("Jobs") > ls[0].index("│"))
        assert "│" in lines[5]

        # Maximized, all tabs are in one bar and one view fills the body; again, restored.
        t.type(ALT_Z)
        lines = s.wait(lambda ls: "│" not in ls[0])
        assert bar_with(lines, "Jobs") == 0 and bar_with(lines, "Files") == 0
        t.type(ALT_Z)
        s.wait(lambda ls: "│" in ls[0])

        # A shell opens at the bottom; Ctrl+\ ↑ takes the focus back to the main area,
        # where keys are the view's again (Ctrl+P: the palette, not the shell's).
        t.type("\x14")
        lines = s.wait(lambda ls: (bar_with(ls, "sh") or 0) > 20)
        t.type("echo hel''lo-below\r")
        s.wait(lambda ls: any("hello-below" in line for line in ls[bar_with(ls, "sh") :]))
        t.type(PREFIX + UP)
        time.sleep(0.3)
        t.type(PALETTE)
        s.wait(lambda ls: any("Commands" in line for line in ls))
        t.type("\x1b")
        time.sleep(0.2)
        # A click in the shell focuses the bottom again: keys are the shell's.
        y = bar_with(s.lines(), "sh") + 3  # 1-based, a row inside the region
        t.type(f"\x1b[<0;10;{y}M\x1b[<0;10;{y}m")
        time.sleep(0.3)
        t.type("echo cli''cked\r")
        s.wait(lambda ls: any("clicked" in line for line in ls[bar_with(ls, "sh") :]))

        # Too small for the side and the bottom: they fold into the main area's tabs.
        s.resize(100, 30)
        lines = s.wait(lambda ls: "│" not in ls[0] and bar_with(ls, "Jobs") == 0 and bar_with(ls, "sh") == 0)
        s.resize(160, 44)
        s.wait(lambda ls: "│" in ls[0] and (bar_with(ls, "sh") or 0) > 20)

        t.type(PREFIX + "\x11")  # the shell has the keys: Ctrl+\ Ctrl+Q quits
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
