"""Terminal views in the workbench: editors in tabs, shells, the Ctrl+\\ prefix."""

import shutil
import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")
W, H = 100, 30


class Screen:
    """The workbench as a terminal shows it."""

    def __init__(self, pty):
        self.pty = pty

    def text(self):
        screen = pyte.Screen(W, H)
        pyte.ByteStream(screen).feed(bytes(self.pty.out))
        return "\n".join(screen.display)

    def wait(self, needle, timeout=15, absent=False):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (needle in self.text()) != absent:
                return
            time.sleep(0.05)
        raise TimeoutError(f"{needle!r} {'still' if absent else 'not'} on screen:\n{self.text()}")

    def tab(self, title):
        """Alt+N for the tab with this title (the other plugins found decide N)."""
        import re

        m = re.search(rf"(\d+) {re.escape(title)}\b", self.text().splitlines()[0])
        assert m, f"no tab {title!r}: {self.text().splitlines()[0]}"
        return f"\x1b{m.group(1)}"


def start(tmp_path, **env):
    work = tmp_path / "work"
    work.mkdir(exist_ok=True)
    pty = Pty([str(BINARY)], work, env_for(tmp_path, **{"MYRCP_EDITOR_MODE": "tab", **env}), cols=W, rows=H)
    return work, pty, Screen(pty)


@pytest.mark.skipif(not shutil.which("vim"), reason="no vim")
def test_vim_in_a_tab(tmp_path):
    work, t, screen = start(tmp_path, VISUAL="vim -u NONE -N -n")
    (work / "notes.txt").write_text("x\n")
    try:
        screen.wait("notes.txt")
        t.type("\x1b[B")  # .., then notes.txt
        time.sleep(0.2)
        t.type("\r")  # opens it: a tab with vim in it
        screen.wait("~")  # vim's empty lines
        assert " notes.txt " in screen.text().splitlines()[0]
        t.type("iHello \x1b")
        time.sleep(0.2)
        t.type(":wq\r")
        screen.wait("~", absent=True)  # vim exited: the tab is gone
        assert (work / "notes.txt").read_text() == "Hello x\n"
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


def test_shell_tab_and_the_prefix(tmp_path):
    _, t, screen = start(tmp_path, SHELL="/bin/sh")
    try:
        screen.wait("Files")
        t.type("\x14")  # Ctrl+T: a shell
        screen.wait("workbench keys")  # the hints of a terminal view
        t.type("echo hel''lo-from-sh\r")
        screen.wait("hello-from-sh")
        t.type("\x10")  # Ctrl+P goes to the shell, not the palette
        time.sleep(0.3)
        assert "Commands" not in screen.text()
        t.type("\x1c\x10")  # Ctrl+\ then Ctrl+P: the palette
        screen.wait("Commands")
        t.type("\x1b")
        time.sleep(0.2)
        t.type("\x15exit\r")  # Ctrl+U drops the ^P the shell got; then it exits and its tab closes
        screen.wait("workbench keys", absent=True)
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


def test_problems_view_opens_the_place(tmp_path):
    record = tmp_path / "editor-args"
    editor = tmp_path / "ed"
    editor.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{record}"\n')
    editor.chmod(0o755)
    work, t, screen = start(tmp_path, VISUAL=str(editor), MYRCP_EDITOR_MODE="suspend")
    (work / "a.py").write_text("x = 1\ny = 2  # TODO tidy\n")
    (work / "b.txt").write_text("FIXME broken\n")
    try:
        screen.wait("b.txt")
        t.type("t")  # Files: Find TODOs
        screen.wait("2 TODO/FIXME")
        t.type("\x1b[19~")  # F8: Problems
        screen.wait("1 errors, 1 warnings")
        lines = screen.text().splitlines()
        fixme = next(n for n, line in enumerate(lines) if "FIXME broken" in line)
        todo = next(n for n, line in enumerate(lines) if "TODO tidy" in line)
        assert fixme < todo  # errors first
        assert "b.txt:1" in lines[fixme] and "error" in lines[fixme]
        t.type("\x1b[B\r")  # the TODO: the editor opens there
        deadline = time.monotonic() + 10
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert record.read_text().split() == ["+2", str(work / "a.py")]
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


@pytest.mark.skipif(not shutil.which("nvim"), reason="no nvim")
def test_neovim_shows_markers_and_jumps_to_them(tmp_path):
    work, t, screen = start(tmp_path, VISUAL="nvim --clean")
    (work / "a.py").write_text("x = 1\n" + "pass\n" * 10 + "y = 2  # TODO tidy\n")
    try:
        screen.wait("a.py")
        t.type("t")  # markers first: Neovim gets them when it connects
        screen.wait("1 TODO/FIXME")
        t.type("\x1b[B\r")  # .., a.py: Neovim in a tab
        screen.wait("■ y = 2  # TODO tidy")  # the marker as a diagnostic (virtual text)
        t.type("\x1c\x1b[19~")  # Ctrl+\ F8: Problems
        screen.wait("0 errors, 1 warnings")
        t.type("\r")  # opens the place in the Neovim that is open, not a new editor
        screen.wait("■ y = 2")
        tab_bar = screen.text().splitlines()[0]
        assert tab_bar.count("a.py") == 1
        t.type(":q\r")
        screen.wait("■", absent=True)
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
