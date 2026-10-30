#!/usr/bin/env python3
"""Minimal async client for the netiso protocol, for tests and the diff harness.

Only what the tests need: connect, drive the commands from
``docs/PS3NETSRV-PROTOCOL.md`` §3, and hand back decoded fields. Paths and names
travel as ``bytes`` so non-UTF-8 names survive the round trip.

Used by ``netsrv_test.py`` (against our own server) and ``netsrv_diff.py``
(against upstream's ``ps3netsrv++`` and ours, back to back).
"""
from __future__ import annotations

import asyncio
import struct

OPEN_RESULT = struct.Struct(">qq")
INT32 = struct.Struct(">i")
INT64 = struct.Struct(">q")
READ_FILE_RESULT = struct.Struct(">i")
ENTRY_V1 = struct.Struct(">qhb")
ENTRY_V2 = struct.Struct(">qqqqhb")
STAT_RESULT = struct.Struct(">qqqqb")
DIR_RECORD = struct.Struct(">qqb512s")

OPEN_FILE = 0x1224
READ_FILE_CRITICAL = 0x1225
READ_CD_2048_CRITICAL = 0x1226
READ_FILE = 0x1227
CREATE_FILE = 0x1228
WRITE_FILE = 0x1229
OPEN_DIR = 0x122A
READ_DIR_ENTRY = 0x122B
DELETE_FILE = 0x122C
MKDIR = 0x122D
RMDIR = 0x122E
READ_DIR_ENTRY_V2 = 0x122F
STAT_FILE = 0x1230
GET_DIR_SIZE = 0x1231
READ_DIR = 0x1232


class ConnectionClosed(Exception):
    """The server hung up: an unknown opcode, or a fatal error on its side."""


def path_trailer(opcode: int, path: bytes) -> bytes:
    if len(path) > 0xFFFF:
        raise ValueError("path too long for a uint16 length")
    return struct.pack(">HH", opcode, len(path)) + b"\0" * 12 + path


def pad14(opcode: int) -> bytes:
    return struct.pack(">H", opcode) + b"\0" * 14


class Netiso:
    """One netiso connection. Sequential by design: one answer per command."""

    def __init__(self, host: str = "127.0.0.1", port: int = 38008,
                 timeout: float = 15.0, local_addr: tuple[str, int] | None = None):
        self.host, self.port, self.timeout = host, port, timeout
        self.local_addr = local_addr
        self.reader = None
        self.writer = None

    async def __aenter__(self):
        await self.connect()
        return self

    async def __aexit__(self, *exc):
        self.close()

    async def connect(self):
        self.reader, self.writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port,
                                    local_addr=self.local_addr), self.timeout)
        sock = self.writer.get_extra_info("socket")
        if sock is not None:
            sock.setsockopt(__import__("socket").IPPROTO_TCP,
                            __import__("socket").TCP_NODELAY, 1)
        return self

    def close(self):
        if self.writer is not None:
            try:
                self.writer.close()
            except (ConnectionError, OSError):
                pass
            self.writer = None
            self.reader = None

    # -- transport ---------------------------------------------------------

    async def _request(self, payload: bytes) -> None:
        self.writer.write(payload)
        await self.writer.drain()

    async def _read(self, count: int) -> bytes:
        try:
            return await asyncio.wait_for(self.reader.readexactly(count),
                                          self.timeout)
        except (asyncio.IncompleteReadError, ConnectionError, OSError) as exc:
            raise ConnectionClosed(str(exc) or "connection closed") from exc

    async def _int32(self) -> int:
        return INT32.unpack(await self._read(4))[0]

    # -- commands ----------------------------------------------------------

    async def open_file(self, path: bytes) -> tuple[int, int]:
        await self._request(path_trailer(OPEN_FILE, path))
        return OPEN_RESULT.unpack(await self._read(16))

    async def close_file(self) -> tuple[int, int]:
        return await self.open_file(b"/CLOSEFILE")

    async def read_file_critical(self, offset: int, num_bytes: int) -> bytes:
        self.request_bulk_read(offset, num_bytes)
        return await self._read(num_bytes)

    def request_bulk_read(self, offset: int, num_bytes: int) -> None:
        """Send a READ_FILE_CRITICAL without waiting for the answer."""
        self.writer.write(struct.pack(">HHIQ", READ_FILE_CRITICAL, 0,
                                      num_bytes, offset))

    async def drain_bulk(self, count: int) -> int:
        """Take *count* bytes and throw them away, counting only.

        A benchmark client must not become the bottleneck: ``readexactly``
        refills a 64 KiB buffer one read at a time, which caps what the loopback
        measurement can show. This drains in 1 MiB ``recv`` calls, the way a
        client that is going to write the bytes to disk would.
        """
        await self.writer.drain()
        got = 0
        while got < count:
            chunk = await self.reader.read(min(1 << 20, count - got))
            if not chunk:
                raise ConnectionClosed("connection closed mid-bulk-read")
            got += len(chunk)
        return got

    async def read_cd_2048(self, start_sector: int, sector_count: int) -> bytes:
        await self._request(struct.pack(">HHIII", READ_CD_2048_CRITICAL, 0,
                                        start_sector, sector_count, 0))
        return await self._read(sector_count * 2048)

    async def read_file(self, offset: int, num_bytes: int) -> bytes:
        count, data = await self.read_file_status(offset, num_bytes)
        if count < 0:
            raise AssertionError(f"READ_FILE reported {count}")
        return data

    async def read_file_status(self, offset: int, num_bytes: int):
        """READ_FILE answers a count, so a failure is reportable (§3.4)."""
        await self._request(struct.pack(">HHIQ", READ_FILE, 0, num_bytes, offset))
        (count,) = READ_FILE_RESULT.unpack(await self._read(4))
        if count <= 0:
            return count, b""
        return count, await self._read(count)

    async def create_file(self, path: bytes) -> int:
        await self._request(path_trailer(CREATE_FILE, path))
        return await self._int32()

    async def write_file(self, data: bytes) -> int:
        await self._request(struct.pack(">HHI", WRITE_FILE, 0, len(data))
                            + b"\0" * 8 + data)
        return await self._int32()

    async def delete_file(self, path: bytes) -> int:
        await self._request(path_trailer(DELETE_FILE, path))
        return await self._int32()

    async def mkdir(self, path: bytes) -> int:
        await self._request(path_trailer(MKDIR, path))
        return await self._int32()

    async def rmdir(self, path: bytes) -> int:
        await self._request(path_trailer(RMDIR, path))
        return await self._int32()

    async def open_dir(self, path: bytes) -> int:
        await self._request(path_trailer(OPEN_DIR, path))
        return await self._int32()

    async def _entry(self, opcode: int, fmt: struct.Struct, fixed: int):
        await self._request(pad14(opcode))
        head = fmt.unpack(await self._read(fixed))
        size = head[0]
        if size == -1:
            return None
        fn_len = head[-2]
        name = await self._read(fn_len) if fn_len else b""
        return head + (name,)

    async def read_dir_entry(self):
        """(file_size, fn_len, is_directory, name) or None at the end."""
        return await self._entry(READ_DIR_ENTRY, ENTRY_V1, ENTRY_V1.size)

    async def read_dir_entry_v2(self):
        """(file_size, mtime, ctime, atime, fn_len, is_directory, name)."""
        return await self._entry(READ_DIR_ENTRY_V2, ENTRY_V2, ENTRY_V2.size)

    async def read_dir(self) -> list[dict]:
        await self._request(pad14(READ_DIR))
        (count,) = INT64.unpack(await self._read(8))
        out = []
        for _ in range(max(count, 0)):
            size, mtime, is_dir, name = DIR_RECORD.unpack(await self._read(DIR_RECORD.size))
            out.append({"size": size, "mtime": mtime, "is_dir": is_dir,
                        "name": name.split(b"\0", 1)[0]})
        return out

    async def stat(self, path: bytes) -> dict:
        await self._request(path_trailer(STAT_FILE, path))
        size, mtime, ctime, atime, is_dir = STAT_RESULT.unpack(await self._read(33))
        return {"size": size, "mtime": mtime, "ctime": ctime, "atime": atime,
                "is_dir": is_dir}

    async def get_dir_size(self, path: bytes) -> int:
        await self._request(path_trailer(GET_DIR_SIZE, path))
        return INT64.unpack(await self._read(8))[0]

    async def zero_length(self, opcode: int):
        """Send a command that announces a zero-length trailer, read its reply.

        Upstream treats ``fp_len == 0`` differently per command: ``OPEN_FILE``
        answers, the rest block forever waiting for a trailer of no bytes.
        """
        await self._request(struct.pack(">HH", opcode, 0) + b"\0" * 12)
        fixed = {OPEN_FILE: 16, STAT_FILE: 33, GET_DIR_SIZE: 8}.get(opcode, 4)
        return await self._read(fixed)

    async def raw(self, payload: bytes) -> None:
        """Send an exact request, for the odd cases the helpers do not cover."""
        await self._request(payload)

    async def read_raw(self, count: int) -> bytes:
        return await self._read(count)

    def header(self, opcode: int, data: bytes = b"") -> bytes:
        """A bare 16-byte header, padded, with no trailer."""
        return struct.pack(">H", opcode) + (data + b"\0" * 14)[:14]

    async def expect_closed(self, payload: bytes) -> None:
        await self._request(payload)
        try:
            await self._read(16)
        except ConnectionClosed:
            return
        raise AssertionError("expected the server to close the connection")

    async def collect_entries(self) -> list:
        """Drain READ_DIR_ENTRY until the sentinel."""
        out = []
        while True:
            entry = await self.read_dir_entry()
            if entry is None:
                return out
            out.append(entry)
