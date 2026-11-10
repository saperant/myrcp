"""Starting the workbench from Python: the `myrcp` command of the wheel, and what an
application calls from its own entry point.

    # pyproject.toml of an application
    [project.optional-dependencies]
    ui = ["myrcp>=0.9"]
    [project.scripts]
    songs-ui = "songs.ui:main"

    # songs/ui/__init__.py, next to product.toml and plugins/
    from pathlib import Path
    from myrcp.launch import run_product

    def main() -> None:
        run_product(Path(__file__).parent)

The plugin host runs in this Python (unless $MYRCP_PYTHON says otherwise), so the
application's plugins import the application's own packages without naming an
interpreter in their manifests.
"""

from __future__ import annotations

import os
import shutil
import sys
from collections.abc import Sequence
from pathlib import Path
from typing import NoReturn

NATIVE = Path(__file__).resolve().parent / "_native"


def launcher(gtk: bool = False) -> Path:
    """The native launcher: this package's (a wheel has it), else `myrcp`/`myrcp-gtk` on
    $PATH. The wheel has no GTK workbench; it comes from building myrcp from source."""
    name = "myrcp-gtk" if gtk else "myrcp"
    bundled = NATIVE / "bin" / name
    if bundled.is_file() and os.access(bundled, os.X_OK):
        return bundled
    found = shutil.which(name)
    if found:
        return Path(found)
    hint = "build myrcp with -DMYRCP_GTK=ON" if gtk else "pip install myrcp (a wheel), or build it from source"
    raise FileNotFoundError(f"no {name} here: {hint} (https://github.com/saperant/myrcp)")


def run(args: Sequence[str] = (), *, gtk: bool = False) -> NoReturn:
    """Replace this process with the workbench, given these command-line arguments."""
    try:
        exe = launcher(gtk)
    except FileNotFoundError as e:
        print(f"myrcp: {e}", file=sys.stderr)
        raise SystemExit(1) from None
    env = dict(os.environ)
    env.setdefault("MYRCP_PYTHON", sys.executable)
    os.execve(exe, [exe.name, *args], env)


def run_product(
    product_dir: str | os.PathLike[str], args: Sequence[str] | None = None, *, gtk: bool | None = None
) -> NoReturn:
    """The workbench for an application's product directory (product.toml, plugins/).
    ``args`` default to this program's; ``gtk`` to $MYRCP_GTK=1."""
    if gtk is None:
        gtk = os.environ.get("MYRCP_GTK") == "1"
    run(["--product", str(product_dir), *(sys.argv[1:] if args is None else args)], gtk=gtk)


def main() -> NoReturn:
    """The `myrcp` command."""
    run(sys.argv[1:])
