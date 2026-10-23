# ps3netsrv / netiso protocol (TCP 38008)

Reference document for `server/netsrv/ps3netsrv.py`, our own server for PS3
game streaming over the network.

> **How this document was made.** The behaviour described here was obtained by
> reading the `aldostools/webMAN-MOD` repository (GPL-3.0) as a reference, and
> by running its `ps3netsrv++` binary and comparing bytes on the wire. It is a
> *reference-derived* spec, not a clean-room one — no code was copied, but the
> same person read the upstream source and wrote both this file and our
> implementation. Every claim below is either marked **confirmed** (checked in
> the reference source or observed on the wire) or **unconfirmed** (listed in
> §8, still to be settled against a real PS3).
>
> Where upstream is self-contradictory or plainly broken we say so and record
> what `server/netsrv/ps3netsrv.py` does instead. §9 lists those deviations.

---

## 1. Overview

ps3netsrv exposes a directory tree as a virtual filesystem that a PS3 running
webMAN MOD, multiMAN or similar mounts and streams games from.

| Property | Value | Status |
|---|---|---|
| Transport | TCP, IPv4 | **confirmed** |
| Default port | 38008 | **confirmed** (`NETISO_PORT`) |
| Bind address | `INADDR_ANY` | **confirmed** |
| Listen backlog | 1 | **confirmed** |
| Concurrent clients | 5 (`MAX_CLIENTS`) | **confirmed** |
| Per-connection IO buffer | 4 MiB (`BUFFER_SIZE`) | **confirmed** |
| Max path trailer | 510 (`MAX_PATH_LEN`) | **confirmed** as a constant; not enforced on receive — see §7.3 |
| Max listed file name | 255 (`MAX_FILE_LEN`) | **confirmed** — longer names are silently skipped |
| Max `READ_DIR` entries | 4096 (`MAX_ENTRIES`) | **confirmed** |
| Authentication | none | **confirmed** |
| Write commands | present unless built `-DREAD_ONLY` | **confirmed** |

A client sends one fixed 16-byte command header, sometimes followed by a
path or file payload, then reads a fixed-size response, sometimes followed by
raw data. The server keeps per-connection state and treats the connection as a
state machine: one open read file, one open write file, one open directory, one
CD sector size.

Command handling is strictly sequential per connection. One thread (or task)
per client, and the client never pipelines: it waits for each response before
sending the next command.

### 1.1 Connection policy (confirmed)

These details matter for real clients and are easy to miss:

* A **new connection from an IP that is already connected** closes the old
  connection and takes over its slot. A PS3 that reconnects without dropping
  the old socket would otherwise be refused.
* The IP whitelist is only consulted for a *new* IP, so a connected client is
  never locked out by a whitelist change.
* Once all 5 slots hold different IPs, further connections are closed.
* Only IPv4 is served.

## 2. Packet format

### 2.1 Command header (16 bytes, all requests)

| Offset | Size | Field | Encoding |
|---|---|---|---|
| 0 | 2 | `opcode` | big-endian uint16 |
| 2 | 14 | per-opcode fields | big-endian |

**Every multi-byte field on the wire is big-endian**, in requests and
responses alike, including the `fn_len` and `fp_len`/`dp_len` lengths.
Confirmed from the packed structs in `netiso.h` plus the `BE16`/`BE32`/`BE64`
calls around every field in `main.cpp`.

The PS3 itself is big-endian (Cell PPU), so the client's `BE16()` helpers
compile to nothing there. A previous draft of this document claimed the
length fields of `STAT_FILE` and `OPEN_DIR` were sent unswapped and "worked by
accident"; that reasoning was wrong — there is no inconsistency, and a server
must simply read big-endian.

Single-byte fields (`is_directory`) are unsigned chars. Strings are raw bytes,
**not** NUL-terminated; their length comes from the header. File names arrive
as whatever bytes the filesystem holds (see §7.5 for how we round-trip them).

### 2.2 Path trailer

```
+------------------+---------------------+
| 16-byte header   |  fp_len / dp_len B  |
+------------------+---------------------+
```

Read in full with one blocking `recv(..., MSG_WAITALL)`. A short read ends the
connection. The server NUL-terminates the buffer for its own `stat`/`opendir`
calls; the client never sends a NUL.

### 2.3 Responses

Fixed-size packed structs, big-endian, optionally followed by raw bytes. The
sentinel for "error / not found / no more entries" is `-1` in the relevant
signed field. `0` means success in the `*_result` status fields.

Two responses carry no status at all: `READ_FILE_CRITICAL` and
`READ_CD_2048_CRITICAL`. They either stream exactly the promised number of
bytes or the connection is torn down mid-response. The upstream comments say
what that means to the client: "if file read fails, client is exited".

## 3. Commands

Values come from the `NETISO_CMD` enum, which numbers itself from `0x1224`.

| Opcode | Name | Handler group | § |
|---|---|---|---|
| `0x1224` | `OPEN_FILE` | read | 3.1 |
| `0x1225` | `READ_FILE_CRITICAL` | read | 3.2 |
| `0x1226` | `READ_CD_2048_CRITICAL` | read | 3.3 |
| `0x1227` | `READ_FILE` | read | 3.4 |
| `0x1228` | `CREATE_FILE` | write | 3.5 |
| `0x1229` | `WRITE_FILE` | write | 3.6 |
| `0x122A` | `OPEN_DIR` | read | 3.7 |
| `0x122B` | `READ_DIR_ENTRY` | read¹ | 3.8 |
| `0x122C` | `DELETE_FILE` | write | 3.9 |
| `0x122D` | `MKDIR` | write | 3.10 |
| `0x122E` | `RMDIR` | write | 3.11 |
| `0x122F` | `READ_DIR_ENTRY_V2` | read¹ | 3.12 |
| `0x1230` | `STAT_FILE` | read | 3.13 |
| `0x1231` | `GET_DIR_SIZE` | read | 3.14 |
| `0x1232` | `READ_DIR` | read | 3.15 |
| `0x2412` | `CUSTOM_0` | none | 3.16 |

¹ Upstream builds these read-only **and** write-only: their handlers sit inside
`#ifndef READ_ONLY`, so a `READ_ONLY` ps3netsrv answers `-1` to every directory
listing. That is an upstream bug. Our server always serves them; §9.1.

Anything not in this table takes the `default:` arm: the server logs the opcode
and **closes the connection**. Same for a write command on a read-only server.

### 3.1 `OPEN_FILE` — `0x1224`

Request: `opcode` (BE16), `fp_len` (BE16), 12 B pad. Then `fp_len` bytes of path.

Response (16 B): `int64_t file_size` (BE64, `-1` on error), `uint64_t mtime` (BE64).

* The previous read file is always closed first, whatever happens next.
* `fp_len == 0` is an **error** (`file_size = -1`). Upstream's comment "closes
  the active ro file" is misleading: to close without opening, the client sends
  the literal 10-byte path `/CLOSEFILE`, which is checked *before* path
  translation, returns `file_size = -1` and leaves no file open. Confirmed.
* `-1` is also what a client sees for a file that genuinely does not exist, so
  `OPEN_FILE` cannot distinguish "closed" from "missing". Use `STAT_FILE` if a
  client needs to.
* Paths starting `/***PS3***/` or `/***DVD***/` build a virtual ISO on the fly
  (§7.4). Our server does not implement this; §9.2.
* A path that resolves to the share root itself plus `/.` is also treated as a
  virtual ISO (upstream's way of letting a client mount the whole share).
* Sector-size probing happens here; §5.
* `mtime` is the file's `st_mtime` as a Unix epoch. On error it is `0`.

### 3.2 `READ_FILE_CRITICAL` — `0x1225`

Request: `opcode` (BE16), `pad` (BE16), `num_bytes` (BE32), `offset` (BE64).
Response: exactly `num_bytes` raw bytes, no length prefix, no status.

* Seeks the open file to `offset` and transfers in `BUFFER_SIZE` (4 MiB) chunks
  until `num_bytes` are sent.
* A short read at EOF, a failed seek, or a failed send ends the connection
  without a response — this command has no way to report an error.
* `num_bytes == 0` sends nothing and succeeds.
* This is the command the PS3 uses to stream a game, so throughput here is the
  whole game experience. See §6.

### 3.3 `READ_CD_2048_CRITICAL` — `0x1226`

Request: `opcode` (BE16), `pad` (BE16), `start_sector` (BE32), `sector_count`
(BE32), `pad2` (BE32). Response: `sector_count * 2048` raw bytes.

Extracts the 2048-byte user data out of raw sectors:

```
byte_offset = start_sector * CD_SECTOR_SIZE + 24
```

and then, per sector, reads 2048 bytes from `byte_offset` and advances
`byte_offset` by `CD_SECTOR_SIZE`. The 24 skipped bytes are the sector's sync
(12 B), header (4 B) and sub-header (8 B) — a raw Mode 2 Form 1 sector. Not
"subchannel data", as one draft of this document claimed; subchannel data lives
in the lead-in and is never in a `.bin`. Confirmed against the reference source.

* Fails (connection closed) if `sector_count * 2048 > BUFFER_SIZE`, so
  `sector_count <= 2048` per request.
* If the probe settled on `CD_SECTOR_SIZE == 2048` the `+24` is still applied,
  which shifts a plain 2048-byte image by 24 bytes. Measured, not theorised: on
  a plain image we answer at stride 2048 but 24 bytes late, and both upstreams
  answer at stride 2352 *and* 24 bytes late, because their probe misses the 2048
  stride entirely (§5, §8). A client that wants a plain image has to use
  `READ_FILE_CRITICAL`, which has no `+24`.
* Used by PS1-class clients (`READ_CD_2048_CRITICAL` in the client's
  `netiso.h`). `Tested locally against a raw MODE2/2352 image`; §6.2.

### 3.4 `READ_FILE` — `0x1227`

Request: same shape as §3.2 — `opcode`, `pad`, `num_bytes` (BE32), `offset` (BE64).

Response: `int32_t bytes_read` (BE32), then `bytes_read` raw bytes when
positive. `bytes_read` is `-1` on error or when no file is open, `0` at EOF.

* `num_bytes > BUFFER_SIZE` returns `-1`; the connection stays open.
* A single read of at most `num_bytes`, so a client that wants a whole file
  drives its own loop. Used for ordinary transfers (webMAN's copy/FTP-style
  file access), where a failure must be reportable instead of fatal.

### 3.5 `CREATE_FILE` — `0x1228` (write)

Request: `opcode`, `fp_len` (BE16), 12 B pad, then the path.
Response: `int32_t create_result` (BE32; `0` ok, `-1` error).

Closes the previous write file, then opens with `O_WRONLY|O_CREAT|O_TRUNC` — an
existing file is truncated. `fp_len == 0` is an error. If the target is a
directory the open fails but upstream still answers `0` with no handle
attached, so a following `WRITE_FILE` answers `-1`. Confirmed.

### 3.6 `WRITE_FILE` — `0x1229` (write)

Request: `opcode`, `pad`, `num_bytes` (BE32), `pad2` (BE64), then `num_bytes`
of data. Response: `int32_t bytes_written` (BE32; `-1` on error).

* `num_bytes > BUFFER_SIZE` closes the connection (upstream's abort path).
* Written to the file opened by §3.5. One write file per connection.

### 3.7 `OPEN_DIR` — `0x122A`

Request: `opcode`, `dp_len` (BE16), 12 B pad, then the path.
Response: `int32_t open_result` (BE32; `0` ok, `-1` error).

* Closes any previously open directory first.
* The directory must exist and be listable, otherwise `-1`.
* `subdirs` is set to 1 when the path *as received* contains `//` anywhere
  (a substring test, not a trailing-slash rule). That flag drives recursion in
  §3.15 only; it does not otherwise change the path.
* The stored directory path keeps a trailing `/` internally; `dirpath` is used
  by §3.8 and §3.15 to build full paths.
* `dp_len == 0` yields `opendir("")`, which fails, so `-1`.

### 3.8 `READ_DIR_ENTRY` — `0x122B`

Request: `opcode`, 14 B pad (ignored).

Response (11 B fixed, then the name):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `int64_t file_size` (BE64) |
| 8 | 2 | `uint16_t fn_len` (BE16) |
| 10 | 1 | `int8_t is_directory` |
| 11 | `fn_len` | name bytes, only when `file_size != -1` |

* `file_size` is the file's size, `0` for a directory, `-1` for "no more
  entries" or an error. On `-1` no name is sent and `fn_len`/`is_directory` are
  zero, so a client must check `file_size` first.
* `.` and `..` are skipped. Hidden files (names starting with `.`) are listed.
* Names longer than 255 bytes are skipped, silently and permanently.
* The directory is closed automatically at the end of the listing or if the
  `stat` of an entry fails, so a further call after `-1` also answers `-1`.
* Iteration order is raw `readdir` order — on Linux that is filesystem order,
  **not** sorted. Clients sort for themselves. Our server sorts by name by
  default; §9.3 explains how to match upstream byte for byte.

### 3.9 `DELETE_FILE` — `0x122C` (write)

Request: `opcode`, `fp_len`, pad, path. Response: `int32_t delete_result`
(BE32) — the raw `unlink()` return, so `0` or `-1`. Deleting a directory
returns `-1`.

### 3.10 `MKDIR` — `0x122D` (write)

Request: `opcode`, `dp_len`, pad, path. Response: `int32_t mkdir_result`
(BE32) — the raw `mkdir()` return.

### 3.11 `RMDIR` — `0x122E` (write)

Request: `opcode`, `dp_len`, pad, path. Response: `int32_t rmdir_result`
(BE32) — the raw `rmdir()` return, so it only succeeds on an empty directory
(that is the host's `rmdir(2)`, not a server rule).

### 3.12 `READ_DIR_ENTRY_V2` — `0x122F`

Same request and same rules as §3.8, with timestamps. Response is 35 B + name:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `int64_t file_size` (BE64) |
| 8 | 8 | `uint64_t mtime` (BE64) |
| 16 | 8 | `uint64_t ctime` (BE64) |
| 24 | 8 | `uint64_t atime` (BE64) |
| 32 | 2 | `uint16_t fn_len` (BE16) |
| 34 | 1 | `int8_t is_directory` |
| 35 | `fn_len` | name bytes, only when `file_size != -1` |

### 3.13 `STAT_FILE` — `0x1230`

Request: `opcode`, `fp_len` (BE16), 12 B pad, then the path.
Response is 33 B:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `int64_t file_size` (BE64; `0` for a directory, `-1` on error) |
| 8 | 8 | `uint64_t mtime` (BE64) |
| 16 | 8 | `uint64_t ctime` (BE64) |
| 24 | 8 | `uint64_t atime` (BE64) |
| 32 | 1 | `int8_t is_directory` |

`STAT_FILE` is the only command that distinguishes "missing" from "empty": a
missing path gives `file_size == -1` with the connection left open. A failed
`translate_path` (an unsafe path) *does* close the connection.

`ctime` is `st_ctime` (inode change time on Linux), not creation time.

### 3.14 `GET_DIR_SIZE` — `0x1231`

Request: `opcode`, `dp_len` (BE16), 12 B pad, then the path.
Response: `int64_t dir_size` (BE64; `-1` on error).

Recursively sums the sizes of regular files at any depth. Directories are
descended, not counted. Symlinks are `stat`ed through but count as neither
regular file nor directory, so they add 0. A `stat` failure part way through
returns `-1` for the whole call. Names longer than 255 bytes are skipped.

### 3.15 `READ_DIR` — `0x1232`

Bulk listing: one request, whole listing in one response.

Request: `opcode`, 14 B pad (ignored). Uses the directory opened by §3.7.

Response: `int64_t dir_size` (BE64) — the entry count — then `dir_size` records
of exactly 529 bytes each:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `int64_t file_size` (BE64; `0` for a directory) |
| 8 | 8 | `uint64_t mtime` (BE64) |
| 16 | 1 | `int8_t is_directory` |
| 17 | 512 | `char name[512]` |

* The whole record array is sent as one block, so the 512-byte name field is
  **always** present in full: the name plus a NUL, then zero padding. Confirmed
  (upstream memsets the array, then sends `529 * items`).
* `name` is the bare entry name, or the path **relative to the opened
  directory** when `subdirs` is set — so recursive listings return
  ` subdir / file ` style names that the client joins itself.
* `mtime` falls back to `ctime`, then to `atime`, if `mtime` is 0.
* If `subdirs` is set, directories are descended into and are **not** listed as
  entries themselves. Recursion is one level deep in practice: the recursive
  call is guarded so a subdirectory of a subdirectory is skipped entirely, and
  its files are therefore missing from the listing. Confirmed; §9.3.
* If `subdirs` is not set, directories appear as entries with `file_size = 0`.
* An entry whose `stat` fails is reported as a directory of size 0 and mtime 0.
* Hard cap of 4096 entries, and entries are dropped rather than failing when
  `dirpath + name` would overflow the reference's `MAX_PATH_LEN + root_len + …`
  scratch buffer.
* Entries come back in raw `readdir` order; our server sorts them by name like
  every other listing (§9.3).
* **`READ_DIR` consumes the open directory**: upstream closes the handle after
  listing, so a second `READ_DIR` without another `OPEN_DIR` answers
  `dir_size = 0`. Confirmed.

### 3.16 `CUSTOM_0` — `0x2412`

An enum slot commented "replace this with any custom command". No handler
upstream, so it hits `default:` and the connection is closed. Reserved for
future extensions in the `0x24xx` range; our server may answer an unknown
`0x24xx` opcode differently once a real use exists.

## 4. Command flows

Mount and stream a PS3 ISO (folder games and virtual ISOs are upstream-only;
§9.2):

```
OPEN_FILE  "/PS3ISO/BCUS12345-[Game].iso"     -> file_size, mtime
loop while playing:
    READ_FILE_CRITICAL  offset, 4 MiB           -> 4 MiB raw
```

Mount and stream a PS1 CD image (a raw `.bin`, §5):

```
OPEN_FILE  "/PSXISO/Driver 2 (USA) (Disc 1) (Rev 1).bin"  -> file_size, mtime, sector size 2352
loop:
    READ_CD_2048_CRITICAL  start_sector, sector_count     -> sector_count * 2048 raw
```

List a directory two ways:

```
OPEN_DIR  "/GAMES"          -> 0
READ_DIR_ENTRY (repeat)     -> one entry each, until file_size == -1

OPEN_DIR  "/GAMES//"         -> 0   (subdirs: recursive)
READ_DIR                     -> dir_size, then dir_size * 529 B
```

Copy a file **from the server to the PS3** (read commands only; the client
writes to its own HDD):

```
STAT_FILE  "/GAMES/<file>.iso"   -> file_size, mtime, ctime, atime
loop offset < file_size:
    READ_FILE  offset, <=4 MiB   -> bytes_read, then bytes_read B
```

Copy **from the PS3 to the server** (needs a writable server):

```
CREATE_FILE "/GAMES/upload.bin"  -> 0
loop:
    WRITE_FILE  num_bytes + payload   -> bytes_written
```

## 5. CD sector-size detection

Done inside `OPEN_FILE`, on the server's per-connection 4 MiB buffer.

* `CD_SECTOR_SIZE` starts at 2352 for every connection and is reset to 2352 on
  every `OPEN_FILE`.
* The probe only runs when `2 MiB <= file_size <= 848 MiB`. Larger files (real
  PS3 game ISOs) are never probed, and stay at the default 2352 — harmless,
  because a PS3 client streams those with `READ_FILE_CRITICAL`, which is
  sector-size agnostic.
* Candidate sector sizes, tried in this order:
  `2352, 2048, 2336, 2448, 2328, 2368, 2340`.
* For each candidate, sector 16 is the first volume descriptor. Upstream seeks
  to `(sector_size << 4) + 0x18` and looks for `"PLAYSTATION "` or the ISO 9660
  volume-descriptor pattern (`byte 0 == 0x01`, then `"CD001"`).
* The first candidate that matches wins.

> **Upstream's probe is broken in two ways.** It reads 12 bytes, checks bytes
> 8..19 for `"PLAYSTATION "`, then reads 5 more bytes into the *same* buffer and
> checks bytes 1..5 for `"CD001"` with `byte 0 == 1`. Both checks therefore read
> past the data that was just fetched, comparing against whatever the shared
> 4 MiB buffer still held from earlier traffic. It also only ever looks at
> `+24`, which is right for raw Mode 2 Form 1 sectors (and for the 2048 case by
> coincidence of stale data) but wrong for plain 2048-byte images, where volume
> descriptor 1 begins at exactly `2048 * 16` with no 24-byte prefix — a plain
> PSX `.bin` in mode 1 form gets missed and read with the wrong stride.

Our implementation probes each candidate deterministically at both plausible
offsets — `sector_size * 16` and `sector_size * 16 + 24` — with no reused
buffer, so the answer depends on the file and not on what the connection did
before.

**What the probe is worth, measured.** No command reports the probe's answer, but
`READ_CD_2048_CRITICAL` gives it away: sector *i* of the reply is the file at
`i * stride + 24`, so rebuilding the reply at each candidate and finding the one
that is byte-identical reads the stride back off the wire
(`netsrv_diff.py`'s `cd_stride` scenarios; `--psx FILE` runs the same measurement
on a real rip).

| image | the image is | ours | `ps3netsrv` | `ps3netsrv++` |
|---|---|---|---|---|
| `Driver 2` rip, 647 MB, `MODE2/2352` | 2352 | **2352** — measured | 2352 — never left its default | 2352 — hardcoded |
| synthetic raw 2352 | 2352 | **2352** | 2352 | 2352 |
| synthetic plain 2048 | 2048 | **2048** | 2352 ✗ | 2352 ✗ |

Only the third row shows anything, and it is the only fixture whose answer is
*not* 2352: there both upstreams stay at 2352, which is what "the probe found
nothing" looks like. On the first two rows every server agrees because 2352 is
everybody's default, so agreement proves nothing about anybody's probe.
`ps3netsrv` cannot do better: its `"PLAYSTATION "` check inspects `buffer[8..19]`
after a 12-byte read, so eight of the twelve bytes it compares are whatever the
shared 4 MiB buffer last held, and its follow-up 5-byte read continues 12 bytes
into the volume descriptor and reads `"STATI"`, so its `"CD001"` check can never
pass at all. `ps3netsrv++` has no probe in the first place — it hardcodes 2352
wherever it reads a CD. Ours is the only answer that comes from the file, and it
is right on all three fixtures; `server/netsrv/netsrv_test.py` checks all seven
candidate strides, plus the real rip when `PSX_BIN` points at one. See §9.5.

`Driver 2 (USA) (Disc 1) (Rev 1).bin`, 647,268,048 bytes, `MODE2/2352` per its
`.cue`, first volume descriptor at `16*2352 + 24` holding `0x01 'C' 'D' '0' '0'
'1'` at +0 and `"PLAYSTATION "` at +8. Not in the repository — it is a 647 MB
rip, and the harnesses take its path as an argument.

## 6. Throughput notes

`READ_FILE_CRITICAL` is what a PS3 spends its life in, so it is the only path
worth measuring. The payload is streamed in `CHUNK` pieces (1 MiB): one
`os.pread` per piece, off the event loop in a worker thread, and the result
handed to asyncio's transport. `pread` rather than `seek`+`read` because it
takes the position as an argument, so nothing has to be shared with the file
object, and because a read that returns the whole request — the usual case —
is passed on exactly as the kernel gave it. The accumulate-and-recopy path only
runs when a read comes back short, and at these sizes that extra copy costs a
few hundred MiB/s.

Two things were tried and rejected. `os.sendfile` on the raw socket mixes with
a transport that already owns that fd, and the two fight over the kernel's
writer registration; the transport-level `BaseTransport.sendfile` that would
have done it correctly was removed in Python 3.14. So the payload does pass
through userspace, once, and the numbers below say what that costs.

Measured with `server/netsrv/netsrv_bench.py` over loopback on one machine, the
same 617 MB `Driver 2` image, 4 MiB requests, and one loopback address per
client — the server hands out a slot per *IP*, so five connections from one
address is five clients taking each other's slot (§1.1), not five clients
streaming:

| server | one client | five at once (combined) |
|---|---|---|
| ours | 1255–1358 MiB/s | 2267–2559 MiB/s |
| `ps3netsrv` (webMAN) | 1625–1692 MiB/s | 1378–1775 MiB/s |

Read it honestly: upstream is faster at one stream, which is what a raw C `send`
loop with no thread hop per megabyte buys it, and slower in total, because its
thread per client starts contending. Both figures are ten times a gigabit link
(119 MiB/s), and the console's own HDD is the tighter limit before that, so on
the real network the difference is invisible. What the table does show is that
the Python server has enough headroom to keep five consoles busy at wire speed
at once, which is the case it has to survive.

Results from each run are appended to `log.md`.

### 6.1 Verified locally

* `server/netsrv/netsrv_test.py` — 62 unit and loopback end-to-end tests: the
  wire layouts and their sizes, the probe against all seven candidate strides,
  path handling, the slot rules, and streaming a real 617 MiB rip through the
  server.
* `server/netsrv/netsrv_diff.py` — the same command script against our server,
  webMAN's `ps3netsrv` and dirkvdb's `ps3netsrv++` on one tree: **48 identical,
  15 declared (§9), 0 undeclared**, with `--psx` adding the stride measurement on
  a real rip (§5). It runs our server with `LISTING_SORTED` switched off for the
  comparison, so the difference is the wire and not our chosen order.
* `server/netsrv/netsrv_bench.py` — the table above.

### 6.2 PS1 fixture

`Driver 2 (USA) (Disc 1) (Rev 1).bin` and its `.cue` — 647,268,048 bytes
(617 MiB), not in the repository, taken as an argument by all three harnesses
(`PSX_BIN=…` for the tests, `--psx` for the differential, `--file` for the
benchmark). Exercises: the probe against a real `MODE2/2352` image (§5),
`READ_CD_2048_CRITICAL` at stride 2352, sequential reads across a file far larger
than the page-cache-familiar range, and reads at the end of a large file.

## 7. Security and limits

### 7.1 Path sandboxing (confirmed)

* The share root comes from the command line. Every client path is
  normalised, must start with `/`, and is appended to the root.
* Normalisation replaces `\` with `/` and strips a trailing `\r`. Only a
  trailing `\r` is stripped, not interior ones.
* Upstream rejects the first occurrence of `/..` only when it is followed by
  `/` or the end of the string, and it does **not** collapse `//` before
  checking. So `a/../../b` slips past the check (`/..` followed by `.`), while
  `a//../b` gets a legitimate path rejected. Our server is stricter: any `..`
  path component is rejected, whatever the spelling. §9.6.
* Upstream never resolves symlinks: it `stat`s them, so a symlink inside the
  share pointing outside it is served as if it were the target, and directory
  reads follow it out of the tree. Our server never follows a symlink at all:
  they are `lstat`ed like upstream, but refuse to open. §9.4.
* `root == "/"` is refused at startup.

### 7.2 Read-only vs writable (confirmed)

Upstream's `-DREAD_ONLY` removes the handlers for `CREATE_FILE`, `WRITE_FILE`,
`DELETE_FILE`, `MKDIR`, `RMDIR`, `READ_DIR_ENTRY` and `READ_DIR_ENTRY_V2`;
those opcodes then hit `default:` and close the connection. Dropping the two
directory-entry commands is a bug — a read-only server cannot list folders.
Our server has a runtime flag for the write commands, defaults to off, and
serves the directory commands either way. §9.1.

There is no per-user or per-path permission model, and no authentication of any
kind: every accepted connection sees the whole share with the server's own
file permissions. Keep this server on a trusted LAN, or behind the whitelist.

### 7.3 Length limits

| Constant | Value | Effect |
|---|---|---|
| `MAX_PATH_LEN` | 510 | root path buffer, and the limit on a *relative* path built from the root — not on the incoming trailer |
| `MAX_FILE_LEN` | 255 | names longer than this are skipped by all three listing commands |
| `MAX_ENTRIES` | 4096 | `READ_DIR` cap |
| `MAX_CLIENTS` | 5 | concurrent connections, per IP as well |
| `BUFFER_SIZE` | 4 MiB | largest single read, write, or CD read |

`fp_len`/`dp_len` are `uint16`, so up to 65535 bytes of path can be announced.
Upstream `malloc`s `MAX_PATH_LEN + len + 1` and `recv`s `len` bytes into it, so
a large length is accepted rather than rejected — it is not a fixed-buffer
overflow, but it is an unbounded allocation per request and an easy memory-dot
DoS. Our server rejects `fp_len > 65535` trivially and `> MAX_PATH_LEN`
outright, closing the connection. §9.7. Upstream blocks forever on a
zero-length trailer instead (§8).

### 7.4 Virtual ISO

`/***PS3***/<dir>` turns a game folder into a PS3 ISO and `/***DVD***/<dir>`
into a DVD/BD ISO, read on demand through upstream's `VIsoFile`. It means
generating ISO 9660 with Rock Ridge and Joliet, ECMA-130 paths, and the PS3
disc layout — a project on its own. Not implemented here; §9.2.

### 7.5 Filenames

Paths are raw bytes. Our server decodes them with UTF-8 `surrogateescape` and
re-encodes the same way, so non-UTF-8 names round-trip unchanged and the byte
count in `fn_len` stays honest. Directory names are listed as raw bytes read
from the filesystem for the same reason.

## 8. Settled against the upstream binaries, and what a PS3 still has to answer

`server/netsrv/netsrv_diff.py` runs a script of commands against our server and
against upstream's own binaries on the same tree, and compares the answers. Its
current tally is **48 byte-identical, 15 declared deviations (§9), 0
undeclared**, and it is what the claims below rest on. Eight of the fifteen are
one cause: the symlink inside the test share, which we refuse to follow (§9.4).
Two more are the zero-length-trailer wedge (§9.9) and two are the sector probe
(§9.5); the rest are one each for recursion depth, path checks, trailer length,
and opening a directory. §9.2 (no virtual ISO) and §9.12 (no `.INI` merge) never
show up, because the fixture cannot produce a valid synthesised ISO and neither
binary merges folders the way the other does; those two stay declared and
unproven.

Six findings are worth more than a line, and all of them were measured, not
read out of the source:

* **Upstream wedges a connection — permanently — on any zero-length trailer
  except `OPEN_FILE`.** Sending `opcode, len = 0` makes `CREATE_FILE`,
  `OPEN_DIR`, `DELETE_FILE`, `STAT_FILE` and `GET_DIR_SIZE` block forever, with
  no answer and the connection left open. Their handlers are:

  ```c
  char *filepath = (char *)malloc(MAX_PATH_LEN + fp_len + 1);
  filepath[fp_len] = 0;
  int ret = recv_all(client->s, (void *)filepath, fp_len);   // recv(..., 0, MSG_WAITALL)
  ```

  A blocking `recv` for zero bytes never returns on Linux, so the thread parks
  inside `recv` and the slot is lost. Five such requests and the whole server is
  dead — and five requests is one browser tab with keep-alive. Our server answers
  `-1` and moves on. This is the reason §9.9 exists.
* **Neither upstream's sector probe can be shown to work, and one of them does
  not exist.** The measurement that matters is the plain-2048 fixture, because it
  is the only one whose answer is not 2352 — and there both binaries answer 2352,
  which is their default, i.e. "found nothing". `ps3netsrv` could hardly do
  better: its `"PLAYSTATION "` check inspects twelve bytes after reading twelve
  into a shared 4 MiB buffer, so eight of the bytes it compares were never
  fetched, and its follow-up read continues twelve bytes into the volume
  descriptor, so its `"CD001"` check cannot pass at all (§5). `ps3netsrv++` has
  no probe: it hardcodes 2352 wherever it reads a CD. On a real `MODE2/2352` rip
  all three agree at 2352, for three different reasons, and that agreement proves
  nothing. Ours measures, and is right on every fixture (§5, §9.5). The same
  binary's `-w` option is advertised in its usage text and has no case in its
  `getopt` switch, so passing it prints the usage and exits — which is why
  `netsrv_diff.py --psx` serves a hardlink of the rip rather than the rip's own
  directory.
* **`READ_CD_2048_CRITICAL` on a plain 2048-byte image is wrong everywhere.**
  The `+24` skip is unconditional, so all three servers return sector data
  shifted by 24 bytes. §3.3's warning is confirmed, in the direction that
  matters least: the command is only sent for images the client found raw.
* **A failed `STAT_FILE` leaks upstream's stack.** On a missing path only
  `file_size` is assigned, so the eight-byte timestamp fields and the
  `is_directory` byte go out uninitialised: `-752867497375432704` and `-56` in
  one run, other garbage in the next. Any client that reads them without
  checking `file_size` first gets noise, and a remote peer can read stack bytes
  that were left there by something else. Ours zeroes them (§9.10).
* **`CREATE_FILE` on a directory answers `-1`**, not success-with-no-handle as
  the earlier draft of this document claimed. Both binaries agree; the draft was
  wrong and §3.5 is corrected.
* **The two upstreams disagree with each other** in 23 of the 63 scenarios, which
  is the real reason "byte-identical to upstream" needs a named upstream. The
  interesting ones:

  | Scenario | `ps3netsrv` (webMAN) | `ps3netsrv++` (dirkvdb) | ours |
  |---|---|---|---|
  | `OPEN_FILE` with a `\`-separated path | opens it | `-1` | opens it (§7.1) |
  | trailing slash / trailing `\r` in a path | opens it | `-1` | opens it |
  | `STAT_FILE` of a directory | `0` | its `st_size` (80, 140) | `0` (§3.13) |
  | unknown opcode | closes | answers `-1`, stays open | closes |
  | `CREATE_FILE` on a directory | `-1` | `-1` | `-1` |
  | `RMDIR` of a non-empty directory | `0`, directory gone | `-1`, directory kept | `-1`, directory kept |

  The `RMDIR` row is not a style difference: webMAN's server deletes a
  non-empty directory tree with the client's permissions. Ours is `rmdir(2)`,
  which is what the name promises (§3.11). `ps3netsrv++` also has no `.INI`
  merge, no `/***PS3***/`, and no recursive `READ_DIR`, which is why §9.2 and
  §9.12 can only be checked against webMAN's.

Still to be settled with a PS3 running webMAN MOD, with results logged in
`log.md`:

* Whether a client ever sends `READ_CD_2048_CRITICAL` for an image whose stride
  is 2048, which would put the `+24` bug of §3.3 in front of a real user.
* Whether the probe's 848 MiB ceiling starves a client streaming a large raw
  `.bin`. The ceiling is upstream's, and now that the probe itself is correct a
  1 GB raw image at 2352 is reported as 2352 by us and left at the default by
  upstream — different answers, and only a PS3 says which one the client wanted.
* The candidate table for oddball rips. `netsrv_test.py` proves our probe finds
  2448/2328/2368/2340 when they are there, not that any rip uses one.
* Whether webMAN keeps more than one connection per console, and whether it
  relies on the same-IP takeover of §1.1.
* Whether any client sends an opcode outside §3's table, in particular in the
  `0x24xx` range.
* Whether `ctime` is displayed as a creation time (it is not one on Linux).

## 9. Where we deliberately differ from upstream

"Upstream" here means **webMAN MOD's `ps3netsrv`**, the primary reference in
§8; where dirkvdb's `ps3netsrv++` differs from both, its row is in §8's table.
Each deviation was chosen so that a PS3 cannot take the server down or read
outside the share. Everything else is byte-identical, which
`server/netsrv/netsrv_diff.py` checks.

1. **Directory listing in read-only mode.** Upstream's `READ_ONLY` build drops
   `READ_DIR_ENTRY`/`_V2`. We always serve them.
2. **Virtual ISO.** `/***PS3***/` and `/***DVD***/` are not implemented; such a
   path is reported as not found (`OPEN_FILE` → `-1`) rather than answered with
   a synthesised ISO. Keep upstream's binary available if a library needs
   folder games.
3. **Listings are sorted, and recursion is complete.** Two related choices.
   Upstream hands back raw `readdir` order, which is not sorted and differs
   between filesystems, so the same library can arrive in a different order on
   two machines; we sort by name, because that is reproducible. And upstream's
   recursive `READ_DIR` skips a subdirectory of a subdirectory *entirely*
   (§3.15), so a listing quietly omits files; we recurse to full depth. These
   are the two deviations that can change what a client displays, which is why
   they are here first. `netsrv_diff.py` runs with `LISTING_SORTED = False` so
   that everything else is compared honestly, and the unit tests follow the
   switch.
4. **Symlinks are never followed.** Files are opened with `O_NOFOLLOW` and
   everything is `lstat`ed, exactly as upstream stats. A share therefore has to
   hold real files: a symlinked game is listed, but cannot be opened, and
   `GET_DIR_SIZE` does not count it. §7.1.
5. **Sector-size probe.** Deterministic, no reused buffer, probes both `+0` and
   `+24` (§5) — and is right about the one real rip available to test, where
   both upstream binaries are wrong (§8).
6. **Path checks.** Any `..` component is rejected, and empty components are
   dropped before the check, so `a/../../b` is caught (upstream serves that
   file) and `a//../b`-style spellings cannot punish a legitimate path.
7. **Trailer length.** `fp_len`/`dp_len > 510` closes the connection instead of
   allocating whatever the client announces (§7.3).
8. **Root path is made absolute at startup, and a symlinked root is refused.**
   Upstream accepts a relative root and concatenates, which is fine until the
   root itself is a symlink.
9. **A zero-length trailer is answered, not waited for.** All of them, with the
   command's own error value (`-1`), and the connection stays usable. Upstream
   hangs forever on five of the six (§8).
10. **A failed `STAT_FILE` answers zeroes.** Upstream leaves the timestamp
    fields and `is_directory` uninitialised and sends them (§8). A client must
    not look at them after `file_size == -1`; a server has no business sending
    them at all.
11. **A directory cannot be opened as a read file.** Upstream opens one, reports
    the host's directory size (`st_size`: 80, 140) as `file_size`, and lets the
    first read fail. We answer `-1`, because the number means nothing and a
    client that trusts it is reading a game that is not there.
12. **`.INI` merge.** Upstream lets `<dir>.INI` files outside the share redirect
    a listing or a lookup to other directories (their "merge folders" feature).
    Our server ignores `.INI` files: it serves one directory tree and nothing
    else, and ps-linker already unifies its library through `catalog.json`.
    A client relying on it will see those paths as missing.
