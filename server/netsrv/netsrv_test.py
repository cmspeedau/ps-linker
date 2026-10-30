#!/usr/bin/env python3
"""Tests for the in-house ps3netsrv (docs/PS3NETSRV-PROTOCOL.md).

Unit tests for the parts that are pure logic — wire layouts, path rules, the
sector probe, the whitelist — plus an end-to-end pass over a loopback
connection, where the byte layouts and the sandboxing have to hold up together.

    python3 server/netsrv/netsrv_test.py
    PSX_BIN='/Projects/Driver 2 (USA) (Disc 1) (Rev 1).bin' python3 server/netsrv/netsrv_test.py

``netsrv_diff.py`` is the companion script that compares our answers with
upstream's ``ps3netsrv++`` byte for byte.
"""
from __future__ import annotations

import asyncio
import os
import socket
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import netsrv_client as nc
import ps3netsrv as ns
import virtual_iso as viso

# A real PS1 rip, when this machine has one (not in the repo: 647 MB).
PSX_BIN = os.environ.get("PSX_BIN", "")


def loopback_aliases():
    """Whether this machine lets a client bind any address in 127/8.

    Two tests need to look like several consoles at once. Linux answers yes for
    the whole range; macOS only for 127.0.0.1, so there they can only be
    skipped, not weakened.
    """
    with socket.socket() as probe:
        try:
            probe.bind(("127.0.9.9", 0))
        except OSError:
            return False
    return True


LOOPBACK_ALIASES = loopback_aliases()


def param_sfo(title_id: bytes = b"BCES00104") -> bytes:
    """A minimal valid PARAM.SFO, with TITLE_ID in the data table."""
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



def fill(size: int) -> bytes:
    """A byte ramp, so a contiguous read cannot be mistaken for a sector read.

    Consecutive bytes always differ, so no run of ramp bytes can accidentally
    look like ``CD001`` or ``PLAYSTATION `` to the sector probe.
    """
    return bytes(i % 251 for i in range(size))


def raw_image(path: Path, sector: int, size: int) -> None:
    """A file whose sector 16 holds a volume descriptor at a given stride.

    The descriptor sits at ``sector * 16 + 24`` with the ``PLAYSTATION`` tag 8
    bytes into the user area, which is how a raw MODE2/2352 PS1 rip looks.
    """
    path.write_bytes(fill(size))
    with path.open("r+b") as fh:
        fh.seek(sector * 16 + 24)
        fh.write(b"\x01CD001\x00\x00PLAYSTATION ")


def extract(path: Path, stride: int, first: int, count: int) -> bytes:
    """The bytes a READ_CD_2048 of ``count`` sectors must return (§3.3)."""
    with path.open("rb") as fh:
        out = bytearray()
        for sector in range(first, first + count):
            fh.seek(sector * stride + 24)
            out += fh.read(2048)
        return bytes(out)


def plain_image(path: Path, size: int) -> None:
    """A plain 2048-byte-sector image: volume descriptor at ``2048 * 16``."""
    path.write_bytes(fill(size))
    with path.open("r+b") as fh:
        fh.seek(2048 * 16)
        fh.write(b"\x01CD001\x00\x00PLAYSTATION ")


class WireLayout(unittest.TestCase):
    """The struct sizes and field orders the protocol doc commits to."""

    def test_sizes(self):
        self.assertEqual(ns.OPEN_RESULT.size, 16)
        self.assertEqual(ns.INT32.size, 4)
        self.assertEqual(ns.INT64.size, 8)
        self.assertEqual(ns.ENTRY_V1.size, 11)
        self.assertEqual(ns.ENTRY_V2.size, 35)
        self.assertEqual(ns.STAT_RESULT.size, 33)
        self.assertEqual(ns.DIR_RECORD.size, 529)

    def test_field_orders(self):
        # dir entry v1: file_size, fn_len, is_directory — length before flag.
        self.assertEqual(ns.ENTRY_V1.pack(-1, 0, 0),
                         b"\xff" * 8 + b"\x00\x00" + b"\x00")
        self.assertEqual(struct.unpack(">qqqqhb", ns.ENTRY_V2.pack(1, 2, 3, 4, 5, 1)),
                         (1, 2, 3, 4, 5, 1))
        # stat: file_size, mtime, ctime, atime, is_directory — flag last.
        self.assertEqual(ns.STAT_RESULT.unpack(ns.STAT_RESULT.pack(1, 2, 3, 4, 1)),
                         (1, 2, 3, 4, 1))

    def test_everything_is_big_endian(self):
        self.assertEqual(ns.OPEN_RESULT.pack(1, 0),
                         b"\x00" * 7 + b"\x01" + b"\x00" * 8)
        self.assertEqual(ns.INT32.pack(-1), b"\xff\xff\xff\xff")

    def test_dir_record_nul_terminates_and_pads(self):
        record = ns.dir_record(7, 9, 0, b"a.bin")
        self.assertEqual(len(record), 529)
        size, mtime, is_dir, name = ns.DIR_RECORD.unpack(record)
        self.assertEqual((size, mtime, is_dir), (7, 9, 0))
        self.assertEqual(name, b"a.bin" + b"\x00" * 507)

    def test_name_too_long_for_a_record_is_refused_not_truncated(self):
        with self.assertRaises(AssertionError):
            ns.dir_record(0, 0, 0, b"x" * 512)


class PathRules(unittest.TestCase):
    def test_absolute_path_required(self):
        self.assertIsNone(ns.path_parts(b"GAMES/x.iso"))

    def test_parent_components_refused(self):
        for bad in (b"/..", b"/a/../b", b"/a/../../b", b"/a//../b", b"/\\..",
                    b"/a/\\../b"):
            with self.subTest(bad=bad):
                self.assertIsNone(ns.path_parts(bad))

    def test_dot_and_empty_components_dropped(self):
        self.assertEqual(ns.path_parts(b"/a//b/./c"), [b"a", b"b", b"c"])
        self.assertEqual(ns.path_parts(b"/"), [])

    def test_parent_lookalikes_are_directories_not_escapes(self):
        self.assertEqual(ns.path_parts(b"/a/..b"), [b"a", b"..b"])
        self.assertEqual(ns.path_parts(b"/a/..."), [b"a", b"..."])

    def test_backslash_and_trailing_cr(self):
        self.assertEqual(ns.normalize_trailer(b"/a\\b\r"), b"/a/b")
        self.assertEqual(ns.normalize_trailer(b"/a\\b\r\r"), b"/a/b\r")

    def test_non_utf8_names_survive_the_round_trip(self):
        raw = b"/bad\xff\xfename"
        self.assertEqual(ns.path_parts(raw), [b"bad\xff\xfename"])
        self.assertEqual(ns.encode_path(ns.decode_path(raw)), raw)

    def test_virtual_iso_prefixes_recognised(self):
        self.assertEqual(ns.virtual_iso_mode(b"/***PS3***/GAME"), "ps3")
        self.assertEqual(ns.virtual_iso_mode(b"/***DVD***/MOVIE"), "dvd")
        # A bare "/." mounts the whole share; anywhere else it is a component.
        self.assertEqual(ns.virtual_iso_mode(b"/."), "dvd")
        self.assertIsNone(ns.virtual_iso_mode(b"/GAMES/."))
        # The marker has to open the path, exactly like upstream's strncmp.
        self.assertIsNone(ns.virtual_iso_mode(b"/***PS3***"))
        self.assertIsNone(ns.virtual_iso_mode(b"/PS3ISO/GAME"))
        self.assertIsNone(ns.virtual_iso_mode(b"/x/***PS3***/GAME"))
        self.assertEqual(ns.strip_virtual_prefix(b"/***PS3***/GAME"), b"/GAME")
        self.assertEqual(ns.strip_virtual_prefix(b"/***DVD***/"), b"/")
        self.assertEqual(ns.strip_virtual_prefix(b"/PS3ISO/GAME"), b"/PS3ISO/GAME")

    def test_listable(self):
        self.assertFalse(ns.listable("."))
        self.assertFalse(ns.listable(".."))
        self.assertTrue(ns.listable(".hidden"))          # hidden files are listed
        self.assertTrue(ns.listable("x" * 255))          # NAME_MAX is 255 anyway
        self.assertFalse(ns.listable("x" * 256))


class SectorProbe(unittest.TestCase):
    def test_only_cd_sized_images_are_probed(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "img.bin"
            raw_image(path, 2352, ns.PROBE_MIN + 4096)
            with path.open("rb") as fh:
                for size in (ns.PROBE_MIN - 1, ns.PROBE_MAX + 1):
                    with self.subTest(size=size):
                        self.assertEqual(ns.detect_sector_size(fh, size), 2352)

    def test_raw_mode2_2352(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "raw2352.bin"
            raw_image(path, 2352, ns.PROBE_MIN + 4096)
            with path.open("rb") as fh:
                self.assertEqual(ns.detect_sector_size(fh, path.stat().st_size), 2352)

    def test_plain_2048_image_is_detected(self):
        """Upstream misses this one and streams it at the wrong stride (§5)."""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "plain2048.iso"
            plain_image(path, ns.PROBE_MIN + 4096)
            with path.open("rb") as fh:
                self.assertEqual(ns.detect_sector_size(fh, path.stat().st_size), 2048)

    def test_odd_strides(self):
        for sector in (2336, 2448, 2328, 2368, 2340):
            with self.subTest(sector=sector):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "odd.bin"
                    raw_image(path, sector, ns.PROBE_MIN + 8192)
                    with path.open("rb") as fh:
                        self.assertEqual(ns.detect_sector_size(fh, path.stat().st_size),
                                         sector)

    def test_no_probe_result_when_sector_16_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "tiny.bin"
            path.write_bytes(b"\x01CD001")
            with path.open("rb") as fh:
                self.assertEqual(ns.detect_sector_size(fh, ns.PROBE_MIN), 2352)

    @unittest.skipUnless(PSX_BIN and os.path.exists(PSX_BIN), "set PSX_BIN")
    def test_real_ps1_rip_reads_at_2352(self):
        size = os.path.getsize(PSX_BIN)
        with open(PSX_BIN, "rb") as fh:
            self.assertEqual(ns.detect_sector_size(fh, size), 2352)


# --------------------------------------------------------------------------
# Virtual ISO 9660 images
# --------------------------------------------------------------------------

def iso_entries(image: bytes, start: int, size: int):
    """Directory records of one directory, in on-disk order."""
    base = start * 2048
    position = 0
    while position < size:
        length = image[base + position]
        if length == 0:
            # Records never cross a sector boundary; skip the rest of it.
            position = (position // 2048 + 1) * 2048
            continue
        record = image[base + position:base + position + length]
        yield {
            "start": int.from_bytes(record[2:6], "little"),
            "size": int.from_bytes(record[10:14], "little"),
            "is_dir": bool(record[25] & 2),
            "name": record[33:33 + record[32]],
        }
        position += length


def iso_lookup(image: bytes, path, joliet: bool = False):
    """Walk an ISO 9660 image the way a reader would, and return the record."""
    pvd = image[(17 if joliet else 16) * 2048:]
    record = {
        "start": int.from_bytes(pvd[156 + 2:156 + 6], "little"),
        "size": int.from_bytes(pvd[156 + 10:156 + 14], "little"),
        "is_dir": True,
        "name": b"\0",
    }
    for component in path:
        if joliet:
            wanted = [component.decode("utf8").encode("utf-16-be"),
                      (component.decode("utf8") + ";1").encode("utf-16-be")]
        else:
            wanted = [component.upper(), component.upper() + b";1"]
        for entry in iso_entries(image, record["start"], record["size"]):
            if entry["name"] in wanted:
                record = entry
                break
        else:
            raise KeyError(component)
    return record


def iso_file(image: bytes, path, joliet: bool = False) -> bytes:
    record = iso_lookup(image, path, joliet)
    start = record["start"] * 2048
    return image[start:start + record["size"]]


class VirtualIsoTest(unittest.TestCase):
    """The synthesised filesystem, checked with an ISO reader of our own."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name) / "Game Folder"
        (self.root / "PS3_GAME" / "USRDIR").mkdir(parents=True)
        (self.root / "PS3_GAME" / "PARAM.SFO").write_bytes(param_sfo())
        (self.root / "PS3_GAME" / "USRDIR" / "EBOOT.BIN").write_bytes(b"boot" * 800)
        (self.root / "PS3_GAME" / "USRDIR" / "ICON0.PNG").write_bytes(b"\x89PNG" + b"z" * 5000)
        (self.root / "PS3_DISC.SFB").write_bytes(b"disc")
        (self.root / "empty.dat").write_bytes(b"")
        (self.root / "a.bin").write_bytes(bytes(range(256)) * 9)
        (self.root / "unicode-\u00e9.txt").write_bytes(b"accent")
        (self.root / "SPLIT.BIN.66600").write_bytes(b"A" * 4096)
        (self.root / "SPLIT.BIN.66601").write_bytes(b"B" * 1000)
        (self.root / "deep" / "one" / "two").mkdir(parents=True)
        (self.root / "deep" / "one" / "two" / "far.dat").write_bytes(b"far away")

    def tearDown(self):
        self.tmp.cleanup()

    def image(self, ps3=True, **kwargs):
        iso = viso.VirtualIso(self.root, ps3, **kwargs)
        self.addCleanup(iso.close)
        return iso, iso.read_at(0, iso.size)

    def test_volume_descriptors_and_disc_info(self):
        iso, image = self.image()
        self.assertEqual(len(image) % 2048, 0)
        self.assertEqual(image[16 * 2048:16 * 2048 + 7], b"\x01CD001\x01")
        self.assertEqual(image[17 * 2048:17 * 2048 + 7], b"\x02CD001\x01")
        self.assertEqual(image[18 * 2048:18 * 2048 + 6], b"\xffCD001")
        self.assertEqual(image[2048:2060], b"PlayStation3")
        self.assertEqual(image[2048 + 0x10:2048 + 0x30],
                         b"BCES-00104" + b" " * 22)
        # Volume space size (both byte orders) equals the image in sectors.
        self.assertEqual(int.from_bytes(image[16 * 2048 + 80:16 * 2048 + 84], "little"),
                         len(image) // 2048)
        self.assertEqual(int.from_bytes(image[16 * 2048 + 84:16 * 2048 + 88], "big"),
                         len(image) // 2048)

    def test_files_are_readable_through_the_filesystem(self):
        _, image = self.image()
        self.assertEqual(iso_file(image, [b"PS3_DISC.SFB"]), b"disc")
        self.assertEqual(iso_file(image, [b"PS3_GAME", b"USRDIR", b"EBOOT.BIN"]),
                         b"boot" * 800)
        self.assertEqual(iso_file(image, [b"a.bin"]), bytes(range(256)) * 9)
        self.assertEqual(iso_file(image, [b"empty.dat"]), b"")
        self.assertEqual(iso_file(image, [b"deep", b"one", b"two", b"far.dat"]),
                         b"far away")

    def test_joliet_names_keep_their_case_and_accents(self):
        _, image = self.image()
        self.assertEqual(iso_file(image, [b"unicode-\xc3\xa9.txt"], joliet=True),
                         b"accent")
        self.assertEqual(iso_lookup(image, [b"a.bin"], joliet=True)["size"], 9 * 256)

    def test_666_split_files_are_one_logical_file(self):
        iso, image = self.image()
        self.assertEqual(iso_lookup(image, [b"SPLIT.BIN"])["size"], 5096)
        self.assertEqual(iso_file(image, [b"SPLIT.BIN"]),
                         b"A" * 4096 + b"B" * 1000)
        root = iso_lookup(image, [])
        names = [e["name"] for e in iso_entries(image, root["start"], root["size"])]
        self.assertNotIn(b"SPLIT.BIN.66601;1", names)
        self.assertEqual(sum(1 for e in iso._extents if e[3].name.startswith("SPLIT")), 2)

    def test_ps3_mode_needs_a_title_id(self):
        (self.root / "PS3_GAME" / "PARAM.SFO").write_bytes(b"not an sfo")
        with self.assertRaises(OSError):
            viso.VirtualIso(self.root, True)

    def test_dvd_mode_uses_the_folder_name_and_no_ps3_sectors(self):
        iso, image = self.image(ps3=False)
        self.assertEqual(image[2048:2056], b"\0" * 8)
        self.assertEqual(image[16 * 2048 + 40:16 * 2048 + 72].rstrip(b" "),
                         b"GAME FOLDER")
        self.assertEqual(iso.mtime, int(self.root.stat().st_mtime))

    def test_reads_are_clamped_to_the_image(self):
        iso, _ = self.image()
        self.assertEqual(iso.read_at(iso.size, 10), b"")
        self.assertEqual(len(iso.read_at(iso.size - 10, 100)), 10)
        self.assertEqual(iso.read_at(1024, 0), b"")

    def test_padding_between_files_and_at_the_end_is_zero(self):
        iso, image = self.image()
        # The last file ends before the final 32-sector pad.
        self.assertEqual(image[-2048:], b"\0" * 2048)
        self.assertTrue(iso.read_at(iso.size - 32 * 2048, 2048 * 32) == b"\0" * (2048 * 32))

    def test_symlinks_and_special_files_are_not_in_the_image(self):
        outside = Path(self.tmp.name) / "outside.txt"
        outside.write_bytes(b"secret")
        os.symlink(outside, self.root / "leak.bin")
        _, image = self.image()
        root = iso_lookup(image, [])
        names = [e["name"] for e in iso_entries(image, root["start"], root["size"])]
        self.assertNotIn(b"LEAK.BIN;1", names)
        self.assertEqual(iso_file(image, [b"PS3_DISC.SFB"]), b"disc")

    def test_a_file_past_4_GiB_gets_one_record_per_extent(self):
        # A 4 GiB fixture is not worth building; the record split is arithmetic.
        big = viso.Entry(name=b"BIG.BIN", path=self.root / "a.bin",
                        size=viso.MULTIEXTENT_PART * 2 + 1000, mtime=0,
                        parts=[(self.root / "a.bin", 0)])
        self.assertEqual(viso._extent_sizes(big),
                         [viso.MULTIEXTENT_PART, viso.MULTIEXTENT_PART, 1000])
        self.assertEqual(sum(viso._extent_sizes(big)), big.size)
        # An ordinary file is one record, however many parts it is split over.
        split = viso.Entry(name=b"SPLIT.BIN", path=self.root / "a.bin",
                           size=5096, mtime=0,
                           parts=[(self.root / "a.bin", 4096),
                                  (self.root / "a.bin", 1000)])
        self.assertEqual(viso._extent_sizes(split), [5096])

    def test_directory_content_matches_the_size_it_declares(self):
        # The size a directory advertises in its own record has to be the size
        # it was built to, or a client walks off the end of it.
        iso, _ = self.image()
        for directory in iso._dirs:
            self.assertEqual(directory.size, len(directory.content))
            self.assertEqual(directory.size_joliet, len(directory.content_joliet))
            self.assertEqual(directory.size % 2048, 0)

    def test_a_read_across_the_end_of_a_file_is_data_then_padding(self):
        iso, image = self.image()
        record = iso_lookup(image, [b"a.bin"])
        start = record["start"] * 2048
        data = bytes(range(256)) * 9
        self.assertEqual(iso.read_at(start, 100), data[:100])
        # a.bin is 2304 bytes in a 4096-byte area: the rest of the last sector
        # is padding, and the next file's bytes are not a continuation of it.
        self.assertEqual(record["size"], len(data))
        self.assertEqual(iso.read_at(start + 2000, 2000),
                         data[2000:] + b"\0" * (2000 - (len(data) - 2000)))
        self.assertEqual(iso.read_at(start, 4096),
                         data + b"\0" * (4096 - len(data)))
        self.assertEqual(image[start:start + 4096],
                         data + b"\0" * (4096 - len(data)))

    def test_a_read_across_a_split_file_boundary_joins_the_parts(self):
        iso, image = self.image()
        record = iso_lookup(image, [b"SPLIT.BIN"])
        start = record["start"] * 2048
        joined = b"A" * 4096 + b"B" * 1000
        # 100 bytes either side of the seam between .66600 and .66601.
        self.assertEqual(iso.read_at(start + 4096 - 100, 200), joined[3996:4196])
        self.assertEqual(iso_file(image, [b"SPLIT.BIN"]), joined)
        self.assertEqual(image[start:start + len(joined)], joined)


class Whitelist(unittest.TestCase):
    def test_patterns(self):
        self.assertIsNone(ns.parse_whitelist(""))
        self.assertEqual(ns.parse_whitelist("192.168.1.*"), (0xC0A80100, 0xC0A801FF))
        self.assertEqual(ns.parse_whitelist("*.*.*.*"), (0, 0xFFFFFFFF))
        self.assertEqual(ns.parse_whitelist("10.0.0.7"), (0x0A000007, 0x0A000007))

    def test_matches(self):
        bounds = ns.parse_whitelist("192.168.1.*")
        self.assertTrue(ns.in_whitelist("192.168.1.44", bounds))
        self.assertFalse(ns.in_whitelist("192.168.2.44", bounds))
        self.assertTrue(ns.in_whitelist("8.8.8.8", None))
        self.assertFalse(ns.in_whitelist("::1", bounds))

    def test_bad_patterns_exit(self):
        for bad in ("192.168.1", "192.168.1.999", "a.b.c.d", "192.168.1.*.*"):
            with self.subTest(bad=bad):
                with self.assertRaises(SystemExit):
                    ns.parse_whitelist(bad)


# --------------------------------------------------------------------------
# End to end, over a real socket
# --------------------------------------------------------------------------

ODD_NAME = b"bad\xff\xfename"          # not valid UTF-8
GAME_ISO = b"PS3ISO/game.iso"
RAW_BIN = b"PSXISO/raw2352.bin"
PLAIN_ISO = b"PSXISO/plain2048.iso"
VISO_PS3 = b"VISO/BCES00104"            # a folder game, for /***PS3***/ (§7.4)
VISO_DVD = b"VISO/Movie Disc"           # a folder, for /***DVD***/


class EndToEnd(unittest.TestCase):
    """A loopback client against the real server, read-only and writable."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        addCleanup = cls.tmp  # noqa: F841 (name kept for the reader)
        cls.base = Path(cls.tmp.name)
        cls.root = cls.base / "share"
        cls.root.mkdir()
        for rel in (GAME_ISO, RAW_BIN, PLAIN_ISO, b"GAMES/.hidden",
                    b"GAMES/notes.txt", b"GAMES/sub/deep.dat",
                    b"GAMES/sub/deeper/deepest.dat"):
            path = cls.root / ns.decode_path(rel)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"filler")
        (cls.root / ns.decode_path(GAME_ISO)).write_bytes(bytes(range(256)) * 64)
        (cls.root / "GAMES/notes.txt").write_bytes(b"hello world")
        raw_image(cls.root / ns.decode_path(RAW_BIN), 2352, ns.PROBE_MIN + 4096)
        plain_image(cls.root / ns.decode_path(PLAIN_ISO), ns.PROBE_MIN + 4096)
        # A file outside the share, and two symlinks that point at it and at a
        # file inside: neither may be readable through the share.
        (cls.base / "outside-secret.txt").write_bytes(b"do not read me")
        os.symlink(cls.base / "outside-secret.txt", cls.root / "GAMES/escape.iso")
        os.symlink(cls.root / "GAMES/notes.txt", cls.root / "GAMES/linked.txt")
        # A name that is not valid UTF-8, on a filesystem that will hold one.
        # APFS refuses it (Errno 92), so the check on it is a bonus, not a
        # requirement: the share still has to work without it.
        cls.odd_name = None
        try:
            (cls.root / "GAMES").joinpath(os.fsdecode(ODD_NAME)).write_bytes(b"odd")
            cls.odd_name = ODD_NAME
        except OSError:
            pass
        # Folder games, for the /***PS3***/ and /***DVD***/ mounts (§7.4).
        game = cls.root / ns.decode_path(VISO_PS3)
        (game / "PS3_GAME" / "USRDIR").mkdir(parents=True)
        (game / "PS3_GAME" / "PARAM.SFO").write_bytes(param_sfo())
        (game / "PS3_GAME" / "USRDIR" / "EBOOT.BIN").write_bytes(fill(6000))
        (game / "PS3_DISC.SFB").write_bytes(b"disc")
        movie = cls.root / ns.decode_path(VISO_DVD)
        (movie / "VIDEO_TS").mkdir(parents=True)
        (movie / "VIDEO_TS" / "VIDEO_TS.IFO").write_bytes(b"ifodata")
        (cls.root / "VISO/NoSfo").mkdir(parents=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def setUp(self):
        self.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(self.loop)
        self.servers = []
        self.clients = []

    def tearDown(self):
        for client in self.clients:
            client.close()
        for srv in self.servers:
            self.loop.run_until_complete(_close(srv))
        self.loop.run_until_complete(self.loop.shutdown_asyncgens())
        self.loop.close()
        asyncio.set_event_loop(None)

    def run_async(self, coro):
        return self.loop.run_until_complete(coro)

    def start(self, writable=False):
        """Bind the server's connection callback on an ephemeral port.

        ``NetisoServer.serve`` never returns, so tests use ``start_server``
        directly and read the port off the listening socket.
        """
        async def bind():
            server = ns.NetisoServer(ns.Share(self.root), writable=writable)
            srv = await asyncio.start_server(server.connection, "127.0.0.1", 0,
                                             backlog=1, reuse_address=True)
            self.servers.append(srv)
            return server, srv.sockets[0].getsockname()[1]

        return self.run_async(bind())

    def client(self, port, source="127.0.0.1"):
        client = nc.Netiso("127.0.0.1", port, timeout=10,
                           local_addr=(source, 0))
        self.clients.append(client)
        return self.run_async(client.connect())

    def listing(self, client, path=b"/GAMES"):
        self.assertEqual(self.run_async(client.open_dir(path)), 0)
        return self.run_async(client.read_dir())

    # -- OPEN_FILE / reads ----------------------------------------------------

    def test_open_file_reports_size_and_mtime(self):
        _, port = self.start()
        client = self.client(port)
        size, mtime = self.run_async(client.open_file(b"/" + GAME_ISO))
        st = (self.root / ns.decode_path(GAME_ISO)).stat()
        self.assertEqual(size, st.st_size)
        self.assertEqual(mtime, int(st.st_mtime))

    def test_missing_file_is_minus_one_but_stays_open(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_file(b"/nope.iso"))[0], -1)
        self.assertEqual(self.run_async(client.open_file(b"/GAMES/notes.txt"))[0], 11)

    def test_closefile(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_file(b"/" + GAME_ISO))
        self.assertEqual(self.run_async(client.close_file()), (-1, 0))
        self.assertEqual(self.run_async(client.stat(b"/GAMES/notes.txt"))["size"], 11)

    def test_zero_length_path_is_an_error(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_file(b""))[0], -1)
        self.run_async(client.raw(nc.path_trailer(ns.STAT_FILE, b"")))
        self.assertEqual(ns.STAT_RESULT.unpack(
            self.run_async(client.read_raw(33)))[0], -1)

    def test_reads_match_the_file(self):
        _, port = self.start()
        client = self.client(port)
        body = (self.root / ns.decode_path(GAME_ISO)).read_bytes()
        self.run_async(client.open_file(b"/" + GAME_ISO))
        self.assertEqual(self.run_async(client.read_file(0, len(body))), body)
        self.assertEqual(self.run_async(client.read_file_critical(3, 100)), body[3:103])
        # A read larger than the server's chunk size must still come back whole.
        self.assertEqual(self.run_async(client.read_file_critical(0, len(body))), body)
        self.assertEqual(self.run_async(client.read_file(len(body), 16)), b"")
        self.assertEqual(self.run_async(client.read_file_status(0, 0))[0], 0)

    def test_read_file_over_the_buffer_answers_minus_one_and_stays_open(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_file(b"/" + GAME_ISO))
        self.run_async(client.raw(struct.pack(">HHIQ", ns.READ_FILE, 0,
                                              ns.BUFFER_SIZE + 1, 0)))
        self.assertEqual(ns.READ_FILE_RESULT.unpack(
            self.run_async(client.read_raw(4)))[0], -1)
        self.assertEqual(self.run_async(client.stat(b"/GAMES/notes.txt"))["size"], 11)

    def test_critical_read_without_a_file_closes(self):
        _, port = self.start()
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.read_file_critical(0, 16))

    def test_cd_read_follows_the_probed_stride(self):
        _, port = self.start()
        client = self.client(port)
        body = (self.root / ns.decode_path(RAW_BIN)).read_bytes()
        self.run_async(client.open_file(b"/" + RAW_BIN))
        got = self.run_async(client.read_cd_2048(16, 2))
        self.assertEqual(len(got), 2 * 2048)
        self.assertEqual(got[:6], b"\x01CD001")
        self.assertEqual(got, extract(self.root / ns.decode_path(RAW_BIN), 2352, 16, 2))
        # A contiguous slice would be wrong, and equal only by luck here.
        self.assertNotEqual(got, body[16 * 2352 + 24:][:2 * 2048])

    def test_plain_2048_image_is_streamed_at_its_own_stride(self):
        _, port = self.start()
        client = self.client(port)
        body = (self.root / ns.decode_path(PLAIN_ISO)).read_bytes()
        self.run_async(client.open_file(b"/" + PLAIN_ISO))
        got = self.run_async(client.read_cd_2048(16, 1))
        self.assertEqual(got, extract(self.root / ns.decode_path(PLAIN_ISO), 2048, 16, 1))

    def test_cd_read_over_the_buffer_closes(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_file(b"/" + RAW_BIN))
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.read_cd_2048(0, 2049))

    def test_opening_a_new_file_resets_the_sector_size(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_file(b"/" + RAW_BIN))
        self.run_async(client.open_file(b"/GAMES/notes.txt"))
        # notes.txt is far too small to probe, so a CD read is out of range of
        # the file rather than mis-strided: the server must not answer garbage.
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.read_cd_2048(0, 1))

    # -- STAT ----------------------------------------------------------------

    def test_stat_file(self):
        _, port = self.start()
        client = self.client(port)
        st = self.run_async(client.stat(b"/GAMES/notes.txt"))
        self.assertEqual((st["size"], st["is_dir"]), (11, 0))
        self.assertEqual(st["mtime"], int((self.root / "GAMES/notes.txt").stat().st_mtime))
        directory = self.run_async(client.stat(b"/GAMES"))
        self.assertEqual((directory["size"], directory["is_dir"]), (0, 1))
        missing = self.run_async(client.stat(b"/nothing/here"))
        self.assertEqual((missing["size"], missing["is_dir"]), (-1, 0))

    def test_stat_of_a_symlink_does_not_follow_it(self):
        _, port = self.start()
        client = self.client(port)
        st = self.run_async(client.stat(b"/GAMES/escape.iso"))
        self.assertEqual(st["is_dir"], 0)
        self.assertNotEqual(st["size"], (self.base / "outside-secret.txt").stat().st_size)

    # -- virtual ISO (§7.4) ---------------------------------------------------

    def mount(self, client, path: bytes) -> tuple[bytes, int]:
        """OPEN_FILE on a folder, and the whole image the server answers with."""
        size, mtime = self.run_async(client.open_file(path))
        self.assertGreater(size, 0, path)
        self.assertEqual(size % 2048, 0)
        image = self.run_async(client.read_file(0, size))
        self.assertEqual(len(image), size)
        return image, mtime

    def test_a_folder_game_mounts_as_an_iso(self):
        _, port = self.start()
        client = self.client(port)
        image, mtime = self.mount(client, b"/***PS3***/" + VISO_PS3)
        self.assertEqual(image[2048:2060], b"PlayStation3")
        self.assertEqual(image[2048 + 0x10:2048 + 0x1A], b"BCES-00104")
        self.assertEqual(image[16 * 2048:16 * 2048 + 7], b"\x01CD001\x01")
        folder = self.root / ns.decode_path(VISO_PS3)
        self.assertEqual(mtime, int(folder.stat().st_mtime))
        root = iso_lookup(image, [])
        names = [e["name"] for e in iso_entries(image, root["start"], root["size"])]
        self.assertIn(b"PS3_GAME", names)
        self.assertIn(b"PS3_DISC.SFB;1", names)
        self.assertEqual(iso_file(image, [b"PS3_GAME", b"USRDIR", b"EBOOT.BIN"]),
                         fill(6000))

    def test_a_dvd_folder_mounts_under_its_own_name(self):
        _, port = self.start()
        client = self.client(port)
        image, _ = self.mount(client, b"/***DVD***/" + VISO_DVD)
        # DVD mode writes no PS3 disc info, and names the volume after the folder.
        self.assertEqual(image[2048:2056], b"\0" * 8)
        self.assertEqual(image[16 * 2048 + 40:16 * 2048 + 72].rstrip(b" "),
                         b"MOVIE DISC")
        self.assertEqual(iso_file(image, [b"VIDEO_TS", b"VIDEO_TS.IFO"]), b"ifodata")

    def test_a_cd_read_of_a_virtual_iso_has_no_raw_header(self):
        # A synthesised image is plain 2048-byte sectors, so sector 16 is at
        # 16*2048 exactly — there is nothing to skip (§7.4).
        _, port = self.start()
        client = self.client(port)
        image, _ = self.mount(client, b"/***PS3***/" + VISO_PS3)
        self.assertEqual(self.run_async(client.read_cd_2048(16, 2)),
                         image[16 * 2048:18 * 2048])

    def test_a_folder_without_a_param_sfo_is_not_mounted(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_file(b"/***PS3***/VISO/NoSfo"))[0], -1)
        # Refused like a missing file, so the connection stays usable.
        self.assertEqual(self.run_async(client.stat(b"/GAMES/notes.txt"))["size"], 11)

    def test_the_share_root_mounts_as_an_iso(self):
        # A bare "/." is how upstream lets a client mount the whole share, and
        # it is DVD mode: the volume is named after the share directory and the
        # PS3 disc-info sector stays empty (§3.1, §7.4).
        _, port = self.start()
        client = self.client(port)
        size, _ = self.run_async(client.open_file(b"/."))
        self.assertGreater(size, 0)
        self.assertEqual(size % 2048, 0)
        # The header and the directory blocks, without reading the files in them.
        head = self.run_async(client.read_file(0, 0x14000))
        self.assertEqual(head[16 * 2048 + 40:16 * 2048 + 72].rstrip(b" "), b"SHARE")
        self.assertEqual(head[2048:2060], b"\0" * 12)
        root = iso_lookup(head, [])
        names = {e["name"] for e in iso_entries(head, root["start"], root["size"])}
        wanted = {b"GAMES", b"PS3ISO", b"PSXISO", b"VISO"}
        self.assertEqual(wanted & names, wanted)

    def test_a_virtual_path_cannot_escape_the_share(self):
        _, port = self.start()
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.open_file(b"/***PS3***/../outside-secret.txt"))

    def test_the_marker_mounts_for_open_file_only(self):
        # Upstream looks for the marker in OPEN_FILE alone (``translate_path``
        # gets a null viso pointer everywhere else), so for every other command
        # the marker is a path component that is not there (§7.4).
        _, port = self.start()
        client = self.client(port)
        st = self.run_async(client.stat(b"/***PS3***/" + VISO_PS3))
        self.assertEqual((st["size"], st["is_dir"]), (-1, 0))
        self.assertEqual(self.run_async(client.get_dir_size(b"/***PS3***/" + VISO_PS3)), -1)
        self.assertEqual(self.run_async(client.open_dir(b"/***DVD***/" + VISO_DVD)), -1)
        # The folder is still there under its own name, marker or no marker.
        plain = self.run_async(client.stat(b"/" + VISO_PS3))
        self.assertEqual(plain["is_dir"], 1)

    # -- directory listing ----------------------------------------------------

    def test_read_dir_entry_listing(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_dir(b"/GAMES")), 0)
        entries = self.run_async(client.collect_entries())
        names = [e[-1] for e in entries]
        self.assertEqual(names, sorted(names) if ns.LISTING_SORTED else names)
        self.assertIn(b"notes.txt", names)
        self.assertIn(b".hidden", names)
        if self.odd_name is not None:
            self.assertIn(ODD_NAME, names)
        self.assertIn(b"sub", names)
        for size, fn_len, is_dir, name in entries:
            self.assertEqual(fn_len, len(name))
            self.assertEqual(size, 0 if is_dir else size)
            self.assertLessEqual(len(name), ns.MAX_FILE_LEN)

    def test_read_dir_entry_v2_carries_timestamps(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_dir(b"/GAMES"))
        entry = self.run_async(client.read_dir_entry_v2())
        size, mtime, ctime, atime, fn_len, is_dir, name = entry
        self.assertEqual(fn_len, len(name))
        self.assertGreater(mtime, 0)
        self.assertGreaterEqual(ctime, 0)
        self.assertGreaterEqual(atime, 0)
        self.assertEqual(size, 0 if is_dir else size)

    def test_v1_name_follows_the_header_on_the_same_connection(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_dir(b"/GAMES"))
        self.run_async(client.raw(client.header(ns.READ_DIR_ENTRY)))
        size, fn_len, is_dir = ns.ENTRY_V1.unpack(self.run_async(client.read_raw(11)))
        self.assertNotEqual(size, -1)
        name = self.run_async(client.read_raw(fn_len))
        self.assertEqual(len(name), fn_len)
        self.assertNotIn(b"\x00", name)

    def test_listing_ends_with_the_sentinel_and_closes(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_dir(b"/GAMES/sub"))
        entries = self.run_async(client.collect_entries())
        self.assertEqual(sorted(e[-1] for e in entries), [b"deep.dat", b"deeper"])
        # Past the end, without a fresh OPEN_DIR, the answer stays -1.
        self.assertIsNone(self.run_async(client.read_dir_entry()))

    def test_read_dir_flat(self):
        _, port = self.start()
        client = self.client(port)
        records = self.listing(client)
        names = [r["name"] for r in records]
        self.assertIn(b"notes.txt", names)
        self.assertIn(b"sub", names)                 # flat lists directories too
        self.assertNotIn(b"sub/deep.dat", names)
        by_name = {r["name"]: r for r in records}
        self.assertEqual(by_name[b"notes.txt"]["size"], 11)
        self.assertEqual(by_name[b"sub"]["size"], 0)
        self.assertEqual(by_name[b"sub"]["is_dir"], 1)
        for record in records:
            self.assertGreater(record["mtime"], 0)
            self.assertLess(len(record["name"]), ns.NAME_FIELD)

    def test_read_dir_recursive(self):
        _, port = self.start()
        client = self.client(port)
        records = self.listing(client, b"/GAMES//")
        names = [r["name"] for r in records]
        self.assertIn(b"sub/deep.dat", names)
        self.assertIn(b"sub/deeper/deepest.dat", names)   # full depth (§9.4)
        self.assertNotIn(b"sub", names)                   # dirs become paths
        self.assertNotIn(b"sub/deeper", names)

    def test_read_dir_consumes_the_handle(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.open_dir(b"/GAMES"))
        self.assertTrue(self.run_async(client.read_dir()))
        self.assertEqual(self.run_async(client.read_dir()), [])

    def test_read_dir_without_open_dir_answers_zero(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.read_dir()), [])

    def test_open_dir_rejects_files_and_missing_paths(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_dir(b"/GAMES/notes.txt")), -1)
        self.assertEqual(self.run_async(client.open_dir(b"/GAMES/nope")), -1)

    def test_get_dir_size_sums_recursively(self):
        _, port = self.start()
        client = self.client(port)
        expected = sum(p.lstat().st_size
                       for p in (self.root / "GAMES").rglob("*")
                       if p.is_file() and not p.is_symlink())
        self.assertEqual(self.run_async(client.get_dir_size(b"/GAMES")), expected)
        self.assertEqual(self.run_async(client.get_dir_size(b"/GAMES/sub/deeper")),
                         (self.root / "GAMES/sub/deeper/deepest.dat").stat().st_size)
        self.assertEqual(self.run_async(client.get_dir_size(b"/nope")), -1)

    # -- sandboxing -----------------------------------------------------------

    def test_escape_spellings_are_refused(self):
        """None of these may be read, and each ends the connection (§7.1)."""
        for bad in (b"/../outside-secret.txt", b"/GAMES/../../outside-secret.txt",
                    b"/GAMES//../outside-secret.txt", b"/\\..\\outside-secret.txt",
                    b"/GAMES/sub/../../outside-secret.txt", b"no-leading-slash"):
            with self.subTest(bad=bad):
                _, port = self.start()
                client = self.client(port)
                with self.assertRaises(nc.ConnectionClosed):
                    self.run_async(client.stat(bad))
                self.assertFalse((self.base / "outside-secret.txt").read_bytes() == b"")

    def test_escape_over_open_dir_and_get_dir_size_closes_the_connection(self):
        """An untranslatable path is fatal, as it is upstream (§7.1)."""
        _, port = self.start()
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.open_dir(b"/../etc"))
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.get_dir_size(b"/GAMES/../../"))

    def test_symlinks_cannot_be_opened(self):
        _, port = self.start()
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_file(b"/GAMES/escape.iso"))[0], -1)
        self.assertEqual(self.run_async(client.open_file(b"/GAMES/linked.txt"))[0], -1)

    def test_oversized_path_trailer_closes(self):
        _, port = self.start()
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.raw(nc.path_trailer(ns.STAT_FILE, b"x" * 511)))
            self.run_async(client.read_raw(33))

    def test_unknown_opcode_closes(self):
        _, port = self.start()
        client = self.client(port)
        self.run_async(client.expect_closed(client.header(0x2412)))

    def test_a_truncated_header_closes_once_the_client_stops_writing(self):
        """Two bytes of a 16-byte header, then nothing more: no command to run."""
        _, port = self.start()
        client = self.client(port)
        client.writer.write(b"\x12\x24")
        self.run_async(client.writer.drain())
        # Half-close (the drain above left nothing pending), so the server sees
        # the end of the stream rather than a client it must keep waiting for.
        client.writer.write_eof()
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.read_raw(16))

    # -- writes ---------------------------------------------------------------

    def test_read_only_server_refuses_write_commands(self):
        _, port = self.start(writable=False)
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.create_file(b"/GAMES/new.bin"))

    def test_read_only_server_still_lists_directories(self):
        """Upstream's READ_ONLY build loses directory listing; we keep it (§9.1)."""
        _, port = self.start(writable=False)
        client = self.client(port)
        self.assertEqual(self.run_async(client.open_dir(b"/GAMES")), 0)
        self.assertTrue(self.run_async(client.read_dir()))
        _, port = self.start(writable=False)
        client = self.client(port)
        self.run_async(client.open_dir(b"/GAMES"))
        self.assertIsNotNone(self.run_async(client.read_dir_entry()))

    def test_writable_round_trip(self):
        _, port = self.start(writable=True)
        client = self.client(port)
        self.assertEqual(self.run_async(client.mkdir(b"/UPLOADS")), 0)
        self.assertEqual(self.run_async(client.mkdir(b"/UPLOADS")), -1)   # exists
        self.assertEqual(self.run_async(client.create_file(b"/UPLOADS/out.bin")), 0)
        payload = os.urandom(1 << 20)
        self.assertEqual(self.run_async(client.write_file(payload)), len(payload))
        self.assertEqual((self.root / "UPLOADS/out.bin").read_bytes(), payload)
        self.assertEqual(self.run_async(client.delete_file(b"/UPLOADS/out.bin")), 0)
        self.assertEqual(self.run_async(client.delete_file(b"/UPLOADS/out.bin")), -1)
        self.assertEqual(self.run_async(client.rmdir(b"/UPLOADS")), 0)
        self.assertFalse((self.root / "UPLOADS").exists())
        self.assertEqual(self.run_async(client.rmdir(b"/UPLOADS")), -1)

    def test_create_truncates(self):
        _, port = self.start(writable=True)
        client = self.client(port)
        (self.root / "trunc.bin").write_bytes(b"x" * 100)
        self.assertEqual(self.run_async(client.create_file(b"/trunc.bin")), 0)
        self.assertEqual((self.root / "trunc.bin").stat().st_size, 0)

    def test_create_on_a_directory_is_refused(self):
        """Both upstream binaries answer -1 here, so we do too (§3.5)."""
        _, port = self.start(writable=True)
        client = self.client(port)
        self.assertEqual(self.run_async(client.create_file(b"/GAMES")), -1)
        # ...and the connection is still usable, because nothing was opened.
        self.assertEqual(self.run_async(client.write_file(b"data")), -1)

    def test_write_commands_cannot_escape_either(self):
        """A refused path is fatal, as upstream, and nothing is touched."""
        _, port = self.start(writable=True)
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.create_file(b"/../outside.bin"))
        client = self.client(port)
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(client.delete_file(b"/../outside-secret.txt"))
        self.assertFalse((self.base / "outside.bin").exists())
        self.assertTrue((self.base / "outside-secret.txt").exists())

    def test_write_without_create_answers_minus_one(self):
        _, port = self.start(writable=True)
        client = self.client(port)
        self.assertEqual(self.run_async(client.write_file(b"data")), -1)

    # -- connection policy ----------------------------------------------------

    def test_same_ip_takes_over_its_own_slot(self):
        """A reconnecting PS3 must not be locked out by its own stale socket."""
        server, port = self.start()
        first = self.client(port)
        self.run_async(first.open_file(b"/" + GAME_ISO))
        self.assertEqual(server.slots, 1)

        second = nc.Netiso("127.0.0.1", port, timeout=10)
        self.run_async(second.connect())
        self.assertEqual(self.run_async(second.open_file(b"/GAMES/notes.txt"))[0], 11)
        # The first connection is now dead.
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(first.open_file(b"/GAMES/notes.txt"))
        second.close()

    def test_more_than_max_clients_is_refused(self):
        """Distinct source IPs, so the same-IP takeover rule does not collapse
        the slots: 127.0.0.0/8 is all local on Linux."""
        if not LOOPBACK_ALIASES:
            self.skipTest("this machine will not bind a second loopback address")
        _, port = self.start()
        clients = [self.client(port, source=f"127.0.0.{i + 1}")
                   for i in range(ns.MAX_CLIENTS)]
        self.run_async(asyncio.gather(*[c.stat(b"/GAMES/notes.txt") for c in clients]))
        extra = nc.Netiso("127.0.0.1", port, timeout=5, local_addr=("127.0.1.1", 0))
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(extra.connect())
            self.run_async(extra.stat(b"/GAMES/notes.txt"))

    def test_whitelist_rejects_other_addresses(self):
        # The addresses here are only about the whitelist decision, and that is
        # covered by the Whitelist unit tests; binding them is the part that
        # needs a machine which allows it.
        if not LOOPBACK_ALIASES:
            self.skipTest("this machine will not bind a second loopback address")

        async def bind():
            server = ns.NetisoServer(ns.Share(self.root),
                                     whitelist=ns.parse_whitelist("127.0.9.*"))
            srv = await asyncio.start_server(server.connection, "127.0.0.1", 0, backlog=1)
            self.servers.append(srv)
            return srv.sockets[0].getsockname()[1]

        port = self.run_async(bind())
        allowed = nc.Netiso("127.0.0.1", port, timeout=5,
                            local_addr=("127.0.9.9", 0))
        self.run_async(allowed.connect())
        self.clients.append(allowed)
        self.assertEqual(self.run_async(allowed.stat(b"/GAMES/notes.txt"))["size"], 11)
        blocked = nc.Netiso("127.0.0.1", port, timeout=5, local_addr=("127.0.8.8", 0))
        with self.assertRaises(nc.ConnectionClosed):
            self.run_async(blocked.connect())
            self.run_async(blocked.stat(b"/GAMES/notes.txt"))


async def _close(srv):
    srv.close()
    await srv.wait_closed()


@unittest.skipUnless(PSX_BIN and os.path.exists(PSX_BIN), "set PSX_BIN")
class RealRip(unittest.TestCase):
    """The PS1 game on this machine, served from its own directory."""

    def test_stream_like_a_client(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)

        async def go():
            server = ns.NetisoServer(ns.Share(Path(PSX_BIN).parent))
            srv = await asyncio.start_server(server.connection, "127.0.0.1", 0, backlog=1)
            port = srv.sockets[0].getsockname()[1]
            name = ("/" + os.path.basename(PSX_BIN)).encode("utf8", "surrogateescape")
            size = os.path.getsize(PSX_BIN)
            try:
                async with nc.Netiso("127.0.0.1", port, timeout=60) as client:
                    got_size, _ = await client.open_file(name)
                    self.assertEqual(got_size, size)
                    sectors = 16
                    expected = extract(Path(PSX_BIN), 2352, 16, sectors)
                    self.assertEqual(await client.read_cd_2048(16, sectors), expected)
                    with open(PSX_BIN, "rb") as fh:
                        fh.seek(0)
                        self.assertEqual(await client.read_file(0, 4096), fh.read(4096))
                        fh.seek(size - 8192)
                        tail = fh.read(8192)
                    self.assertEqual(await client.read_file_critical(size - 8192, 8192), tail)
            finally:
                await _close(srv)

        try:
            loop.run_until_complete(go())
        finally:
            loop.close()
            asyncio.set_event_loop(None)


if __name__ == "__main__":
    unittest.main(verbosity=2)
