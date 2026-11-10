"""The plugin host over the real protocol, with the demo plugin."""

import sys

from conftest import Peer


def find(tree, pred):
    if pred(tree):
        return tree
    for c in tree.get("children", []) + ([tree["child"]] if "child" in tree else []):
        if (hit := find(c, pred)) is not None:
            return hit
    return None


def entries(tree):
    lst = find(tree, lambda n: n.get("id") == "entries")
    return [i["key"] for i in lst["items"]]


def test_initialize_reports_manifests_without_importing(peer):
    res = peer.request("initialize", {"protocol": 1})
    demo = next(p for p in res["plugins"] if p["id"] == "demo")
    assert [v["id"] for v in demo["views"]] == ["demo.files", "demo.jobs", "demo.table", "demo.widgets", "demo.note"]
    up = next(c for c in demo["commands"] if c["id"] == "demo.files.up")
    assert up["keys"] == ["backspace"] and up["view"] == "demo.files"
    assert res["errors"] == []
    log = peer.close()
    assert "activated demo" not in log  # lazy: nothing opened, nothing imported


def test_files_view_events_and_commands(peer, tmp_path):
    peer.request("initialize", {"protocol": 1})
    tree = peer.request("view.open", {"view": "demo.files"})["tree"]
    assert entries(tree) == ["..", "sub", "a.txt"]

    peer.notify("command.run", {"command": "demo.files.hidden", "view": "demo.files"})
    update = peer.wait_notify("view.update")
    assert ".hidden" in entries(update["tree"])

    peer.notify("view.event", {"view": "demo.files", "widget": "entries", "kind": "activate", "value": "sub"})
    update = peer.wait_notify("view.update")
    panel = find(update["tree"], lambda n: n.get("t") == "panel")
    assert panel["title"] == str(tmp_path / "sub")

    peer.notify("command.run", {"command": "demo.files.up", "view": "demo.files"})
    update = peer.wait_notify("view.update")
    lst = find(update["tree"], lambda n: n.get("id") == "entries")
    assert lst["selected"] == "sub"  # back up, the cursor on where we came from

    peer.notify("view.event", {"view": "demo.files", "widget": "entries", "kind": "activate", "value": "a.txt"})
    opened = peer.wait_notify("editor.open")
    assert opened["path"] == str(tmp_path / "a.txt")


def test_unchanged_tree_is_not_resent(peer):
    peer.request("initialize", {"protocol": 1})
    peer.request("view.open", {"view": "demo.files"})
    peer.notify("view.event", {"view": "demo.files", "widget": "nope", "kind": "press", "value": None})
    peer.notify("command.run", {"command": "demo.hello", "view": None})
    status = peer.wait_notify("status")
    assert status == {"text": "Hello from the demo plugin", "style": "ok"}
    assert not [m for m in peer.inbox if m[0] == 2 and m[1] == "view.update"]


def test_jobs_report_progress_and_finish(peer):
    peer.request("initialize", {"protocol": 1})
    peer.request("view.open", {"view": "demo.jobs"})
    peer.notify("command.run", {"command": "demo.jobs.start", "view": "demo.jobs"})
    begin = peer.wait_notify("job.begin")
    peer.wait_notify("job.progress", lambda p: p["job"] == begin["job"])
    end = peer.wait_notify("job.end", lambda p: p["job"] == begin["job"], timeout=10)
    assert end["ok"] is True
    update = peer.wait_notify("view.update", lambda p: "ok, 40 steps" in str(p["tree"]))
    assert update["view"] == "demo.jobs"


def test_job_cancel(peer):
    peer.request("initialize", {"protocol": 1})
    peer.request("view.open", {"view": "demo.jobs"})
    peer.notify("command.run", {"command": "demo.jobs.start", "view": "demo.jobs"})
    begin = peer.wait_notify("job.begin")
    peer.notify("job.cancel", {"job": begin["job"]})
    end = peer.wait_notify("job.end", timeout=10)
    assert end == {"job": begin["job"], "ok": False, "text": "cancelled"}


def test_broken_plugin_is_reported_not_fatal(tmp_path):
    bad = tmp_path / "plugins" / "bad"
    bad.mkdir(parents=True)
    (bad / "myrcp.toml").write_text('[plugin]\nid="bad"\nmodule="bad_mod"\n[[views]]\nid="bad.v"\n')
    (bad / "bad_mod.py").write_text("raise RuntimeError('boom at import')\n")
    (tmp_path / "plugins" / "worse").mkdir()
    (tmp_path / "plugins" / "worse" / "myrcp.toml").write_text("[plugin]\n")
    p = Peer(tmp_path, tmp_path / "plugins")
    try:
        res = p.request("initialize", {"protocol": 1})
        assert [pl["id"] for pl in res["plugins"]] == ["bad"]
        assert len(res["errors"]) == 1 and "worse" in res["errors"][0]
        p.send([0, 99, "view.open", [{"view": "bad.v"}]])
        msg = p.wait(lambda m: m[0] == 1 and m[1] == 99)
        assert "boom at import" in msg[2]
        assert p.request("initialize", {"protocol": 1})  # still alive
    finally:
        log = p.close()
    assert "boom at import" in log


def test_print_in_plugins_does_not_corrupt_the_protocol(tmp_path):
    d = tmp_path / "plugins" / "noisy"
    d.mkdir(parents=True)
    (d / "myrcp.toml").write_text('[plugin]\nid="noisy"\nmodule="noisy_mod"\n[[views]]\nid="noisy.v"\n')
    (d / "noisy_mod.py").write_text(
        "import sys\nfrom myrcp import Plugin, View, ui\nplugin = Plugin()\n"
        "print('stdout noise')\nsys.stdin.read()\n"
        "@plugin.view('noisy.v')\nclass V(View):\n    def render(self):\n        print('more')\n"
        "        return ui.text('quiet')\n"
    )
    p = Peer(tmp_path, tmp_path / "plugins")
    try:
        p.request("initialize", {"protocol": 1})
        assert p.request("view.open", {"view": "noisy.v"})["tree"]["text"] == "quiet"
    finally:
        log = p.close()
    assert "stdout noise" in log
    assert sys.executable  # keep the import used


def test_markers_are_reported(peer, tmp_path):
    (tmp_path / "todo.txt").write_text("fine\nTODO later\n")
    peer.request("initialize", {"protocol": 1})
    peer.request("view.open", {"view": "demo.files"})
    peer.notify("command.run", {"command": "demo.files.todos", "view": "demo.files"})
    m = peer.wait_notify("markers.set")
    assert m["set"] == "todos"
    assert m["markers"] == [
        {
            "message": "TODO later",
            "path": str(tmp_path / "todo.txt"),
            "line": 2,
            "col": 0,
            "severity": "warning",
            "source": "demo",
            "view": "",
        }
    ]


def test_rows_carry_their_selection(peer, tmp_path):
    peer.request("initialize", {"protocol": 1})
    tree = peer.request("view.open", {"view": "demo.files"})["tree"]
    lst = find(tree, lambda n: n.get("id") == "entries")
    by_key = {i["key"]: i for i in lst["items"]}
    assert "sel" not in by_key[".."]
    (sel,) = by_key["a.txt"]["sel"]
    assert sel["type"] == "file" and sel["path"] == str(tmp_path / "a.txt") and sel["label"] == "a.txt"
    assert dict(sel["props"])["Size"]
    assert by_key["sub"]["sel"][0]["type"] == "directory"


def test_views_hear_the_selection(tmp_path):
    d = tmp_path / "plugins" / "follow"
    d.mkdir(parents=True)
    (d / "myrcp.toml").write_text('[plugin]\nid="follow"\nmodule="follow_mod"\n[[views]]\nid="follow.v"\n')
    (d / "follow_mod.py").write_text(
        "from myrcp import Plugin, Selection, View, ui\nplugin = Plugin()\n"
        "@plugin.view('follow.v')\nclass V(View):\n"
        "    seen = Selection()\n"
        "    def on_selection(self, sel):\n        self.seen = sel\n"
        "    def render(self):\n"
        "        files = self.seen.of_type('file')\n"
        "        ctx = self.rcp.selection\n"
        "        return ui.text(f'{self.seen.title}: {[(i.label, i.line, i.prop(\"Size\")) for i in files]} {ctx is self.seen}')\n"
    )
    p = Peer(tmp_path, tmp_path / "plugins")
    try:
        p.request("initialize", {"protocol": 1})
        p.request("view.open", {"view": "follow.v"})
        item = {"type": "file", "label": "a.txt", "path": "/x/a.txt", "line": 3, "props": [["Size", "2 B"]]}
        p.notify("selection.changed", {"view": "demo.files", "title": "Files", "items": [item]})
        update = p.wait_notify("view.update")
        assert update["tree"]["text"] == "Files: [('a.txt', 3, '2 B')] True"
    finally:
        p.close()


def test_views_hear_preferences(tmp_path):
    d = tmp_path / "plugins" / "pref"
    d.mkdir(parents=True)
    (d / "myrcp.toml").write_text(
        '[plugin]\nid="pref"\nmodule="pref_mod"\n[[views]]\nid="pref.v"\n'
        '[[preferences]]\nid="greeting"\ntitle="Greeting"\ndefault="hello"\nenv="PREF_GREETING"\n'
        '[[preferences]]\nid="size"\ntype="choice"\nchoices=["s", "m"]\n'
    )
    (d / "pref_mod.py").write_text(
        "from myrcp import Plugin, View, ui\nplugin = Plugin()\n"
        "@plugin.view('pref.v')\nclass V(View):\n"
        "    heard = frozenset()\n"
        "    def on_preferences(self, changed):\n        self.heard = changed\n"
        "    def render(self):\n"
        "        return ui.text(f'{self.rcp.pref(\"pref.greeting\")} {sorted(self.heard)}')\n"
    )
    p = Peer(tmp_path, tmp_path / "plugins")
    try:
        res = p.request("initialize", {"protocol": 1})
        (plug,) = res["plugins"]
        assert plug["preferences"] == [
            {"id": "pref.greeting", "title": "Greeting", "description": "", "type": "string", "default": "hello",
             "choices": [], "env": "PREF_GREETING"},
            {"id": "pref.size", "title": "size", "description": "", "type": "choice", "default": "s",
             "choices": ["s", "m"], "env": ""},
        ]  # fmt: skip
        p.notify("preferences.changed", {"values": {"pref.greeting": "hi"}})
        p.request("view.open", {"view": "pref.v"})
        p.notify("preferences.changed", {"values": {"pref.greeting": "hey"}})
        update = p.wait_notify("view.update")
        assert update["tree"]["text"] == "hey ['pref.greeting']"
    finally:
        p.close()


def test_tables_sort_on_request(peer, tmp_path):
    (tmp_path / "big.txt").write_text("x" * 5000)
    peer.request("initialize", {"protocol": 1})
    tree = peer.request("view.open", {"view": "demo.table"})["tree"]
    table = find(tree, lambda n: n.get("id") == "entries")
    names = [r["key"] for r in table["rows"]]
    assert names == sorted(names, key=str.lower) and table["sortable"] is True
    peer.notify("view.event", {"view": "demo.table", "widget": "entries", "kind": "sort", "value": [2, True, "sub"]})
    update = peer.wait_notify("view.update")
    table = find(update["tree"], lambda n: n.get("id") == "entries")
    assert table["sorted"] == [2, True]
    assert table["rows"][0]["key"] == "big.txt"  # the biggest first; folders (no size) last
    assert table["rows"][-1]["key"] == "sub"
    assert table["follow"] == [len(table["rows"]) - 1, 1]
