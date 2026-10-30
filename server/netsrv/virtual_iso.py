#!/usr/bin/env python3
"""Virtual ISO 9660 images over a directory tree (§7.4).

``/***PS3***/<dir>`` and ``/***DVD***/<dir>`` let webMAN MOD mount a game *folder*
as if it were an ISO: the server synthesises an ISO 9660 filesystem (Primary
plus Joliet) around the directory and serves it through the normal file
commands. The layout here is a port of upstream's ``VIsoFile`` — the same
extents, the same directory records, the same path tables — so a client sees
the image webMAN's own server would have built.

Only the filesystem structures are kept in memory; file contents are read from
disk on demand, at whatever offset a client asks for. A 30 GB folder game costs
a few hundred kilobytes of RAM.

Deliberate differences from upstream (kept small, and each one is tested):

* Symlinks and special files are never included; upstream follows a symlink out
  of the share and writes the file it points at into the image. A game folder is
  real files on disk.
* ``.66600``-split files extend correctly. Upstream advertises their summed size
  but then refuses to read them.
* A file part that crosses the 4 GiB record limit is split into multiple
  extents, as the format requires; upstream does this too.
* A ``/.`` mount — the whole share, which upstream also answers as an ISO — is
  named after the share directory. Upstream takes the last element of the path it
  was handed, which for that spelling is a bare ``.``.
"""
from __future__ import annotations

import bisect
import os
import stat as statmod
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

SECTOR = 2048
SECTOR_MASK = SECTOR - 1
MAX_ISONAME = 207
MAX_ISODIR = 254
# ISO 9660 directory record data length is 32 bits. Upstream splits a larger
# file into extents of this size (a whole number of sectors, ~4 GiB).
MULTIEXTENT_PART = 0xFFFFF800
# Upstream builds a directory's records in a 4 MiB scratch buffer; a directory
# that needs more is refused rather than silently truncated.
MAX_DIR_CONTENT = 4 * 1024 * 1024
# 16 system sectors, PVD, Joliet PVD, terminator, an empty sector, then the
# four path tables start at byte 0xA000.
PVD_SECTOR = 16
PATH_TABLE_SECTOR = 0xA000 // SECTOR
PS3_PREFIX = b"/***PS3***/"
DVD_PREFIX = b"/***DVD***/"
DEFAULT_SYSTEM_SECTORS = 16


def sectors(size: int) -> int:
    return (size + SECTOR_MASK) // SECTOR


def le16(value: int) -> bytes:
    return struct.pack("<H", value)


def be16(value: int) -> bytes:
    return struct.pack(">H", value)


def le32(value: int) -> bytes:
    return struct.pack("<I", value)


def be32(value: int) -> bytes:
    return struct.pack(">I", value)


def both16(value: int) -> bytes:
    return le16(value) + be16(value)


def both32(value: int) -> bytes:
    return le32(value) + be32(value)


def iso_time(mtime: int) -> bytes:
    """The 7-byte directory-record timestamp, in local time as upstream does."""
    t = time.localtime(mtime)
    return bytes((max(0, t.tm_year - 1900) & 0xFF, t.tm_mon, t.tm_mday,
                  t.tm_hour, t.tm_min, t.tm_sec, 0))


def volume_time(now: float) -> bytes:
    """The 17-byte PVD timestamp: 16 ASCII digits, then NUL (upstream's pad)."""
    t = time.localtime(now)
    digits = "%04d%02d%02d%02d%02d%02d" % (t.tm_year, t.tm_mon, t.tm_mday,
                                           t.tm_hour, t.tm_min, t.tm_sec)
    return digits.encode("ascii") + b"00\0"


def iso_name(name: bytes, limit: int) -> bytes:
    """Upstream's ``strncpy_upper``: truncate, uppercase ASCII only."""
    return name[:limit].upper()


def joliet_name(name: bytes, limit: int) -> bytes:
    """UTF-8 filesystem name -> UTF-16BE, truncated to *limit* bytes."""
    text = name.decode("utf-8", "surrogateescape")
    raw = text.encode("utf-16-be", "surrogatepass")
    return raw[:limit - (limit & 1)]


@dataclass
class Entry:
    """One file in the image."""
    name: bytes                     # name as it appears in the directory
    path: Path                      # first on-disk part
    size: int                       # summed size of all parts
    mtime: int
    parts: list[tuple[Path, int]]   # (path, size) for each part
    lba: int = 0                    # first sector, relative to the file area


@dataclass
class Directory:
    path: Path
    name: bytes                     # raw name, b"" for the root
    parent: "Directory | None"
    mtime: int = 0
    index: int = 0                  # path-table number, 1-based
    dirs: list["Directory"] = field(default_factory=list)
    files: list[Entry] = field(default_factory=list)
    lba: int = 0                    # ISO directory area, relative
    lba_joliet: int = 0
    size: int = 0                   # padded ISO content size
    size_joliet: int = 0
    content: bytes = b""
    content_joliet: bytes = b""


# --------------------------------------------------------------------------
# Scanning
# --------------------------------------------------------------------------

def _scan(root: Path, root_mtime: int) -> list[Directory]:
    """Breadth-first directory list, entries sorted like upstream's alphasort.

    Directories and files are sorted by raw name bytes (strcmp order), the way
    ``alphasort`` does in the C locale. Symlinks and special files are skipped:
    a virtual ISO must not follow anything out of the share (§9.4).
    """
    root_dir = Directory(path=root, name=b"", parent=None, mtime=root_mtime,
                         index=1)
    order = [root_dir]
    queue = [root_dir]
    while queue:
        current = queue.pop(0)
        dirs, files = [], []
        try:
            entries = list(os.scandir(current.path))
        except OSError:
            entries = []
        entries.sort(key=lambda e: os.fsencode(e.name))
        for entry in entries:
            try:
                st = entry.stat(follow_symlinks=False)
            except OSError:
                continue
            raw = os.fsencode(entry.name)
            if statmod.S_ISDIR(st.st_mode):
                child = Directory(path=Path(entry.path), name=raw,
                                  parent=current, mtime=int(st.st_mtime),
                                  index=len(order) + 1)
                order.append(child)
                dirs.append(child)
            elif statmod.S_ISREG(st.st_mode):
                files.extend(_file_entries(Path(entry.path), raw, st))
        current.dirs, current.files = dirs, files
        queue.extend(dirs)
    return order


def _split_name(name: bytes) -> bool:
    """``.66600`` … ``.66699``, upstream's naming for a FAT-split file."""
    return (len(name) > 6 and name[-6] == ord(".") and name[-5:-2] == b"666"
            and name[-2:-1].isdigit() and name[-1:].isdigit())


def _file_entries(path: Path, name: bytes, st: os.stat_result) -> list[Entry]:
    """One entry, merging a ``.66600`` split file into a single logical file."""
    mtime = int(st.st_mtime)
    if not _split_name(name) or name[-2:] != b"00":
        # A .66601 and up is a part of the .66600 next to it, not an entry.
        return [] if _split_name(name) else [
            Entry(name=name, path=path, size=st.st_size, mtime=mtime,
                  parts=[(path, st.st_size)])]
    parts: list[tuple[Path, int]] = []
    for index in range(100):
        part = path.with_name(os.fsdecode(name[:-6] + b".666%02d" % index))
        try:
            part_st = part.lstat()
        except OSError:
            break
        if not statmod.S_ISREG(part_st.st_mode):
            break
        parts.append((part, part_st.st_size))
    if not parts:
        return [Entry(name=name, path=path, size=st.st_size, mtime=mtime,
                      parts=[(path, st.st_size)])]
    return [Entry(name=name[:-6], path=parts[0][0],
                  size=sum(size for _, size in parts), mtime=mtime,
                  parts=parts)]


# --------------------------------------------------------------------------
# ISO 9660 structures
# --------------------------------------------------------------------------

def _record_length(name: bytes) -> int:
    """Upstream writes ``0x27 + len(name)`` bytes — 6 zero bytes of slack past
    the name — then pads to an even length. Readers skip by ``len_dr``."""
    return (0x27 + len(name) + 1) & ~1


def _record(name: bytes, start: int, size: int, mtime: int, is_dir: bool,
            multiextent: bool = False) -> bytes:
    record = bytearray(_record_length(name))
    record[0] = len(record)
    record[2:6] = le32(start)
    record[6:10] = be32(start)
    record[10:14] = le32(size)
    record[14:18] = be32(size)
    record[18:25] = iso_time(mtime)
    record[25] = 0x80 if multiextent else (2 if is_dir else 0)
    record[28:30] = le16(1)
    record[30:32] = be16(1)
    record[32] = len(name)
    record[33:33 + len(name)] = name
    return bytes(record)


def _place(position: int, length: int) -> int:
    """Upstream starts a new sector when a record would *touch* the next one."""
    if position // SECTOR != (position + length) // SECTOR:
        return (position + SECTOR_MASK) & ~SECTOR_MASK
    return position


def _data_ranges(entry: Entry) -> list[tuple[int, int, int, Path, int]]:
    """(image byte offset, data end, padded end, path, offset in that file).

    A ``.66600`` split file is one logical file, so its parts are laid end to
    end with no padding between them; only the last part is padded out to a
    sector boundary. Reading the image needs these byte ranges, while the
    directory records need the coarser ``_extent_sizes`` below.
    """
    out = []
    base = entry.lba * SECTOR
    cursor = base
    last = len(entry.parts) - 1
    for index, (path, part_size) in enumerate(entry.parts):
        data_end = cursor + part_size
        pad_end = base + sectors(entry.size) * SECTOR if index == last else data_end
        # An empty part covers no bytes; leaving it in would give a range that
        # starts where the next file starts and shadow it on lookup.
        if part_size:
            out.append((cursor, data_end, pad_end, path, 0))
        cursor = data_end
    return out


def _extent_sizes(entry: Entry) -> list[int]:
    """The data lengths of a file's directory records.

    One record for an ordinary file; upstream's ``MULTIEXTENT_PART`` blocks
    once it passes the 32-bit field, and the blocks are whole sectors so the
    records stay sector-aligned.
    """
    if entry.size <= 0xFFFFFFFF:
        return [entry.size]
    sizes = [MULTIEXTENT_PART] * (entry.size // MULTIEXTENT_PART)
    rest = entry.size % MULTIEXTENT_PART
    if rest:
        sizes.append(rest)
    return sizes


class VirtualIso:
    """An ISO 9660 image built over *root*, readable at arbitrary offsets."""

    def __init__(self, root: Path, ps3_mode: bool, now: float | None = None,
                 rng=os.urandom):
        self.root = Path(root)
        self.ps3_mode = ps3_mode
        st = self.root.lstat()
        if statmod.S_ISLNK(st.st_mode) or not statmod.S_ISDIR(st.st_mode):
            raise OSError(f"not a directory: {root}")
        self.mtime = int(st.st_mtime)
        self.game_code = self._title_id() if ps3_mode else b""
        if ps3_mode and len(self.game_code) < 9:
            raise OSError(f"no usable TITLE_ID in {root}/PS3_GAME/PARAM.SFO")
        volume = b"PS3VOLUME" if ps3_mode else os.fsencode(self.root.name)[:31]

        self._dirs = _scan(self.root, self.mtime)
        self._assign_file_lbas()
        self._assign_dir_lbas()
        self._iso_lba, self._joliet_lba, self._files_lba = self._layout()
        for directory in self._dirs:
            directory.content = self._directory_content(directory, False)
        for directory in self._dirs:
            directory.content_joliet = self._directory_content(directory, True)

        self._path_table = self._path_table_content(False, False)
        self._path_table_joliet = self._path_table_content(True, False)
        iso_table = sectors(len(self._path_table))
        joliet_table = sectors(len(self._path_table_joliet))
        volume_size = self._files_lba + self._file_sectors
        pad_sectors = 0x20
        if volume_size & 0x1F:
            pad_sectors += 0x20 - (volume_size & 0x1F)
        self._volume_size = volume_size + pad_sectors
        self._pad_start = volume_size * SECTOR
        self.size = self._volume_size * SECTOR
        self._header = self._build_header(
            volume, iso_table, joliet_table,
            time.time() if now is None else now, rng)

        self._extents = []
        for directory in self._dirs:
            for entry in directory.files:
                for offset, data_end, pad_end, path, file_offset in _data_ranges(entry):
                    self._extents.append((self._files_lba * SECTOR + offset,
                                          self._files_lba * SECTOR + data_end,
                                          self._files_lba * SECTOR + pad_end,
                                          path, file_offset))
        self._extents.sort(key=lambda extent: extent[0])
        self._extent_starts = [extent[0] for extent in self._extents]
        self._fh = None
        self._fh_path = None

    # -- layout -------------------------------------------------------------

    def _assign_file_lbas(self) -> None:
        position = 0
        for directory in self._dirs:
            for entry in directory.files:
                entry.lba = position
                position += sectors(entry.size)
        self._file_sectors = position

    def _assign_dir_lbas(self) -> None:
        position = joliet = 0
        for directory in self._dirs:
            directory.lba = position
            directory.size = self._content_size(directory, False)
            position += directory.size // SECTOR
            directory.lba_joliet = joliet
            directory.size_joliet = self._content_size(directory, True)
            joliet += directory.size_joliet // SECTOR

    def _layout(self) -> tuple[int, int, int]:
        iso_table = sectors(self._path_table_len(False))
        joliet_table = sectors(self._path_table_len(True))
        iso_lba = PATH_TABLE_SECTOR + iso_table * 2 + joliet_table * 2
        joliet_lba = iso_lba + sum(d.size for d in self._dirs) // SECTOR
        files_lba = joliet_lba + sum(d.size_joliet for d in self._dirs) // SECTOR
        return iso_lba, joliet_lba, files_lba

    def _path_table_len(self, joliet: bool) -> int:
        total = 0
        for directory in self._dirs:
            name = b"\0" if directory.parent is None \
                else self._dir_name(directory, joliet)
            total += 8 + len(name) + (len(name) & 1)
        return total

    def _title_id(self) -> bytes:
        """TITLE_ID out of PS3_GAME/PARAM.SFO, parsed the way upstream does."""
        try:
            data = (self.root / "PS3_GAME" / "PARAM.SFO").read_bytes()
        except OSError:
            return b""
        if len(data) < 0x14:
            return b""
        key_start = int.from_bytes(data[8:10], "little")
        data_start = int.from_bytes(data[0xC:0xE], "little")
        position, data_pos, index = key_start, data_start, 0
        while position < len(data):
            if data[position] == 0:
                break
            end = data.find(b"\0", position)
            if end < 0:
                break
            if data[position:end] == b"TITLE_ID" and data_pos < len(data):
                return data[data_pos:data_pos + 9].split(b"\0")[0]
            position = end + 1
            at = 0x1C + index
            if at + 2 > len(data):
                break
            data_pos += int.from_bytes(data[at:at + 2], "little")
            index += 16
        return b""

    # -- names and records --------------------------------------------------

    def _file_name(self, entry: Entry, joliet: bool) -> bytes:
        if joliet:
            return joliet_name(entry.name + b";1", MAX_ISONAME // 2 * 2)
        return iso_name(entry.name, MAX_ISONAME - 3) + b";1"

    def _dir_name(self, directory: Directory, joliet: bool) -> bytes:
        if joliet:
            return joliet_name(directory.name, MAX_ISODIR // 2 * 2)
        return iso_name(directory.name, MAX_ISODIR)

    def _content_size(self, directory: Directory, joliet: bool) -> int:
        position = 2 * _record_length(b"\0")
        for entry in directory.files:
            length = _record_length(self._file_name(entry, joliet))
            for _ in _extent_sizes(entry):
                position = _place(position, length)
                position += length
        for child in directory.dirs:
            length = _record_length(self._dir_name(child, joliet))
            position = _place(position, length)
            position += length
        size = (position + SECTOR_MASK) & ~SECTOR_MASK
        if size > MAX_DIR_CONTENT:
            raise OSError(f"directory too large for an ISO: {directory.path}")
        return size

    def _directory_content(self, directory: Directory, joliet: bool) -> bytes:
        base = self._joliet_lba if joliet else self._iso_lba
        parent = directory.parent or directory
        parent_lba = parent.lba_joliet if joliet else parent.lba
        parent_size = parent.size_joliet if joliet else parent.size
        size = directory.size_joliet if joliet else directory.size
        lba = directory.lba_joliet if joliet else directory.lba
        out = bytearray()
        out += _record(b"\0", base + lba, size, directory.mtime, True)
        out += _record(b"\1", base + parent_lba, parent_size, parent.mtime, True)
        for entry in directory.files:
            name = self._file_name(entry, joliet)
            sizes = _extent_sizes(entry)
            placed = 0
            for index, length in enumerate(sizes):
                out = self._append_record(out, _record(
                    name, self._files_lba + entry.lba + placed // SECTOR, length,
                    entry.mtime, False, multiextent=index + 1 < len(sizes)))
                placed += length
        for child in directory.dirs:
            child_lba = child.lba_joliet if joliet else child.lba
            child_size = child.size_joliet if joliet else child.size
            out = self._append_record(out, _record(
                self._dir_name(child, joliet), base + child_lba, child_size,
                child.mtime, True))
        return bytes(out) + b"\0" * ((-len(out)) % SECTOR)

    @staticmethod
    def _append_record(out: bytearray, record: bytes) -> bytearray:
        if len(out) // SECTOR != (len(out) + len(record)) // SECTOR:
            out += b"\0" * ((-len(out)) % SECTOR)
        out += record
        return out

    def _path_table_content(self, joliet: bool, msb: bool) -> bytes:
        out = bytearray()
        base = self._joliet_lba if joliet else self._iso_lba
        for directory in self._dirs:
            if directory.parent is None:
                name, parent = b"\0", 1
            else:
                name = self._dir_name(directory, joliet)
                parent = directory.parent.index
            relative = directory.lba_joliet if joliet else directory.lba
            out += bytes((len(name), 0))
            out += be32(base + relative) if msb else le32(base + relative)
            out += be16(parent) if msb else le16(parent)
            out += name
            if len(name) & 1:
                out += b"\0"
        return bytes(out)

    # -- image header -------------------------------------------------------

    def _build_header(self, volume: bytes, iso_table: int, joliet_table: int,
                      now: float, rng) -> bytes:
        header = bytearray(self._files_lba * SECTOR)
        if self.ps3_mode:
            header[0:4] = be32(1)
            header[8:12] = be32(0)
            header[12:16] = be32(self._volume_size - 1)
            info = memoryview(header)[SECTOR:2 * SECTOR]
            info[0:12] = b"PlayStation3"
            info[0x10:0x30] = b" " * 0x20
            info[0x10:0x14] = self.game_code[:4]
            info[0x14:0x15] = b"-"
            info[0x15:0x1A] = self.game_code[4:9]
            info[0x40:0x1F0] = rng(0x1B0)
            info[0x1F0:0x200] = rng(0x10)
            del info

        root = self._dirs[0]
        root_record = bytes((34,)) + root.content[1:34]
        pvd = self._volume_descriptor(1, volume)
        pvd[132:140] = both32(len(self._path_table))
        pvd[140:144] = le32(PATH_TABLE_SECTOR)
        pvd[148:152] = be32(PATH_TABLE_SECTOR + iso_table)
        pvd[156:190] = root_record
        pvd[813:830] = volume_time(now)
        header[PVD_SECTOR * SECTOR:(PVD_SECTOR + 1) * SECTOR] = pvd

        joliet = self._volume_descriptor(2, volume)
        joliet[88:91] = b"%/@"
        joliet[132:140] = both32(len(self._path_table_joliet))
        joliet[140:144] = le32(PATH_TABLE_SECTOR + iso_table * 2)
        joliet[148:152] = be32(PATH_TABLE_SECTOR + iso_table * 2 + joliet_table)
        joliet[156:190] = bytes((34,)) + root.content_joliet[1:34]
        joliet[190:222] = joliet[40:72]
        joliet[813:830] = volume_time(now)
        header[17 * SECTOR:18 * SECTOR] = joliet

        header[18 * SECTOR:18 * SECTOR + 6] = b"\xffCD001"

        base = PATH_TABLE_SECTOR * SECTOR
        msb = self._path_table_content(False, True)
        msb_joliet = self._path_table_content(True, True)
        header[base:base + len(self._path_table)] = self._path_table
        header[base + iso_table * SECTOR:base + iso_table * SECTOR + len(msb)] = msb
        jbase = base + iso_table * 2 * SECTOR
        header[jbase:jbase + len(self._path_table_joliet)] = self._path_table_joliet
        header[jbase + joliet_table * SECTOR:
               jbase + joliet_table * SECTOR + len(msb_joliet)] = msb_joliet
        for directory in self._dirs:
            at = (self._iso_lba + directory.lba) * SECTOR
            header[at:at + len(directory.content)] = directory.content
            at = (self._joliet_lba + directory.lba_joliet) * SECTOR
            header[at:at + len(directory.content_joliet)] = directory.content_joliet
        return bytes(header)

    def _volume_descriptor(self, kind: int, volume: bytes) -> bytearray:
        pvd = bytearray(SECTOR)
        pvd[0:7] = bytes((kind,)) + b"CD001\x01"
        if kind == 1:
            pvd[8:40] = b" " * 32
            pvd[40:72] = iso_name(volume, 32).ljust(32, b" ")
        else:
            name = joliet_name(volume, 32)
            pvd[40:40 + len(name)] = name
        pvd[80:88] = both32(self._volume_size)
        pvd[120:132] = both16(1) + both16(1) + both16(SECTOR)
        if kind == 1:
            pvd[190:318] = pvd[40:72] + b" " * 96
            pvd[318:813] = b" " * (813 - 318)
        else:
            pvd[190:222] = pvd[40:72]
        pvd[830:846] = b"0" * 16
        pvd[847:863] = b"0" * 16
        pvd[864:880] = b"0" * 16
        pvd[881] = 1
        return pvd

    # -- reading ------------------------------------------------------------

    def read_at(self, offset: int, size: int) -> bytes:
        """Bytes ``[offset, offset+size)`` of the image, clamped to its end."""
        if size <= 0 or offset >= self.size:
            return b""
        size = min(size, self.size - offset)
        out = bytearray()
        header_end = len(self._header)
        if offset < header_end:
            count = min(size, header_end - offset)
            out += self._header[offset:offset + count]
            offset += count
            size -= count
        while size:
            found = self._extent(offset)
            if found is None:
                stop = self._pad_start if offset < self._pad_start else self.size
                count = min(size, stop - offset)
                if count <= 0:
                    break
                out += b"\0" * count
                offset += count
                size -= count
                continue
            start, data_end, pad_end, path, file_offset = found
            if offset >= data_end:
                count = min(size, pad_end - offset)
                out += b"\0" * count
                offset += count
                size -= count
                continue
            count = min(size, data_end - offset)
            data = self._read_file(path, file_offset + (offset - start), count)
            out += data
            offset += len(data)
            size -= len(data)
            if len(data) < count:
                break
        return bytes(out)

    def _extent(self, offset: int):
        index = bisect.bisect_right(self._extent_starts, offset) - 1
        if index < 0:
            return None
        extent = self._extents[index]
        return extent if offset < extent[2] else None

    def _read_file(self, path: Path, offset: int, size: int) -> bytes:
        if self._fh is None or self._fh_path != path:
            self.close()
            try:
                fd = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
                if not statmod.S_ISREG(os.fstat(fd).st_mode):
                    os.close(fd)
                    return b""
                self._fh = os.fdopen(fd, "rb", closefd=True)
            except OSError:
                return b""
            self._fh_path = path
        try:
            self._fh.seek(offset)
            return self._fh.read(size)
        except OSError:
            return b""

    def close(self) -> None:
        if self._fh is not None:
            try:
                self._fh.close()
            except OSError:
                pass
        self._fh = None
        self._fh_path = None
