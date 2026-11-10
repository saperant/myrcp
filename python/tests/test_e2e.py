"""The built workbench binary, headless (--list) and in a pseudo-terminal."""

import fcntl
import os
import pty
import struct
import subprocess
import sys
import termios
import threading
import time

import pytest
from conftest import BINARY, PLUGINS_DIR

pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")


def env_for(tmp_path, **extra):
    env = dict(
        os.environ,
        MYRCP_PYTHON=sys.executable,
        MYRCP_PLUGIN_PATH="",
        XDG_STATE_HOME=str(tmp_path / "state"),
        XDG_CONFIG_HOME=str(tmp_path / "config"),  # never the user's own preferences
        XDG_RUNTIME_DIR=str(tmp_path / "run"),  # never the user's own Vims linking in
        TERM="xterm-256color",
    )
    (tmp_path / "run").mkdir(exist_ok=True)
    for name in (
        "MYRCP_THEME",
        "MYRCP_EDITOR_MODE",
        "MYRCP_MOUSE",
        "MYRCP_NVIM_SERVER",
        "MYRCP_VIM_SOCKET",
        "MYRCP_DIFF",
    ):
        env.pop(name, None)  # the user's settings would win over the tests'
    env.update(extra)
    return env


def test_list(tmp_path):
    out = subprocess.run(
        [BINARY, "--list"], check=False, capture_output=True, text=True, env=env_for(tmp_path), timeout=30
    )
    assert out.returncode == 0, out.stderr
    assert "plugin demo (Demo)" in out.stdout
    assert "demo.files.up" in out.stdout and "[backspace]" in out.stdout
    assert PLUGINS_DIR.exists()


class Pty:
    def __init__(self, argv, cwd, env, cols=100, rows=30):
        self.master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.proc = subprocess.Popen(
            argv, stdin=slave, stdout=slave, stderr=slave, cwd=cwd, env=env, start_new_session=True
        )
        os.close(slave)
        self.out = bytearray()
        self.lock = threading.Lock()
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        while True:
            try:
                data = os.read(self.master, 65536)
            except OSError:
                return
            if not data:
                return
            with self.lock:
                self.out += data

    def text(self) -> str:
        with self.lock:
            return self.out.decode("utf-8", "replace")

    def wait_for(self, needle: str, timeout=10.0, since=0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.text()[since:]:
                return
            time.sleep(0.05)
        raise TimeoutError(f"{needle!r} not on screen; output tail: {self.text()[-2000:]!r}")

    def mark(self) -> int:
        return len(self.text())

    def type(self, data: str):
        os.write(self.master, data.encode())


def test_workbench_in_a_pty(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "notes.txt").write_text("x\n")
    (work / "subdir").mkdir()
    record = tmp_path / "editor-args"
    editor = tmp_path / "fake-editor"
    editor.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{record}"\n')
    editor.chmod(0o755)

    t = Pty([str(BINARY)], work, env_for(tmp_path, VISUAL=str(editor)))
    try:
        t.wait_for("Files")
        t.wait_for("notes.txt")
        t.wait_for("subdir/")

        # Down to notes.txt (.., subdir/, notes.txt), Enter: the editor gets the file.
        t.type("\x1b[B\x1b[B")
        time.sleep(0.3)
        t.type("\r")
        deadline = time.monotonic() + 10
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert record.read_text().split() == [str(work / "notes.txt")]

        # The palette finds plugin commands; running one shows its status.
        m = t.mark()
        t.type("\x10")  # ctrl+p
        t.wait_for("Commands", since=m)
        t.type("say hello")
        time.sleep(0.2)
        t.type("\r")
        t.wait_for("Hello from the demo plugin", since=m)

        # Themes switch from the palette: the classic one paints blue windows.
        m = t.mark()
        t.type("\x10")
        t.wait_for("Commands", since=m)
        t.type("theme classic")
        time.sleep(0.2)
        t.type("\r")
        t.wait_for("\x1b[0;97;44m", since=m)

        # Alt+2 switches to Jobs, s starts one, its result lands in History.
        m = t.mark()
        t.type("\x1b2")
        t.wait_for("Start job", since=m)
        t.type("s")
        t.wait_for("ok, 40 steps", timeout=15, since=m)

        # The mouse: a click on the first tab, then on the second, then on its button.
        m = t.mark()
        t.type("\x1b[<0;12;1M")  # " 1 Files " starts at column 9
        t.wait_for("notes.txt", since=m)
        m = t.mark()
        t.type("\x1b[<0;20;1M\x1b[<0;20;1m")
        t.wait_for("Start job", since=m)
        time.sleep(0.3)
        m = t.mark()
        t.type("\x1b[<0;3;2M")  # [ Start job ] on the view's first row
        t.wait_for("ok, 40 steps", timeout=15, since=m)

        t.type("\x11")  # ctrl+q
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
    screen = t.text()
    assert "\x1b[?1049l" in screen  # left the alternate screen on exit


def test_launcher_runs_a_product(tmp_path):
    """`myrcp --product DIR`: an application without a program of its own."""
    from conftest import LAUNCHER

    product = PLUGINS_DIR.parent
    out = subprocess.run([LAUNCHER, "--product", product, "--list"], check=False, capture_output=True, text=True,
                         env=env_for(tmp_path), timeout=30)  # fmt: skip
    assert out.returncode == 0, out.stderr
    assert "plugin demo (Demo)" in out.stdout
    version = subprocess.run([LAUNCHER, "--product", product, "--version"], capture_output=True, text=True, check=True)
    assert version.stdout.startswith("myrcp 0.")
    # Without a product: the plugins on MYRCP_PLUGIN_PATH, none here.
    bare = subprocess.run(
        [LAUNCHER, "--list"], check=False, capture_output=True, text=True, env=env_for(tmp_path), timeout=30
    )
    assert bare.returncode == 0 and "plugin demo" not in bare.stdout


def test_search_is_built_in(tmp_path):
    """Ctrl+F: the workbench's own Search view, at the bottom; Enter on a match opens it."""
    work = tmp_path / "work"
    work.mkdir()
    (work / "a.txt").write_text("one\nthe needle here\n")
    record = tmp_path / "editor-args"
    editor = tmp_path / "fake-editor"
    editor.write_text(f'#!/bin/sh\nprintf "%s\\n" "$@" > "{record}"\n')
    editor.chmod(0o755)
    t = Pty([str(BINARY)], work, env_for(tmp_path, VISUAL=str(editor)), cols=120, rows=40)
    try:
        t.wait_for("a.txt")
        m = t.mark()
        t.type("\x06")  # ctrl+f
        t.wait_for("Type what to look for", since=m)
        t.type("needle\r")
        t.wait_for("1 match in 1 fil", since=m)
        t.type("\t\t\t")  # past regex and match case to the results
        time.sleep(0.3)
        t.type("\x1b[B\r")  # the file's row, then its match
        deadline = time.monotonic() + 10
        while not record.exists() and time.monotonic() < deadline:
            time.sleep(0.05)
        assert str(work / "a.txt") in record.read_text()
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
