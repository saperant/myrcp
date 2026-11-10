"""Consoles: a program's output in a view of its own; its path:line lines open."""

import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for

pyte = pytest.importorskip("pyte")
from test_regions import Screen

pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")


def test_run_a_command_in_a_console(tmp_path):
    work = tmp_path / "work"
    (work / "src").mkdir(parents=True)
    (work / "src" / "m.py").write_text("x = 1\n")
    record = tmp_path / "editor-args"
    editor = tmp_path / "fake-editor"
    editor.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" >> "{record}"\n')
    editor.chmod(0o755)
    t = Pty([str(BINARY)], work, env_for(tmp_path, VISUAL=str(editor)), cols=120, rows=36)
    s = Screen(t, 120, 36)
    try:
        s.wait(lambda ls: "Files" in ls[0])
        t.type("\x10")
        s.wait(lambda ls: any("Commands" in line for line in ls))
        t.type("run a command")
        time.sleep(0.3)
        t.type("\r")
        s.wait(lambda ls: any("Type a command" in line for line in ls))
        t.type("echo start; echo 'src/m.py:1: E501 too long'; echo boom >&2; exit 4\r")
        lines = s.wait(lambda ls: any("exit 4" in line for line in ls))
        assert any("src/m.py:1: E501" in line for line in lines) and any("boom" in line for line in lines)
        assert any("✘ echo" in line for line in lines)  # the tab tells it failed
        t.type("\t")  # from the command to its output
        time.sleep(0.2)
        t.type("\x1b[A\r")  # from boom (it follows the end) to the src/m.py line: Enter opens it
        deadline = time.monotonic() + 5
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert record.read_text().split() == ["+1", str(work / "src" / "m.py")]
        t.type("r")  # again
        s.wait(lambda ls: any("exit 4" in line for line in ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
