"""A small msgpack codec, so the SDK installs into any venv without dependencies."""

from __future__ import annotations

import struct
from collections.abc import Iterator
from typing import Any, NamedTuple

__all__ = ["DecodeError", "Ext", "Unpacker", "packb", "unpackb"]

_MAX_DEPTH = 64


class Ext(NamedTuple):
    code: int
    data: bytes


class DecodeError(ValueError):
    pass


class _Incomplete(Exception):
    pass


def packb(obj: Any) -> bytes:
    out = bytearray()
    _pack(obj, out, 0)
    return bytes(out)


def _header(out: bytearray, n: int, fix: int | None, fix_limit: int, c8: int | None, c16: int, c32: int) -> None:
    if fix is not None and n < fix_limit:
        out.append(fix | n)
    elif c8 is not None and n <= 0xFF:
        out += bytes((c8, n))
    elif n <= 0xFFFF:
        out.append(c16)
        out += struct.pack(">H", n)
    else:
        out.append(c32)
        out += struct.pack(">I", n)


def _pack(o: Any, out: bytearray, depth: int) -> None:
    if depth > _MAX_DEPTH:
        raise ValueError("msgpack: nesting too deep")
    if o is None:
        out.append(0xC0)
    elif o is True:
        out.append(0xC3)
    elif o is False:
        out.append(0xC2)
    elif isinstance(o, int):
        if 0 <= o < 0x80:
            out.append(o)
        elif -32 <= o < 0:
            out.append(o & 0xFF)
        elif 0 <= o <= 0xFF:
            out += bytes((0xCC, o))
        elif 0 <= o <= 0xFFFF:
            out.append(0xCD)
            out += struct.pack(">H", o)
        elif 0 <= o <= 0xFFFFFFFF:
            out.append(0xCE)
            out += struct.pack(">I", o)
        elif 0 <= o <= 0xFFFFFFFFFFFFFFFF:
            out.append(0xCF)
            out += struct.pack(">Q", o)
        elif -0x80 <= o:
            out.append(0xD0)
            out += struct.pack(">b", o)
        elif -0x8000 <= o:
            out.append(0xD1)
            out += struct.pack(">h", o)
        elif -0x80000000 <= o:
            out.append(0xD2)
            out += struct.pack(">i", o)
        elif -0x8000000000000000 <= o:
            out.append(0xD3)
            out += struct.pack(">q", o)
        else:
            raise OverflowError("msgpack: integer out of range")
    elif isinstance(o, float):
        out.append(0xCB)
        out += struct.pack(">d", o)
    elif isinstance(o, str):
        data = o.encode("utf-8", "surrogateescape")
        _header(out, len(data), 0xA0, 32, 0xD9, 0xDA, 0xDB)
        out += data
    elif isinstance(o, (bytes, bytearray, memoryview)):
        data = bytes(o)
        _header(out, len(data), None, 0, 0xC4, 0xC5, 0xC6)
        out += data
    elif isinstance(o, Ext):
        n = len(o.data)
        fixed = {1: 0xD4, 2: 0xD5, 4: 0xD6, 8: 0xD7, 16: 0xD8}
        if n in fixed:
            out.append(fixed[n])
        else:
            _header(out, n, None, 0, 0xC7, 0xC8, 0xC9)
        out += struct.pack(">b", o.code)
        out += o.data
    elif isinstance(o, (list, tuple)):
        _header(out, len(o), 0x90, 16, None, 0xDC, 0xDD)
        for e in o:
            _pack(e, out, depth + 1)
    elif isinstance(o, dict):
        _header(out, len(o), 0x80, 16, None, 0xDE, 0xDF)
        for k, v in o.items():
            _pack(k, out, depth + 1)
            _pack(v, out, depth + 1)
    else:
        raise TypeError(f"msgpack: cannot encode {type(o).__name__}")


class _Reader:
    __slots__ = ("buf", "pos")

    def __init__(self, buf: bytes | bytearray, pos: int) -> None:
        self.buf = buf
        self.pos = pos

    def take(self, n: int) -> bytes:
        end = self.pos + n
        if end > len(self.buf):
            raise _Incomplete
        b = bytes(self.buf[self.pos : end])
        self.pos = end
        return b

    def u(self, fmt: str, n: int) -> int:
        return struct.unpack(fmt, self.take(n))[0]

    def value(self, depth: int = 0) -> Any:
        if depth > _MAX_DEPTH:
            raise DecodeError("msgpack: nesting too deep")
        c = self.take(1)[0]
        if c <= 0x7F:
            return c
        if c >= 0xE0:
            return c - 0x100
        if c & 0xF0 == 0x80:
            return self.map(c & 0x0F, depth)
        if c & 0xF0 == 0x90:
            return self.array(c & 0x0F, depth)
        if c & 0xE0 == 0xA0:
            return self.str(c & 0x1F)
        if c == 0xC0:
            return None
        if c == 0xC2:
            return False
        if c == 0xC3:
            return True
        if c in (0xC4, 0xC5, 0xC6):
            return self.take(self.u(*_LEN[c]))
        if c in (0xC7, 0xC8, 0xC9):
            n = self.u(*_LEN[c])
            code = self.u(">b", 1)
            return Ext(code, self.take(n))
        if c == 0xCA:
            return self.u(">f", 4)
        if c == 0xCB:
            return self.u(">d", 8)
        if c in _INT:
            return self.u(*_INT[c])
        if 0xD4 <= c <= 0xD8:
            code = self.u(">b", 1)
            return Ext(code, self.take(1 << (c - 0xD4)))
        if c in (0xD9, 0xDA, 0xDB):
            return self.str(self.u(*_LEN[c]))
        if c in (0xDC, 0xDD):
            return self.array(self.u(*_LEN[c]), depth)
        if c in (0xDE, 0xDF):
            return self.map(self.u(*_LEN[c]), depth)
        raise DecodeError(f"msgpack: invalid type byte 0x{c:02x}")

    def str(self, n: int) -> str:
        return self.take(n).decode("utf-8", "surrogateescape")

    def array(self, n: int, depth: int) -> list[Any]:
        return [self.value(depth + 1) for _ in range(n)]

    def map(self, n: int, depth: int) -> dict[Any, Any]:
        out: dict[Any, Any] = {}
        for _ in range(n):
            k = self.value(depth + 1)
            out[k] = self.value(depth + 1)
        return out


_LEN = {
    0xC4: (">B", 1), 0xC5: (">H", 2), 0xC6: (">I", 4),
    0xC7: (">B", 1), 0xC8: (">H", 2), 0xC9: (">I", 4),
    0xD9: (">B", 1), 0xDA: (">H", 2), 0xDB: (">I", 4),
    0xDC: (">H", 2), 0xDD: (">I", 4), 0xDE: (">H", 2), 0xDF: (">I", 4),
}  # fmt: skip
_INT = {
    0xCC: (">B", 1), 0xCD: (">H", 2), 0xCE: (">I", 4), 0xCF: (">Q", 8),
    0xD0: (">b", 1), 0xD1: (">h", 2), 0xD2: (">i", 4), 0xD3: (">q", 8),
}  # fmt: skip


def unpackb(data: bytes) -> Any:
    r = _Reader(data, 0)
    try:
        v = r.value()
    except _Incomplete:
        raise DecodeError("msgpack: truncated data") from None
    if r.pos != len(data):
        raise DecodeError("msgpack: extra data")
    return v


class Unpacker:
    """Feed bytes as they arrive; iterate to get the complete values."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> None:
        self._buf += data

    def __iter__(self) -> Iterator[Any]:
        pos = 0
        try:
            while pos < len(self._buf):
                r = _Reader(self._buf, pos)
                try:
                    v = r.value()
                except _Incomplete:
                    break
                pos = r.pos
                yield v
        finally:
            del self._buf[:pos]
