"""Preferences: settings declared by the workbench and plugins, changed in the
Preferences view or by hand in preferences.toml, kept across sessions; an environment
variable that is set wins."""

import time

import pytest
from conftest import BINARY
from test_e2e import Pty, env_for
from test_regions import Screen, command

pyte = pytest.importorskip("pyte")
pytestmark = pytest.mark.skipif(not BINARY.exists(), reason="the demo is not built (./build.sh test)")

DOWN = "\x1b[B"


def row(lines, title):
    """The screen line of a setting's row."""
    return next((line for line in lines if title in line), "")


def start(tmp_path, work, **env):
    t = Pty([str(BINARY)], work, env_for(tmp_path, **env), cols=120, rows=40)
    return t, Screen(t, 120, 40)


def quit_(t):
    t.type("\x11")
    assert t.proc.wait(timeout=10) == 0


def test_preferences_view_file_and_environment(tmp_path):
    work = tmp_path / "work"
    (work / "sub").mkdir(parents=True)
    (work / "notes.txt").write_text("hello\n")
    (work / ".secret").write_text("hidden\n")
    (work / "sub" / "inner.txt").write_text("inside\n")
    prefs = tmp_path / "config" / "myrcp" / "preferences.toml"
    # "Preferences: Edit the file" runs the editor in the foreground; this one adds a line.
    editor = tmp_path / "editor.sh"
    editor.write_text('#!/bin/sh\nprintf \'\\n[workbench]\\ntheme = "classic"\\n\' >> "$1"\n')
    editor.chmod(0o755)

    t, s = start(tmp_path, work, VISUAL=str(editor))
    try:
        s.wait(lambda ls: "notes.txt" in "\n".join(ls))
        assert ".secret" not in "\n".join(s.lines())
        command(t, s, "preferences show")
        lines = s.wait(lambda ls: "Show dotfiles" in "\n".join(ls) and "Editor mode" in "\n".join(ls))
        # The cursor starts on the first row (Theme): down to demo's.
        first = next(y for y, line in enumerate(lines) if "Theme" in line)
        for _ in range(next(y for y, line in enumerate(lines) if "Show dotfiles" in line) - first):
            t.type(DOWN)
            time.sleep(0.1)
        t.type("\r")  # a bool: Enter toggles it, no dialog
        s.wait(lambda ls: " on " in row(ls, "Show dotfiles") and "file" in row(ls, "Show dotfiles"))
        assert "[demo]\nhidden = true\n" in prefs.read_text()

        # A text setting: Enter opens a dialog with a field; Enter in it sets the value.
        t.type(DOWN + "\r")
        s.wait(lambda ls: any("The folder Files opens in" in line for line in ls[3:]) and "OK" in "\n".join(ls))
        t.type(str(work / "sub") + "\r")
        s.wait(lambda ls: "file" in row(ls, "Start folder") and "OK" not in "\n".join(ls))
        assert f'start = "{work / "sub"}"' in prefs.read_text()

        # Edited by hand: read again when the editor exits.
        command(t, s, "preferences edit the file")
        s.wait(lambda ls: "classic" in row(ls, "Theme") and "file" in row(ls, "Theme"))
        text = prefs.read_text()
        assert "hidden = true" in text and 'theme = "classic"' in text
        quit_(t)
    finally:
        if t.proc.poll() is None:
            t.proc.kill()

    # The next session starts with them: Files opens in sub/ (the session comes back
    # as it was left, with Preferences in front).
    t, s = start(tmp_path, work, MYRCP_MOUSE="0")
    try:
        s.wait(lambda ls: "Show dotfiles" in "\n".join(ls) and "Files" in ls[0])
        t.type("\x1b1")  # Alt+1: Files
        s.wait(lambda ls: "inner.txt" in "\n".join(ls))
        # An environment variable wins, and says so.
        command(t, s, "preferences show")
        s.wait(lambda ls: "$MYRCP_MOUSE" in row(ls, "Mouse") and " off " in row(ls, "Mouse"))
        quit_(t)
    finally:
        if t.proc.poll() is None:
            t.proc.kill()
