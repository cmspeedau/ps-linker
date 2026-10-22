# Development Log

Running log of build, verification, and on-device testing work on PS Linker.
Newest entries first. This complements the [README roadmap](README.md#roadmap) —
it records *what was actually run and observed*, including bugs found while testing.

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
