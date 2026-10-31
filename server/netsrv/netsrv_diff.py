#!/usr/bin/env python3
"""Differential test: our ps3netsrv against the upstream servers.

Every server is handed the same directory tree, the same commands are sent to
each, and the answers are compared. This is how the open questions in
``docs/PS3NETSRV-PROTOCOL.md`` §8 get settled without a PS3: what a reference
server answers on the wire is what a client sees.

Two references are worth having, because they are not identical:

* ``ps3netsrv`` (webMAN-MOD) — the one PS3 clients actually talk to. Primary.
* ``ps3netsrv++`` (dirkvdb) — an independent C++ rewrite people also deploy.

Where they disagree with each other the harness says so, because that is a fact
about the protocol worth knowing. Our server is judged against the primary.

Deviations already chosen on purpose (§9) are declared per scenario: they print
as ``expected`` and do not fail the run, but they print every time, so a
deviation cannot quietly turn into a bug. Anything undeclared fails.

    python3 server/netsrv/netsrv_diff.py --reference /path/to/ps3netsrv \
                                         --reference /path/to/ps3netsrv++

``$PS3NETSRV_REF`` and ``$PS3NETSRVXX_REF`` are the defaults. Scenarios for a
reference that is missing are skipped, so the script never breaks a run that
has no reference build. Build them outside this repository: both are GPL-3.0
and stay out of our image.
"""
from __future__ import annotations

import argparse
import asyncio
import hashlib
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import netsrv_bench as nb
import netsrv_client as nc
import ps3netsrv as ns

RAMP = bytes(i % 251 for i in range(4096))


def param_sfo(title_id: bytes = b"BCES00104") -> bytes:
    """A minimal valid PARAM.SFO, with TITLE_ID in the data table.

    The same builder is in ``netsrv_test.py``; the harnesses each keep their own
    fixtures so either one stays runnable on its own.
    """
    entries = sorted(((b"TITLE", b"Test Game"), (b"TITLE_ID", title_id)))
    keys = b""
    data = b""
    index = []
    for key, value in entries:
        key_at = len(keys)
        keys += key + b"\0"
        data_at = len(data)
        data += value
        maximum = (len(value) + 3) & ~3
        data += b"\0" * (maximum - len(value))
        index.append((key_at, 0x0204, len(value), maximum, data_at))
    while len(keys) & 3:
        keys += b"\0"
    start = 0x14 + 16 * len(index)
    data_start = start + len(keys)
    out = struct.pack("<4sIIII", b"\0PSF", 0x00000101, start, data_start,
                      len(index))
    for key_at, fmt, length, maximum, data_at in index:
        out += struct.pack("<HHIII", key_at, fmt, length, maximum, data_at)
    return out + keys + data


FIXTURE_FILES = {
    "GAMES/notes.txt": b"hello world",
    "GAMES/.hidden": b"hidden",
    "GAMES/sub/deep.dat": b"deep contents",
    "GAMES/sub/deeper/deepest.dat": b"the deepest",
    "GAMES/unicode-é.bin": RAMP[:1000],
    "PS3ISO/game.iso": bytes(range(256)) * 64,
    # A folder game and a plain folder, for the virtual-ISO mounts (§7.4). The
    # PARAM.SFO has to be real: upstream reads TITLE_ID out of it and refuses a
    # PS3 mount without one. Small, because the scenarios read an image back.
    "PS3_GAME_DIR/PS3_GAME/PARAM.SFO": param_sfo(),
    "PS3_GAME_DIR/PS3_GAME/USRDIR/EBOOT.BIN": RAMP[:4000],
    "PS3_GAME_DIR/PS3_DISC.SFB": b"disc",
    "MOVIE_DIR/VIDEO_TS/VIDEO_TS.IFO": b"ifodata",
}
OUTSIDE = b"outside-secret.txt"
# A virtual ISO's header, up to and including the path tables, is the part two
# servers can be compared on; 40 KiB is more than a small fixture needs.
VISO_HEAD = 0xA000
# The ranges a synthesised image is allowed to differ in: the PS3 disc-info
# sector is random bytes by design, and each volume descriptor carries the
# clock of the moment it was built.
VISO_MASKS = ((0x800, 0x1000), (0x8000 + 813, 0x8000 + 847),
              (0x8800 + 813, 0x8800 + 847))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def cd_image(path: Path, stride: int, size: int, raw: bool = True) -> None:
    """A CD image with volume descriptor 1 in sector 16 at the given stride.

    ``raw`` puts the descriptor 24 bytes into the sector, as a MODE2/2352 rip
    has it; without it the image is plain 2048-byte sectors.
    """
    with path.open("wb") as fh:
        for start in range(0, size, len(RAMP)):
            fh.write(RAMP[:min(len(RAMP), size - start)])
        fh.seek(stride * 16 + (24 if raw else 0))
        fh.write(b"\x01CD001\x00\x00PLAYSTATION ")


def synth_image(path: Path, stride: int, total: int, raw: bool = True) -> None:
    """A *sparse* CD image of *total* bytes: PVD in sector 16 at *stride*.

    The probe only ever reads ``sector * 16`` and ``sector * 16 + 24`` (§5), and
    ``measure_stride`` reconstructs its reply from sector 0, so a head of varying
    bytes is all the content the file needs; the rest is a hole and the fixture
    builds instantly at any size. A real 850 MiB rip would say the same thing.
    """
    head = PROBE_HEAD * (PROBE_HEAD_LEN // len(PROBE_HEAD) + 1)
    with path.open("wb") as fh:
        fh.write(head[:PROBE_HEAD_LEN])
        fh.truncate(total)
        fh.seek(stride * 16 + (24 if raw else 0))
        fh.write(b"\x01CD001\x00\x00PLAYSTATION ")


# Images straddling the probe's size window (§5, §8): the ceiling is upstream's,
# so every row here is measured against it, and the two 2448 rows are where our
# correct probe and upstream's answer are guaranteed to part company. The
# ``truth`` column is what the file actually is — where ours disagrees with it,
# the ceiling defeated the probe, which is the finding.
CEILING_IMAGES = {
    # name: (size, stride, raw, truth, ours, note)
    "PSXISO-atceiling-2448.bin":
        (ns.PROBE_MAX, 2448, True, 2448, 2448, "last size still scanned"),
    "PSXISO-overceiling-2448.bin":
        (ns.PROBE_MAX + 1, 2448, True, 2448, ns.CD_SECTOR_DEFAULT,
         "one byte past, and the default is wrong for the file"),
    "PSXISO-giant-2352.bin":
        (1 << 30, 2352, True, 2352, 2352, "Q2's raw .bin: default agrees by luck"),
    "PSXISO-giant-2048.iso":
        (1 << 30, 2048, False, 2048, ns.CD_SECTOR_DEFAULT,
         "a DVD-sized ISO: the default disagrees with the file"),
    "PSXISO-belowfloor-2352.bin":
        (ns.PROBE_MIN - 1, 2352, True, 2352, 2352, "the floor behaves the same"),
    "PSXISO-under-2448.bin":
        (ns.PROBE_MAX - (8 << 20), 2448, True, 2448, 2448,
         "oddball stride inside the window: found"),
}
PROBE_HEAD_LEN = 2448 * 128 + 24 + 2048  # measure_stride's 128-sector window
PROBE_HEAD = bytes((i * 7 + i // 251) % 256 for i in range(4097))


def build_tree(root: Path) -> Path:
    """One tree for every server, holding the shapes the spec cares about."""
    for rel, body in FIXTURE_FILES.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(body)
    (root / "EMPTY").mkdir(exist_ok=True)
    (root / "two spaces  here").mkdir(exist_ok=True)
    (root.parent / OUTSIDE.decode()).write_bytes(b"never serve this")
    cd_image(root / "PSXISO-raw2352.bin", 2352, ns.PROBE_MIN + 4096)
    cd_image(root / "PSXISO-plain2048.iso", 2048, ns.PROBE_MIN + 4096, raw=False)
    for name, (size, stride, raw, *_rest) in CEILING_IMAGES.items():
        synth_image(root / name, stride, size, raw)
    os.symlink(root.parent / OUTSIDE.decode(), root / "GAMES/escape.iso")
    return root


# --------------------------------------------------------------------------
# Scenario helpers
# --------------------------------------------------------------------------

async def entries(client, root, v2=False):
    if await client.open_dir(b"/GAMES") != 0:
        return "open_dir failed"
    out = []
    while True:
        entry = await (client.read_dir_entry_v2() if v2 else client.read_dir_entry())
        if entry is None:
            return out
        out.append(entry)


async def records(client, root, path=b"/GAMES"):
    if await client.open_dir(path) != 0:
        return "open_dir failed"
    return await client.read_dir()


async def records_twice(client, root):
    await client.open_dir(b"/GAMES")
    return {"first": records_shape(await client.read_dir()),
            "second": records_shape(await client.read_dir())}


async def read_after_open(client, root, path, offset, count):
    await client.open_file(path)
    return await client.read_file_status(offset, count)


async def critical_after_open(client, root, path, offset, count):
    await client.open_file(path)
    try:
        return await client.read_file_critical(offset, count)
    except nc.ConnectionClosed:
        return "closed"


async def cd_after_open(client, root, path, first, count):
    await client.open_file(path)
    try:
        return await client.read_cd_2048(first, count)
    except nc.ConnectionClosed:
        return "closed"


def measure_stride(image: Path, returned: bytes, sectors: int):
    """Which stride a server read at, judged from what it sent back.

    No command reports what the sector probe settled on, but
    ``READ_CD_2048_CRITICAL`` gives it away: sector *i* of the reply is the file
    at ``i * stride + 24``. Rebuilding the reply at each candidate stride and
    finding the one that is byte-identical reads the answer back off the wire.
    More than one candidate matching means the window holds nothing but the same
    bytes, and the measurement says nothing — said out loud rather than guessed.
    """
    window = max(ns.CD_CANDIDATES) * sectors + 24 + 2048
    try:
        with image.open("rb") as fh:
            data = fh.read(window)
    except OSError as exc:
        return f"unreadable ({exc})"
    found = [stride for stride in ns.CD_CANDIDATES
             if b"".join(data[i * stride + 24:i * stride + 24 + 2048]
                         for i in range(sectors)) == returned]
    return found[0] if len(found) == 1 else f"ambiguous {found}"


def cd_stride(path_part: str, sectors: int = 128):
    async def run(client, root):
        name = path_part.encode("utf8", "surrogateescape")
        await client.open_file(name)
        try:
            reply = await client.read_cd_2048(0, sectors)
        except nc.ConnectionClosed:
            return "closed"
        return measure_stride(root / path_part.lstrip("/"), reply, sectors)
    return run


async def then_stat(client, root):
    """Two commands on one connection: state must not leak between them."""
    await client.open_file(b"/GAMES/notes.txt")
    return await client.stat(b"/GAMES")


async def expect_close(client, root, opcode):
    try:
        await client.expect_closed(client.header(opcode))
        return "closed"
    except AssertionError:
        return "answered"


async def answered_or_closed(client, root, func, path):
    """Whether the server answered (and with what) or hung up is the finding."""
    try:
        return "answered", await func(path)
    except nc.ConnectionClosed:
        return "closed"


async def writable_round_trip(client, root):
    return [
        ("mkdir", await client.mkdir(b"/UPLOADS")),
        ("mkdir twice", await client.mkdir(b"/UPLOADS")),
        ("open dir", await client.open_dir(b"/UPLOADS")),
        ("create", await client.create_file(b"/UPLOADS/out.bin")),
        ("write", await client.write_file(RAMP[:7000])),
        ("delete", await client.delete_file(b"/UPLOADS/out.bin")),
        ("delete again", await client.delete_file(b"/UPLOADS/out.bin")),
        ("rmdir", await client.rmdir(b"/UPLOADS")),
        ("rmdir again", await client.rmdir(b"/UPLOADS")),
    ]


async def create_truncates(client, root):
    (root / "trunc.bin").write_bytes(b"x" * 100)
    result = await client.create_file(b"/trunc.bin")
    return result, (root / "trunc.bin").stat().st_size


def iso_head(image: bytes) -> dict:
    """A virtual ISO's opening 40 KiB, in a form two servers can be compared on.

    A synthesised image is not byte-stable by design: the PS3 disc-info sector is
    filled with random bytes, and both volume descriptors carry the clock at the
    moment the mount happened. Those ranges are blanked out and the rest is
    hashed — everything else about the layout has to be identical.
    """
    masked = bytearray(image)
    for start, end in VISO_MASKS:
        masked[start:end] = b"\0" * (end - start)
    pvd = image[0x8000:0x8800]
    return {"sha1": hashlib.sha1(bytes(masked)).hexdigest(),
            "volume": pvd[40:72].rstrip(b" "),
            "sectors": int.from_bytes(pvd[80:84], "little"),
            "root_record": pvd[156:190].hex()}


async def virtual_iso_open(client, root, marker=b"/***PS3***/", folder=b"PS3_GAME_DIR"):
    """OPEN_FILE on a folder: what the server reports as its size and mtime."""
    return await client.open_file(marker + folder)


async def virtual_iso_image(client, root, marker=b"/***PS3***/", folder=b"PS3_GAME_DIR"):
    """The image itself: its size, its header, and the pad sector it ends with."""
    size, _ = await client.open_file(marker + folder)
    if size < 0:
        return "not-found"
    head = await client.read_file(0, min(size, VISO_HEAD))
    return {"size": size, **iso_head(head),
            "tail": await client.read_file(size - 2048, 2048)}


async def virtual_iso_dvd_image(client, root):
    """``/***DVD***/`` names the volume after the folder, and writes no PS3 sectors."""
    size, _ = await client.open_file(b"/***DVD***/MOVIE_DIR")
    if size < 0:
        return "not-found"
    head = await client.read_file(0, min(size, VISO_HEAD))
    return {"size": size, "disc_info": head[0x800:0x80C], **iso_head(head),
            "tail": await client.read_file(size - 2048, 2048)}


async def read_all(client, size: int, chunk: int = 1 << 20) -> bytes:
    """A whole file or image, in pieces under the server's own limit."""
    out = bytearray()
    while len(out) < size:
        part = await client.read_file(len(out), min(chunk, size - len(out)))
        if not part:
            break
        out += part
    return bytes(out)


async def virtual_iso_whole(client, root, path=b"/."):
    """The whole image a mount answers with, masked and hashed (§7.4)."""
    size, _ = await client.open_file(path)
    if size < 0:
        return "not-found"
    return {"size": size, **iso_head(await read_all(client, size))}


async def virtual_iso_cd_read(client, root):
    """Where a CD read takes sector 16 from, judged by what comes back.

    A synthesised image is plain 2048-byte sectors, so the answer has to be the
    same 2048 bytes an ordinary read of ``16 * 2048`` returns.
    """
    size, _ = await client.open_file(b"/***PS3***/PS3_GAME_DIR")
    if size < 0:
        return "not-found"
    try:
        sector = await client.read_cd_2048(16, 1)
    except nc.ConnectionClosed:
        return "closed"
    return {"sector 16 is the file at 16*2048": sector == await client.read_file(16 * 2048, 2048),
            "signature": sector[:7]}


async def virtual_iso_listing(client, root):
    """OPEN_DIR on a marker path, which only OPEN_FILE looks for (§7.4)."""
    return await records(client, root, b"/***PS3***/PS3_GAME_DIR")


def unsort(value):
    """Compare listings as sets: our sorted order vs upstream's readdir order."""
    if not isinstance(value, list):
        return value
    return sorted(value, key=lambda e: (e[-1], e[0], e[-2]))


def drop_times(value):
    """Blank out mtime/ctime/atime: reading a file moves atime, per run."""
    value = unsort(value)
    if not isinstance(value, list):
        return value
    return [(e[0], None, None, None) + e[4:] if len(e) == 7 else e for e in value]


def ignore_stat_times(value):
    """Blank the timestamp and flag fields of a *failed* STAT_FILE.

    Upstream assigns only ``file_size`` on that path, so the rest of the struct
    goes out as whatever the thread's stack happened to hold: different every
    run, and stack bytes handed to any client that asks for a missing file. We
    zero them, and no comparison can match a stack (§8).
    """
    if isinstance(value, dict) and value.get("size") == -1:
        return {**value, "mtime": None, "ctime": None, "atime": None,
                "is_dir": None}
    return value


def records_shape(value):
    """READ_DIR records without timestamps, in name order."""
    if not isinstance(value, list):
        return value
    return sorted((r["size"], None, r["is_dir"], r["name"]) for r in value)


def scenario(name, func, *, normalize=None, declares=(), primary_only=False):
    return dict(name=name, func=func, normalize=normalize or (lambda x: x),
                declares=list(declares), primary_only=primary_only)


DEVIATION_SYMLINK = ("the symlink inside the share is listed with the size of "
                     "the link itself; upstream follows it and lists the size of "
                     "the file outside the share (§9.4). Listing order is not a "
                     "difference in this run: our sort is switched off for it")
ESCAPE = b"/GAMES/../../" + OUTSIDE

SCENARIOS = [
    scenario("open existing file", lambda c, r: c.open_file(b"/PS3ISO/game.iso")),
    scenario("open missing file", lambda c, r: c.open_file(b"/nope/nothere.iso")),
    scenario("open a directory", lambda c, r: c.open_file(b"/GAMES"),
             declares=["upstream opens a directory as a read file and answers "
                       "its host size (140); a directory is not a file to us, "
                       "so we answer -1 (§3.1, §9.11)"]),
    scenario("open zero-length path", lambda c, r: c.open_file(b"")),
    scenario("CLOSEFILE", lambda c, r: c.close_file()),
    scenario("backslash path", lambda c, r: c.open_file(b"\\GAMES\\notes.txt")),
    scenario("trailing slash path", lambda c, r: c.open_file(b"/GAMES/notes.txt/")),
    scenario("trailing CR in path", lambda c, r: c.open_file(b"/GAMES/notes.txt\r")),
    scenario("double slash in path", lambda c, r: c.open_file(b"/GAMES//notes.txt")),
    scenario("stat file", lambda c, r: c.stat(b"/GAMES/notes.txt")),
    scenario("stat directory", lambda c, r: c.stat(b"/GAMES/sub")),
    scenario("stat root", lambda c, r: c.stat(b"/")),
    scenario("stat missing", lambda c, r: c.stat(b"/nope"),
             normalize=ignore_stat_times,
             declares=["upstream leaves the timestamp fields and is_directory "
                       "uninitialised on this path and sends them; we zero them "
                       "(§8, §9.10)"]),
    scenario("stat symlink", lambda c, r: c.stat(b"/GAMES/escape.iso"),
             declares=["we lstat a symlink and report the link itself; the "
                       "references follow it and report the target (§9.4)"]),
    scenario("open symlink", lambda c, r: c.open_file(b"/GAMES/escape.iso"),
             declares=["we refuse to open a symlink; the references serve the "
                       "file it points at, outside the share (§9.4)"]),
    scenario("read entries v1", entries, normalize=unsort,
             declares=[DEVIATION_SYMLINK]),
    scenario("read entries v2", lambda c, r: entries(c, r, v2=True),
             normalize=drop_times, declares=[DEVIATION_SYMLINK]),
    scenario("open dir on a file", lambda c, r: c.open_dir(b"/GAMES/notes.txt")),
    scenario("open missing dir", lambda c, r: c.open_dir(b"/nope")),
    scenario("open root dir", lambda c, r: c.open_dir(b"/")),
    scenario("open dir double slash", lambda c, r: c.open_dir(b"/GAMES//")),
    scenario("open dir zero length", lambda c, r: c.open_dir(b""),
             declares=["upstream blocks forever in recv(…, 0, MSG_WAITALL): no "
                       "answer, connection left open, one of its five slots "
                       "gone for good. We answer -1 and move on (§8, §9.9)"]),
    scenario("read_dir flat", records, normalize=records_shape,
             declares=[DEVIATION_SYMLINK]),
    scenario("read_dir empty dir", lambda c, r: records(c, r, b"/EMPTY"),
             normalize=records_shape),
    scenario("read_dir spaces in name", lambda c, r: records(c, r, b"/two spaces  here"),
             normalize=records_shape),
    scenario("read_dir recursive", lambda c, r: records(c, r, b"/GAMES//"),
             normalize=records_shape,
             declares=["we recurse to full depth; upstream skips "
                       "sub-subdirectories, so its listing omits deepest.dat "
                       "(§9.3), and the symlink inside the share is listed with "
                       "its own size (§9.4)"]),
    scenario("read_dir twice no reopen", records_twice,
             declares=[DEVIATION_SYMLINK]),
    scenario("read_dir without open_dir", lambda c, r: c.read_dir()),
    scenario("get_dir_size", lambda c, r: c.get_dir_size(b"/GAMES"),
             declares=["upstream follows the symlink inside the share and adds "
                       "the size of the file outside it; we never follow a "
                       "symlink, so it counts as nothing (§9.4)"]),
    scenario("get_dir_size of a file", lambda c, r: c.get_dir_size(b"/GAMES/notes.txt")),
    scenario("get_dir_size missing", lambda c, r: c.get_dir_size(b"/nope")),
    scenario("read_file whole", lambda c, r: read_after_open(c, r, b"/PS3ISO/game.iso", 0, 16384)),
    scenario("read_file near eof", lambda c, r: read_after_open(c, r, b"/PS3ISO/game.iso", 16000, 4096)),
    scenario("read_file over buffer",
             lambda c, r: read_after_open(c, r, b"/PS3ISO/game.iso", 0, ns.BUFFER_SIZE + 1)),
    scenario("read_file without open", lambda c, r: c.read_file_status(0, 16)),
    scenario("critical read whole", lambda c, r: critical_after_open(c, r, b"/PS3ISO/game.iso", 0, 16384)),
    scenario("critical read unaligned", lambda c, r: critical_after_open(c, r, b"/PS3ISO/game.iso", 3, 1000)),
    scenario("critical read past eof", lambda c, r: critical_after_open(c, r, b"/PS3ISO/game.iso", 16000, 4096)),
    scenario("critical read without open", lambda c, r: c.read_file_critical(0, 16)),
    scenario("cd read raw 2352 image",
             lambda c, r: cd_after_open(c, r, b"/PSXISO-raw2352.bin", 16, 8)),
    scenario("cd read plain 2048 image",
             lambda c, r: cd_after_open(c, r, b"/PSXISO-plain2048.iso", 16, 4),
             declares=["our probe finds the 2048 stride; upstream's probe reads "
                       "past what it fetched and misses it, so upstream answers "
                       "at the wrong stride (§5)"]),
    scenario("cd read too many sectors",
             lambda c, r: cd_after_open(c, r, b"/PSXISO-raw2352.bin", 0, 2049)),
    scenario("cd read without open", lambda c, r: c.read_cd_2048(0, 1)),
    # Nothing reports what the probe settled on, so read it back off the wire:
    # these two are the rows of §5's table, and they say more than the payloads.
    scenario("cd stride raw 2352 image", cd_stride("/PSXISO-raw2352.bin")),
    scenario("cd stride plain 2048 image", cd_stride("/PSXISO-plain2048.iso"),
             declares=["our probe finds the 2048 stride, upstream's misses it and "
                       "answers 2352 for a plain image (§5)"]),
    # The probe's size window itself (§5, §8): sparse fixtures straddling
    # PROBE_MIN/PROBE_MAX. Where ours reports the default on a file whose real
    # stride is something else, only a real client can say whether it cares —
    # the §8 open question these rows are built for.
    scenario("cd stride at ceiling 2448", cd_stride("/PSXISO-atceiling-2448.bin"),
             declares=["ours is the only probe that finds an odd stride at all; "
                       "upstream answers its default (§5, §9.5)"]),
    scenario("cd stride over ceiling 2448",
             cd_stride("/PSXISO-overceiling-2448.bin")),
    scenario("cd stride giant 2352", cd_stride("/PSXISO-giant-2352.bin")),
    scenario("cd stride giant 2048", cd_stride("/PSXISO-giant-2048.iso")),
    scenario("cd stride below floor 2352",
             cd_stride("/PSXISO-belowfloor-2352.bin")),
    scenario("cd stride under ceiling 2448",
             cd_stride("/PSXISO-under-2448.bin"),
             declares=["as at the ceiling: 2448 inside the window is ours alone "
                       "to find (§5, §9.5)"]),
    scenario("state across two commands", then_stat),
    scenario("unknown opcode 0x2412", lambda c, r: expect_close(c, r, 0x2412)),
    scenario("unknown opcode 0x2fff", lambda c, r: expect_close(c, r, 0x2FFF)),
    scenario("unknown opcode 0x0000", lambda c, r: expect_close(c, r, 0x0000)),
    scenario("parent dir in stat", lambda c, r: answered_or_closed(c, r, c.stat, ESCAPE),
             declares=["we refuse the path and disconnect; upstream's weaker "
                       "check accepts ``a/../../b`` and serves a file from "
                       "outside the share (§7.1, §9.6)"]),
    scenario("parent dir in open",
             lambda c, r: answered_or_closed(c, r, c.open_file, b"/GAMES/sub/../../" + OUTSIDE),
             declares=["as above: the references read through the escape"]),
    scenario("parent dir in open_dir",
             lambda c, r: answered_or_closed(c, r, c.open_dir, b"/..")),
    scenario("parent dir in get_dir_size",
             lambda c, r: answered_or_closed(c, r, c.get_dir_size, ESCAPE)),
    scenario("no leading slash",
             lambda c, r: answered_or_closed(c, r, c.stat, b"GAMES/notes.txt")),
    scenario("path over MAX_PATH_LEN",
             lambda c, r: answered_or_closed(c, r, c.stat, b"/" + b"x" * 600),
             declares=["we close the connection above 510 bytes; upstream "
                       "mallocs whatever the client announces and answers, so a "
                       "client can make it allocate 64 KiB per request at will "
                       "(§7.3, §9.7)"]),
    # §7.4: a folder game mounted as an ISO. Upstream is the primary reference
    # for this and the only one — ps3netsrv++ has no virtual ISO at all.
    scenario("virtual ISO open", virtual_iso_open, primary_only=True),
    scenario("virtual ISO image", virtual_iso_image, primary_only=True),
    scenario("virtual ISO dvd mode", virtual_iso_dvd_image, primary_only=True),
    scenario("virtual ISO cd read", virtual_iso_cd_read, primary_only=True,
             declares=["upstream applies the raw-image +24 to a folder game too, "
                       "so its answer for sector 16 is 24 bytes past the start of "
                       "the one we mean; a synthesised image has no raw header "
                       "to skip (§7.4, §9.2)"]),
    scenario("virtual ISO listing", virtual_iso_listing, primary_only=True,
             normalize=records_shape),
    scenario("virtual ISO stat", lambda c, r: c.stat(b"/***PS3***/PS3_GAME_DIR"),
             primary_only=True, normalize=ignore_stat_times,
             declares=["both answers are -1; upstream also sends the timestamp "
                       "fields and is_directory of a failed STAT uninitialised, "
                       "which is its own declared difference (§8, §9.10)"]),
    scenario("virtual ISO dir size",
             lambda c, r: c.get_dir_size(b"/***PS3***/PS3_GAME_DIR"),
             primary_only=True),
    scenario("virtual ISO of a folder that is not a game",
             lambda c, r: c.open_file(b"/***PS3***/GAMES"), primary_only=True),
    scenario("virtual ISO of a file",
             lambda c, r: c.open_file(b"/***PS3***/PS3ISO/game.iso"),
             primary_only=True),
    # ``/.`` is upstream's way of mounting the whole share, so this is the one
    # scenario that reads a synthesised image back in full. Everything below is
    # the symlink in the share; the rest of 4.4 MB agrees byte for byte.
    scenario("virtual ISO of the share root", virtual_iso_whole,
             primary_only=True,
             declares=["this image covers the share's symlink: upstream follows "
                       "it, gives it a record with the size of the file *outside "
                       "the share* it points at and copies that file in, which "
                       "moves every later file by one sector; we leave it out "
                       "(§9.4). Upstream also names this volume ``.``, the last "
                       "element of the path it was handed, where we name it "
                       "after the share directory (§9.2)"]),
    scenario("virtual ISO marker further in",
             lambda c, r: c.open_file(b"/GAMES/***PS3***/notes.txt"),
             primary_only=True),
    scenario("virtual ISO escape",
             lambda c, r: answered_or_closed(c, r, c.open_file,
                                             b"/***PS3***/../../" + OUTSIDE),
             primary_only=True,
             declares=["we refuse any path with a ``..`` in it and disconnect; "
                       "upstream answers -1 here (§7.1, §9.6)"]),
    scenario("mkdir create write delete rmdir", writable_round_trip),
    scenario("create truncates", create_truncates),
    scenario("write without create", lambda c, r: c.write_file(b"data")),
    scenario("create over a directory", lambda c, r: c.create_file(b"/GAMES")),
    scenario("create zero length", lambda c, r: c.create_file(b""),
             declares=["the same recv(0) wedge as OPEN_DIR (§8, §9.9)"]),
    scenario("mkdir over an existing file", lambda c, r: c.mkdir(b"/GAMES/notes.txt")),
    scenario("rmdir over a file", lambda c, r: c.rmdir(b"/GAMES/notes.txt")),
]


# --------------------------------------------------------------------------
# Servers
# --------------------------------------------------------------------------

class Reference:
    """One upstream server, running as a subprocess on its own port."""

    def __init__(self, binary: str, root: Path, name: str = ""):
        self.binary = binary
        self.name = name or Path(binary).name
        self.port = free_port()
        argv = self.launch_args(binary, self.port, root)
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                     stderr=subprocess.STDOUT)
        for _ in range(200):
            self.proc.poll()
            if self.proc.returncode is not None:
                raise SystemExit(f"[diff] {binary} exited with {self.proc.returncode}")
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=0.25):
                    return
            except OSError:
                time.sleep(0.05)
        self.proc.terminate()
        raise SystemExit(f"[diff] {binary} never accepted a connection")

    @staticmethod
    def launch_args(binary: str, port: int, root: Path) -> list:
        name = Path(binary).name
        if name.startswith("ps3netsrv++"):
            # dirkvdb: getopt style. No -d: that daemonizes and detaches. And no
            # -w either — its usage text advertises a whitelist that its getopt
            # switch has no case for, so passing -w prints the usage and exits.
            return [binary, "-p", str(port), str(root)]
        # webMAN-MOD: positional root, port, whitelist.
        return [binary, str(root), str(port)]

    async def drive(self, case, root: Path):
        return await drive(case, root, self.port)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


async def drive(case, root: Path, port: int, timeout: float = 10.0):
    """One scenario against one server, with a hard ceiling.

    A server that never answers is itself a finding, so it is reported as a
    timeout rather than hanging the whole run.
    """
    client = nc.Netiso("127.0.0.1", port, timeout=timeout)
    try:
        await asyncio.wait_for(client.connect(), timeout)
        return await asyncio.wait_for(case["func"](client, root), timeout)
    except nc.ConnectionClosed as exc:
        return f"closed ({exc})"
    except asyncio.TimeoutError:
        return "TIMEOUT"
    except Exception as exc:
        return f"{type(exc).__name__}: {exc}"
    finally:
        client.close()


async def run_against_ours(case, root: Path):
    """Our server in-process, on its own port, one listening socket per case."""
    server = ns.NetisoServer(ns.Share(root), writable=True)
    srv = await asyncio.start_server(server.connection, "127.0.0.1", 0, backlog=1)
    port = srv.sockets[0].getsockname()[1]
    try:
        return await drive(case, root, port)
    finally:
        srv.close()
        await srv.wait_closed()


def describe(value) -> str:
    if isinstance(value, bytes):
        return f"<{len(value)}B {value[:12].hex()}{'…' if len(value) > 12 else ''}>"
    if isinstance(value, bool):
        return str(value)
    if isinstance(value, dict):
        return "{" + ", ".join(f"{k}: {describe(v)}"
                              for k, v in sorted(value.items(), key=lambda kv: str(kv[0]))) + "}"
    if isinstance(value, (list, tuple)):
        return "[" + ", ".join(describe(v) for v in value) + "]"
    return repr(value)


def first_difference(a, b, path="") -> str:
    """A short description of where two compared results stop agreeing."""
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        if len(a) != len(b):
            return f"at {path or 'root'}: {len(a)} items vs {len(b)}"
        for i, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f"{path}[{i}]")
            if found:
                return found
        return ""
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            return f"at {path or 'root'}: keys differ {sorted(set(a) ^ set(b))}"
        for key in sorted(a, key=str):
            found = first_difference(a[key], b[key], f"{path}.{key}")
            if found:
                return found
        return ""
    if type(a) is not type(b):
        return f"at {path or 'root'}: {type(a).__name__} vs {type(b).__name__}"
    if a != b:
        return f"at {path or 'root'}: {describe(a)} vs {describe(b)}"
    return ""


def pick_references(given: list) -> list:
    """--reference wins; otherwise the two environment variables."""
    chosen = [p for p in given if p and os.path.exists(p)]
    if not chosen:
        chosen = [p for p in (os.environ.get("PS3NETSRV_REF", ""),
                              os.environ.get("PS3NETSRVXX_REF", ""))
                  if p and os.path.exists(p)]
    return chosen


def sheet_stride(image: Path) -> str:
    """What the ``.cue`` sheet says about the stride, when there is one."""
    cue = image.with_suffix(".cue")
    if not cue.is_file():
        return "no .cue"
    try:
        text = cue.read_text(errors="replace")
    except OSError as exc:
        return f"unreadable ({exc})"
    track = re.search(r"TRACK\s+\d+\s+(\S+(?:\s+\S+)*)", text, re.IGNORECASE)
    stride = re.search(r"MODE\d\s*/\s*(\d+)", text)
    if stride:
        return f"{track.group(1) if track else '?'} → {stride.group(1)}"
    return track.group(1) if track else "no TRACK line"


async def measure_psx(image: Path, binaries: list) -> int:
    """The first row of §5's table: the probe's answer for a real rip.

    This is the one measurement the synthetic images cannot make. The rip is
    hardlinked into a private directory first, because a server shares a whole
    tree and the image's own directory is not something to put on the network for
    the duration — and because ``ps3netsrv++`` has no working whitelist (§8).
    """
    if not image.is_file():
        print(f"[diff] --psx: no such file: {image}")
        return 0
    truth = sheet_stride(image)
    tmp, staged = nb.stage(image)
    root, name = tmp, "/" + staged.name
    case = scenario("cd stride", cd_stride(name))
    print(f"\n[diff] real rip    {image.name}, {image.stat().st_size:,} bytes, "
          f"sheet: {truth}")
    results = {}
    try:
        server = ns.NetisoServer(ns.Share(root), writable=False)
        srv = await asyncio.start_server(server.connection, "127.0.0.1", 0,
                                         backlog=1)
        try:
            results["ours"] = await drive(case, root,
                                          srv.sockets[0].getsockname()[1])
        finally:
            await server.shutdown()
            srv.close()

        for binary in binaries:
            try:
                ref = Reference(binary, root)
            except SystemExit as exc:
                print(f"             {Path(binary).name:<16} could not start: {exc}")
                continue
            try:
                results[ref.name] = await ref.drive(case, root)
            finally:
                ref.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    stride = truth.rsplit("/", 1)[-1]
    want = int(stride) if stride.isdigit() else None
    for label, value in results.items():
        verdict = ""
        if want and isinstance(value, int):
            verdict = "  ✓" if value == want else f"  ✗ (the image is {want})"
        print(f"             {label:<16} stride {value}{verdict}")
    return 0


async def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="Differential test against upstream ps3netsrv")
    parser.add_argument("--reference", action="append", default=[],
                        help="a reference server binary; repeatable")
    parser.add_argument("--dir", default="", help="tree to serve (default: build one)")
    parser.add_argument("--keep", action="store_true", help="keep the temporary tree")
    parser.add_argument("--only", default="", help="run scenarios whose name has this")
    parser.add_argument("--psx", default="",
                        help="a real CD image (e.g. a MODE2/2352 .bin) to measure "
                             "the sector probe against, outside the test tree")
    args = parser.parse_args(argv)

    # Compare the wire, not our chosen listing order (§9.3): with our own sort
    # switched off the answers come back in readdir order like upstream's, so a
    # set comparison cannot be hiding a byte-level difference.
    ns.LISTING_SORTED = False

    paths = pick_references(args.reference)
    if not paths and not args.psx:
        print("[diff] no reference server found (use --reference, $PS3NETSRV_REF "
              "or $PS3NETSRVXX_REF); skipping")
        return 0
    if not paths:
        # Nothing to compare against, but the sector probe can still be read
        # back off our own server.
        return await measure_psx(Path(args.psx), [])

    tmp = Path(tempfile.mkdtemp(prefix="netsrv-diff-"))
    root = tmp / "share"
    root.mkdir()
    if args.dir:
        shutil.copytree(args.dir, root, dirs_exist_ok=True)
    else:
        build_tree(root)

    refs = [Reference(p, root) for p in paths]
    primary = next((r for r in refs if r.name.startswith("ps3netsrv")
                    and not r.name.startswith("ps3netsrv++")), refs[0])
    print(f"[diff] tree        {root}")
    for ref in refs:
        print(f"[diff] reference   {ref.name} ({ref.binary}) on :{ref.port}"
              f"{'  <- primary' if ref is primary else ''}")
    print(f"[diff] ours        in-process, same tree\n")

    matched = declared = unexpected = 0

    def emit(text):
        print(text, flush=True)

    try:
        for case in SCENARIOS:
            if args.only and args.only not in case["name"]:
                continue
            ours = await run_against_ours(case, root)
            theirs = {ref.name: await ref.drive(case, root) for ref in refs}
            norm = case["normalize"]
            detail = first_difference(norm(ours), norm(theirs[primary.name]))
            label = case["name"].ljust(34)
            short = describe(ours)[:58]

            if not detail:
                matched += 1
                emit(f"  match      {label} {short}")
            elif case["declares"]:
                declared += 1
                emit(f"  expected   {label} ours:     {short}")
                emit(f"             {' ' * 34}primary:  "
                             f"{describe(theirs[primary.name])[:58]}")
                for note in case["declares"]:
                    emit(f"             why: {note}")
            else:
                unexpected += 1
                emit(f"  UNEXPECTED {label}")
                emit(f"             ours:     {describe(ours)[:86]}")
                emit(f"             primary:  {describe(theirs[primary.name])[:86]}")
                emit(f"             {detail}")

            # Where the two upstreams disagree with each other is also news.
            if len(refs) > 1 and not case["primary_only"]:
                names = [r.name for r in refs if r is not primary]
                other = names[0]
                cross = first_difference(norm(theirs[primary.name]), norm(theirs[other]))
                if cross:
                    emit(f"             note: {primary.name} and {other} differ: {cross[:80]}")
    finally:
        for ref in refs:
            ref.stop()
        if args.keep:
            print(f"\n[diff] tree kept at {root}")
        else:
            shutil.rmtree(tmp, ignore_errors=True)

    if args.psx:
        await measure_psx(Path(args.psx), paths)

    print(f"\n[diff] {matched} match, {declared} declared deviation, "
          f"{unexpected} unexpected")
    return 1 if unexpected else 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
