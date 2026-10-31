#!/usr/bin/env python3
"""Tests for cover_art.py (PBP / ISO9660 / CSO readers, sidecar cache).

The fixtures are written here from the formats' published layouts: a tiny
ISO9660 image (PVD + one directory sector + file data) and a CISO file
(raw-deflate blocks, 2048-scaled index), so the readers are checked
against spec-shaped bytes, not against themselves.

    python3 server/store/cover_test.py
"""

import json
import os
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cover_art  # noqa: E402
import generate_catalog  # noqa: E402

SECTOR = 2048


def make_png(w=4, h=4, rgb=(255, 0, 0)):
    """Smallest honest PNG: IHDR + one zlib'd all-white-ish scanline IDAT."""
    import binascii

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", binascii.crc32(tag + data) & 0xffffffff))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))
    idat = zlib.compress(raw)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", idat) + chunk(b"IEND", b""))


def make_pbp(icon=make_png(), pic1=b""):
    header_len = 68
    parts = [b"PARAMSFO", icon, pic1, b"snd", b"picon", b"data", b"upd", b"aud"]
    off, offs = header_len, []
    for p in parts:
        offs.append((off, len(p)))
        off += len(p)
    head = b"\x00PBP" + b"".join(struct.pack(">II", o, s) for o, s in offs)
    return head + b"".join(parts)


def iso_dir_record(name, lba, size, is_dir):
    raw = name.encode()
    # The *record* (33-byte header + name) must be even: pad odd 33+len.
    padded = raw + b"\x00" if (33 + len(raw)) % 2 else raw
    rec = bytearray(33 + len(padded))
    rec[0] = len(rec)
    rec[1] = 0
    rec[2:6] = struct.pack("<I", lba)
    rec[6:10] = struct.pack(">I", lba)
    rec[10:14] = struct.pack("<I", size)
    rec[14:18] = struct.pack(">I", size)
    rec[18:25] = b"\x26\x10\x31\x00\x00\x00\x00"
    rec[25] = 2 if is_dir else 0
    rec[26] = 0
    rec[27:31] = struct.pack("<H", 1)
    rec[31:33] = struct.pack(">H", 1)
    rec[32] = len(raw)                       # name length, padding excluded
    rec[33:33 + len(raw)] = raw
    return bytes(rec)


def make_iso(pbpblob, sub=None):
    """One-file homebrew ISO: empty 0..15, PVD 16, root dir 17, data 18+.
    With *sub*, the PBP lives at SUBDIR/EBOOT.PBP (dir 17, subdir 18)."""
    sectors = [b"\x00" * SECTOR] * 16
    root_entries = [
        iso_dir_record("\x00", 17, SECTOR, True),
        iso_dir_record("\x01", 17, SECTOR, True),
    ]
    if sub:
        sub_lba = 18
        data_lba = 19
        subdir = iso_dir_record("\x00", 17, SECTOR, True) + \
            iso_dir_record("\x01", 17, SECTOR, True) + \
            iso_dir_record("EBOOT.PBP;1", data_lba, len(pbpblob), False)
        pad = SECTOR - len(subdir) % SECTOR
        subdir += b"\x00" * pad
        root_entries.append(iso_dir_record("SUBDIR  ;1", sub_lba,
                                           len(subdir), True))
        file_sectors = [subdir, pad_bytes(pbpblob)]
        total = 18 + 1 + (len(subdir) // SECTOR) \
            + ((len(pbpblob) + SECTOR - 1) // SECTOR)
    else:
        data_lba = 18
        root_entries.append(iso_dir_record("EBOOT.PBP;1", data_lba,
                                           len(pbpblob), False))
        file_sectors = [pad_bytes(pbpblob)]
        total = 18 + ((len(pbpblob) + SECTOR - 1) // SECTOR)
    root = b"".join(root_entries)
    root = pad_bytes(root)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 0
    pvd[8:40] = b"PSLINKER_TEST".ljust(32)
    pvd[80:88] = struct.pack("<I", total) + struct.pack(">I", total)
    pvd[120:124] = struct.pack("<H", SECTOR) + struct.pack(">H", SECTOR)
    pvd[124:128] = struct.pack("<H", 1) + struct.pack(">H", 1)
    root_rec = bytearray(34)
    root_rec[0] = 34
    root_rec[2:6] = struct.pack("<I", 17)
    root_rec[6:10] = struct.pack(">I", 17)
    root_rec[10:14] = struct.pack("<I", len(root))
    root_rec[14:18] = struct.pack(">I", len(root))
    root_rec[25] = 2
    root_rec[27:29] = struct.pack("<H", 1)
    root_rec[29:31] = struct.pack(">H", 1)
    root_rec[32] = 0
    pvd[156:190] = bytes(root_rec)
    image = b"".join(sectors) + bytes(pvd) + root + b"".join(file_sectors)
    return image


def pad_bytes(data):
    pad = -len(data) % SECTOR
    return data + b"\x00" * pad


def make_cso(iso_bytes, block=SECTOR):
    """CISO ver 1: raw-deflate blocks, align 0 (index scaled by 2048, so
    every stored block is padded to a 2048 multiple like the real tools)."""
    total = (len(iso_bytes) + block - 1) // block
    index_off = 24
    table_len = 4 * (total + 1)
    body_base = 24 + table_len + (-(24 + table_len) % 2048)   # align 0
    body = bytearray()
    index = []
    for i in range(total):
        blk = iso_bytes[i * block:(i + 1) * block]
        index.append((body_base + len(body)) // 2048)
        z = zlib.compressobj(6, zlib.DEFLATED, -15)
        raw = z.compress(blk) + z.flush()
        body += blk if len(raw) >= len(blk) else raw
        body += b"\x00" * (-len(body) % 2048)
    index.append((body_base + len(body)) // 2048)
    header = struct.pack("<6I", 0x4F534943, block, total, 1, index_off, 0)
    table = struct.pack("<%dI" % len(index), *index)
    table += b"\x00" * (body_base - 24 - len(table))
    return header + table + bytes(body)


class PbpTests(unittest.TestCase):
    def test_icon0_extracted(self):
        png = make_png(8, 8, (0, 255, 0))
        blob = cover_art.pbp_icon(bytes_reader(make_pbp(icon=png)))
        self.assertEqual(blob, png)

    def test_pic1_fallback(self):
        png = make_png(2, 2)
        blob = cover_art.pbp_icon(
            bytes_reader(make_pbp(icon=b"not a png", pic1=png)))
        self.assertEqual(blob, png)

    def test_not_a_pbp(self):
        self.assertEqual(cover_art.pbp_icon(bytes_reader(b"junk" * 40)), b"")


def bytes_reader(data):
    import io
    return io.BytesIO(data)


class IsoTests(unittest.TestCase):
    def test_finds_root_pbp(self):
        iso = make_iso(make_pbp())
        loc = cover_art.iso_find(bytes_reader(iso))
        self.assertIsNotNone(loc)
        lba, size = loc
        self.assertEqual(iso[lba * SECTOR:lba * SECTOR + size][:4], b"\x00PBP")

    def test_finds_nested_pbp(self):
        iso = make_iso(make_pbp(), sub=True)
        loc = cover_art.iso_find(bytes_reader(iso))
        self.assertIsNotNone(loc)
        lba, size = loc
        self.assertEqual(iso[lba * SECTOR:lba * SECTOR + size][:4], b"\x00PBP")

    def test_garbage_is_none(self):
        self.assertIsNone(cover_art.iso_find(bytes_reader(b"\x00" * 40000)))


class CsoTests(unittest.TestCase):
    def test_roundtrip_decompressed_view(self):
        data = pad_bytes(b"hello cso world " * 4000)      # compresses fine
        cso = make_cso(data)
        f = cover_art.CsoFile(write_temp("x.cso", cso))
        try:
            r = cover_art._CsoReader(f)
            r.seek(100)
            self.assertEqual(r.read(50), data[100:150])
            r.seek(4000)
            self.assertEqual(r.read(9000), data[4000:13000])
        finally:
            f.close()

    def test_cso_iso_cover_end_to_end(self):
        png = make_png(16, 16, (0, 0, 255))
        iso = make_iso(make_pbp(icon=png))
        path = write_temp("game.cso", make_cso(iso))
        got = cover_art.extract_cover(os.path.dirname(path),
                                       os.path.basename(path))
        self.assertEqual(got, png)


def write_temp(name, blob):
    fd, path = tempfile.mkstemp(suffix="-" + name)
    with os.fdopen(fd, "wb") as fh:
        fh.write(blob)
    return path


class LibraryTests(unittest.TestCase):
    """ensure_cover + generate_catalog over a miniature library."""

    def setUp(self):
        self.lib = tempfile.mkdtemp(prefix="cover-lib-")
        os.makedirs(os.path.join(self.lib, "PSP"))
        self.png = make_png(32, 32, (9, 9, 250))
        iso_path = os.path.join(self.lib, "PSP", "Game.iso")
        with open(iso_path, "wb") as fh:
            fh.write(make_iso(make_pbp(icon=self.png)))
        with open(os.path.join(self.lib, "PSP", "NoPbp.iso"), "wb") as fh:
            fh.write(b"\x00" * 300000)

    def test_sidecar_created_hidden_and_url(self):
        url = cover_art.ensure_cover(self.lib, "PSP/Game.iso")
        self.assertEqual(url, "/PSP/.Game.icon0.png")
        sidecar = os.path.join(self.lib, "PSP", ".Game.icon0.png")
        self.assertEqual(open(sidecar, "rb").read(), self.png)

    def test_cache_reused_and_force(self):
        cover_art.ensure_cover(self.lib, "PSP/Game.iso")
        sidecar = os.path.join(self.lib, "PSP", ".Game.icon0.png")
        mtime = os.path.getmtime(sidecar)
        self.assertEqual(cover_art.ensure_cover(self.lib, "PSP/Game.iso"),
                         "/PSP/.Game.icon0.png")
        self.assertEqual(os.path.getmtime(sidecar), mtime)   # cache reused
        os.utime(sidecar, (mtime - 500, mtime - 500))        # stale again
        self.assertEqual(cover_art.ensure_cover(self.lib, "PSP/Game.iso",
                                                force=True),
                         "/PSP/.Game.icon0.png")
        self.assertGreater(os.path.getmtime(sidecar), mtime)  # regenerated
        self.assertEqual(cover_art.extract_cover(self.lib, "PSP/NoPbp.iso"),
                         b"")
        self.assertIsNone(cover_art.ensure_cover(self.lib, "PSP/NoPbp.iso"))

    def test_catalog_publishes_icon_url(self):
        out = os.path.join(self.lib, "catalog.json")
        self.assertEqual(run_generate(self.lib, out), 0)
        cat = json.load(open(out))
        icons = {e["url"]: e.get("icon") for e in cat["entries"]}
        self.assertEqual(icons["/PSP/Game.iso"], "/PSP/.Game.icon0.png")
        self.assertNotIn("icon", [e for e in cat["entries"]
                                  if e["url"] == "/PSP/NoPbp.iso"][0])
        # The sidecar itself must not become a catalog entry.
        self.assertNotIn("/PSP/.Game.icon0.png", icons)

    def test_ps3_folder_icon0_url_keeps_folder(self):
        game = os.path.join(self.lib, "PS3", "MyGame", "PS3", "TM0001")
        os.makedirs(game)
        png = make_png(5, 5)
        with open(os.path.join(game, "EBOOT.BIN"), "wb") as fh:
            fh.write(b"binary")
        with open(os.path.join(game, "ICON0.PNG"), "wb") as fh:
            fh.write(png)
        out = os.path.join(self.lib, "catalog2.json")
        run_generate(self.lib, out)
        cat = json.load(open(out))
        entry = [e for e in cat["entries"] if e["url"].endswith("EBOOT.BIN")][0]
        self.assertEqual(entry["icon"],
                         "/PS3/MyGame/PS3/TM0001/ICON0.PNG")


def run_generate(lib, out):
    argv = sys.argv
    sys.argv = ["generate_catalog.py", lib, out]
    try:
        return generate_catalog.main()
    finally:
        sys.argv = argv


if __name__ == "__main__":
    unittest.main(verbosity=2)
