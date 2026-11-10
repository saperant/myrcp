"""An installed myrcp wheel works: the command, the SDK it finds, a product's plugins in
this Python (cibuildwheel's test-command: python tools/wheel_smoke.py <source dir>)."""

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import myrcp
from myrcp import launch

source = Path(sys.argv[1])
exe = launch.launcher()
assert exe.is_relative_to(launch.NATIVE), exe  # the wheel's own, not one on $PATH
command = Path(sys.executable).parent / "myrcp"
with tempfile.TemporaryDirectory() as state:
    env = dict(os.environ, XDG_STATE_HOME=state, MYRCP_PLUGIN_PATH="")
    env.pop("MYRCP_PYTHON", None)
    version = subprocess.run(
        [command, "--version"], capture_output=True, text=True, check=True, env=env
    )
    assert version.stdout.strip() == f"myrcp {myrcp.__version__}", (
        version.stdout,
        myrcp.__version__,
    )
    listed = subprocess.run(
        [command, "--product", source / "examples" / "demo", "--list"],
        capture_output=True,
        text=True,
        check=False,
        env=env,
    )
    assert listed.returncode == 0, listed.stderr
    assert "plugin demo (Demo)" in listed.stdout, listed.stdout
    # The plugin host ran in this Python, with the SDK of the wheel.
    log = listed.stderr
    assert f"python {sys.version.split()[0]}" in log, log
print("wheel ok:", exe, version.stdout.strip())
