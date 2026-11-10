"""The Neovim link both ways: its cursor is the workbench selection, its own
diagnostics are problems; a Neovim of the user's own (workbench.nvim_server) is where
files open."""

import re
import shutil
import subprocess
import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen

pyte = pytest.importorskip("pyte")
pytestmark = [
    pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)"),
    pytest.mark.skipif(not shutil.which("nvim"), reason="no nvim"),
]

PREFIX, PALETTE, DOWN = "\x1c", "\x10", "\x1b[B"


def side(lines):
    x = lines[0].index("│")
    return "\n".join(line[x + 1 :] for line in lines)


def workbench_command(t, s, text, in_terminal=False):
    t.type((PREFIX if in_terminal else "") + PALETTE)
    s.wait(lambda ls: any("Commands" in line for line in ls))
    t.type(text)
    time.sleep(0.3)
    t.type("\r")


def test_neovim_in_a_tab_tells_its_cursor_and_diagnostics(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "a.py").write_text("".join(f"line_{n} = {n}\n" for n in range(1, 21)))
    env = env_for(tmp_path, MYRCP_EDITOR_MODE="tab", VISUAL="nvim --clean")
    t = Pty([str(BINARY)], work, env, cols=160, rows=44)
    s = Screen(t, 160, 44)
    try:
        s.wait(lambda ls: "a.py" in "\n".join(ls))
        t.type(DOWN + "\r")  # .., a.py: Neovim in a tab
        s.wait(lambda ls: "line_20 = 20" in "\n".join(ls))
        workbench_command(t, s, "properties show", in_terminal=True)
        s.wait(lambda ls: "│" in ls[0] and "Properties" in ls[0][ls[0].index("│") :])
        t.type("7G")  # Neovim moves; once the cursor rests, it is the selection
        lines = s.wait(lambda ls: "a.py:7" in side(ls) and "line_7 = 7" in side(ls))
        assert "From" in side(lines) and "python" in side(lines)

        # Neovim's own diagnostics (a linter's, an LSP server's) are in Problems.
        lua = (
            ":lua vim.diagnostic.set(vim.api.nvim_create_namespace('lint'), 0, {{lnum = 2, col = 0,"
            " message = 'from the linter', source = 'lint', severity = vim.diagnostic.severity.ERROR}})\r"
        )
        t.type(lua)
        t.type(PREFIX + "\x1b[19~")  # Ctrl+\ F8: Problems
        lines = s.wait(lambda ls: "from the linter" in "\n".join(ls) and "1 errors" in "\n".join(ls))
        assert any("a.py:3" in line and "lint" in line for line in lines)
        tab = re.search(r"(\d+) a\.py", lines[0]).group(1)
        t.type("\x1b" + tab)  # back to the Neovim tab (Alt+N)
        time.sleep(0.3)
        t.type(":q!\r")  # its diagnostics go with it
        s.wait(lambda ls: not re.search(rf"\b{tab} a\.py", ls[0]))  # its tab closed
        t.type("\x1b[19~")
        s.wait(lambda ls: "No problems reported." in "\n".join(ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


def test_your_own_neovim_is_where_files_open(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "a.py").write_text("".join(f"line_{n} = {n}\n" for n in range(1, 21)))
    sock = tmp_path / "nvim.sock"
    nvim = subprocess.Popen(
        ["nvim", "--clean", "--headless", "--listen", str(sock)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )

    def remote(expr):
        out = subprocess.run(
            ["nvim", "--clean", "--headless", "--server", str(sock), "--remote-expr", expr],
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )
        return (out.stdout + out.stderr).strip()

    env = env_for(tmp_path, MYRCP_NVIM_SERVER=str(sock), VISUAL="false")  # never the fallback editor
    t = Pty([str(BINARY)], work, env, cols=160, rows=44)
    s = Screen(t, 160, 44)
    try:
        for _ in range(100):
            if sock.exists():
                break
            time.sleep(0.05)
        s.wait(lambda ls: "a.py" in "\n".join(ls))
        s.wait(lambda ls: "Linked to your Neovim" in ls[-1])
        workbench_command(t, s, "properties show")
        s.wait(lambda ls: "│" in ls[0])
        t.type("\x1b1" + DOWN + "\r")  # Files, a.py: opens in that Neovim
        s.wait(lambda ls: "in your Neovim" in ls[-1])
        deadline = time.monotonic() + 10
        while remote("expand('%:t')") != "a.py" and time.monotonic() < deadline:
            time.sleep(0.1)
        assert remote("expand('%:t')") == "a.py"
        # Its cursor is the selection even though it is not in the workbench.
        subprocess.run(
            ["nvim", "--clean", "--headless", "--server", str(sock), "--remote-send", "12G"], check=True, timeout=10
        )
        s.wait(lambda ls: "a.py:12" in side(ls) and "From Neovim" in side(ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
        assert nvim.poll() is None  # the user's own: it keeps running
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
        nvim.kill()
