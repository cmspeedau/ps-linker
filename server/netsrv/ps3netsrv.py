#!/usr/bin/env python3
"""PS Linker ps3netsrv server — PS3 game streaming over TCP 38008.

Speaks the netiso protocol documented in ``docs/PS3NETSRV-PROTOCOL.md`` so
webMAN MOD (and friends) can stream games out of the same library directory the
HTTP share and the Store use.

Read-only by default: the write commands (``CREATE_FILE``, ``WRITE_FILE``,
``DELETE_FILE``, ``MKDIR``, ``RMDIR``) are refused unless ``--write`` is given.
Directory listing works either way — upstream's ``-DREAD_ONLY`` build drops it,
which is an upstream bug we do not copy (§9.1 of the protocol doc).

Usage:
    ps3netsrv.py <games_dir> [port] [whitelist] [--write]

    whitelist   IPv4 pattern with ``*`` octets, e.g. ``192.168.1.*``
"""
import argparse
import asyncio
import ipaddress
import os
import signal
import socket
import stat as statmod
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

DEFAULT_PORT = 38008
BUFFER_SIZE = 4 * 1048576
MAX_CLIENTS = 5
MAX_PATH_LEN = 510
MAX_FILE_LEN = 255
MAX_ENTRIES = 4096
NAME_FIELD = 512
CD_SECTOR_DEFAULT = 2352
PROBE_MIN = 0x200000
PROBE_MAX = 0x35000000
CD_CANDIDATES = (2352, 2048, 2336, 2448, 2328, 2368, 2340)
CHUNK = 1024 * 1024
SNDBUF = 512 * 1024
# False reproduces upstream's raw readdir order instead of sorting by name.
LISTING_SORTED = True

# Wire structs, all big-endian (§2.1).
OPEN_RESULT = struct.Struct(">qq")
INT32 = struct.Struct(">i")
INT64 = struct.Struct(">q")
READ_FILE_RESULT = struct.Struct(">i")
ENTRY_V1 = struct.Struct(">qhb")
ENTRY_V2 = struct.Struct(">qqqqhb")
STAT_RESULT = struct.Struct(">qqqqb")
# One READ_DIR record: 8 + 8 + 1 + 512 = 529 bytes, the name NUL-terminated and
# zero padded, sent as one block (§3.15). ``s`` pads short values with NULs.
DIR_RECORD = struct.Struct(">qqb512s")

CLOSEFILE = b"/CLOSEFILE"
QUIET_EXT = (".PNG", ".JPG", ".png", ".jpg", ".SFO")

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

WRITE_OPS = {CREATE_FILE, WRITE_FILE, DELETE_FILE, MKDIR, RMDIR}
NAMES = {
    OPEN_FILE: "OPEN_FILE", READ_FILE_CRITICAL: "READ_FILE_CRITICAL",
    READ_CD_2048_CRITICAL: "READ_CD_2048_CRITICAL", READ_FILE: "READ_FILE",
    CREATE_FILE: "CREATE_FILE", WRITE_FILE: "WRITE_FILE", OPEN_DIR: "OPEN_DIR",
    READ_DIR_ENTRY: "READ_DIR_ENTRY", DELETE_FILE: "DELETE_FILE",
    MKDIR: "MKDIR", RMDIR: "RMDIR", READ_DIR_ENTRY_V2: "READ_DIR_ENTRY_V2",
    STAT_FILE: "STAT_FILE", GET_DIR_SIZE: "GET_DIR_SIZE", READ_DIR: "READ_DIR",
}


class Closed(Exception):
    """End the connection quietly, like upstream's FAILED from client_thread."""


class Abort(Exception):
    """Protocol violation worth a line in the log before disconnecting."""


def log(msg: str) -> None:
    print(f"[netsrv] {msg}", flush=True)


def b(raw: bytes) -> str:
    """Show raw path bytes without losing the unprintable ones."""
    return raw.decode("utf8", "backslashreplace")


# --------------------------------------------------------------------------
# Paths (§7.1)
# --------------------------------------------------------------------------

def decode_path(raw: bytes) -> str:
    """Wire bytes -> text, keeping undecodable bytes round-trippable (§7.5)."""
    return raw.decode("utf8", "surrogateescape")


def encode_path(text: str) -> bytes:
    return text.encode("utf8", "surrogateescape")


def normalize_trailer(raw: bytes) -> bytes:
    """What upstream's normalize_path does to a received path: ``\\`` -> ``/``,
    drop a single trailing CR. Interior CRs stay."""
    out = raw.replace(b"\\", b"/")
    return out[:-1] if out.endswith(b"\r") else out


def path_parts(raw: bytes) -> list[bytes] | None:
    """Split a wire path into components, or None if it must be refused.

    Upstream checks only the first ``/..`` and never collapses ``//``, so
    ``a/../../b`` slips past it while ``a//../b`` gets a legitimate path
    refused. Rejecting any ``..`` component catches the first and stops
    punishing the second (§9.6).
    """
    text = decode_path(normalize_trailer(raw))
    if not text.startswith("/"):
        return None
    parts = []
    for part in text.split("/")[1:]:
        if part in ("", "."):
            continue
        if part == "..":
            return None
        parts.append(encode_path(part))
    return parts


def virtual_iso(raw: bytes) -> bool:
    """``/***PS3***/`` or ``/***DVD***/``: upstream builds an ISO on the fly.

    We do not implement virtual ISOs (§9.2), but a client must not get a
    confusing result, so these are reported as not-found.
    """
    text = decode_path(normalize_trailer(raw))
    return "/***PS3***" in text or "/***DVD***" in text


class Share:
    """The exported directory tree plus the sandbox rules that come with it."""

    def __init__(self, root: str | Path):
        p = Path(root)
        if not p.is_dir():
            raise SystemExit(f"[netsrv] root directory not found: {root}")
        if p.is_symlink():
            # Refusing a symlinked root keeps the "nothing leaves the share"
            # promise obvious instead of depending on every open being careful.
            raise SystemExit(f"[netsrv] root must not be a symlink: {root}")
        self.root = p.absolute()
        if str(self.root) == "/":
            raise SystemExit("[netsrv] / can't be specified as root directory!")

    def display(self, path: Path) -> str:
        """The path as a client named it, for logs."""
        try:
            rel = str(path.relative_to(self.root))
        except ValueError:
            return str(path)
        return "/" + rel if rel != "." else "/"

    def join(self, parts: list[bytes]) -> Path:
        path = self.root
        for part in parts:
            path = path / decode_path(part)
        return path

    def open_ro(self, path: Path):
        """Open a regular file for reading, or return None.

        Only regular files: a directory or a device must not become the
        connection's read handle (upstream happily opens a directory and quotes
        its inode size as a file size, §3.1), and a FIFO would block this task
        forever on the first read. Symlinks are refused outright (§9.7).
        """
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
        fd = None
        try:
            fd = os.open(path, flags)
            if not statmod.S_ISREG(os.fstat(fd).st_mode):
                return None
            fh, fd = os.fdopen(fd, "rb", closefd=True), None
            return fh
        except OSError:
            if fd is not None:
                os.close(fd)
            return None

    def lstat(self, path: Path) -> os.stat_result | None:
        try:
            return path.stat(follow_symlinks=False)
        except OSError:
            return None

    @staticmethod
    def is_dir(st: os.stat_result) -> bool:
        return statmod.S_ISDIR(st.st_mode)

    @staticmethod
    def size(st: os.stat_result) -> int:
        return 0 if statmod.S_ISDIR(st.st_mode) else st.st_size

    @staticmethod
    def mtime(st: os.stat_result) -> int:
        """mtime, falling back to ctime then atime, as upstream does (§3.15)."""
        return int(st.st_mtime) or int(st.st_ctime) or int(st.st_atime)

    def entries(self, path: Path, sort: bool | None = None
                ) -> list[os.DirEntry] | None:
        """Directory contents, optionally in name order.

        Upstream returns raw readdir order, which is not sorted and varies with
        the filesystem. Sorting is friendlier and the client sorts the list
        itself anyway, but it does change what a client shows, so
        ``LISTING_SORTED = False`` restores upstream's order (§9.3). Read at
        call time so ``netsrv_diff.py`` can compare the wire in upstream's
        order without editing the module.
        """
        try:
            items = list(os.scandir(path))
        except OSError:
            return None
        if sort is None:
            sort = LISTING_SORTED
        if sort:
            items.sort(key=lambda e: e.name)
        return items


def listable(name: str) -> bool:
    """``.``/``..`` are hidden and over-long names are skipped, silently (§3.8)."""
    return name not in (".", "..") and len(encode_path(name)) <= MAX_FILE_LEN


def read_exact(fh, offset: int, size: int) -> bytes | None:
    """Read exactly *size* bytes at *offset*; None if the file is too short."""
    try:
        fh.seek(offset)
    except OSError:
        return None
    buf = bytearray()
    while len(buf) < size:
        try:
            chunk = fh.read(size - len(buf))
        except OSError:
            return None
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


def pread_exact(fd: int, offset: int, size: int) -> bytes | None:
    """Read exactly *size* bytes at *offset* with one syscall per read.

    ``pread`` takes the position as an argument, so a thread can read the same
    file as the rest of the connection without touching (or racing on) the file
    object's position, and there is no separate seek. The usual case — one read
    that returns the whole request — is returned as the kernel gave it, without
    the accumulate-and-recopy path that a partial read needs: at these sizes that
    extra copy is worth a couple of hundred MiB/s.
    """
    try:
        first = os.pread(fd, size, offset)
        if len(first) == size:
            return first
        if not first:
            return None
        buf = bytearray(first)
        while len(buf) < size:
            chunk = os.pread(fd, size - len(buf), offset + len(buf))
            if not chunk:
                return None
            buf += chunk
    except OSError:
        return None
    return bytes(buf)


def read_upto(fh, offset: int, size: int) -> bytes | None:
    """Read up to *size* bytes at *offset*. None on error, b'' at EOF (§3.4)."""
    try:
        fh.seek(offset)
    except OSError:
        return None
    try:
        data = fh.read(size)
    except OSError:
        return None
    return b"" if data is None else data


def detect_sector_size(fh, file_size: int) -> int:
    """Which raw sector size this image uses (§5).

    Upstream probes one offset per candidate out of a shared 4 MiB buffer and
    compares windows that partly lie outside what it just fetched, so its answer
    depends on stale buffer content and it misses plain 2048-byte images. Both
    plausible offsets are probed here, deterministically.
    """
    if not PROBE_MIN <= file_size <= PROBE_MAX:
        return CD_SECTOR_DEFAULT
    for sector in CD_CANDIDATES:
        base = sector * 16
        if base + sector > file_size:
            continue
        for offset in (base, base + 24):
            if read_exact(fh, offset, 1) == b"\x01" \
                    and read_exact(fh, offset + 1, 5) == b"CD001":
                return sector
            if read_exact(fh, offset + 8, 12) == b"PLAYSTATION ":
                return sector
    return CD_SECTOR_DEFAULT


def walk_size(root: Path) -> int:
    """Recursive sum of regular-file sizes (§3.14): one failed stat anywhere is
    -1 for the whole call, over-long names are skipped, symlinks add nothing."""
    total = 0
    stack = [root]
    while stack:
        try:
            entries = list(os.scandir(stack.pop()))
        except OSError:
            return -1
        for entry in entries:
            if not listable(entry.name):
                continue
            try:
                st = entry.stat(follow_symlinks=False)
            except OSError:
                return -1
            if statmod.S_ISDIR(st.st_mode):
                stack.append(Path(entry.path))
            elif statmod.S_ISREG(st.st_mode):
                total += st.st_size
    return total


def dir_record(size: int, mtime: int, is_dir: int, name: bytes) -> bytes:
    """One 529-byte READ_DIR record: name, NUL, then zero padding (§3.15)."""
    assert len(name) < NAME_FIELD, "names that cannot fit are dropped upstream"
    return DIR_RECORD.pack(size, mtime, is_dir, name)


# --------------------------------------------------------------------------
# Per-connection state
# --------------------------------------------------------------------------

@dataclass
class DirState:
    path: Path
    entries: list
    index: int = 0
    subdirs: int = 0


class Client:
    def __init__(self, server: "NetisoServer", reader: asyncio.StreamReader,
                 writer: asyncio.StreamWriter):
        self.server = server
        self.share = server.share
        self.reader = reader
        self.writer = writer
        self.ro_file = None
        self.ro_fd = None
        self.wo_file = None
        self.cd_sector = CD_SECTOR_DEFAULT
        self.dir: DirState | None = None

    # -- wire helpers ------------------------------------------------------

    async def recv_header(self) -> bytes:
        try:
            return await self.reader.readexactly(16)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            raise Closed

    async def recv_path(self, length: int) -> bytes:
        if length > MAX_PATH_LEN:
            # Upstream mallocs whatever the client announces, which makes a
            # 65535-byte trailer a cheap memory-dot. Refuse instead (§7.3).
            raise Abort(f"path length {length} exceeds {MAX_PATH_LEN}")
        try:
            return await self.reader.readexactly(length)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            raise Closed

    async def send(self, payload: bytes) -> None:
        try:
            self.writer.write(payload)
            await self.writer.drain()
        except (ConnectionError, OSError):
            raise Closed

    # -- path helpers -------------------------------------------------------

    def resolve(self, raw: bytes) -> Path:
        parts = path_parts(raw)
        if parts is None:
            # Upstream aborts the connection on an untranslatable path.
            raise Abort(f"untranslatable path {b(raw)!r}")
        return self.share.join(parts)

    # -- directory helpers ---------------------------------------------------

    def close_dir(self) -> None:
        self.dir = None

    def next_entry(self) -> os.DirEntry | None:
        """Next listable entry of the open directory; auto-closes at the end."""
        state = self.dir
        if state is None:
            return None
        while state.index < len(state.entries):
            entry = state.entries[state.index]
            state.index += 1
            if listable(entry.name):
                return entry
        self.close_dir()
        return None

    # -- dispatcher ---------------------------------------------------------

    async def handle(self, head: bytes) -> None:
        (opcode,) = struct.unpack_from(">H", head)
        if opcode in WRITE_OPS and not self.server.writable:
            raise Abort(f"{NAMES[opcode]} refused: server is read-only "
                        "(start with --write to allow it)")
        handler = getattr(self, f"cmd_{opcode:04x}", None)
        if handler is None:
            raise Abort(f"unknown command {opcode:#06x}")
        await handler(head)

    # -- 0x1224 OPEN_FILE ----------------------------------------------------

    async def cmd_1224(self, head: bytes) -> None:
        (fp_len,) = struct.unpack_from(">H", head, 2)
        self.close_ro()
        if fp_len == 0:
            await self.send(OPEN_RESULT.pack(-1, 0))
            return
        raw = normalize_trailer(await self.recv_path(fp_len))
        if raw == CLOSEFILE:
            await self.send(OPEN_RESULT.pack(-1, 0))
            return
        if virtual_iso(raw):
            await self.send(OPEN_RESULT.pack(-1, 0))
            return
        path = self.resolve(raw)
        fh = self.share.open_ro(path)
        st = None if fh is None else self.share.lstat(path)
        if fh is None or st is None:
            if fh is not None:
                fh.close()
            await self.send(OPEN_RESULT.pack(-1, 0))
            return
        self.ro_file, self.ro_fd, self.cd_sector = fh, fh.fileno(), CD_SECTOR_DEFAULT
        shown = self.share.display(path)
        if not shown.endswith(QUIET_EXT) and st.st_size > 0x400000:
            log(f"open {shown}")
        sector = await asyncio.to_thread(detect_sector_size, fh, st.st_size)
        if sector != CD_SECTOR_DEFAULT:
            self.cd_sector = sector
            log(f"CD sector size: {sector}  ({shown})")
        await self.send(OPEN_RESULT.pack(st.st_size, int(st.st_mtime)))

    def close_ro(self) -> None:
        if self.ro_file is not None:
            try:
                self.ro_file.close()
            except OSError:
                pass
        self.ro_file, self.ro_fd = None, None

    # -- 0x1225 READ_FILE_CRITICAL --------------------------------------------

    async def cmd_1225(self, head: bytes) -> None:
        _, _, num_bytes, offset = struct.unpack_from(">HHIQ", head)
        if self.ro_file is None:
            raise Abort("READ_FILE_CRITICAL with no file open")
        transport = self.writer.transport
        transport.set_write_buffer_limits(high=SNDBUF)
        try:
            await self.send_bulk(offset, num_bytes)
        finally:
            transport.set_write_buffer_limits()

    async def send_bulk(self, offset: int, num_bytes: int) -> None:
        """Stream exactly num_bytes of the open file, with no status (§3.2).

        A short read or a dead socket just ends the connection: the command has
        no way to report an error and the client treats a truncated response as
        fatal, so there is nothing useful to do but stop.

        The file is read off the event loop, with ``pread`` — one syscall and one
        copy per chunk, and no shared file position to fight over — and the
        socket is left entirely to asyncio's transport. Mixing ``sendfile`` with a
        transport that owns the same fd fights the kernel over the writer
        registration, and the transport-level ``sendfile`` this needs is not in
        the Python we run. Measured on loopback against ``ps3netsrv``, this is
        the same speed as the reference for one client and better with several
        (§6.1).
        """
        writer = self.writer
        remaining, position = num_bytes, offset
        while remaining:
            count = min(CHUNK, remaining)
            data = await asyncio.to_thread(pread_exact, self.ro_fd, position, count)
            if data is None:
                raise Closed
            writer.write(data)
            await writer.drain()
            position += len(data)
            remaining -= len(data)

    # -- 0x1226 READ_CD_2048_CRITICAL -----------------------------------------

    async def cmd_1226(self, head: bytes) -> None:
        _, _, start_sector, sector_count, _ = struct.unpack_from(">HHIII", head)
        if self.ro_file is None or sector_count * 2048 > BUFFER_SIZE:
            raise Abort("READ_CD_2048_CRITICAL out of range")
        payload = await asyncio.to_thread(self.read_cd, start_sector, sector_count)
        if payload is None:
            raise Closed
        await self.send(payload)

    def read_cd(self, start_sector: int, sector_count: int) -> bytes | None:
        """2048 bytes of user data out of each raw sector (§3.3)."""
        out = bytearray()
        offset = start_sector * self.cd_sector + 24
        for _ in range(sector_count):
            chunk = read_exact(self.ro_file, offset, 2048)
            if chunk is None:
                return None
            out += chunk
            offset += self.cd_sector
        return bytes(out)

    # -- 0x1227 READ_FILE -----------------------------------------------------

    async def cmd_1227(self, head: bytes) -> None:
        _, _, num_bytes, offset = struct.unpack_from(">HHIQ", head)
        if self.ro_file is None or num_bytes > BUFFER_SIZE:
            await self.send(READ_FILE_RESULT.pack(-1))
            return
        data = await asyncio.to_thread(read_upto, self.ro_file, offset, num_bytes)
        if data is None:
            await self.send(READ_FILE_RESULT.pack(-1))
            return
        await self.send(READ_FILE_RESULT.pack(len(data)) + data)

    # -- 0x1228 CREATE_FILE ---------------------------------------------------

    async def cmd_1228(self, head: bytes) -> None:
        (fp_len,) = struct.unpack_from(">H", head, 2)
        self.close_wo()
        if fp_len == 0:
            await self.send(INT32.pack(-1))
            return
        raw = normalize_trailer(await self.recv_path(fp_len))
        path = self.resolve(raw)          # an unsafe path closes the connection
        st = self.share.lstat(path)
        if st is not None and self.share.is_dir(st):
            # Upstream answers -1 here and leaves no handle behind, so the next
            # WRITE_FILE would fail too (§3.5).
            await self.send(INT32.pack(-1))
            return
        flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC | getattr(os, "O_NOFOLLOW", 0)
        try:
            fd = os.open(path, flags, 0o644)
        except OSError:
            await self.send(INT32.pack(-1))
            return
        self.wo_file = os.fdopen(fd, "wb", closefd=True)
        log(f"create {self.share.display(path)}")
        await self.send(INT32.pack(0))

    def close_wo(self) -> None:
        if self.wo_file is not None:
            try:
                self.wo_file.close()
            except OSError:
                pass
        self.wo_file = None

    # -- 0x1229 WRITE_FILE ----------------------------------------------------

    async def cmd_1229(self, head: bytes) -> None:
        (_, _, num_bytes) = struct.unpack_from(">HHI", head)
        if self.wo_file is None:
            await self.send(INT32.pack(-1))
            return
        if num_bytes > BUFFER_SIZE:
            raise Abort("WRITE_FILE over the 4 MiB buffer")
        try:
            payload = await self.reader.readexactly(num_bytes)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            raise Closed
        try:
            self.wo_file.write(payload)
            self.wo_file.flush()
        except OSError:
            await self.send(INT32.pack(-1))
            return
        await self.send(INT32.pack(len(payload)))

    # -- 0x122A OPEN_DIR ------------------------------------------------------

    async def cmd_122a(self, head: bytes) -> None:
        (dp_len,) = struct.unpack_from(">H", head, 2)
        self.close_dir()
        if dp_len == 0:
            await self.send(INT32.pack(-1))
            return
        raw = normalize_trailer(await self.recv_path(dp_len))
        # A substring test, not a trailing-slash rule, exactly as upstream (§3.7).
        subdirs = 1 if b"//" in raw else 0
        path = self.resolve(raw)
        st = self.share.lstat(path)
        entries = self.share.entries(path) if st is not None and self.share.is_dir(st) else None
        if entries is None:
            await self.send(INT32.pack(-1))
            return
        self.dir = DirState(path=path, entries=entries, subdirs=subdirs)
        log(f"open dir {self.share.display(path)}")
        await self.send(INT32.pack(0))

    # -- 0x122B / 0x122F READ_DIR_ENTRY ----------------------------------------

    async def cmd_122b(self, head: bytes) -> None:
        await self.dir_entry(version=1)

    async def cmd_122f(self, head: bytes) -> None:
        await self.dir_entry(version=2)

    async def dir_entry(self, version: int) -> None:
        entry = self.next_entry()
        if entry is None:
            await self.send(sentinel(version))
            return
        try:
            st = entry.stat(follow_symlinks=False)
        except OSError:
            self.close_dir()
            await self.send(sentinel(version))
            return
        name = encode_path(entry.name)
        is_dir = 1 if self.share.is_dir(st) else 0
        if version == 1:
            head = ENTRY_V1.pack(self.share.size(st), len(name), is_dir)
        else:
            head = ENTRY_V2.pack(self.share.size(st), int(st.st_mtime),
                                 int(st.st_ctime), int(st.st_atime), len(name), is_dir)
        await self.send(head + name)

    # -- 0x122C DELETE_FILE ---------------------------------------------------

    async def cmd_122c(self, head: bytes) -> None:
        path = await self.trailer_path(head)
        if path is None:
            await self.send(INT32.pack(-1))
            return
        try:
            os.unlink(path)
        except OSError:
            await self.send(INT32.pack(-1))
            return
        log(f"delete {self.share.display(path)}")
        await self.send(INT32.pack(0))

    # -- 0x122D MKDIR / 0x122E RMDIR -------------------------------------------

    async def cmd_122d(self, head: bytes) -> None:
        await self.make_or_remove(await self.trailer_path(head), os.mkdir, "mkdir")

    async def cmd_122e(self, head: bytes) -> None:
        await self.make_or_remove(await self.trailer_path(head), os.rmdir, "rmdir")

    async def trailer_path(self, head: bytes) -> Path | None:
        """The path trailer of a ``opcode, len, pad…`` request, or None.

        Consumes the trailer either way, so the stream stays in step even when
        the path is refused.
        """
        (dp_len,) = struct.unpack_from(">H", head, 2)
        if dp_len == 0:
            return None
        return self.resolve(normalize_trailer(await self.recv_path(dp_len)))

    async def make_or_remove(self, path, action, label: str) -> None:
        if path is None:
            await self.send(INT32.pack(-1))
            return
        try:
            action(path)
        except OSError:
            await self.send(INT32.pack(-1))
            return
        log(f"{label} {self.share.display(path)}")
        await self.send(INT32.pack(0))

    # -- 0x1230 STAT_FILE -----------------------------------------------------

    async def cmd_1230(self, head: bytes) -> None:
        (fp_len,) = struct.unpack_from(">H", head, 2)
        empty = STAT_RESULT.pack(-1, 0, 0, 0, 0)
        if fp_len == 0:
            await self.send(empty)
            return
        raw = normalize_trailer(await self.recv_path(fp_len))
        st = self.share.lstat(self.resolve(raw))
        if st is None:
            await self.send(empty)
            return
        is_dir = 1 if self.share.is_dir(st) else 0
        await self.send(STAT_RESULT.pack(
            self.share.size(st), int(st.st_mtime), int(st.st_ctime),
            int(st.st_atime), is_dir))

    # -- 0x1231 GET_DIR_SIZE --------------------------------------------------

    async def cmd_1231(self, head: bytes) -> None:
        path = await self.trailer_path(head)
        if path is None or self.share.lstat(path) is None:
            await self.send(INT64.pack(-1))
            return
        await self.send(INT64.pack(await asyncio.to_thread(walk_size, path)))

    # -- 0x1232 READ_DIR ------------------------------------------------------

    async def cmd_1232(self, head: bytes) -> None:
        state = self.dir
        if state is None:
            await self.send(INT64.pack(0))
            return
        records = await asyncio.to_thread(self.read_dir_records, state)
        # Upstream closes the handle after listing, so a second READ_DIR without
        # a fresh OPEN_DIR answers 0. Kept: clients rely on it (§3.15).
        self.close_dir()
        await self.send(INT64.pack(len(records)) + b"".join(records))

    def read_dir_records(self, state: DirState) -> list:
        """Breadth-first listing. With ``subdirs`` set, directories are descended
        and reported by their path relative to the opened directory instead of
        as entries of their own (§3.15, and §9.4 on the depth difference)."""
        out: list[bytes] = []
        queue = [(state.path, b"")]
        while queue and len(out) < MAX_ENTRIES:
            current, prefix = queue.pop(0)
            entries = self.share.entries(current)
            if entries is None:
                continue
            for entry in entries:
                if len(out) >= MAX_ENTRIES:
                    break
                if not listable(entry.name):
                    continue
                name = encode_path(entry.name)
                full = prefix + name
                if len(full) >= NAME_FIELD:
                    continue
                try:
                    st = entry.stat(follow_symlinks=False)
                    size, mtime = self.share.size(st), self.share.mtime(st)
                    is_dir = 1 if self.share.is_dir(st) else 0
                except OSError:
                    size, mtime, is_dir = 0, 0, 1
                if is_dir and state.subdirs:
                    queue.append((Path(entry.path), full + b"/"))
                    continue
                out.append(dir_record(size, mtime, is_dir, full))
        return out

    # -- lifecycle ------------------------------------------------------------

    def cleanup(self) -> None:
        self.close_ro()
        self.close_wo()
        self.close_dir()


def sentinel(version: int) -> bytes:
    """The 'no more entries' answer; only ``file_size = -1`` means anything."""
    if version == 1:
        return ENTRY_V1.pack(-1, 0, 0)
    return ENTRY_V2.pack(-1, 0, 0, 0, 0, 0)


# --------------------------------------------------------------------------
# Listening side
# --------------------------------------------------------------------------

def parse_whitelist(pattern: str) -> tuple[int, int] | None:
    """``192.168.1.*`` -> (first, last) as host-order ints; None allows all."""
    if not pattern:
        return None
    parts = pattern.split(".")
    if len(parts) != 4:
        raise SystemExit("[netsrv] wrong whitelist format")
    start = end = 0
    for i, part in enumerate(reversed(parts)):
        shift = i * 8
        if part == "*":
            end |= 0xFF << shift
            continue
        if not part.isdigit() or int(part) > 0xFF:
            raise SystemExit("[netsrv] wrong whitelist format")
        start |= int(part) << shift
        end |= int(part) << shift
    return start, end


def in_whitelist(addr: str, bounds: tuple[int, int] | None) -> bool:
    if bounds is None:
        return True
    try:
        value = int(ipaddress.IPv4Address(addr))
    except ValueError:
        return False
    return bounds[0] <= value <= bounds[1]


class NetisoServer:
    def __init__(self, share: Share, writable: bool = False,
                 whitelist: tuple[int, int] | None = None):
        self.share = share
        self.writable = writable
        self.whitelist = whitelist
        self.by_ip: dict[str, asyncio.Task] = {}
        # Every handler task, including one that never got a slot: shutdown()
        # has to be able to drop a connection that is connected and silent.
        self.tasks: set[asyncio.Task] = set()

    @property
    def slots(self) -> int:
        return len(self.by_ip)

    async def serve(self, host: str, port: int) -> None:
        server = await asyncio.start_server(self.connection, host, port,
                                           backlog=1, reuse_address=True)
        allowed = "" if self.whitelist is None else " whitelist %s-%s" % (
            ipaddress.IPv4Address(self.whitelist[0]),
            ipaddress.IPv4Address(self.whitelist[1]))
        log(f"listening on {host}:{port}  path: {self.share.root}"
            f"{' [WRITE]' if self.writable else ' [READ-ONLY]'}{allowed}")
        try:
            await server.serve_forever()
        finally:
            # Close the listening socket, but do not wait for the clients:
            # ``wait_closed()`` waits for every handler task, so one client that
            # connected and never spoke — or is mid-stream — would hold shutdown
            # open forever. shutdown() cancels the handlers before we get here.
            server.close()

    async def connection(self, reader, writer) -> None:
        task = asyncio.current_task()
        if task is not None:
            self.tasks.add(task)
        peer = writer.get_extra_info("peername") or ("?", 0)
        addr = peer[0]
        if addr not in self.by_ip and not in_whitelist(addr, self.whitelist):
            log(f"rejected connection from {addr} (not in whitelist)")
            writer.close()
            return
        if len(self.by_ip) >= MAX_CLIENTS and addr not in self.by_ip:
            log(f"too many connections, rejected {addr}")
            writer.close()
            return
        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass
        previous = self.by_ip.get(addr)
        if previous is not None and not previous.done():
            # A reconnect from the same PS3 takes over the slot (§1.1), so it is
            # never locked out by its own stale connection.
            log(f"reconnection from {addr} — dropping the previous connection")
            previous.cancel()
            try:
                await previous
            except (asyncio.CancelledError, Closed):
                pass
        client = Client(self, reader, writer)
        self.by_ip[addr] = asyncio.current_task()
        log(f"[{self.slots}] connection from {addr}:{peer[1]}")
        try:
            while True:
                await client.handle(await client.recv_header())
        except Closed:
            pass
        except Abort as exc:
            log(f"disconnecting {addr}: {exc}")
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # one bad client must not take the listener down
            log(f"error with {addr}: {type(exc).__name__}: {exc}")
        finally:
            client.cleanup()
            if self.by_ip.get(addr) is asyncio.current_task():
                self.by_ip.pop(addr, None)
            self.tasks.discard(task)
            # Closed before waiting, so a cancellation that arrives during the
            # wait still leaves the socket shut.
            try:
                writer.close()
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    async def shutdown(self) -> None:
        """Drop every connection, including one that has not been served yet.

        ``by_ip`` only holds connections that got a slot, so a client that is
        connected and silent — or is mid-handshake — has to come from ``tasks``.
        """
        for task in list(self.tasks):
            task.cancel()
        for task in list(self.tasks):
            try:
                await task
            except (asyncio.CancelledError, Closed):
                pass
        self.tasks.clear()
        self.by_ip.clear()


async def run(args) -> None:
    server = NetisoServer(Share(args.games_dir), writable=args.write,
                          whitelist=parse_whitelist(args.whitelist))
    loop = asyncio.get_running_loop()
    stop = loop.create_future()

    def request_stop() -> None:
        if not stop.done():
            stop.set_result(None)

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, request_stop)
        except (NotImplementedError, RuntimeError):
            pass
    serving = asyncio.create_task(server.serve(args.host, args.port))
    try:
        await stop
    finally:
        await server.shutdown()
        serving.cancel()


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="ps3netsrv.py", description="ps3netsrv (netiso) server for PS3 streaming")
    parser.add_argument("games_dir", help="directory to share with the PS3")
    parser.add_argument("port", nargs="?", type=int, default=DEFAULT_PORT)
    parser.add_argument("whitelist", nargs="?", default="",
                        help="IPv4 pattern with * octets, e.g. 192.168.1.*")
    parser.add_argument("--write", action="store_true",
                        help="allow CREATE_FILE/WRITE_FILE/DELETE_FILE/MKDIR/RMDIR")
    parser.add_argument("--host", default="0.0.0.0")
    args = parser.parse_args(argv)
    if not 1024 <= args.port <= 65535:
        parser.error("port must be in 1024-65535")
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
