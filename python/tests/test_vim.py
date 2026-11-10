"""The Vim link (+channel): Vim connects to a socket of ours and sources our script; its
cursor is the workbench selection, its quickfix list is in Problems, our problems are
its signs; a Vim of the user's own links itself through the shared socket."""

import re
import shutil
import subprocess
import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_neovim import DOWN, PREFIX, side, workbench_command
from test_regions import Screen

pyte = pytest.importorskip("pyte")


def vim_links():
    if not shutil.which("vim"):
        return False
    out = subprocess.run(["vim", "--version"], check=False, capture_output=True, text=True)
    return all(f"+{f}" in out.stdout for f in ("channel", "timers", "signs"))


pytestmark = [
    pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)"),
    pytest.mark.skipif(not vim_links(), reason="no vim with +channel"),
]

VIM = "vim -N -u NONE -i NONE"


def test_vim_in_a_tab_tells_its_cursor_and_quickfix(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    text = [f"line_{n} = {n}\n" for n in range(1, 21)]
    text[4] = "# TODO soon\n"
    (work / "a.py").write_text("".join(text))
    env = env_for(tmp_path, MYRCP_EDITOR_MODE="tab", VISUAL=VIM)
    t = Pty([str(BINARY)], work, env, cols=160, rows=44)
    s = Screen(t, 160, 44)
    try:
        s.wait(lambda ls: "a.py" in "\n".join(ls))
        t.type(DOWN + "\r")  # .., a.py: Vim in a tab
        s.wait(lambda ls: "line_20 = 20" in "\n".join(ls))
        workbench_command(t, s, "properties show", in_terminal=True)
        s.wait(lambda ls: "│" in ls[0] and "Properties" in ls[0][ls[0].index("│") :])
        t.type("7G")
        s.wait(lambda ls: "a.py:7" in side(ls) and "line_7 = 7" in side(ls))

        # Its quickfix list (:make, :grep) is in Problems...
        t.type(f":cexpr ['{work / 'a.py'}:3:from make']\r")
        t.type(PREFIX + "\x1b[19~")  # Ctrl+\\ F8: Problems
        lines = s.wait(lambda ls: "from make" in "\n".join(ls))
        assert any("a.py:3" in line and "quickfix" in line for line in lines)
        tab = re.search(r"(\d+) a\.py", lines[0]).group(1)

        # ...and a plugin's problems are signs in it, with the message after the line.
        t.type("\x1b1")  # Files
        workbench_command(t, s, "find todos")  # the demo plugin marks TODO lines
        t.type("\x1b" + tab)
        time.sleep(0.3)
        t.type(":call writefile([string(sign_getplaced('%', {'group': 'myrcp'})[0].signs)], 'signs.txt')\r")
        deadline = time.monotonic() + 5
        while not (work / "signs.txt").exists() and time.monotonic() < deadline:
            time.sleep(0.1)
        assert "'lnum': 5" in (work / "signs.txt").read_text()

        t.type(":q!\r")  # its diagnostics go with it
        s.wait(lambda ls: not re.search(rf"\b{tab} a\.py", ls[0]))  # its tab closed
        t.type("\x1b[19~")
        s.wait(lambda ls: "TODO soon" in "\n".join(ls) and "from make" not in "\n".join(ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
    finally:
        if t.proc.poll() is None:
            t.proc.kill()


def test_your_own_vim_links_itself(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "a.py").write_text("".join(f"line_{n} = {n}\n" for n in range(1, 21)))
    (work / "todo.txt").write_text("fine\nTODO later\n")
    script = tmp_path / "myrcp.vim"
    script.write_text(subprocess.run([BINARY, "--vim-script"], check=True, capture_output=True, text=True).stdout)
    env = env_for(tmp_path, VISUAL="false")  # never the fallback editor

    # The user's Vim, started before myrcp: it links once myrcp is there.
    vim_env = dict(env, TERM="vt100")  # no xterm key modes for the test's screen
    vim = Pty(["vim", "-N", "-u", "NONE", "-i", "NONE", "-S", str(script)], work, vim_env, cols=80, rows=24)
    vs = Screen(vim, 80, 24)
    t = Pty([str(BINARY)], work, env, cols=160, rows=44)
    s = Screen(t, 160, 44)
    try:
        s.wait(lambda ls: "a.py" in "\n".join(ls))
        s.wait(lambda ls: "Linked to your Vim" in ls[-1], timeout=10)
        workbench_command(t, s, "properties show")
        s.wait(lambda ls: "│" in ls[0])
        t.type("\x1b1" + DOWN + "\r")  # Files, a.py: opens in that Vim
        s.wait(lambda ls: "in your Vim" in ls[-1])
        vs.wait(lambda ls: "line_20 = 20" in "\n".join(ls))
        vim.type("12G")  # its cursor is the selection
        s.wait(lambda ls: "a.py:12" in side(ls) and "From Vim" in side(ls))

        # A plugin's problems: a sign and the message after the line.
        t.type("\x1b1")
        workbench_command(t, s, "find todos")
        vim.type(":e todo.txt\r")
        vs.wait(lambda ls: any("TODO later" in line and "demo: TODO later" in line for line in ls))
        t.type("\x11")
        assert t.proc.wait(timeout=10) == 0
        vim.type(":e a.py\r")
        vs.wait(lambda ls: "line_1 = 1" in "\n".join(ls))
        assert vim.proc.poll() is None  # the user's own: it keeps running
        assert not (tmp_path / "run" / "myrcp-vim.sock").exists()
    finally:
        for p in (t, vim):
            if p.proc.poll() is None:
                p.proc.kill()
