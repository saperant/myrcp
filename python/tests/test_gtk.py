"""myrcp-gtk (-DMYRCP_GTK=ON, ./build.sh gtk) under Xvfb: it starts the plugin host,
shows the demo's views, takes workbench keys and saves the arrangement on Ctrl+Q."""

import shutil
import subprocess
import time
from pathlib import Path

import pytest
from test_e2e import env_for

GTK_BINARY = Path(__file__).resolve().parents[2] / "build-gtk" / "examples" / "demo" / "myrcp-demo-gtk"

pytestmark = pytest.mark.skipif(
    not GTK_BINARY.exists() or not shutil.which("Xvfb") or not shutil.which("xdotool"),
    reason="needs build-gtk/examples/demo/myrcp-demo-gtk (./build.sh gtk), Xvfb and xdotool",
)


@pytest.fixture
def display():
    for n in range(90, 120):
        if not Path(f"/tmp/.X11-unix/X{n}").exists():
            break
    xvfb = subprocess.Popen(["Xvfb", f":{n}", "-screen", "0", "1280x800x24", "-nolisten", "tcp"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)  # fmt: skip
    deadline = time.monotonic() + 10
    while not Path(f"/tmp/.X11-unix/X{n}").exists() and time.monotonic() < deadline:
        time.sleep(0.05)
    yield f":{n}"
    xvfb.terminate()
    xvfb.wait(timeout=10)


def xdotool(display, *args):
    subprocess.run(["xdotool", *args], check=True, env={"DISPLAY": display}, timeout=10)


def wait_for(cond, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if cond():
            return True
        time.sleep(0.1)
    return False


def test_starts_takes_keys_and_saves_on_quit(tmp_path, display):
    env = env_for(tmp_path, DISPLAY=display, GDK_BACKEND="x11", NO_AT_BRIDGE="1", GSK_RENDERER="cairo")
    env.pop("WAYLAND_DISPLAY", None)
    log = tmp_path / "gtk.log"
    with log.open("w") as out:
        proc = subprocess.Popen([str(GTK_BINARY)], env=env, stdout=out, stderr=subprocess.STDOUT)
    try:
        host_log = tmp_path / "state" / "myrcp-demo" / "host.log"
        assert wait_for(lambda: host_log.exists() and "activated demo" in host_log.read_text()), log.read_text()
        time.sleep(0.5)
        xdotool(display, "key", "F8")  # Problems opens at the bottom
        time.sleep(0.5)
        xdotool(display, "key", "ctrl+q")
        assert proc.wait(timeout=15) == 0, log.read_text()
    finally:
        if proc.poll() is None:
            proc.kill()
    state = (tmp_path / "state" / "myrcp-demo" / "workbench.state").read_bytes()  # msgpack
    assert b"demo.files" in state and b"problems" in state, state
    assert "CRITICAL" not in log.read_text(), log.read_text()
