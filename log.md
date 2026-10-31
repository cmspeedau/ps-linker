# Development Log

Running log of build, verification, and on-device testing work on PS Linker.
Newest entries first. This complements the [README roadmap](README.md#roadmap) —
it records *what was actually run and observed*, including bugs found while testing.

---

## 2026-10-30 (server) — folder games: the virtual ISO is implemented

`/***PS3***/<dir>` and `/***DVD***/<dir>` had been answered with "not found" —
the one deviation in the protocol a real library could hit (§9.2). They now
build the image. `server/netsrv/virtual_iso.py` is a port of webMAN's
`VIsoFile`: the same extents, the same directory records, the same path tables,
with file contents read off disk at whatever offset a client asks for, so a
30 GB folder game costs a few hundred kilobytes of RAM and no start-up pause.
The image ships in two files now (`COPY netsrv/ps3netsrv.py
netsrv/virtual_iso.py`), and `--build-arg NETSRV=ps3netsrv++` is no longer a
reason to reach for the fallback: that one has no virtual ISO either.

### How it was checked

Three ways, because "it looks like an ISO" is not a claim worth much.

* **Against upstream's own builder.** `VIsoFile.cpp` compiles standalone, so a
  `makeiso` binary was built outside the repo and pointed at the same fixtures:
  a plain PS3 game, a complex one (deep nesting, a 70-file directory, a Joliet
  name, an empty file, a name truncated at 204 bytes), and the complex one again
  in DVD mode. **No differing bytes**, in any of them, once the PS3 disc-info
  sector (random by design) and each volume descriptor's creation timestamp are
  blanked.
* **Against upstream's server, on the wire.** `netsrv_diff.py` grew twelve
  virtual-ISO scenarios: the same folder mounted on both servers, comparing the
  size and mtime; then 40 KiB of the image read out of each and compared with
  those same fields blanked; then DVD mode, the CD-read stride, `STAT_FILE` /
  `OPEN_DIR` / `GET_DIR_SIZE` on a marked path, a folder that is not a game, a
  file, the share root, a marker further in, and a marked path with `..` in it.
  One of them, `/.`, reads the whole synthesised image back from both servers —
  4,456,448 bytes of a mixed tree: nested directories, a non-ASCII name, a
  hidden file, names with spaces, a symlink. **Every byte of the two images
  agrees** except the record upstream gives that symlink (with the size of the
  file outside the share it points at, which then shifts every later file by one
  sector) and the volume identifier of that particular mount (§9.4, §9.2). The
  tally is now **57 identical answers, 17 declared deviations, 0 undeclared**,
  against a reference built on this Mac.
* **82 tests** in `netsrv_test.py` (was 62), run on Alpine 3.20's Python 3.12 as
  well as here: an ISO reader of our own walks the synthesised image and asks it
  for the folder's files by name, in both name spaces, plus an end-to-end pass
  that mounts a folder game over a loopback socket and reads it back.

### What reading upstream turned up

* **The marker is `OPEN_FILE`'s alone.** `translate_path` takes a `viso`
  pointer and only `process_open_cmd` passes one — `STAT_FILE`, `OPEN_DIR`,
  `GET_DIR_SIZE`, `CREATE`, `DELETE`, `MKDIR`, `RMDIR` all pass `NULL` — so
  upstream does *not* strip the marker for those and answers not-found. This
  server stripped it everywhere until the new scenarios caught it; that
  deviation is gone rather than declared.
* **Upstream's CD read is wrong for a folder game.**
  `process_read_cd_2048_critical_cmd` always seeks to
  `sector * CD_SECTOR_SIZE + 24`, and a synthesised image is plain 2048-byte
  sectors with no raw header: upstream answers 24 bytes into the sector the
  client asked for. We read at `sector * 2048` and declare it (§3.3, §9.2).
* **Upstream's `.66600` support does not work.** The path it builds for a part
  truncates one byte too many, so a folder with a split file fails the whole
  build rather than serving it. Ours concatenates the parts into one file, which
  is what the summed size in upstream's own directory record promises.
* **`/.`, exactly, is a third mount spelling**: upstream mounts the whole share,
  in DVD mode. Implemented; the reported size matches on the harness tree.

### Bugs that fell out of testing

A split file came out of the image as two directory records with the same name,
because the code laying out the *image* was also deciding what the *directory*
would say, and those are different questions: one record per file, extents only
past 4 GiB. A zero-length file put an extent starting exactly where the next
file starts, which hid that file from any lookup at its beginning.
`netsrv_test.py`'s `setUpClass` wrote a non-UTF-8 name, which APFS refuses
outright — so the suite could not run on a Mac at all; three checks that need a
second loopback address, or a disk that will hold such a name, now skip instead,
and both platforms are green. And `test_truncated_command_closes` was passing
for the wrong reason: on Python 3.11+ `asyncio.TimeoutError` *is* `TimeoutError`,
which *is* an `OSError`, so the client's 10-second read timeout was laundered
into the `ConnectionClosed` the test asserted. It now half-closes the socket and
watches the server notice, which takes no time at all.

Getting the suite to run here at all needed `from __future__ import annotations`
in the four scripts (the images and annotations are 3.10 syntax and this Mac is
on 3.9) and `Path.lstat()` for one `stat(follow_symlinks=False)`, which is 3.10
too. Nothing about the behaviour changed; the deployment image is Alpine 3.20 on
Python 3.12 either way, and the suite now runs on both.

---

## 2026-10-24 (server) — ps3netsrv rewritten in Python, checked against the real binaries

The image used to `git clone` dirkvdb's `ps3netsrv--` and compile a C++ server
whose behaviour we could only guess at. This replaces it with
`server/netsrv/ps3netsrv.py` — stdlib asyncio, no build step — and settles what
the protocol actually is by running the upstream binaries and reading the wire.
Spec: [`docs/PS3NETSRV-PROTOCOL.md`](docs/PS3NETSRV-PROTOCOL.md).

### How it was checked

Two harnesses, both kept in the repo:

```
python3 server/netsrv/netsrv_test.py          # 62 unit + loopback end-to-end tests
python3 server/netsrv/netsrv_diff.py \
        --reference .../ps3netsrv --reference .../ps3netsrv++   # differential
python3 server/netsrv/netsrv_bench.py --file <617MiB .bin> --reference .../ps3netsrv
```

The references are built outside the repo (`/tmp/opencode/ref/`); webMAN-MOD's
`ps3netsrv` is the primary because that is what a PS3 talks to, and dirkvdb's
`ps3netsrv++` the secondary. Same tree, same command script, compared with a
normaliser where a difference is meaningless (timestamps on a listing, order
while our sort is switched off for the run):

```
[diff] 48 match, 15 declared deviation, 0 unexpected
Ran 62 tests in 11.380s — OK
```

Eight of the fifteen declared deviations are one cause: the symlink in the test
share, which we refuse to follow and upstream follows out of the tree. Two are
upstream's zero-length-trailer wedge, two the sector probe, one each for
recursion depth, path checks, trailer length, and opening a directory.

Throughput, loopback, one machine, 4 MiB `READ_FILE_CRITICAL` requests, the same
617 MB `Driver 2` rip, and one loopback address per client (the server gives a
slot per *IP*, so five connections from one address would be five clients taking
each other's slot, not five clients streaming):

| server | one client | five at once |
|---|---|---|
| ours | 1255–1358 MiB/s | 2267–2559 MiB/s |
| `ps3netsrv` | 1625–1692 MiB/s | 1378–1775 MiB/s |

Upstream wins one stream and loses the total. Both are 10× a gigabit link, so on
a real network the row that matters is the second one, and there we are 1.3–1.6×
ahead. A detour: an early run showed *my* server at 467 MiB/s, which was the
measurement — `readexactly()` refills a 64 KiB buffer a read at a time, so the
test client was the bottleneck. It also found a real bug: `serve()`'s
`async with server` waited on `wait_closed()`, which waits for every handler, so
one client that connected and never spoke blocked shutdown forever. The read path
is `pread` per 1 MiB chunk off the event loop; the single-syscall fast path that
avoids re-copying the chunk was worth 705 → 1261 MiB/s.

### What the upstream binaries did that the source did not make obvious

* **A zero-length trailer kills a slot permanently.** `opcode, len = 0` on
  `CREATE_FILE`, `OPEN_DIR`, `DELETE_FILE`, `STAT_FILE` or `GET_DIR_SIZE` parks
  the thread inside `recv(…, 0, MSG_WAITALL)`, which never returns: no answer,
  connection left open, slot gone. Five requests — one browser tab with
  keep-alive — and the server is dead. Only `OPEN_FILE` guards for it. Ours
  answers `-1` and moves on.
* **A failed `STAT_FILE` sends stack garbage.** Only `file_size` is assigned on
  that path, so the three 8-byte timestamps and `is_directory` go out
  uninitialised: `-752867497375432704` and `-56` in one run, something else in
  the next. Ours zeroes them.
* **`CREATE_FILE` on a directory answers `-1`.** Both binaries, consistently. An
  earlier draft of the spec doc claimed it answered 0 with no handle; that was
  wrong and the doc and the test now say `-1`.
* **`RMDIR` of a non-empty directory deletes the tree** in webMAN's server
  (returns 0, directory and contents gone, with the server's own permissions).
  dirkvdb's returns `-1`. Ours is `rmdir(2)`.
* **Neither upstream's sector probe can be shown to work**, and `ps3netsrv++`
  has none (it hardcodes 2352). The discriminating fixture is the plain-2048
  image, because it is the only one whose answer is not 2352; both binaries
  answered 2352 there. webMAN's check inspects twelve bytes after reading twelve
  into a shared 4 MiB buffer, so eight of the bytes it compares were never
  fetched, and its follow-up read lands mid-volume-descriptor, so its `CD001`
  check can never pass.
* **The two upstreams disagree with each other in 23 of the 63 scenarios** —
  `\`-separated paths, trailing slashes and `\r`, `STAT_FILE` of a directory,
  unknown opcodes, `RMDIR`. "Byte-identical to upstream" needs a named upstream,
  which is why the harness prints a cross-note for every one of them.

A correction to keep, because the first version of these notes was wrong: an
earlier harness run had webMAN reading the `Driver 2` rip at 2048 and dirkvdb at
2368. That measurement came from a stride probe that assumed the image held a
recognisable byte pattern, which is fine for the synthetic fixtures and
meaningless for a real rip. Replaced with one that rebuilds the server's reply at
each candidate stride from the file and takes the byte-identical match
(`netsrv_diff.py --psx FILE`), the answer on the real rip is 2352 from all three
servers — which proves nothing about anybody's probe, since 2352 is also
everybody's default. §5 of the spec now carries the measured table.

### Packaging

`server/Dockerfile` no longer clones or compiles anything by default; the Python
server is a `COPY`. The C++ fallback stays available as `--build-arg
NETSRV=ps3netsrv++`, and that stage now carries the three things upstream's build
needs and its Makefile does not do: `--recursive` for the `utils/` submodule,
`make CXX=g++` (the Makefile hardcodes `clang++`, absent from Alpine's
build-base), and `#include <cstdint>` at the top of `ps3netsrv.cpp` for g++ 13.
Worth knowing separately: dirkvdb's `-w` whitelist is advertised in its own usage
text and has no case in its `getopt` switch, so passing it prints the usage and
exits. `entrypoint.sh` picks the implementation from `$NETSRV` and no longer
starts the C++ one with `-d` — that daemonises, so the parent exits on purpose
and `wait -n` was taking the container down with it.

Built and run:

```
$ docker build -t ps-linker server/ && docker run -d -p 38008:38008 … ps-linker
[ps-linker] starting ps3netsrv (python) on :38008 (library: /games, clients: any, read-only)
[netsrv] listening on 0.0.0.0:38008  path: /games [READ-ONLY]
```

…then driven from the host with the test client against the running container:
listing, `STAT_FILE`, `OPEN_FILE`, `READ_FILE_STATUS`, `READ_FILE_CRITICAL`,
`READ_CD_2048_CRITICAL` and `GET_DIR_SIZE` all answered correctly, and
`CREATE_FILE` was refused and logged as `server is read-only`.

### Status / next steps

- All of this is **uncommitted** (`docs/PS3NETSRV-PROTOCOL.md`, `server/netsrv/*`
  new; `README.md`, `log.md`, `server/Dockerfile`, `server/entrypoint.sh`,
  `server/ps-linker.service`, `server/docker-compose.yml` edited).
- Nothing here has been seen by a PS3. The open questions at the end of spec §8 —
  opcodes a real webMAN client sends, one connection or several, what it makes of
  the probe's 848 MiB ceiling — need hardware, and the answers go in this log.
- Folder games (`/***PS3***/) are not implemented: that means synthesising an ISO
  9660 filesystem over a directory. A library with folder games needs
  `--build-arg NETSRV=ps3netsrv++` until then.

---

## 2026-10-23 (server) — HTTP-share rewrite broke browsing and resume; fixed and checked

Rebasing the Cover Flow work onto `origin/main` pulled in nine commits, one of
which (`Add Cover Flow UI with category menu bar` and its neighbours) rewrote
`server/store/http_server.py` to serve the new web GUI. `server/entrypoint.sh`
runs *that* file as the port-8000 share the PSP and PS3 apps use:

```
entrypoint.sh:18  python3 /opt/store/http_server.py "${GAMES_DIR}" 8000 &
```

So I ran it. Both servers, same `games/` tree (a 5,000,000-byte
`PSP/test.iso`), same URLs — the shape the apps actually request:

| Request | `7923e27` (RPCS3-verified) | `origin/main` |
|---|---|---|
| `GET /` | 200 directory listing | 200 web GUI |
| `GET /PSP/` | 200 directory listing | **404** |
| `GET /PSP/test.iso` | 200, 5000000 bytes | **404** |
| `GET` + `Range: bytes=1000-2999` | **206**, `Content-Range: bytes 1000-2999/5000000`, 2000 bytes | **200**, full body (6756 of 6756) |

**Both device apps are unusable against the current share**: they parse the
directory listing at the share root to build their file list, and there is no
longer one, nor any file to fetch. Resume is dead independently of that: the
new `file()` never returns 206, so `ps3_resume_download()` / the PSP equivalent
fall back to the full-GET restart every time (that fallback is the behaviour we
verified on RPCS3, so a download would restart from zero rather than fail —
except there is nothing to download).

Two causes, both readable in the file:

1. `do_GET` hands the **`ui/` directory** to `self.file()` for *every* path
   (`self.file(ui, '/index.html' if p=='/' else p[3:] if p.startswith('/ui/') else p)`),
   so `root`/the games tree is only ever consulted for `catalog.json`. There is
   no directory-listing branch at all.
2. The regexes are double-escaped, as if a JSON-escaped payload were written
   into the file verbatim:
   `RANGE_RE=re.compile(r'^bytes=(\\d*)-(\\d*)$')` matches a literal `\` then
   `d`, never a real `bytes=1000-2999`; `BAD=re.compile(r'[\\x00-\\x1f\\x7f/\\\\]')`
   is likewise not the control-character class it looks like, so the upload
   filename guard is not doing what its name suggests either.

### Fixed

`server/store/http_server.py` rewritten (readable, stdlib-only): routing is
`/api/*` → `/gui/` (with `/ui/*` as an alias) → the library, where files and
listings come back through the `7923e27` code path. The two regexes are
single-escaped so `RANGE_RE` matches digits and `BAD_NAME_RE` a control-char
class. Bodies are streamed in 256 KiB chunks — the rewrite did
`f.read(end - start + 1)`, i.e. a whole 10 GB ISO into RAM per request. A
`<name>.sha256` sidecar is now served as `X-SHA256`, which closes the open item
"the shipped share never sends a digest". Uploads land via `.part` + rename, and
`catalog.json` is rewritten in whatever top-level shape it already had instead
of being flattened to `{"games": …}`.

The GUI moved to `/gui/` because `/` cannot be both the library listing (what
the apps request first, and what the README tells people to open) and the
browser shell.

**The web GUI never worked either.** `ui.js` fetched the *relative* URL
`catalog.json`, which resolved against the shell's own path — and the handler
answered `{"games": …}` while `ui.js` read `data.items`. Loading `/gui/` showed
"Failed to load catalog: HTTP 404" for any catalogue. It now fetches
`/api/catalog`, which answers `{"items": [...], "categories": [...]}`. In the
browser, `/gui/` renders the carousel with all four fixture titles, and clicking
`PS3 Games` narrows it to *Demon's Souls* + *Journey* with the metadata panel
filled in.

**Also restored:** `docs/UI-REFERENCE.md` had been overwritten — its 175-line
device UI specification (design tokens, the NAS browser and download-view
layouts that `psp/main.c`'s colour `#defines` implement) replaced by an 11-line
web-GUI note. The spec is back from `7923e27`; the web-GUI notes live in
`docs/WEB-GUI.md`.

**Roadmap wording corrected:** two Done lines described GUI features that are
not in the code — an "Add Games" upload panel (multi-file, drag-and-drop) and
"Store queue management, and download activity view". `ui.js` has no upload or
queue code and `index.html` has exactly one control (the Grid toggle); the
upload *endpoint* works, the panel does not exist. Roadmap now says what is
there and the missing panels are open items.

### Checks (in the repo now: `server/store/check_share.sh`, 42 assertions)

```
library   / is the listing · DIR/ and bare-name hrefs · catalog.json and
          .part hidden · Accept-Ranges · X-SHA256 from sidecar · HEAD
          headers-only · 404 for a missing file
Range     206 + Content-Range · closed / open-ended / suffix slices are
          byte-identical to the file · past-EOF 416 · unparseable → full body
GUI + API /gui/ · /gui/ui.css content type · /api/catalog items and categories
upload    201, file on disk, catalog shape preserved · bad extension 400 ·
          encoded and double-encoded traversal refused
--with-dl compiles ps3/resume.c for the host (it needs nothing but BSD
          sockets; its <net/net.h> include is unused there) and runs the
          console's own downloader against the share:
            cold 16,777,216 B → rc=0, digest equals the sidecar
            resume from 8,388,608 B → "Range offset=8388608", status=206,
              exactly 8,388,608 bytes sent, result byte-identical
            lying sidecar → rc=-11 · no sidecar → rc=0, "no response digest"
```

## 2026-10-23 (coverflow) — shared Cover Flow module landed in `ui/`, verified on host + both toolchains

An uploaded `coverflow.{c,h}` + smoke test arrived with an `INTEGRATION.md`
describing wiring into `psp/` and `ps3/`, and claiming the module mirrored
"the web GUI Cover Flow listed in the README Done section". My working base
(`7923e27`) had no web GUI and no such roadmap line, so I started out writing
this entry to correct the premise — and then the push was rejected: origin had
nine new commits, including `server/store/ui/*` (Media Go-style web GUI) and
"Cover Flow UI (web GUI)" in Done. The premise was right, my checkout was
stale. Rebased onto `origin/main`; the module's category labels, view toggle
and favourite semantics line up with `ui.js`, so `ui/INTEGRATION.md` now
records the two places where the two implementations differ on purpose.

What was not right was the code: as delivered it did not run. Landed it as
`ui/` (shared by both apps; the per-directory copy convention only holds where
the APIs really differ), fixed what was broken, and covered each fix with an
assertion.

**Reproduced first, then fixed** (each is now an assertion in
`ui/coverflow_test.c`):

1. **Favourite toggle → SIGSEGV.** `items` was declared `const` while
   `COVERFLOW_KEY_FAVORITE` wrote through a cast-away const. The delivered
   smoke test pressed FAVORITE on a `static const` table:
   `gdb` → `Program received signal SIGSEGV … in coverflow_handle_key`,
   `nm` → `0000000000003220 r kItems` (read-only `.rodata`). Items are
   writable through the API now, and the header says why.
2. **Re-filter kept the wrong cover.** `coverflow_recompute_filter()` compared
   the old *slot* against new *item ids*. Slot 2 of the 5-item PS3 list is
   item 9 ("Uncharted 1"); switching to `All Games` selected item 2. It looked
   right only in `All Games`, where slot == id. Capturing the selected item id
   before the rebuild fixes it; the test asserts both the carry-over and the
   documented fallback (item absent → first entry of the new list).
3. **Centre cover chopped in half.** `render_flow()` looped over the side-tile
   height (7) while the centre tile is 11 rows, so rows 7–10 — its bottom
   border included — were never printed. Loop now walks the centre height and
   blank-pads the shorter neighbours (visible in the frame dump).
4. **Selected category could be invisible.** The menu row clipped to
   `screen_w` *bytes*, ANSI escapes included, and the 7 labels total 91
   columns: at 80 columns `Settings` was dropped, reachable but never shown.
   `coverflow_menu_row()` now windows around the current category with ASCII
   `<` / `>` markers; test asserts every category is visible and the row never
   exceeds the width at 1/8/20/24/40/60/80/120 columns.
5. **Grid ignored `screen_h`** and printed every item (`row` set but never
   used — the one `-Wall` warning the drop shipped with). Now one screenful
   around the selection plus a `[ page x / y ]` strip.
6. **Edge presses reported a change** (`LEFT` on the first cover returned
   "redraw"), so every stray press repainted the screen. Movement keys return
   0 when nothing moved.
7. Hardening: `strncmp` instead of fixed `category[0..2]` reads, label table
   only reachable via `coverflow_category_label()`, out-of-range category
   clamped, `cap` guards on every tile/row builder.

**Verified:**

```
make -C ui test        -std=c11 -Wall -Wextra -Wpedantic -Werror → clean
                       197 assertions pass
make -C ui test-asan   ASan + UBSan, incl. 1×1…400×40 render sweep → clean
psp-gcc  -c -O2 -G0 -Wall -Wextra                    → no warnings (ELF32 LE)
ppu-gcc  -c -O2 -Wall -Wextra -std=gnu99 -mcpu=cell -mminimal-toc
                                                       → no warnings (ELF62 BE)
```

The psp-gcc pass caught a `-Wformat-truncation` the host gcc missed on the
`%*s%s%*s` centring in `cover_title()` (it cannot bound an incoming
`const char *`); that now builds its padded text by hand.

Not claimed: that this appears on a device. The module is **not wired into
either app** and is deliberately kept out of `psp/Makefile`'s `OBJS` and
`ps3/Makefile`'s `SRC` — nothing calls it, so it would only cost space in
`EBOOT.PBP` / `nasdl.self`. The blocker is a product decision, recorded in
`ui/INTEGRATION.md`: both browsers list `FileEntry {name,url,is_dir}`
(`psp/main.c:181`, `ps3/main.c:97`) — no size, category, favourite — while
`psp/store.c` has the right
shape but is neither in `OBJS` nor called anywhere, so `docs/STORE.md`
documents a view that has never been built. Option A (Cover Flow as the Store
renderer, catalog metadata already generated server-side) is recommended over
Option B (Cover Flow as the browser, which needs the HTTP share to start
reporting sizes/categories). `ps3/main.c`'s `pad_buttons()` also maps only 8
bits today — L1/R1/Triangle must be added before the category and favourite
keys are reachable.

## 2026-10-23 (resume) — PS3 Range resume restored + verified on RPCS3 (206, SHA-256)

*Closes the "PS3 resume (Range/206) is gone" item flagged in the netdebug entry
below.* Pulled the new `ps3/resume.c`, `ps3/resume.h` and updated `ps3/main.c`
(the download path goes through `ps3_resume_download()` again), then built,
ran, and tested it for real.

**The new `resume.c` had never been compiled.** The first real `ppu-gcc` build
failed on one line:

```
resume.c:56: error: invalid operands to binary + (have 'char *' and 'char *')
```

`NETDEBUG("... Range header=%s ...", from ? req + strstr(req, "Range: ") : "none")`
adds a pointer to a pointer. Replaced with a plain `range_line[]` buffer
(`bytes=<offset>-` / `none`) and a single request-building branch; the request
text itself is unchanged. Also cleaned two pre-existing `main.c` warnings an
earlier restore left behind (unused `prev`, `Style *` discarding `const`).

**New:** `ps3/Makefile` accepts `EXTRA_CFLAGS`, so the netdebug build is
`make EXTRA_CFLAGS=-DNASDL_NETDEBUG` instead of retyping the whole `CFLAGS`
line. Both the netdebug build and a stock build are warning-free.

### The SHA-256 is real (checked before trusting the emulator log)

The app prints its own digest, so the digest code was checked against
coreutils first, on the host:

```
n = 0 1 55 56 63 64 65 1000 1048576 bytes → all match sha256sum
```

### Bug found while reviewing the draft: a short body counted as complete

`ps3_resume_download()` looped `while (recv(...) > 0)` and treated `n == 0` as
success, so a connection closed early produced a truncated file that was hashed
and reported as a finished download — exactly the failure mode resume must not
have. Now the bytes actually received are compared with `Content-Length`:

```
download incomplete received=1835008 expected=16777216   → rc -9, UI "Failed (-9)"
```

The partial file is left on disk, so the next attempt resumes from it.

### On-device E2E — PS3 / RPCS3

Built with the real toolchain, booted as the `.self` (the loose ELF still
crashes, see below):

```bash
docker run --rm -v /tmp/pslinker-build2:/src -w /src/ps3 \
  ps3dev/ps3dev:submodules make EXTRA_CFLAGS=-DNASDL_NETDEBUG
# → nasdl.self
```

Share: 16 MiB `Homebrew/ResumeTest.bin` on `127.0.0.1:18000`, served by the
repo's own `server/store/http_server.py` handler (the Range/206 code under
test), wrapped in a harness that only throttles body writes (so a transfer is
long enough to interrupt) and logs the `Range` header. Guest keys injected via
XTEST into the RPCS3 *game* window (`nasdl` title — focusing the main RPCS3
window sends keys to the GUI and nothing happens).

**1. Interrupt + resume (the requested test).** Started the download, killed
RPCS3 mid-file at 14,941,184 bytes, relaunched, re-selected the same file.
Netdebug output (`stderr` → RPCS3 `sys_tty_write`):

```
download start URL=http://127.0.0.1:18000/Homebrew/ResumeTest.bin dest=/dev_hdd0/NASDL/ResumeTest.bin resume_offset=14941184
GET URL=http://127.0.0.1:18000/Homebrew/ResumeTest.bin Range=bytes=14941184- result=133
HTTP status=206 Range offset=14941184 content_length=1836032 Accept-Ranges=present
response status=206 mode=206 append resume_offset=14941184
download complete bytes=16777216 sha256=936100238d71cabd2e1749644733a55cc90b333ac351a256060605b7d30b8874
```

Server access log for the same request:

```
[harness] /Homebrew/ResumeTest.bin Range=bytes=14941184-
[http] 127.0.0.1 - "GET /Homebrew/ResumeTest.bin HTTP/1.0" 206 -
```

On-disk file: `16777216 bytes`, SHA-256
`936100238d71cabd2e1749644733a55cc90b333ac351a256060605b7d30b8874` — **equal to
the source file's `sha256sum`**. An earlier run of the same test interrupted at
4,455,424 bytes and resumed from there with the same result.

**2. Server ignores `Range` → safe restart.** Seeded a 3 MiB partial against a
share that strips `Range` and answers `200`:

```
download start ... resume_offset=3145728
HTTP status=200 Range offset=3145728 content_length=16777216 Accept-Ranges=present
response status=200 mode=200 restart resume_offset=0
download complete bytes=16777216 sha256=9361...8874
```

Final size is exactly 16 MiB (not 19 MiB), so the client truncated and rewrote
instead of appending to a partial.

**3. Response digest comparison.** With the harness sending `X-SHA256`:
`SHA-256 verified=9361...8874`. With no header (the shipped server doesn't send
one) it logs `SHA-256 computed (no response digest to compare)=...`, so the
digest is still auditable.

**4. Truncated transfer.** Harness advertised 16 MiB then aborted at ~1.8 MiB:
`download incomplete received=1835008 expected=16777216`, UI `Failed (-9)` — no
false "complete", no digest claim on a short file.

### Still open

- **HTTP 416 wedges a retry**: if the local partial is *larger* than the remote
  file (share replaced/shrunk), the server answers 416 and the client returns
  -7 without discarding the stale partial, so every retry fails the same way.
  Not fixed here; the safe fix is to drop the partial and retry once.
- `NETDEBUG("bytes written=…")` per 64 KiB chunk is ~250 lines for a 16 MiB
  file. Fine for a test, noisy for a real log.
- The shipped `http_server.py` does not emit `X-SHA256`; only the test harness
  exercised the comparison path. Adding it server-side would make every
  download self-verifying.
- `nasdl.elf` still needs the `.self` (or `.pkg`) in RPCS3 — unchanged.

### Regression check

`psp/` untouched: `pspdev/pspdev:latest make` still produces `EBOOT.PBP`, and
the host `url_cwd_test` passes. RPCS3 config was restored to
`Internet enabled: Disconnected` and the emulator stopped afterwards.

---

## 2026-10-23 (even later) — netdebug console on RPCS3: clean URLs + HTTP 200

Pulled the remote `ps3: add NETDEBUG logging` commits (`26214c7`, `5fe7d66`) and
re-ran the PS3 app under RPCS3 booting `nasdl.self`, watching the netdebug
console output.

**Found and repaired a build regression in the netdebug rewrite.** The rewritten
`ps3/main.c` did **not compile**: it reverted to `pd.BTN` and the `PAD_BTN_*`
macros (which PSL1GHT's `padData` doesn't provide) and dropped the
`#define ANSI_BG_ERR` while still using it. So the file was never actually built
or run by whoever wrote it. Repaired it by re-adding the `pad_buttons()` bitmask
helper + `PAD_BTN_*` defines and the `ANSI_BG_ERR` define (same fixes as
`38722a5`); netdebug code untouched. Stock `make` and a `-DNASDL_NETDEBUG` build
both succeed now.

**Also dropped (not yet restored):** the rewrite replaced the `resume.c`
download path with an inline socket `download_file()`, so **PS3 resume
(Range/206) is gone** in the current code — it always does a full GET. Flagging
for a decision (restore `ps3_resume_download` vs keep inline+netdebug). Not
silently re-added because it changes download behaviour beyond the URL fix.

**RPCS3 run — the URL fix is confirmed live.** Built with `-DNASDL_NETDEBUG`;
stderr (where `NETDEBUG` writes) is routed by RPCS3 to the emulated TTY, so the
lines show up in the RPCS3 log. The logged GET URLs are now **single-slash** and
every request is **HTTP/1.0 200 OK**:

- `http://127.0.0.1:18000/`                      → server `GET /`                      200
- `http://127.0.0.1:18000/Homebrew/`             → server `GET /Homebrew/`             200
- `http://127.0.0.1:18000/Homebrew/ResumeTest.bin`→ server `GET /Homebrew/ResumeTest.bin` 200
- downloaded file **SHA-256 matches** the source.

(The old `/Homebrew//…` double slash is gone — the normalized `build_url` works
end-to-end, not just in the unit test.)

**PSP side.** `psp/main.c` already carries the same normalized `build_url`
(landed in `422b2fa`), so the `//` fix is present; the shared host unit test
(`url_cwd_test.c`) passes and the PSP `make` still produces `EBOOT.PBP`. Did
**not** port the `NETDEBUG` macro to PSP: on the PSP every `printf` already goes
to the on-screen debug console (`pspDebugScreenPrintf`), and there is no
BSD-stderr-over-UDP "netdebug" equivalent without `psplink`/`nethost` — so the
macro would need a different mechanism and adds nothing over what the debug
screen already shows. Left as-is (the request marked it optional).

Net: PS3 netdebug run confirms clean paths + 200; PSP `//` fix verified. The
PS3 build fixes (pad_buttons/ANSI_BG_ERR) are in the working tree; the lost PS3
resume path is an open decision.

---

## 2026-10-23 (later) — PS3 on-device E2E on RPCS3: PASSED via the `.self`

*Supersedes the "PS3 blocked" finding in the entry below.* Pulled the remote
fix `157a4ad` (`-mminimal-toc`) and re-ran the PS3 app under RPCS3.

**Result: the PS3 app now runs and is verified end-to-end — when booted as the
`.self`.** Two things had to line up:

1. `-mminimal-toc` (remote fix) restored a proper TOC (the ELF now has a `.toc1`
   section). But the **loose `nasdl.elf` still crashes at C-startup** in RPCS3
   (`Access violation executing 0x7c0802a4`) — RPCS3's ELF loader rejects the
   trailing bogus `LOAD` segment (`ppu_load_exec(): Unexpected segments state`)
   and sets up `r2` wrong. Confirmed under both the LLVM recompiler and the PPU
   interpreter, in a clean run with a fresh JIT cache — so it is not a JIT bug.
2. Booting the **ship artifact `nasdl.self`** (self segments are clean) runs
   correctly. Conclusion: on RPCS3, boot the `.self` (or install the `.pkg`) —
   not the loose ELF. The roadmap "PS3: RSX renderer" item is unchanged, but
   RPCS3's emulated **TTY renders the app's ANSI UI into the log**, so the app
   *is* observable/testable on RPCS3 today.

Driving it with RPCS3's default keyboard mapping (Cross=X, Circle=C, Square=Z,
Start=Return, Select=Space, D-pad=arrows), share at `http://127.0.0.1:18000`,
Internet enabled, verified via the RPCS3 log + HTTP server log + on-disk files:

- `netInitialize` succeeds; `settings.txt` read from `/dev_hdd0/NASDL/`.
- Directory browse: root `/` and `Homebrew/` listings render.
- **Full download**: `GET /Homebrew/ResumeTest.bin → 200`, file written to
  `/dev_hdd0/NASDL/`, **SHA-256 matches source**.
- **Resume**: seeded a 1 MiB partial, re-selected the file → server returns
  **`206 Partial Content`**, file appended 1 MiB → 3 MiB, **SHA-256 matches**.
- Directory navigation and the Settings view both exercised.

Two bugs surfaced on the PS3 side (not yet fixed):

- **URL double-slash** — `g_cwd` carries a leading slash, so joined paths render
  as `/Homebrew//` (the app fetches `/Homebrew//…`). The Range-capable server
  tolerates it (returns 200/206), and the PSP build uses a single slash, so it
  is cosmetic here — but it is a genuine path-join bug to clean up.
- **`nasdl.elf` is not directly bootable in RPCS3** — worth either documenting
  "boot the .self/.pkg" prominently, or fixing the ELF link so the raw ELF has
  a clean segment layout too.

Net: **PSP ✅ and PS3 ✅** both verified end-to-end (browse / download /
resume) against the Range-capable share. Source is unchanged this round
(no new commits); RPCS3 config and the frozen-instance cleanup are reverted to
how they were found.

---

## 2026-10-23 — Toolchain build verification + on-device E2E (PPSSPP / RPCS3)

Goal: finish the roadmap item *"Compile-verify both device builds on real
toolchains (pspdev / PSL1GHT)"* and go further — actually run the PSP and PS3
apps on the locally installed **PPSSPP** and **RPCS3** AppImages.

### Build verification (real toolchains)

Both device builds compile and produce artifacts against the official Docker images.

```bash
docker run --rm -v "$PWD:/src" -w /src/psp pspdev/pspdev:latest make
docker run --rm -v "$PWD:/src" -w /src/ps3 ps3dev/ps3dev:submodules make pkg
```

- **PSP** (`pspdev/pspdev:latest`, GCC 15.2): `psp/EBOOT.PBP`.
- **PS3** (`ps3dev/ps3dev:submodules`, GCC 7.2 / PSL1GHT): `ps3/nasdl.elf` →
  `nasdl.self` (real CEX SELF) and `nasdl.pkg`.

Fixes needed to get from "syntax-clean" to a clean device link (already landed
in the working tree — **not yet committed**):

- `ps3/Makefile`: rewritten to include PSL1GHT's `$(PSL1GHT)/ppu_rules`
  (`OFILES := $(OBJS)` must be set *before* the include; `LD` pointed at
  `ppu-gcc` to override make's built-in `ld`).
- `ps3`: `-lsysmodule` added to `LIBS` (`libnet`'s `netInitialize` references
  `sysModuleLoad`); a `pad_buttons()` bitmask helper added because PSL1GHT's
  `padData` has no `BTN` field / `PAD_BTN_*` macros.
- `psp/Makefile`: `LIBS` trimmed to `-lpsphttp -lpspnet_inet -lpsputility`
  (build.mak appends the SDK libs last, which fixes the import order).
- `psp`: PSP-API corrections (`sceHttpAddExtraHeader`, `SceULong64`, includes).

The recurring `psp-fixup-imports: stubs out of order` warning is a pspsdk quirk
(it reproduces with a minimal `-lpspnet_inet` program); the EBOOT builds fine.

### On-device E2E — PSP / PPSSPP ✅

Setup: test library + a Range-capable HTTP share (`server/store/http_server.py`)
on `127.0.0.1:18000`; `EBOOT.PBP` installed at `~/.config/ppsspp/PSP/GAME/NASDL/`
with a `settings.txt` pointing at the share; PPSSPP driven via X11 (XTEST keys +
window capture). PPSSPP's virtual WLAN must be on (`[Network] EnableWlan = True`),
otherwise `sceNetApctlConnect` returns `WLAN_SWITCH_OFF`.

Real bugs found by actually running it (both fixed in the working tree):

1. **Missing HTTP init.** The app never called `sceHttpInit()` / never loaded
   `PSP_NET_MODULE_HTTP`; every request failed with *"http not initialized yet"*.
   `net_init()` now loads the HTTP net module and calls `sceHttpInit(20000)`.
2. **`cwd_up()` off-by-one.** Truncating at the trailing slash left the last
   path component in place, so "go up one level" re-fetched the *same* directory
   and produced malformed URLs (`/HomebrewResumeTest.bin`). Fixed in both
   `psp/main.c` and `ps3/main.c`.

Verified end-to-end against the share (driving the app blind via injected keys):

- Directory browse: root `/` and `Homebrew/` listings render correctly.
- **Full download**: `TestGame.bin` (2 MiB) downloaded, SHA-256 matches source.
- **Resume**: pre-seeded a 1 MiB partial, re-selected the file → server returns
  `206 Partial Content` and the finished file's SHA-256 matches the source.
- Directory navigation up/down works after the `cwd_up` fix.

### On-device E2E — PS3 / RPCS3 ❌ (blocked — finding, not a pass)

Firmware is present (4.92) at the custom `dev_flash` mapped in
`~/.config/rpcs3/vfs.yml` (`/run/media/google/Data/PCSX3/`). `nasdl.elf` boots
on the PPU runtime, loads its SPRX modules, and reaches `main_thread` — but then
**crashes immediately at C-startup**:

```
VM: Access violation executing location 0x7c0802a4 (unmapped memory)
```

The fault is an indirect call (`ld r9,0(r31); mtctr r9; bctrl`) whose target
pointer holds raw instruction bytes (`CTR = 0x7c0802a6`, i.e. a `mflr r0` word),
reached from CRT init. It happens under both the LLVM recompiler and the PPU
interpreter, so it is **not** an RPCS3 JIT miscompile. Contributing signals from
the loaded ELF:

- No `.TOC.` / `_TOC.` / `_SDA_BASE_` symbol, and at crash time `r2 = 0xf8010010`
  (far from this ELF's `.got` at `0x436b8`) — a TOC/ABI setup mismatch.
- A bogus trailing `LOAD` segment (`p_vaddr=0, memsz=0, flags=0x6600006`)
  triggers `ppu_load_exec(): Unexpected segments state` in RPCS3's loader.

So **compile-verify is necessary but not sufficient**: the PS3 binary does not
yet run. This is the point of on-device testing, and it's now a tracked item
rather than a silent assumption.

Compounding this: the PS3 app currently has **no console/RSX output** (ANSI
`printf` goes nowhere without netdebug/RSX — see the roadmap "PS3: RSX renderer"
item), so even a running build is not yet observable on-screen.

### Status / next steps

- PSP app is functionally verified end-to-end on emulator (browse, download,
  resume, navigation) after the two fixes above.
- PS3 app needs its startup crash resolved (likely a link/TOC issue — try
  PSL1GHT's stock link flags / `-mminimal-toc`, and confirm the ELF gets a proper
  `.TOC.` symbol), plus an output path (RSX or netdebug), before PS3 E2E can pass.
- All source changes from this session are **uncommitted** working-tree edits
  (`README.md`, `ps3/Makefile`, `ps3/main.c`, `psp/Makefile`, `psp/main.c`,
  `psp/resume.c`); nothing has been committed or pushed.

---

## 2026-10-30 (PSP) — Store wired to shared Cover Flow

Implemented the PSP Store integration on a local worker branch. `Triangle` now
opens it from the file browser; the catalog feeds the shared Cover Flow renderer
with `has_colors=0` and a 40-column PSP screen width. The parser accepts both
`items` and generated-catalog `entries`, formats integer `size` values, and the
client derives PSP EBOOT/ISO/CSO destinations locally. Favourites and the eight
most recent successful installs are stored in `store_state.txt` beside
`settings.txt`; installs use the existing resumable downloader.

A small host test was added for the pure size/path helpers. **Validation was
blocked in this sandbox:** `make`, `gcc`/`cc`, and Docker are unavailable, so
neither requested Make target nor the PSP container build could run. No PSP or
emulator runtime check was performed. The roadmap item remains in progress
until builds/tests and runtime behavior are verified.

---

## 2026-10-30 (PSP) — Store view built, run in PPSSPP, five bugs fixed

`make` and `cc` are present on this Mac and podman runs, so the entry above is
now superseded: `psp/` cross-builds in `pspdev/pspdev` and the resulting
`EBOOT.PBP` was driven in PPSSPPSDL 1.20.4 against a logging HTTP share with
Range support. `make -C ui test` (197 assertions) and `make -C psp/tests test`
(24 assertions) pass; the toolchain build is warning-free apart from the
pre-existing `sceHttpCreateRequestWithURL` const discard in `store.c`.

Driving the pad from the CLI needed more than AppleScript: `keystroke` and
`key code` reach PPSSPP's own menus but not the emulated pad, because SDL only
feeds the pad while the window owns input focus. A real mouse click on the
title bar (CoreGraphics, `pslkey.swift` in the scratch dir) before each press
fixes it. `EnableWlan = True` is required or `sceNetApctlConnect` never gets an
IP; on this Mac the live ini is `~/.config/ppsspp/PSP/SYSTEM/ppsspp.ini`.

What was watched, not inferred: Triangle opens the Store
(`GET /catalog.json -> 200 ua=PSLinkerStore`); Left/Right move the carousel and
clamp at the ends; R/L switch category; Select toggles the 3xN grid; Square adds
then removes the `F:` line in `store_state.txt`; Start is unmapped in this view
and changes nothing; O returns to the browser. Installs: two `EBOOT.PBP`s and a
3 MB ISO each landed on the memory stick with a sha256 equal to the served
blob, and `store_state.txt` came back through a restart with `R:` rows newest
first.

Fixed, all of it found by running it:

- `psp/store.c` used `SCREEN_COLS 40`. The debug screen is 480 px on an 8-px
  cell, so the real width is 60: the frame filled 58% of the screen and cut the
  description mid-word ("Needs a WAD you ow"). Truncations now track
  SCREEN_COLS, and the tiles grow to 14+2+20+2+14.
- `psp/store_logic.c` derived an `EBOOT.PBP`'s game folder from the title, and
  `generate_catalog.py` titles every one of them `EBOOT` (the file stem). Two
  homebrew therefore shared `ms0:/PSP/GAME/EBOOT/EBOOT.PBP`, and the second
  resumed against the first file's bytes: `GET /Games/CaveStory/EBOOT.PBP` with
  `Range: bytes=1048576-` against a 512 KB file answered **416**, and it could
  never install. The folder now comes from the URL's parent directory, with the
  title as fallback and `//host` rejected as a folder name.
- `ends_with_ci` folded only the subject, not the suffix, so a suffix passed in
  upper case never matched. That is what hid the bug above; both sides fold now.
- `psp/main.c` did not resync `browser()`'s pad edge-detect after `store_run()`
  returned, so the O that closed the Store was read again as "up one level" —
  and at the share root that quits the app (`sceKernelExitGame()` observed on
  screen). It now waits for the pad to clear; closing the Store lands back on
  the listing.
- `store_derive_path` returns `-2` when the path does not fit, so an unsupported
  type no longer reports "Install path too long".

Open, and not caused by this change:

- Cover Flow's category tabs are a fixed enum filtering on `category` starting
  with "PSP"/"PS3", while the generator sets category to the top-level library
  folder, so a normal library shows `(no items in 'PSP Games')`. `store.c`'s own
  parsed categories (`g_categories`) are collected and never displayed.
- `generate_catalog.py` lists its own `<name>.meta.json` sidecars as catalog
  entries, so they appear as items that cannot be installed.
- `main.c` still uses `SCREEN_COLS 68` on a 60-column screen; only padding
  spills past the edge, which is why it went unnoticed.
- No real PSP hardware has been tried.


## 2026-10-30 (PSP) — catalog-driven Store categories

The catalog-category path arrived as a patch written in a sandbox without any
C toolchain, so its own log text ended with "no runtime behavior is claimed".
This session applied it onto `store-view` and ran everything it could not.
The shared Cover Flow can now opt into a bounded list of category labels and
filters items by exact category equality; its old fixed menu remains the
default and is restored for an empty list. The PSP Store passes the unique
categories it already parses from `catalog.json`, preserving first-seen order.
L/R therefore switch between library folders such as `Games` or `Utilities`
instead of empty PSP/PS3-specific views. The PSP category collector is bounded
to eight labels of 31 visible bytes (`store_category_add`), with host
assertions for deduplication, filtering, switching, boundaries, fallback, and
category registration.

Checked (host): `make -C ui test` compiles clean and prints `all 208
coverflow assertions passed` - 197 before, the new `test_dynamic_categories`
adds 11 and passes first try. `make -C psp/tests test` passes with the added
`store_category_add` assertions (dedup, empty rejected, cap at eight).

Checked (PSP): the scratch-tree build in `pspdev/pspdev` (podman) linked
without new warnings; `EBOOT.PBP` sha256
`b8f9dfd693c25cec648e84a8e335090f360ae8362072c0f610bf40110e9203e6`, installed
to `PSP/GAME/NASDL/`. In PPSSPPSDL against the logging server, `Triangle`
fetched the catalog (`GET "/catalog.json" -> 200 ... ua=PSLinkerStore
sent=2282`) and the menu row rendered the catalog's own folders - `[Games]
Utilities` - not the fixed PSP/PS3 labels. Games showed 6 items, Utilities 4,
each tab listing exactly its own category; R moved to the last tab and
clamped, L returned and clamped at the first (screenshots
`store-open/store-utilities/store-games-clamp` in the e2e harness dir). An
install from the second tab: `GET "/Utilities/clock_tool.PBP" -> 200 ...
sent=65536`, landing at `ms0:/PSP/GAME/clock_tool/clock_tool.PBP` with sha256
`996ce67c6384825d0eb50c095509a7034d3272d8e84754d55cb085b887058020`, equal to
the served payload; the non-`EBOOT` filename still derived a sane folder. The
counts include the generator's `*.meta.json` sidecar listings, the known
`generate_catalog.py` issue noted above, not a filter fault.

Still open from this change: no real PSP hardware has been tried, and in
dynamic mode an item whose category is missing from the (max eight) tabs is
not reachable - the generator guarantees a category for every entry, so that
only bites libraries with more than eight top-level folders.

## 2026-10-31 (PSP) - Graphics Store GUI on sceGu, run in PPSSPP

The Store view left the text screen: `psp/gui.{c,h}` adds a GU core
(double-buffered 480x272, filled rects, an 8x8 bitmap-font atlas, one
textured-quad primitive), `psp/store_gui.{c,h}` draws the browse, status and
install screens from the same `ui/coverflow.c` state the ANSI renderer
consumes, and `psp/store.c` routes every screen through `ui_*` dispatch -
pixels by default, `-DNASDL_TEXT_UI` rebuilds the original debug-screen
layout. Icon-less covers are id-hashed gradient tiles (`store_cover_hue` /
`store_hsl_to_rgb` / `store_cover_pixels` in `store_logic.c`), pinned by
tests to the web GUI's `placeholderBG()` hash exactly ("ab"=225 deg,
`Games/Doom/EBOOT.PBP`=111, `Homebrew_PSXReloaded.pkg`=229). Font: vendored
public-domain `font8x8.h` (dhepper/font8x8, fetched 2026-10-31).

Two GE behaviours the emulator pinned down, both worth remembering:

- `GU_SPRITES` without `GU_TEXTURE_32BITF` made every flat rect invisible -
  the GE packs vertex attributes in TXTR,CMND,POSITN order and starts
  reading at COLOR only when no texcoords are flagged, so the shared
  (u,v,color,pos) struct desynchronised and picked up alpha 0. Rects are
  now textured quads of a 16x16 white texture, one vertex layout everywhere.
- Handing the display back to the debug screen after `sceGuTerm()` froze the
  screen on the store's last frame: with buffered rendering the emulator
  keeps a GPU-side copy of every address the GE drew and presents THAT,
  while the debug screen's writes are CPU-only. `ui_exit()` now re-inits the
  debug screen at VRAM offset 0x155000 (past the GU depth buffer - the GE
  never rendered there), so the display falls back to reading RAM, the same
  path the browser used before the Store ever started. Re-entering the
  Store afterwards re-initialises the GU cleanly (verified both ways).

Checked (host): `make -C psp/tests test` (store logic + cover-hue parity
fixtures) and `make -C ui test` (208 coverflow assertions) pass.
Checked (PSP): pspdev/pspdev container builds BOTH backends warning-free
apart from the pre-existing `sceHttpCreateRequestWithURL` const discard;
shipped `EBOOT.PBP` sha256
`08b4377fa3b2bd135814ce3e1f7773b4f5619940fc07aefb7e3458033e2bf6d4`.
Checked (PSP run): PPSSPPSDL 1.20.4 against the Range-capable share with a
4-entry fixture library. Screenshots `z0`-`z10` in the e2e dir: browser
(debug screen) -> Triangle opens the pixel store (header, tab rail with
active-tab underline, cover-flow cards with gradient + initials + dimmed
neighbours, `2 / 2` counter); R switches to Games; RIGHT moves the cover;
Square toggles the favourite (meta row gains `* fav`, `store_state.txt`
gains `F:Games_Doom_Port_EBOOT.PBP`); Select toggles the 4x2 grid (gold
star on the favourited card); Cross installs - progress screen at 57 %
mid-download, then Doom lands at
`PSP/GAME/Doom_Port/EBOOT.PBP` sha256 `5ffb7eac...` == served bytes, and a
`R:` line records it; the Utilities install derives the `Clock_Tool` folder
(sha `5b5a2173...` == served). O returns to the live browser listing, and a
second Store visit renders correctly and exits again - the access log shows
both `GET /catalog.json` hits plus both file GETs, all 200.

Harness lessons recorded in the scratch dir's `PADMAPPING.txt`: pspdev's
`pspctrl.h` D-pad bits are UP=0x10 RIGHT=0x20 DOWN=0x40 LEFT=0x80 (swapped
vs the PS3 ctrl.h order, and the initial "RIGHT does nothing" mystery was
exactly that), keys only land while the SDL window owns focus, and the
fixture server must run under `python3 -u` or its access log sits
block-buffered.

---

## 2026-10-31 (PS3) — pixel UI + Store on the RSX, run end-to-end in RPCS3

The PS3 app left the TTY. `ps3/gfx.{c,h}` is the renderer core: `rsxInit`,
the XMB's current video mode, two `rsxMemalign` display buffers registered
with `gcmSetDisplayBuffer`, `GCM_FLIP_VSYNC`. Every frame is CPU-drawn into
a cached main-RAM back buffer (`gfx_begin`/`gfx_end`; rects blend, an 8x8
`font8x8.h` font, nearest-neighbour tinted cover blits), handed to the RSX
with one sequential volatile copy, and flipped - no shaders, no RSX draws,
the arrangement RPCS3's emulated GPU and CFW hardware both agree on. A
virtual 1280x720 grid maps through the panel mode (1:1 at 720p, 1.5x at
1080i), so the layouts are written once. `ps3/ui.h` fronts the app screens
as two interchangeable backends - `ui_pix.c` (default) and `ui_text.c`
(`make TEXT_UI=1`, and the automatic fallback when `gfx_init()` fails) -
with exactly one compiled per binary; the Store's screens pair
`store_gui.c`/`store.c` the same way. `ps3/http.{c,h}` lifts main.c's
socket GETs out (plus status code and a capped body read for the catalog),
and `sysUtilRegisterCallback` turns the XMB's quit request into a poll
result that unwinds both the browser and the Store.

Shared-code move first (commit `642acad`): `store_logic.{c,h}` + `font8x8.h`
+ the host test went `psp/ -> ui/`, `psp/store.c`'s tolerant parser became
`ui/catalog.{c,h}` (now also reading the `platform` sidecar, bounded to its
own JSON object - the object-end test pins that no later item's value can
leak into an earlier one), and `store_derive_path_ps3()` maps `.pkg ->
/dev_hdd0/packages/`, `.iso -> PS3ISO` (`PSPISO` when `platform` says psp),
`.cso -> PSPISO`, `.rap -> exdata/`, refusing `.pbp`. `make -C ui test` runs
all three host suites now: 208 coverflow + store + catalog assertions pass.

Checked (builds): `ppu-gcc` in `ps3dev/ps3dev:submodules` compiles both the
pixel and the TEXT_UI variant warning-free; `nasdl.self`
`ffad5278765d...` and `nasdl.pkg` `cdb381522525...` ship. (Docker-as-root
artifacts keep needing a `chown -R 1000:1000 /w/...` inside the container.)

Checked (PS3 run): RPCS3 booted the `.self` against a fixture share on
127.0.0.1:18000 (`server/store/http_server.py`, catalog from
`generate_catalog.py`); keys injected through `/dev/uinput`, evidence
screenshots in the scratch dir's `shots/`. Browser: enter `Homebrew/`,
download `ResumeTest.bin` -> `/dev_hdd0/NASDL/ResumeTest.bin` sha256 equal
to the served bytes. Triangle opened the Store from the live listing:
header, catalog tab rail (Games/Homebrew/Licenses/PS3/Utilities with the
orange active underline), Cover Flow card with id-hashed gradient + initials
+ dimmed neighbour, `* fav` after Square (which wrote `F:` to
`/dev_hdd0/NASDL/store_state.txt`), R1 to the PS3 tab, Cross installed
`Quake_1.08.pkg` -> `/dev_hdd0/packages/Quake_1.08.pkg` sha256 equal to the
served bytes and an `R:` recents line, Select toggled the 4x2 grid, O
returned to a live browser. The access log shows the catalog GET and both
file GETs, all 200.

The run found three real bugs, all fixed on the spot:

- `get_pressed()` reported *any* pad change - so the release of the very
  button that started a download read as "a press" and dismissed the result
  panel ~100 ms after it appeared. It now only returns 0 on a press edge.
- directory entries kept the `href`'s trailing `/`, so entering one showed
  `NAS LIBRARY: /Homebrew//`; names are trimmed in `add_file` now.
- the pixel browser's left hint bar and right-aligned status text
  overlapped; the hint is clipped to the space the status leaves.

Harness notes for next time: this kernel's `struct uinput_user_dev` is
`name[80] + id(8) + ff_effects_max(4) + 4 x s32[ABS_CNT]` = **1116 bytes**
(`write()` of the packed-`absinfo` 1628-byte form fails `EINVAL`); RPCS3's
default keyboard mapping is arrows=D-pad, `X`=Cross, `C`=Circle, `Z`=Square,
`V`=Triangle, `Q`=L1, `E`=R1, `Space`=Select, `Return`=Start, and keys only
land while the game window is focused; the first Vulkan launch pays a
one-time ~4-minute shader-variant precompile before boot. RPCS3's
`Internet enabled` was flipped to `Enabled` for the run and restored.
