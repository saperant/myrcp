import pytest

from myrcp import Plugin, View, ui
from myrcp.host import ManifestError, parse_manifest


def test_serialize_strips_handlers():
    def on_activate(key):
        return key

    tree = ui.vbox(
        ui.panel("P", ui.listbox("l", ["a", ui.item("b", "B", "detail")], on_activate=on_activate)),
        False,
        None,
        ui.button("go", "Go", on_press=lambda: None),
    )
    wire, handlers = ui.serialize(tree)
    assert [c["t"] for c in wire["children"]] == ["panel", "button"]
    assert "_on" not in wire["children"][0]["child"]
    assert handlers[("l", "activate")] is on_activate
    assert ("go", "press") in handlers


def test_spans():
    node = ui.text(["plain", ("bold", "bold")], "dim")
    assert node["text"] == [["plain", "dim"], ["bold", "bold"]]


def test_serialize_marks_bad_nodes():
    wire, _ = ui.serialize(ui.vbox("not a node"))  # type: ignore[arg-type]
    assert wire["children"][0]["style"] == "error"


def test_view_commands_belong_to_the_view():
    plugin = Plugin()

    @plugin.view("v")
    class V(View):
        def render(self):
            return ui.text("x")

        @plugin.command("v.cmd")
        def cmd(self):
            pass

    @plugin.command("global")
    def g(rcp):
        pass

    assert V._myrcp_commands == {"v.cmd": "cmd"}
    assert set(plugin.commands) == {"global"}


def test_plugin_view_needs_a_view():
    with pytest.raises(TypeError):
        Plugin().view("x")(object)


def test_parse_manifest():
    m = parse_manifest(
        """
[plugin]
id = "p"
module = "p.mod"
[[views]]
id = "p.v"
[[commands]]
id = "p.c"
key = ["a", "ctrl+b"]
view = "p.v"
""",
        "test",
        None,
    )
    assert m.name == "p"
    assert m.views == [{"id": "p.v", "title": "p.v", "startup": False, "region": "main"}]
    assert m.commands[0]["keys"] == ["a", "ctrl+b"]
    with pytest.raises(ManifestError):
        parse_manifest("[plugin]\nid='x'\n", "test", None)
    with pytest.raises(ManifestError):
        parse_manifest("not toml ===", "test", None)


def test_parse_perspectives():
    head = '[plugin]\nid = "p"\nmodule = "m"\n'
    m = parse_manifest(
        head + '[[perspectives]]\nid = "p"\nmain = ["p.v", "p.w"]\nside = "properties"\nside_size = 40\n', "test", None
    )
    assert m.perspectives == [
        {"id": "p", "title": "p", "views": [["p.v", "main"], ["p.w", "main"], ["properties", "side"]], "side_size": 40}
    ]
    with pytest.raises(ManifestError, match="no views"):
        parse_manifest(head + '[[perspectives]]\nid = "p"\n', "test", None)
    with pytest.raises(ManifestError, match="15 to 75"):
        parse_manifest(head + '[[perspectives]]\nid = "p"\nmain = ["p.v"]\nbottom_size = 90\n', "test", None)


def test_parse_preferences():
    head = '[plugin]\nid = "p"\nmodule = "m"\n'
    m = parse_manifest(head + '[[preferences]]\nid = "dirs"\ntype = "paths"\nenv = "P_DIRS"\n', "test", None)
    assert m.preferences == [
        {
            "id": "p.dirs",
            "title": "dirs",
            "description": "",
            "type": "paths",
            "default": [],
            "choices": [],
            "env": "P_DIRS",
        }
    ]
    for bad, why in [
        ('id = "a.b"', "an id of"),
        ('id = "x"\ntype = "float"', "type is one of"),
        ('id = "x"\ntype = "choice"', "needs choices"),
        ('id = "x"\ntype = "int"\ndefault = true', "does not fit the type int"),
        ('id = "x"\ntype = "choice"\nchoices = ["a"]\ndefault = "b"', "does not fit the type choice"),
    ]:
        with pytest.raises(ManifestError, match=why):
            parse_manifest(head + "[[preferences]]\n" + bad + "\n", "test", None)


def test_sort_key_is_natural():
    texts = ["10 kB", "9 kB", "", "2 MB", "512 B", "1.5K", "file10", "File9", "file9b"]
    assert sorted(texts, key=ui.sort_key) == ["512 B", "1.5K", "9 kB", "10 kB", "2 MB", "File9", "file9b", "file10", ""]


def test_sortable_tables_sort_window_and_follow():
    rows = [ui.row(k, [name, size]) for k, (name, size) in enumerate([("b", "2"), ("a", "10"), ("c", "")])]
    cur = ui.CursorRequest()
    cur.move(1)  # the plugin's row 1: "a"

    def tree(**kw):
        return ui.table("t", [ui.column("Name"), ui.column("Size")], ui.lazy_rows(3, rows.__getitem__),
                        cursor=cur, sortable=True, **kw)  # fmt: skip

    wire, _ = ui.serialize(tree())
    assert wire["sortable"] is True and "sorted" not in wire
    assert [r["key"] for r in wire["rows"]] == [0, 1, 2]

    sorts = {"t": ui.SortState(1, True, key=0, nonce=3)}  # by size, descending; the cursor was on "b"
    wire, _ = ui.serialize(tree(), sorts=sorts)
    assert wire["sorted"] == [1, True]
    assert [r["key"] for r in wire["rows"]] == [1, 0, 2]  # 10, 2, then the empty one
    assert wire["follow"] == [1, 3]  # "b" is now the second row
    assert wire["cursor"] == [0, cur.seq]  # the plugin's "a" is the first

    wire, _ = ui.serialize(tree(), {"t": (1, 1)}, sorts)  # the window cuts the sorted rows
    assert [r["key"] for r in wire["rows"]] == [0] and wire["offset"] == 1 and wire["total"] == 3

    heard = []
    wire, handlers = ui.serialize(tree(on_sort=lambda c, d: heard.append((c, d))), sorts=sorts)
    assert [r["key"] for r in wire["rows"]] == [0, 1, 2]  # it sorts itself: as given
    assert wire["sorted"] == [1, True] and ("t", "sort") in handlers
