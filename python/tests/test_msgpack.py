import pytest

from myrcp._msgpack import DecodeError, Ext, Unpacker, packb, unpackb

VALUES = [
    None, True, False, 0, 1, 127, 128, 255, 256, 65535, 65536, 2**32, 2**64 - 1,
    -1, -32, -33, -128, -129, -32768, -32769, -(2**31), -(2**63),
    1.5, -0.25, "", "hä", "x" * 31, "x" * 32, "y" * 300, "z" * 70000,
    b"", b"\x00\x01", [], [1, [2, [3]]], {}, {"t": "text", "children": [{"a": 1}]},
    Ext(1, b"ab"), Ext(-1, b"\x00" * 16), Ext(5, b"abc"),
]  # fmt: skip


@pytest.mark.parametrize("value", VALUES)
def test_roundtrip(value):
    assert unpackb(packb(value)) == value


def test_tuples_pack_as_arrays():
    assert unpackb(packb((1, "a"))) == [1, "a"]


def test_known_encodings_match_the_core():
    # The same bytes core/tests/tests.cpp expects.
    assert packb([1, "a"]) == b"\x92\x01\xa1a"
    assert packb(-1) == b"\xff"


def test_unpacker_streams_byte_by_byte():
    data = packb([0, 1, "initialize", [{"protocol": 1}]]) + packb("second")
    u = Unpacker()
    got = []
    for i in range(len(data)):
        u.feed(data[i : i + 1])
        got.extend(u)
    assert got == [[0, 1, "initialize", [{"protocol": 1}]], "second"]


def test_errors():
    with pytest.raises(DecodeError):
        unpackb(b"\xc1")
    with pytest.raises(DecodeError):
        unpackb(b"\x92\x01")
    with pytest.raises(TypeError):
        packb(object())
