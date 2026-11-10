"""The selection service: rows say what they are, the focused view's cursor row is the
workbench selection, and the Properties view shows it with the source around its line."""

import re
import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")

DOWN, ALT_ENTER, F6, F8 = "\x1b[B", "\x1b\r", "\x1b[17~", "\x1b[19~"


def side(lines):
    """The side region's columns (right of the separator), as text."""
    x = lines[0].index("│")
    return "\n".join(line[x + 1 :] for line in lines)


def test_properties_follow_the_focused_view(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    notes = work / "notes.txt"
    notes.write_text("line one\nline two\n  TODO: fix the third line\nline four\n")
    editor = tmp_path / "editor.sh"
    editor.write_text('#!/bin/sh\necho "OPENED $*"\nsleep 30\n')
    editor.chmod(0o755)
    env = env_for(tmp_path, MYRCP_EDITOR_MODE="tab", VISUAL=str(editor))
    t = Pty([str(BINARY)], work, env, cols=160, rows=44)
    s = Screen(t, 160, 44)
    try:
        s.wait(lambda ls: "notes.txt" in "\n".join(ls))
        # Alt+Enter: Properties at the side; the focus stays on Files.
        t.type(ALT_ENTER)
        s.wait(lambda ls: "│" in ls[0] and "Properties" in ls[0][ls[0].index("│") :])
        s.wait(lambda ls: "Nothing selected" in side(ls))  # the cursor is on ../
        t.type(DOWN)
        lines = s.wait(lambda ls: "Size" in side(ls) and "line two" in side(ls))
        text = side(lines)
        assert "From Files" in text and "notes.txt  file" in text and "Modified" in text
        assert re.search(r"1 line one", text)

        # The demo's TODOs in Problems, at the bottom: its row is a problem at line 3.
        t.type("t")
        t.type(F8)
        lines = s.wait(lambda ls: "Severity" in side(ls) and "warning" in side(ls))
        text = side(lines)
        assert "From Problems" in text and "notes.txt:3" in text
        assert "3   TODO: fix the third line" in text

        # Focusing Properties itself keeps what it shows (it offers no selection).
        t.type("\x1b[17;2~")  # Shift+F6: bottom → side (passing Files would select its row)
        time.sleep(0.3)
        assert "Severity" in side(s.lines())

        # Enter on a source line opens the editor there.
        t.type(DOWN + "\r")
        s.wait(lambda ls: any("OPENED +4" in line and "notes.txt" in line for line in ls))
        t.type("\x1c\x11")  # Ctrl+\ Ctrl+Q: the editor has the keys
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
