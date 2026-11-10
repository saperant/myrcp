"""Back and Forward (Alt+← / Alt+→): to the view, widget and row you were in."""

import shutil
import time

import pytest

pyte = pytest.importorskip("pyte")
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen

pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")

BACK, FORWARD, DOWN = "\x1b[1;3D", "\x1b[1;3C", "\x1b[B"


def test_back_and_forward(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "notes.txt").write_text("x\n")
    (work / "subdir").mkdir()
    record = tmp_path / "editor-args"
    editor = tmp_path / "fake-editor"
    editor.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" >> "{record}"\n')
    editor.chmod(0o755)

    def opened():
        deadline = time.monotonic() + 5
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        return record.read_text().split() if record.exists() else []

    t = Pty([str(BINARY)], work, env_for(tmp_path, VISUAL=str(editor)))
    s = Screen(t, 100, 30)
    try:
        t.wait_for("notes.txt")
        t.type(DOWN + DOWN)  # .., subdir/, notes.txt
        time.sleep(0.3)
        m = t.mark()
        t.type("\x1b[19~")  # F8: Problems takes the focus
        t.wait_for("No problems", since=m)
        time.sleep(0.3)
        m = t.mark()
        t.type(BACK)  # Files again, on notes.txt: Enter opens it
        time.sleep(0.3)
        t.type("\r")
        assert opened() == [str(work / "notes.txt")]
        record.unlink()
        t.type(FORWARD)  # Problems again: Enter there opens nothing
        time.sleep(0.3)
        t.type("\r")
        time.sleep(0.5)
        assert not record.exists()
        t.type(FORWARD)
        s.wait(lambda ls: "Nowhere to go forward to" in ls[-1])
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


@pytest.mark.skipif(not shutil.which("fzf"), reason="no fzf")
def test_go_to_anything_with_fzf(tmp_path):
    """Ctrl+O: fzf over the files, views and problems; the file chosen opens."""
    work = tmp_path / "work"
    (work / "deep" / "er").mkdir(parents=True)
    (work / "deep" / "er" / "target.txt").write_text("x\n")
    (work / "other.txt").write_text("y\n")
    record = tmp_path / "editor-args"
    editor = tmp_path / "fake-editor"
    editor.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" >> "{record}"\n')
    editor.chmod(0o755)
    env = env_for(tmp_path, VISUAL=str(editor), FZF_DEFAULT_OPTS="")
    t = Pty([str(BINARY)], work, env)
    s = Screen(t, 100, 30)
    try:
        s.wait(lambda ls: "other.txt" in "\n".join(ls))
        t.type("\x0f")  # Ctrl+O
        s.wait(lambda ls: "Go to Anything" in "\n".join(ls) and "deep/er/target.txt" in "\n".join(ls))
        assert any("view" in line and "Files" in line for line in s.lines())
        t.type("targ")
        time.sleep(0.5)
        t.type("\r")
        deadline = time.monotonic() + 5
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert record.read_text().split() == [str(work / "deep" / "er" / "target.txt")]
        s.wait(lambda ls: "Go to Anything" not in "\n".join(ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
