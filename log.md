# Development Log

Running log of build, verification, and on-device testing work on PS Linker.
Newest entries first. This complements the [README roadmap](README.md#roadmap) —
it records *what was actually run and observed*, including bugs found while testing.

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
