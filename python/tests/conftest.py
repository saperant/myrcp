import os
import select
import subprocess
import sys
from pathlib import Path

import pytest

from myrcp._msgpack import Unpacker, packb

REPO = Path(__file__).resolve().parents[2]
PYTHON_DIR = REPO / "python"
# The demo application (examples/demo, -DMYRCP_DEMO=ON: ./build.sh test builds it) is
# what the end-to-end tests drive: libmyrcp with the demo's plugins.
PLUGINS_DIR = REPO / "examples" / "demo" / "plugins"
BINARY = REPO / "build" / "examples" / "demo" / "myrcp-demo"
LAUNCHER = REPO / "build" / "myrcp"
STATE_ID = "myrcp-demo"  # its $XDG_STATE_HOME/<id>


class Peer:
    """Talks to `python -m myrcp.host` as the workbench would."""

    def __init__(self, cwd: Path, plugin_path: Path = PLUGINS_DIR, python: str = sys.executable, env=None) -> None:
        env = dict(os.environ, PYTHONPATH=str(PYTHON_DIR), MYRCP_PLUGIN_PATH=str(plugin_path), **(env or {}))
        self.proc = subprocess.Popen(
            [python, "-m", "myrcp.host"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=cwd,
            env=env,
        )
        self.unpacker = Unpacker()
        self.inbox: list = []
        self.next_id = 1

    def send(self, msg) -> None:
        self.proc.stdin.write(packb(msg))
        self.proc.stdin.flush()

    def notify(self, method, params) -> None:
        self.send([2, method, [params]])

    def request(self, method, params):
        msgid = self.next_id
        self.next_id += 1
        self.send([0, msgid, method, [params]])
        msg = self.wait(lambda m: m[0] == 1 and m[1] == msgid)
        assert msg[2] is None, msg[2]
        return msg[3]

    def wait(self, pred, timeout=5.0):
        import time

        deadline = time.monotonic() + timeout
        while True:
            for i, m in enumerate(self.inbox):
                if pred(m):
                    return self.inbox.pop(i)
            left = deadline - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"no matching message; got {self.inbox}")
            fd = self.proc.stdout.fileno()
            r, _, _ = select.select([fd], [], [], left)
            if r:
                data = os.read(fd, 65536)
                if not data:
                    raise EOFError(self.proc.stderr.read().decode())
                self.unpacker.feed(data)
                self.inbox.extend(self.unpacker)

    def wait_notify(self, method, pred=lambda p: True, timeout=5.0):
        return self.wait(lambda m: m[0] == 2 and m[1] == method and pred(m[2][0]), timeout)[2][0]

    def close(self) -> str:
        if self.proc.poll() is None:
            self.notify("shutdown", {})
            self.proc.stdin.close()
            self.proc.wait(timeout=5)
        return self.proc.stderr.read().decode()


@pytest.fixture
def peer(tmp_path):
    (tmp_path / "sub").mkdir()
    (tmp_path / "a.txt").write_text("hello\n")
    (tmp_path / ".hidden").write_text("")
    p = Peer(tmp_path)
    yield p
    p.close()
