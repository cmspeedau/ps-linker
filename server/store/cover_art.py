#!/usr/bin/env python3
"""Extract real cover art for library entries, stdlib only.

The store tiles want pictures, but most libraries only carry them *inside*
the game files: PSP ISO/CSO images embed an EBOOT.PBP whose ICON0 chunk is
a PNG (homebrew; commercial UMDs encrypt theirs, out of scope), and a bare
EBOOT.PBP carries it directly. PS3 game folders keep ICON0.PNG on the
filesystem (disc layout: PS3/GAMEID/ICON0.PNG).

This module reads just the bytes it needs — the ISO9660 volume descriptor,
the directory records, and the ICON0 slice — never a whole image, and
writes the cover next to the game as a hidden ``<stem>.icon0.png``.
Hidden means every skip already in place (listing, catalog, GUI file
management) ignores the sidecar, while the share still serves it, so the
catalog can point at it. Re-runs reuse an existing cover newer than the
game; ``--force`` regenerates all.

Readers implemented here:
  * PBP:  the 68-byte header's offset/size pairs (ICON0, PIC1 fallback);
  * ISO9660: primary volume descriptor + directory records, no path table,
    no Rock Ridge (enough to find EBOOT.PBP in homebrew images);
  * CSO:  the CISO block index, decompressed one 2048-byte block at a time
    through a seekable wrapper, so the ISO9660 reader runs on it unchanged.
"""

import os
import struct
import sys
import zlib

SECTOR = 2048
MAX_PNG = 8 * 1024 * 1024        # refuse absurd sidecars
MAX_DIR_BYTES = 2 * 1024 * 1024  # directory records read at most this much

# The file we go looking for inside ISO/CSO images.
PBP_NAME = "EBOOT.PBP"


# ----------------------------------------------------------------------
# PBP
# ----------------------------------------------------------------------

def _png_ok(blob: bytes) -> bool:
    return blob[:8] == b"\x89PNG\r\n\x1a\n"


def pbp_icon(fh, base: int = 0) -> bytes:
    """ICON0 PNG bytes of the PBP open at *fh* (data relative to *base*),
    falling back to PIC1 (PSP accepts PNG or BMP there — PNG only passes).
    Returns b'' when there is nothing usable."""
    fh.seek(base)
    head = fh.read(68)
    if len(head) != 68 or head[:4] != b"\x00PBP":
        return b""
    fields = struct.unpack(">16I", head[4:68])
    # Order: PARAM_SFO, ICON0, PIC1, SND0, PICON, DATA, UPDATE, AUDIO.
    for idx in (1, 2):                       # ICON0, then PIC1
        off, size = fields[idx * 2], fields[idx * 2 + 1]
        if size == 0 or size > MAX_PNG:
            continue
        fh.seek(base + off)
        blob = fh.read(size)
        if _png_ok(blob):
            return blob
    return b""


# ----------------------------------------------------------------------
# Seekable wrapper over a CSO
# ----------------------------------------------------------------------

class CsoFile:
    """Read-only seekable view of a compressed .cso image (CISO format:
    ver 1 deflate, index entries scaled by 2048 when align == 0)."""

    def __init__(self, path):
        self._f = open(path, "rb")
        head = self._f.read(24)
        if len(head) < 24 or head[:4] != b"CISO":
            raise ValueError("not a CISO file")
        _, self.block_size, self.total_blocks, ver, \
            index_off, align = struct.unpack("<6I", head)
        if self.block_size in (0, None):
            self.block_size = SECTOR
        if ver != 1:
            raise ValueError(f"cso version {ver} unsupported")
        if self.total_blocks <= 0 or self.total_blocks > 64 * 1024 * 1024:
            raise ValueError("cso block count out of range")
        unit = 2048 if align == 0 else 1
        self._f.seek(index_off)
        raw = self._f.read(4 * (self.total_blocks + 1))
        n = len(raw) // 4
        if n < self.total_blocks:
            raise ValueError("cso index truncated")
        self._index = [v * unit for v in struct.unpack("<%dI" % n, raw)]
        if n == self.total_blocks:           # writer omitted the end entry
            self._index.append(os.fstat(self._f.fileno()).st_size)
        self._cached_block = -1
        self._cached_data = b""
        self._pos = 0

    def seek(self, pos):
        self._pos = pos

    def read(self, size):
        out = b""
        while size > 0:
            blk, off = divmod(self._pos, self.block_size)
            if blk >= self.total_blocks:
                break
            if blk != self._cached_block:
                self._cached_data = self._block(blk)
                self._cached_block = blk
            chunk = self._cached_data[off:off + size]
            if not chunk:                    # block shorter than declared
                break
            out += chunk
            self._pos += len(chunk)
            size -= len(chunk)
        return out

    def _block(self, blk):
        start, end = self._index[blk], self._index[blk + 1]
        if end == start:                     # stored uncompressed
            self._f.seek(start)
            return self._f.read(self.block_size)
        self._f.seek(start)
        raw = self._f.read(end - start)
        # Writers under align 0 pad stored blocks to 2048, so the stream
        # may be followed by junk: a decompressobj stops at its end.
        for wbits in (-15, 15):              # raw deflate, then zlib-framed
            try:
                return zlib.decompressobj(wbits).decompress(raw)
            except zlib.error:
                continue
        raise ValueError(f"cso block {blk} corrupt")

    def close(self):
        self._f.close()


class _CsoReader:
    """Minimal seek/read interface the ISO reader wants, over a CsoFile."""

    def __init__(self, cso: CsoFile):
        self._c = cso

    def seek(self, pos):
        self._c.seek(pos)

    def read(self, size):
        return self._c.read(size)


# ----------------------------------------------------------------------
# ISO9660
# ----------------------------------------------------------------------

def _iso_read_dir(reader, lba: int, size: int):
    """Yield (name, flags, data_lba, data_size) records of one directory."""
    if size <= 0 or size > MAX_DIR_BYTES:
        return
    reader.seek(lba * SECTOR)
    data = reader.read(size)
    i = 0
    while i < len(data):
        rec_len = data[i]
        if rec_len == 0:                     # pad to the next sector
            i = (i // SECTOR + 1) * SECTOR
            continue
        if i + 34 > len(data) or i + rec_len > len(data):
            return
        name_len = data[i + 32]
        flags = data[i + 25]
        data_lba = struct.unpack("<I", data[i + 2:i + 6])[0]
        data_size = struct.unpack("<I", data[i + 10:i + 14])[0]
        name = data[i + 33:i + 33 + name_len].decode("latin-1")
        yield name, flags, data_lba, data_size
        i += rec_len


def _iso_search(reader, lba: int, size: int, want: str, depth: int,
                seen):
    if depth > 8 or lba in seen:
        return None
    seen.add(lba)
    for name, flags, dlba, dsize in _iso_read_dir(reader, lba, size):
        bare = name.split(";")[0].upper()
        if bare in (".", "..", "\x00", "\x01"):     # self/parent records
            continue
        if flags & 2:
            hit = _iso_search(reader, dlba, dsize, want, depth + 1, seen)
            if hit:
                return hit
        elif bare == want:
            return dlba, dsize
    return None


def iso_find(reader, want: str = PBP_NAME):
    """Locate a file (name match, case-insensitive, ;ver stripped) in the
    ISO9660 image behind *reader*. Returns (data_lba, size) or None."""
    reader.seek(16 * SECTOR)
    pvd = reader.read(SECTOR)
    if len(pvd) < 190 or pvd[0] != 1 or pvd[1:6] != b"CD001":
        return None
    root_lba = struct.unpack("<I", pvd[158:162])[0]
    root_size = struct.unpack("<I", pvd[166:170])[0]
    return _iso_search(reader, root_lba, root_size, want.upper(), 0, set())


# ----------------------------------------------------------------------
# Per-game extraction
# ----------------------------------------------------------------------

def cover_path_for(lib_root: str, rel_game: str):
    """Hidden sidecar path/URL next to the game, or None for extensions
    we cannot open."""
    stem, ext = os.path.splitext(rel_game)
    ext = ext.lower()
    if ext not in (".iso", ".cso", ".pbp"):
        return None
    head, tail = os.path.split(stem)          # dot the name, not the folder
    rel = os.path.join(head, "." + tail + ".icon0.png")
    return rel, os.path.join(lib_root, rel)


def extract_cover(lib_root: str, rel_game: str) -> bytes:
    """Cover PNG for one game file, or b''. Raises nothing: an image we
    cannot parse is simply a library entry without a picture."""
    stem, ext = os.path.splitext(rel_game)
    ext = ext.lower()
    path = os.path.join(lib_root, rel_game)
    try:
        if ext == ".pbp":
            with open(path, "rb") as fh:
                return pbp_icon(fh)
        if ext not in (".iso", ".cso"):
            return b""
        if ext == ".iso":
            with open(path, "rb") as fh:
                loc = iso_find(fh)
                if not loc:
                    return b""
                # pbp_icon takes a base offset: header + ICON0 only,
                # never the whole EBOOT.
                return pbp_icon(fh, base=loc[0] * SECTOR)
        cso = CsoFile(path)
        try:
            reader = _CsoReader(cso)
            loc = iso_find(reader)
            if not loc:
                return b""
            return pbp_icon(reader, base=loc[0] * SECTOR)
        finally:
            cso.close()
    except (OSError, ValueError, MemoryError):
        return b""


def ensure_cover(lib_root: str, rel_game: str, force: bool = False):
    """Extract-and-cache entry point used by the catalog generator.
    Returns the URL the catalog should publish, or None."""
    target = cover_path_for(lib_root, rel_game)
    if target is None:
        return None
    rel_out, out_path = target
    url = "/" + rel_out.replace(os.sep, "/")
    game_path = os.path.join(lib_root, rel_game)
    try:
        if not force and os.path.isfile(out_path) and \
                os.path.getmtime(out_path) >= os.path.getmtime(game_path):
            return url
    except OSError:
        return None
    blob = extract_cover(lib_root, rel_game)
    if not blob:
        return None
    tmp = out_path + ".tmp"
    try:
        with open(tmp, "wb") as fh:
            fh.write(blob)
        os.replace(tmp, out_path)
    except OSError:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        return None
    print(f"[covers] extracted {url} ({len(blob)} bytes)")
    return url


def main() -> int:
    lib = None
    force = False
    for arg in sys.argv[1:]:
        if arg == "--force":
            force = True
        elif arg.startswith("-"):
            print(__doc__, file=sys.stderr)
            return 2
        else:
            lib = arg
    if not lib or not os.path.isdir(lib):
        print(__doc__, file=sys.stderr)
        return 2
    made = 0
    for root, _dirs, files in os.walk(lib):
        for name in sorted(files):
            if name.startswith("."):
                continue
            rel = os.path.relpath(os.path.join(root, name), lib)
            if ensure_cover(lib, rel, force=force):
                made += 1
    print(f"[covers] {made} covers available in {lib}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
