# Development Log

Running log of build, verification, and on-device testing work on PS Linker.
Newest entries first. This complements the [README roadmap](README.md#roadmap) —
it records *what was actually run and observed*, including bugs found while testing.

---

## 2026-11-01 (ps3) — the PS3 app's own UI layer is deleted: main.c draws through store_gui

`ps3/ui.h` and `ps3/ui_pix.c` are gone. They were the last thing the PS3 client
had that the PSP client has not: a second UI layer with its own chrome (the
Media Go sidebar rail, blue-cell Settings picker and separate splash / error /
download / result frames). `ps3/main.c` now calls the Store renderers directly,
exactly as `psp/main.c` does — `store_gui_browse()` for the browser,
`store_gui_progress()` for downloads, `store_gui_status()` for the splash,
error and one-line result panels through three local helpers (`draw_splash()`,
`draw_error()`, `draw_result()`) — and opens the screen itself with
`gfx_init()`/`gfx_term()`. `FileEntry` moved into `main.c` (the PSP twin
declares it locally too), and `ps3/store.c` no longer includes the deleted
header. header. The `.self` ended up about 300 bytes smaller with one source fewer.

Fixing the panels surfaced a real layout bug in *both* twins:
`store_gui_status()` clipped its body to one line, so the longest message the
clients show — "`Game_Demo_1.0.pkg` stopped early - download it again to
resume" — ended mid-word at the screen edge. Both `store_gui_status()`
implementations now run their body through a shared `status_lines()` word-wrap
(two lines, breaking at the last space that fits: 50 glyphs per line at the
PS3's scale-3 grid, 58 at the PSP's 480-wide scale 1).

### On-device E2E — PS3 / RPCS3

Rebuilt the fixture share to mirror the real library so the tab rail could be
checked against the PSP client's: `Games/ Homebrew/ PS3/ Utilities/` with a
`catalog.json` labelling two of the files `platform: PSP` and the rest `PS3`,
served by `server/store/http_server.py` on **18000** (the live Store on 8000
untouched), `Net: Internet enabled: Connected` and settings.txt at the vfs
target `…/PCSX3/dev_hdd0/NASDL/`. A hand-written
`Game_Demo_1.0.pkg.part` put a paused row in the Downloads tab.

Observed (`shots/`): the rail reads **`All | Games | Homebrew | Utilities |
Downloads`** — categories in listing order, the own-console `PS3` tab hidden,
and inside `/Games/` only the `.pkg` survives (the PSP `.iso` is dropped, the
folder's own rail collapsing to `All | Games | Downloads`) (`40`, `48`); the
Downloads tab is a row list with a name and a progress bar per row, `paused`
for the partial and `100%` for finished files, exactly the PSP client's layout
(`41`); SELECT opens the Utilities page with the NAS URL row and the
`/dev_hdd0/NASDL/` line (`42`); X on a Downloads row and the panel after a real
download both render as the Store status frame, now with the long message
wrapped over two lines (`44`, `50`); the download itself completed
(`Game_Demo_1.0.pkg`, 524288 bytes, in the emulated HDD).

Torn down: RPCS3 and the fixture server stopped, both fixture files and the
fake `.part` removed from `dev_hdd0/NASDL/`, `settings.txt` and
`Net: Internet enabled: Disconnected` restored from the pre-test copies.

Harness notes:

1. **`pkill -f` matched the harness again, twice** — once because the *launch*
   shared a command line with the kill (`pkill -f "[r]pcs3.appimage"; …
   rpcs3.appimage …`), once because a later argument (`rpcs3_core`) contained
   the pattern's expansion. The kill must be its own command, alone.
2. **`cd X && cmd &` backgrounds the whole chain** — the following `drive.py`
   call then ran from the repo and failed with "No such file or directory";
   the driver is called by absolute path from now on, and RPCS3 is launched
   with `setsid nohup` so a harness-side interrupt cannot take it down.
3. `store_gui_status()`'s one-line clip was invisible until a message longer
   than the panel was put on screen — worth remembering that the *longest*
   string, not the typical one, decides a fixed-width panel.

## 2026-10-31 (ps3) — the whole PS3 app on the Store frame: pixel Utilities page, sidebar chrome deleted

The PS3 client's last foreign-looking screens are gone. `ps3/ui_pix.c` was
rewritten: its Media Go sidebar rail, blue selection cells and the
character-grid `ui_settings()` picker are deleted, and the splash / error /
download / result frames now draw through `frame()` + `hint_bar()` — the orange
header and bottom hint bar of the Store frame, so a boot splash, a Cover Flow
listing and an error read as one app. SELECT opens a pixel Utilities page built
in the same module as the Store (`store_gui_utilities()`: the NAS server URL as
one row with `X edit`, and where downloads land — deliberately no pill switch,
`gclite` is a PSP plugin) over `store_gui_url_edit()`, the PS3 twin of the PSP
keyboard: 13 columns over the shared `STORE_URL_CHARS`, `<` and OK as the last
two slots, the selected cell inverted to orange, the value tail-anchored with a
caret. `main.c` drops `settings_view()`/`SETTINGS_CHARS` for
`utilities_view()`/`url_picker_view()`, and the new `fetch_or_retry()` gives
*every* failed fetch (boot, tab change, new server) the same retry screen whose
SELECT opens Utilities, replacing the boot-only escape and the dead-end "could
not reach the new server" screen the first build showed. `ui_error()` now puts
its second line in the hint bar (where the PSP twin puts key hints) instead of
showing both a centered instruction and a contradicting bar.

### On-device E2E — PS3 / RPCS3

Built with the real toolchain; booted as the `.self`:

```bash
docker run --rm -v "$PWD:/src" -w /src/ps3 ps3dev/ps3dev:submodules make   # nasdl.self
QT_QPA_PLATFORM=xcb ~/AppImages/rpcs3.appimage ps3/nasdl.self &
```

Share: `PS3/` (`Small_Demo.pkg` 384 KB, `Utility_Pack_1.0.pkg` 2 MiB + `.rap`,
later a 24 MiB `Big_Pack_2.0.bin`), `PSP/PSP_Only.PBP` and a three-item
`catalog.json`, served by the repo's `server/store/http_server.py` on
**18000** (the live Store on 8000 was left alone). Guest network: `Net:
Internet enabled: Connected`, and settings.txt written to the *vfs target*
`/run/media/google/Data/PCSX3/dev_hdd0/NASDL/`, not to `~/.config/rpcs3/`.

Observed (`shots/`): boot reaches the Cover Flow frame over the HTTP listing
with the new `SEL:util TRI:store` hint bar (`00`, `30`); SELECT lands on the
Utilities page (`01`), X opens the 13-column keyboard with the cursor on `h`
(`02`); O deletes and X inserts, verified by driving the URL to
`http://127.0.0.1:h` (`04`); typed `19999`, `<` and OK committed it, the page
showed the new URL, START saved `settings.txt` and the refetch failed as it
should (`05`, `06`, `07`). Restarting with that dead port saved then proved the
escape from cold boot: the retry screen offers SELECT (`11`), the page shows the
bad URL (`12`), four deletes + `8000` + OK + START refetch root and the browser
comes back with the working share (`13`–`15`). Downloads rows, folder covers,
the result frame after a real download and the utilities page with no
sidebar-chrome screen anywhere (`16`, `31`, `32`); SELECT inside the keyboard
discards the edit and leaves `settings.txt` untouched (`33`, `34`).

Torn down: RPCS3 and the fixture server stopped, the three fixture files removed
from `dev_hdd0/NASDL/`, `settings.txt` and `Net: Internet enabled: Disconnected`
restored from the pre-test copies.

Harness notes (this session's `/tmp/opencode` was wiped mid-work, so
`ps3-e2e/` is a *new* directory with a fresh driver, `/tmp/opencode/ps3e2e/drive.py`):

1. **python3-Xlib 0.33 has neither `Window.is_visible()` nor
   `get_visibility()`** — an AttributeError inside a `except Exception: continue`
   loop made the window walk return *zero* windows and looked like "no X
   window at all". `get_attributes().map_state == IsViewable` is the portable
   test; a bare `keysym_to_keycode(XK.XK_string_to_keysym(...))` is likewise
   `XK.string_to_keysym` here.
2. **RPCS3 is Qt, not SDL** — `QT_QPA_PLATFORM=xcb` under this Wayland session
   gives a normal X window (`FPS: … | nasdl.self []`), and keys must target
   *that* window, not the `RPCS3 … Alpha` GUI window. One key per XTEST call
   with a re-focus before each; bursts drop events.
3. **Keys pressed at a result screen are *consumed*, not queued** — a `cross`
   sent to dismiss "Done:" re-selected the cover under it and silently
   re-downloaded that file, which first read as "the share lost a new file" when
   the folder listing was in fact fine (`curl` the listing before blaming the
   server or its `.bin` extension filter).
4. RPCS3's native-passthrough guest net pulled 2 MiB in under two seconds, so
   the download progress frame scrolls past too fast to screenshot — catch it
   with a 20 MiB+ fixture or don't bother proving it on screen.

## 2026-10-31 (psp) — SELECT goes straight to a pixel Utilities page; the Settings dialog is deleted

The app's last debug-screen UI is gone. SELECT now opens the pixel
Utilities page directly, and the NAS server URL moved onto that page as a
second focusable row (`store_gui_utilities(enabled, installed, url,
focus)`): Up/Down move focus, X on the URL row opens a full-screen pixel
on-screen keyboard (`store_gui_url_edit()` — a-z row, digits + `- . : / _`
row, then `<` and a double-width OK; X inserts, O deletes, START commits,
SELECT discards; cursor starts on `h`), X on the switch row toggles the
gclite pill, and O/START/Select leave with whatever changed saved to
`settings.txt`. A committed URL change saves and refetches the share from
root — the old Settings dialog only redrew over a stale listing.
`settings_view()`, its character picker and the Settings ⇄ Utilities
ping-pong in the browser's SELECT handler are deleted, along with the
debug style helpers only they used (boot errors keep `Style`/`field()`);
the browser hint bar now reads `SEL:util`. New `wait_button_key()`
reports which button ended a wait: when boot cannot reach the saved
server, the retry screen's SELECT opens the Utilities page too, so a wrong
URL is fixable without reinstalling — the case the E2E drove into by
leaving `http://127.0.0.1:18000h` in settings.txt (PSP's HTTP parser
tolerates the garbage port suffix, so the genuine-failure version stopped
the fixture server instead).

PPSSPP E2E (fixtures as before: `CAT_05Shooters`/`CAT_10Arcade` +
`category_lite` vsh.txt + server :18000; keys via
`/tmp/opencode/ps3-e2e/sdl_drive.py`): SELECT lands on the page with the
URL row, keyboard, pill and detection line (`3*-*.png`); picker insert /
O-delete / SELECT-discard verified, START-commit of a changed URL
refetches root (`36`, `50-53`); OFF leaves raw `CAT_05…` names and an
inert START across a cold boot from `gclite=0` (`37`, `38`, `45`); ON
restores stripped names + the START popup (`39`, `40`, `46`); the
unreachable-server retry screen with its SELECT hint and the fix through
the same page round trip (`56`–`60`); a final clean cold boot re-confirmed
page + popup (`61`, `62`). Entry-key seeding (fresh `sceCtrlReadBufferPositive`
sample per nested screen) kept from the previous fix and reused for the
picker both ways (X that opened it, START/X that closed it never leak).

Harness notes: the zsync-updated PPSSPP AppImage now picks SDL's Wayland
backend (no X window at all), so launches must force
`SDL_VIDEODRIVER=x11` with `WAYLAND_DISPLAY=`; `pkill -f ppsspp` patterns
match the harness's own shell command line (bracket-classes mandatory),
and `pkill` + a short sleep let an old instance's dying window answer the
driver's title regex — one run typed into the PPSSPP menu instead of the
app (screenshots `54`–`55` of that run stayed in `shots/` for honesty;
the reruns are the clean ones). Long synthetic key bursts also lose ~1/3
of the events through XSendEvent, so the keyboard was driven with
single-key steps where it counts. Device build warning-clean (only the
known `store.c` const one), `make -C ui test` green, PS3 untouched.
Fixtures, vsh.txt and the test server removed after; `settings.txt`
restored to the URL-only form (which means gclite ON).

## 2026-10-31 (server) — the Store chain proven end-to-end inside the Docker image

Finished the last open "In progress" roadmap item: container path —
entrypoint → fetcher → catalog regeneration → app download against port
8000. Built `server/Dockerfile` (`NETSRV=python`) as `ps-linker-e2e`, ran
it on a throwaway bind-mount library with `STORE_INTERVAL=8`, and fed it
a real `.store-queue.json`: two raw files plus a zip on a local HTTP CDN.
The CDN had to run as a *peer container* on a user-defined network
(`pscdn`, plain `python3 -m http.server` inside the same image) — the
host's firewall drops container→host-IP connections, which the first
attempt surfaced as the fetcher hanging on a 0-byte `.part` against a
timed-out `urlopen`.

On boot the entrypoint's immediate pass drained everything: both raw
files fetched byte-identical (md5), the zip extracted and its archive
removed, the queue claimed and deleted, `.store-status.json` showing
`phase: idle` with all three items `ok` in history, and
`generate_catalog.py` regenerating `catalog.json` — the sidecar the zip
carried supplied the title ("Clock Tool") and platform `PSP`, which the
listing served as `data-platform="PSP"` on the file row. Through the
published port (host 18000→8000; the machine's real Store already owns
8000) a `Range: bytes=100000-` fetch answered `206` with the md5 of the
tail slice matching, and the netiso port answered a TCP connect
(`[netsrv] [1] connection from 172.18.0.1`).

Then the app against the container: PPSSPP booted NASDL straight at it —
root rail `All | Games | Utilities | Downloads` `[1/2]` (the container's
`PS3/` folder filtered away by the new console rules), `/Utilities/`
holding the zip-extracted `Clock_Tool.PBP 879 KB`, Cross downloading it
intact ("Done: Clock_Tool.PBP"), and the Store showing exactly the
container's two PSP titles (`EBOOT 293 KB`, Clock Tool) under
`All | Games | Utilities`. Screenshots `ctn2-*.png` — an earlier set
(`ctn-*.png`) is wrong and stayed in the shots dir for honesty: the old
pkill pattern `PPSS[P]SDL` never matched the real `PPSSPPSDL` (doubled
P), so a pre-container instance had survived and its window answered the
driver's regex; with exactly one PPSSPPSDL running the rerun is the
clean one. Container, CDN, network and ms0 residue all removed after.

## 2026-10-31 (clients) — one console, one library: the PS3 menu is gone and foreign files hid

"ok lets remove the ps3 menu from the ps3 client and psp and ps3 should
only view and get files for there repective consoles". One shared
predicate, `store_platform_ok(name, platform, console)` in
`ui/store_logic.{c,h}`, decides everywhere: console-exclusive extensions
win (.pkg/.rap only ever help a PS3, .pbp/.cso only a PSP — even against a
contradicting label, since only one machine can open the file), then a
"psp"/"ps3" platform label prefix, then everything else passes (an
unlabelled `.iso` is both machines' business). The suffix test runs over
`strcspn(name, "?")` so full catalog URLs work.

To let the *browser* filter file-by-file, the share listing grew a third
anchor attribute: `data-platform`. `server/store/http_server.py` reads the
catalog into a stat-cached (mtime_ns/size) url→platform map and labels
file rows; directories never carry it, and foreign shares send no
attributes at all — the apps then skip filtering, the safe fallback. Both
clients parse it into `FileEntry.platform` and pass it to the model as
`bm_entry_t.platform`. `bm_build()` gained `hide[]` (a category with no
tab whose entries stay visible — the PS3 client's own `PS3` category now
lives under All instead of offering a menu that duplicates it) and
`console` (files the machine cannot use vanish from items *and* from tab
derivation; folders and Downloads rows pass untouched); tab derivation now
consults only visible entries, so a fully filtered category never leaves
an empty tab behind.

Both Stores run the same predicate over the parsed catalog before any
cover exists: the PSP store compacts `PS3`/`Licenses` items away entirely,
the PS3 store drops standalone `.rap`s and PSP entries, each then rebuilds
its rail from what survived via `store_category_add` — and both gain an
`All` wildcard tab the browser and the web GUI already had. A catalog with
nothing for this console says so instead of drawing an empty carousel. The
filter exposed one server-side wart: `.meta.json` sidecars were listed,
and with files filtered per-console a sidecar could sit orphaned where its
file was hidden — listings skip `.meta.json`/`.sha256` like they already
skip `catalog.json`.

Host side: `browser_model_test.c` moved to the new signature and grew
console-filter, hidden-tab, Downloads-exempt and whole-PS3-shape checks
(coverflow included); `store_logic_test.c` asserts extension-beats-label,
query-URL suffixes, case folding and the NULL-console escape. All four
suites green, ASan/UBSan clean, PS3 build warning-free, PSP build carries
only the old `store.c` const warning.

Device runs, keys injected, one emulator at a time (KWin's focus-stealing
prevention silently ate keystrokes whenever both windows lived — a fatal
RPCS3 error dialog was what the PSP run's keys were actually hitting):
PPSSPP's root rail read `All | Games | Homebrew | Utilities | Downloads`
with no `PS3`/`Licenses` covers, `/Games/UT2004/` still showed its
`UT2004.iso`, and the Store showed exactly four PSP items with no
`.pkg`/`.rap`. RPCS3's rail had no `PS3` tab while the `PS3/` folder sat
on the All carousel `[3/4]`; inside it `Quake_1.08.pkg` and its `.rap`
remained (`[1/2]`, sidecar unlisted), `/Games/UT2004/` answered "(no items
in this tab)", and the Store collapsed to `All | Homebrew` with `Quake
1.08` under All — installing it from there still wrote pkg + license,
md5-identical to the served files. Screenshots `psp4-*.png`, `ps3-*.png`.

## 2026-10-31 (clients) — the Downloads tab became a download *list*

"for the download menu on both clients it should show as a list with the
name and a progress bar". Both browsers' Downloads tab swapped from covers
to rows: file name left, a progress bar (the install screen's orange fill
in a bordered track) and its pct right, selected row = card background +
orange edge marker, L/R walking rows through the same Cover Flow state.
`store_gui_browse()`/`ui_browse()` grew a `progress` parameter — a
per-item-slot pct array, NULL meaning "draw covers as before" — so the
Store's frames and the share view never see the list code; each pixel twin
owns its `draw_downloads()` at its own scale (12 rows at 480x272, 14 at
720p) with the hint bar switching to `L/R row ... X info O leave`.

The progress values come from the device's own folder scan, now extended
with the two states the bar wants: finished files are 100 %, and an
interrupted `<name>.part` — previously hidden — rides the list as its
intended file: a real 0..99 bar when the open share listing carries that
name's total (browser downloads keep the share name, so this is the
normal case), or a dim empty `paused` bar when no total is knowable.
Cross on a partial row explains: "download it again to resume". One real
bug surfaced in the screenshots: PPSSPP's FAT upper-cases 8.3-style names
in `sceIoDread`, so `SETTINGS.TXT` slipped past the exact-match skip of
the app's own folder files — all three skips (settings, store state, the
PSP's own `EBOOT.PBP`, since DEST_DIR is the app folder) are case-insensitive now.

Device runs, keys injected: PPSSPP (walk to Downloads → into
`/Homebrew/` → list again) showed `Clock_Tool.PBP 100`, `mystery.bin
paused`, `ResumeTest.bin 30` — the partial's bar matching its real 600 KB
of 2 MB — and `UT2004.ISO 100`, with the settings file and the app's
EBOOT absent; RPCS3 showed the same three bars at 720p, selection rows
walking with L/R, `settings.txt`/`store_state.txt` hidden. Covers, tabs
and the grid are untouched everywhere else. Warning-free PS3 build, PSP
build unchanged (the pre-existing `store.c` const warning only), host
suites green. Screenshots `dl-psp-*.png`, `dl-ps3-*.png`.

## 2026-10-31 (PS3) — the game's `.rap` rides along; the Licenses menu is gone

Next client ask: "when i install game it should install the rap file with it
(if it has one), and remove the license menu". The web GUI already records
the pairing — the sidecar's `rap` field names a `.rap` in the game's own
folder, and `generate_catalog.py` passes it through — so the PS3 client only
had to consume it. `catalog_item_t` grew a `rap` field, parsed with the same
per-object `}` bound that keeps `platform` from leaking between entries
(`ui/catalog_test.c` pins both leak bounds). The URL rule went into shared
code, `store_license_url()`: folder-style entry URL (trailing `/`) takes the
folder itself, a file URL takes its parent — matching where the GUI put the
file. Destination needed no new rule: `store_derive_path_ps3()` already maps
`.rap` to `/dev_hdd0/exdata/`, exactly where a standalone license entry
landed.

`ps3/store.c` installs the game first, and only on success derives the
license URL and downloads it through the same resumable downloader; a
failed license never undoes the game, and the result panel carries
`(+ license)` / `(license failed)`. Then the menu itself: the browser passes
`{Licenses}` as its skip list and the Store's tab filter drops it too (the
`PS3` category stays visible on a PS3 — unlike on the PSP).

Host: store-logic and catalog suites grew the license-URL and `rap`-parse
assertions; all four `make -C ui test` suites green (208 coverflow +
browser_model + catalog + store_logic), ASan/UBSan clean. PS3 build
warning-free with ppu-gcc.

RPCS3 end-to-end: fixture `Quake_1.08.pkg` gained a 16-byte `Quake_1.08.rap`
beside it and the catalog entry a `"rap"` field. Boot showed the browser
tabs `All | Games | Homebrew | PS3 | Utilities | Downloads` — Licenses gone;
Triangle → Store showed `Games | Homebrew | PS3 | Utilities`, also without
it. R1 to the PS3 tab, Cross on Quake: the result panel read
`Quake 1.08 (0)  (+ license)` and the guest HDD held both files —
`packages/Quake_1.08.pkg` and `exdata/Quake_1.08.rap`, each md5-equal to
the served bytes. Screenshots: `rap-load.png`, `rap-store-tabs.png`,
`rap-store-ps3.png`, `rap-installed.png`.

## 2026-10-31 (clients) — the browser *is* the Cover Flow now, on both devices

The user's target picture was the PSP Store's pixel carousel, with one
instruction: that is what both apps show on load, "remove all the text
browsing" — and the `Licenses`/`PS3` menus, useless on a PSP, disappear
there entirely. Both browsers now boot straight into the shared Cover Flow
state; the row-list screens (and the whole PS3 ANSI backend) are gone.

The bridge is new shared code, `ui/browser_model.c`: the parsed listing
becomes cover items (folder titles carry the trailing `/`, sizes run through
`store_format_size`) plus a slot→entry map, and the tab rail is the wildcard
`All` tab, one tab per share category in first-seen order, then `Downloads`
last — listing the device's own download folder (dotfiles and `.part`
resumes hidden). The PSP passes `{Licenses, PS3}` as a case-insensitive skip
list, so those categories become neither covers nor tabs, browser *and*
Store; the PS3 passes none. `ui/coverflow.c` grew two things for it: the
empty-name category that renders `All` and matches everything, and
`coverflow_set_items()`, which swaps the item storage under a live state
(the share ⇄ Downloads swap keeps the rail and view). `store_gui_browse()`
draws the frame on both pixel backends. Settings stays the one
debug-screen view left on the PSP (behind SELECT, through a `gui_to_debug()`
that reuses the after-GE VRAM trick the Store's exit already needed); the
PS3 app deleted `ui_text.c`, the `TEXT_UI` make branch and every ANSI
`#ifdef`. 208 Cover Flow assertions plus the new `browser_model_test.c` are
green (`make -C ui test`, also ASan/UBSan), and both device builds come out
warning-free in their Docker images.

Emulator runs, with keys injected through a uinput device:

- **PPSSPP** — this build's compiled-in defaults (`Core/KeyMapDefaults.cpp`,
  QWERTY/non-Windows) are Square=`a`, Triangle=`s`, Circle=`x`, Cross=`z`,
  L1=`q`, R1=`w`, Start=Space, **Select=Enter**; `Tab` is fast-forward and
  the analog stick is `j/k/l/i`, which is what the earlier key probes kept
  mistaking for shoulder buttons. Boot lands on the pixel browser with the
  rail `All | Games | Homebrew | Utilities | Downloads` — no Licenses, no
  PS3 — R1/L1 switch tabs, Square flips to the card grid, and the Downloads
  tab lists the real memory-stick files with sizes. Screenshots under
  `/tmp/opencode/ps3-e2e/shots/` (`new2-load`, `new3-downloads*`).
- **RPCS3** — keyboard defaults: R1=`E`, L1=`Q`, Square=`Z`, Cross=`X`,
  Circle=`C`, Triangle=`V`, Start=Enter, Select=Space. Boot reaches the same
  browser with all seven tabs (`… | Licenses | PS3 | …` — the PS3 keeps what
  the PSP hides) over the HTTP listing, and tab/grid/Downloads all check out
  on screen (`ps3-new-load`, `ps3-grid`, `ps3-downloads`).

Three findings worth keeping:

1. **A stale `settings.txt` behind a VFS redirect reads as a frozen
   emulator.** The first PS3 captures all showed one identical error frame;
   it was live output, not a stale Vulkan window — the app was dutifully
   retrying `http://127.0.0.1:180`, a previous session's *truncated* URL
   read from `~/.config/rpcs3/dev_hdd0/NASDL/settings.txt`'s real target:
   today's `vfs.yml` remounts `/dev_hdd0/` onto
   `/run/media/google/Data/PCSX3/dev_hdd0/`. Write the file where the mount
   actually lands and the frame moves. (The truncated port also meant the
   old PS3 run never actually reached the share either — it only ever
   looked like it did.)
2. **RPCS3's guest network is native passthrough, not user-mode NAT** — the
   log line is `[Native] Attempting to connect on <host>:<port>` — so the
   guest dials the LAN address through the host stack and `10.0.0.63:18000`
   connects fine with *Internet enabled: Connected*. There is no `10.0.2.x`
   to dial; that detour only produced ECONNREFUSED noise.
3. **The pixel tab rail rendered the wildcard tab invisible** — the ANSI
   renderer maps the empty category name to `All`, but `store_gui.c`'s
   `draw_tabs` measured and drew the raw empty string, leaving an unlabeled
   40px stub with the underline floating in it. Caught by reading the
   screenshot rather than the assertions; both twins now apply the same
   `"" → "All"` convention, rebuilt and re-verified on both devices.

Real PSP/PS3 hardware is still untested.

---

## 2026-10-31 (server) — the probe's size window, measured against both upstreams

§8's ceiling question — does the 848 MiB edge of the sector probe cost us
something a real client would notice — had an assumption in it: "a 1 GB raw
image is reported as 2352 by us and left at the default by upstream —
different answers". It was never measured, and it is wrong: both answers *are*
the default. It is now measured, in the direction the claim needed, and the
answer turned out to be more interesting than the question.

`server/netsrv/netsrv_synth.py` builds six *sparse* fixtures straddling the
probe window (the probe reads only `sector*16` and `sector*16+24`, and the
stride is read back off `READ_CD_2048_CRITICAL` from sector 0, so a head of
varying bytes and a PVD signature is all a fixture needs — 4,586 apparent MiB
cost 1 MiB on disk) and asserts `detect_sector_size` against each offline,
then reads the same answer back over the wire from our server and, given
binaries, from the upstream ones. The fixtures and six matching `cd stride`
scenarios live in `netsrv_diff.py` (`CEILING_IMAGES`), so §8's table carries
them.

| fixture | file is | ours | `ps3netsrv` | `ps3netsrv++` |
|---|---|---|---|---|
| exactly 848 MiB, stride 2448 | 2448 | **2448** | 2352 ✗ | 2352 ✗ |
| 848 MiB + 1 byte, 2448 | 2448 | 2352 (default) | 2352 | 2352 |
| 1 GiB raw | 2352 | 2352 | 2352 | 2352 |
| 1 GiB plain 2048 | 2048 | 2352 (default) | 2352 | 2352 |
| 2 MiB − 1, raw | 2352 | 2352 | 2352 | 2352 |
| 840 MiB, stride 2448 | 2448 | **2448** | 2352 ✗ | 2352 ✗ |

What it settles, in favour of us and against:

* **webMAN's probe has never left its default.** Its binary answers 2352 on
  *every* fixture — including two 2448-stride images inside its own size
  window, which its `+24`/stale-buffer probe should catch as cleanly as it
  misses plain 2048. §5's "cannot be shown to work" is now shown the hard way.
* **Above the ceiling nobody can differ.** Over 848 MiB all three answer 2352,
  so §8's "different answers" worry is closed. And a real CD can't *reach* the
  ceiling anyway: a single-session raw disc tops out near 807 MiB, so every
  genuine PS1/PS2-CD rip sits inside the window, where our probe does measure.
  The only files that cross the ceiling are DVD-layout images, which are
  streamed with `READ_FILE_CRITICAL` and never consult the sector size.
* **The two ✘ rows are ours alone** — defaults disagreeing with the file — and
  they are provably the same answers upstream gives on the same files. Nothing
  a PS3 could notice that upstream wouldn't also notice.

### How it was checked

```
$ python3 server/netsrv/netsrv_synth.py \
        --reference /tmp/opencode/ref/ps3netsrv --reference /tmp/opencode/ref/ps3netsrv++
[synth] fixtures    /tmp/opencode/netsrv-synth  (1 MiB on disk, apparent 4586 MiB)
[synth] PASS — 6/6 offline, 6/6 over the wire

$ python3 server/netsrv/netsrv_diff.py --reference … --reference …
[diff] 60 match, 20 declared deviation, 0 unexpected        # 80 scenarios, +6
```

Both reference binaries are rebuilt ones, from tag **webMAN-MOD 1.47.50**
(their `ps3netsrv` hides in `_Projects_/ps3netsrv`, builds with
`make -f Makefile.linux`) and dirkvdb **`ps3netsrv--`** (bitbucket `utils`
submodule; needs `-include cstdint` on a current gcc). The old 57/17 tally was
measured against the 2026-10-24 builds; §8's prose now carries 60/20/0 with
the cause breakdown, and the rebuilt binaries changed no *behaviour* the table
depends on — 0 unexpected, as before.

Next, on the console: mount `Driver (Europe).bin` (713 MB, verified 2352, the
largest real rip here) as Phase B of the webMAN session — it is now the
load-bearing case for everything the window table says about real rips.

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

## 2026-10-31 (Server) — Store queue API + web GUI activity panels

The Store's queue was a file you edited by hand; it now has a contract.
`store_fetch.py` stopped trimming `.store-queue.json` in place and *claims*
it instead — an atomic rename to `.store-queue.json.processing` — so an
item the web GUI appends mid-run waits for the next pass instead of being
deleted unprocessed (the old code rewrote the file after each item, and
whatever raced in would vanish). While the claim lives it publishes
`.store-status.json`: `phase` (`idle|downloading|extracting`), a `current`
item with `bytes`/`total` throttled to one write per 0.4 s, and a
newest-first `history` of the last 30 outcomes that *survives across runs*
(each entrypoint tick is a fresh process; the panel wants the log, not just
this run). The claim file is the only proof a download is live, so
`/api/store/status` synthesizes `idle` when the phase says otherwise and no
claim exists — a crashed run's stale `"downloading"` cannot ghost the panel.

`http_server.py` grows `GET/POST/DELETE /api/store/queue`,
`GET /api/store/status`, and `POST /api/store/run`. Adds/removes
read-modify-write the pending file under a `threading.Lock` and refuse to
touch a URL the claim holds (`409`); a duplicate url is `409`, a bad
scheme or a `category` that is not exactly one visible path component is
`400`. `run` spawns the fetcher with `start_new_session=True` (its stdout
lands in the container log), `409` while a claim lives, `400` on an empty
queue, `202` once started — two overlapping spawns are harmless because
only one rename can win the claim.

Two guards the tests earned:

- the first `store_url_ok` reused `BAD_NAME_RE`, which is the *filename*
  guard and contains `/` — so every URL failed validation. URLs need only
  scheme, length and control-char checks; the filename rule was the wrong
  neighbour to borrow.
- a hand-written queue item with `"category": "../Escape"` wrote outside
  the library, because only the GUI-side endpoint validated. The fetcher
  now enforces `_safe_category` itself (queue files are hand-editable), and
  the endpoint mirrors the rule, so neither process trusts the other.

The fetcher also regenerates `catalog.json` after a drain (same command the
entrypoint loop runs; the generator's write is atomic so an overlap is just
idempotent work). Without it, *Download now* finished in seconds and the
new card then took up to a `STORE_INTERVAL` (300 s) to appear — the GUI's
completion refresh would read a catalog the fetcher had not rewritten yet.
The shell side knows the claim is released *before* that rewrite finishes,
so its catalog refetch is id-signature-gated and repeated through a 2-minute
cooldown rather than trusted once.

GUI: the corner dock became a shared transfer dock (`dock.js` — titled,
individually collapsible sections, whole panel hidden while every section
has nothing to say; Uploads moved in unchanged in behaviour). New `store.js`
owns the sidebar Store entry (pending-count badge), the queue modal (add
form with a category datalist over the library's own categories, remove
with `✕`, Download now, current-item bar, recent history), and the dock's
Store section (spinner row + last 3 outcomes, kept up for 2 min after a
run). Polling is 2 s while live or the modal is open, 60 s otherwise;
Escape closes the modal; a finished ok download refetches the catalog and
new cards appear with no reload.

`check_share.sh` reaches 106 assertions with a store section: the validation
codes, the queue file hidden from the device listing, DELETE of a claimed
url refused `409` while a foreign claim lives, a stale-status file reading
back as `idle`, a real `POST /api/store/run` against the share's own file
whose landing bytes sha256-match the source, and the drained item reaching
`/api/catalog` without waiting for the loop tick. The failing download
check leaves a status/library/server-log dump instead of a bare FAIL — the
first version of this section failed on the fixture having renamed
`test.iso` away in an earlier section, which took two blind runs to spot.

Real-browser run against a throttled CDN (a fake CDN handler dripping one
256 KiB chunk per 30 ms, so progress bars have seconds of movement rather
than a local-loopback blur): two items added through the modal (one with a
typed title, one taking its URL stem), `Download now`, the dock's Store
section catching `alpha.bin` at 100 %/`Downloading` while `beta.bin` ran
under it, the badge counting down, the history persisting across a page
reload, `big.zip` landing extracted (`blob.bin` + `sub/inner.txt`, archive
consumed) with all six entries in `/api/catalog` seconds after the drain,
and the grid gaining the new cards on its own. A queue-row `✕` removed a
pending item mid-modal. No console errors. The claim-409 and both
validation guards are covered by the suite rather than the browser, since
the windows are too small to click.

## 2026-10-31 (Server, PSP, PS3) — Browser rows carry store chrome (data-size / data-cat)

The flat listing got the same metadata the store view has been rendering,
without touching the format the apps (and other people's shares) parse.
`list_directory` now emits `<a href="name" data-size="N" data-cat="C">` —
the attributes ride *after* the closing quote, where the apps' `href="`
scan and foreign parsers (stock `http.server`, nginx autoindex) stop and
never look. Category semantics copy `generate_catalog.py`: a file's
category is its top-level folder, a root folder is its own category, root
files are `Uncategorized`; directories carry only `data-cat`.

`FileEntry` grows `size` (−1 = the share didn't say) and `category` on
both apps. The parsers gained `attr_at()`, scanning `key="value"` only
between an anchor's quote and the next `<` so a neighbouring entry can't
leak into this one. All three browser renderers (`psp/main.c`,
`ps3/ui_pix.c`, `ps3/ui_text.c`) right-align `size  category` on file
rows — size via the shared `store_format_size` so it reads exactly like
the store's cards — suppress the category when the path header already
starts with it, and show nothing for directories. A share without the
attributes renders pixel-for-pixel the old rows.

`check_share.sh` reaches 111 assertions with five listing checks (file
size, file category, a dir's own-name category, a nested file keeping the
top folder, root file = `Uncategorized`). PSP `EBOOT.PBP`, the PS3 pixel
`.self` and the `TEXT_UI=1` variant all build warning-free in the pspdev
/ ps3dev containers (same two benign pre-existing PSP warnings), and
`make -C ui test` is green.

RPCS3 end-to-end with the new `.self`: the root listing shows
`TestRoot.bin — 977 KB  Uncategorized`, directory rows stay clean,
`/Homebrew/` shows `2 MB` with the category suppressed (the header says
`/Homebrew`), and the nested `/Games/UT2004/` shows `2 MB` for the ISO.
`X` on the file downloads: server logs a plain `GET … 200`, the app
answers `Done: ResumeTest.bin`, and the file lands on the emulated HDD
sha256-identical to the share's copy.

The earlier "browser download fails with −7" scare from this session
was two unrelated things, neither of them the feature:

1. **A completed file resumes as 416.** The previous session's download
   had left a *finished* 2,000,000-byte `ResumeTest.bin`, so
   `resume.c` sent `Range: bytes=2000000-`, the share answered 416 and
   the app maps "neither 200 nor 206" to −7. Deleting the file fixes the
   test; re-X on a completed download will keep failing until a
   "already downloaded, overwrite?" prompt exists (roadmap fodder).
2. **`cwd_up()` in `ps3/main.c` dropped the trailing slash without
   removing the component**: `"Games/"` went *up* to `"Games"`, so
   Circle looked like a no-op (the next fetch re-requested `/Games/`)
   and entering a directory from that state built `/GamesUT2004/` (404).
   The PSP one handled this correctly; the PS3 copy lost the second
   `strrchr` pass. Fixed to match ("a/" → root, "a/b/" → "a/"), verified
   at both depths in RPCS3 — `GET /Games/UT2004/ 200` then `GET /` on
   the way back.

Emulator-harness findings worth keeping: this box's RPCS3 runs with its
`dev_hdd0` on `/run/media/google/Data/PCSX3/` — the copy under
`~/.config/rpcs3/dev_hdd0` is never read, which is why seeded settings
"didn't apply" and a download "didn't land" for several rounds. Two
`rpcs3.appimage` processes can coexist and take turns at the same
window; `pkill -x rpcs3` matches neither (the comm is
`rpcs3.appimage`). A leftover store-GUI demo server kept holding
:18000 so each "restarted" share silently failed to bind, and a stale
`slow_cdn.py` on :18001 ate a proxy meant to capture the app's exact
bytes. `ss -tlnp` before blaming the app, always.

## 2026-10-31 (Server) — Real cover art: ICON0 out of ISO/CSO, onto the tiles

`server/store/cover_art.py` (stdlib only) pulls the ICON0 PNG out of PSP
files where it has been hiding: a 68-byte PBP header parse (PIC1 as the
fallback), a minimal ISO9660 reader (volume descriptor + directory
records, enough to find `EBOOT.PBP`, works for nested homebrew layouts),
and a CISO reader that deflates one 2048-byte block at a time behind a
seekable wrapper — so the ISO reader runs over a `.cso` unchanged. Each
game's cover is cached next to it as a *hidden* `.name.icon0.png`: the
dot means the listing, the catalog walk, and the GUI's file management
all keep skipping it, while the share still GETs it for the tiles.
Re-runs reuse a sidecar newer than the game; `--force` regenerates.
PS3 game folders need no extraction — their disc-layout `ICON0.PNG` sits
beside the files and `find_icon` now finds it *and* stops flattening
sub-folder icon URLs onto the library root (it used to emit
`/ICON0.PNG` for a game in `PS3/MyGame/...`).

`generate_catalog.py` calls `ensure_cover` when no sibling image exists,
so covers appear during the entrypoint tick / fetcher run with no new
moving part. Device Store views are unchanged on purpose: no PNG codec
on PSP/PS3, they keep drawing initials — the field is consumed by the
web GUI.

`cover_test.py` (12 tests) builds its own fixtures to the published
layouts: a PBP with a real PNG, a hand-assembled ISO9660 image (root and
nested `EBOOT.PBP`), and a CISO of that ISO — writing them caught three
fixture bugs the first drafts had (records padded on the wrong parity,
`struct.pack("<I")` assigned into 8-byte PVD slices silently *shrinking*
the bytearray PVD, and CSO index offsets taken without the index table
that precedes the block data). `check_share.sh` reaches 121 assertions:
the unit suite runs inside it, and the live share gets a mini-ISO and a
mini-CSO fixture to prove extraction, catalog `icon`, hidden-from-
listing-but-still-served, no sidecar-entries, and cache reuse across a
second generator pass. A naming bug the suite caught: prepending the dot
to the whole relative path (`".PSP/Game..."`) instead of just the file
name — hidden means the *basename*.

Browser proof: a two-ISO library (red/green ICON0s) plus a PS3 disc
folder, regenerated, opened in the GUI — all four tiles render real
pictures (extracted sidecars and the folder's own `ICON0.PNG`), none a
placeholder gradient.

## 2026-10-31 (Server) — The Linux front-end became the web GUI: moves + covers

The roadmap's "GTK/Qt GUI front-end for Linux (manage games + edit
catalog metadata)" retired as a native-app idea: the web GUI already
covered upload/rename/metadata/delete/license work, and the two real gaps
were functional, not technological. Both closed on the share itself, so a
native shell would have duplicated one API against two UIs.

`POST /api/item/move` {url, category}: the category guard is the store
queue's rule reused; a game's `.meta.json`, `.sha256` and hidden cover
sidecar travel with it, the entry re-keys with the new category. The
license rule followed the existing contract (co-location *is* the
pairing): a paired `.rap` follows only if nothing else in the source
folder still references it — otherwise the pairing clears on disk and in
the catalog rather than dangling across folders, and moving an attached
`.rap` away is a `409`. Covers got three small endpoints — raw-body
upload (magic bytes, 8 MiB cap → hidden `.<stem>.icon0.<ext>`),
force-extract, clear — all writing the same sidecar `cover_art.py`
extracts into, so no new metadata model exists; `prune_other_covers`
keeps the one-cover-per-game rule that directory-order `find_icon`
requires. Rename gained the carry it was missing (a stem-changing rename
re-derives the dot-sidecar and repoints the entry's `icon`), and delete
now takes the game's covers with it.

Suite lessons: the live `catalog.json` *changes top-level shape*
mid-suite — the API preserves `items`, a `generate_catalog.py` pass
writes `entries` — so assertions after any regeneration must read
whichever list is present, exactly like `catalog_load_items`; and `ls`
without `-a` silently counts zero for dotfiles, which turned two
"sidecar gone" checks into vacuous passes until `find` replaced them.
`check_share.sh` 121 → 153 assertions, green twice running.

Browser proof: moved `UT_Port.iso` to `Handhelds` through the drawer's
Move panel (datalist over existing folders) — file and cover landed in
the new folder, the entry re-keyed, sidebar folder counts followed; the
extracted red cover of `Doom_Port.iso` visibly replaced by an uploaded
violet PNG on the tile, and the Cover panel's Re-extract ran live from
the drawer.

## 2026-10-31 (Harness) — Reading a *file-backed* catalog in tests

The catalog file is the union of two writers (the HTTP API keeps the
existing top-level key; the generator introduces its own), so "read the
entry" tests must go through the same shape-agnostic lookup the server
uses — `d.get('items') or d.get('entries') or []` — or they KeyError
into an empty string that *looks* like a server failure. And hidden
files are invisible to plain `ls`, so `find -name '.x.icon0.*'` is the
only honest way to assert "exactly one cover".

## 2026-10-31 (PSP) — gclite categories: CAT_ names and the START popup

game-categories-lite is a VSH plugin — a fork of ticky's Game Categories
Lite — that turns memory-stick folders named `CAT_05Shooters`/`CAT_Shooters`
into XMB categories on 6.60 CFW. It cannot be linked into a homebrew app,
so the integration is behavioural and PSP-only: the client speaks the same
naming dialect and, when the plugin is *installed*, adds the XMB's
category-list affordance to the browser.

The strip rule lives in shared code (`store_cat_name()`,
`ui/store_logic.{c,h}`): a case-insensitive `CAT_` prefix, then an
optional *exactly-two-digit* 00–99 sort key that must not be followed by
another digit (`CAT_123Games` is a name, not key "12" + "3Games"); a
prefix that would strip to nothing keeps the raw name. The browser applies
it to folder titles and categories, the PSP Store rail to catalog
categories; navigation, URLs and the PS3 client never see the stripped
copy. Detection reads `ms0:/seplugins/vsh.txt` for `category_lite` with a
12-byte carry between read chunks so a match can straddle the buffer
boundary.

The popup (`store_gui_browse` gained a `store_menu_t` parameter, the
`progress` precedent again): dim the frame, draw rows mirroring the tab
rail — mock tile via the same `store_cover_pixels` id-hash as the game
cards, so each category keeps its own hue — label and item count. The
browser owns the keys (menu open = popup block, START gated on detection),
row index maps 1:1 onto the tab index, and the Cross jump reuses the
rail's Downloads-swap verbatim.

E2E lesson worth keeping: with the OpenChamber session running *inside its
own cgroup scope*, uinput nodes created from it are tagged without
`uaccess`/`seat`, so KWin/libinput can never open them and every injected
key silently dies — the earlier same-day E2Es ran where the device was
seat-owning. The path that does work against an SDL window: `XSendEvent`
of a KeyPress *held for real time* before its KeyRelease (press+release
queued in one flush is a coin-flip miss for the app's 16 ms pad poll),
with X keycodes — which are evdev+8, a mix-up that faked a broken keymap
(103 is not Up; 111 is). Screenshots via the window-id grab need no focus
at all.

## 2026-10-31 (PSP) — A Utilities page, and the entry key that cancels itself

The gclite feature moved behind a pill switch on a new pixel Utilities
page (SELECT → Settings → Triangle): the page draws the switch with the
detection result ("plugin: installed — START opens the menu"), the choice
lands in `settings.txt` as `gclite=0/1` (line 2; absent = ON, so old
files keep working), and OFF leaves raw `CAT_` names and an inert START
everywhere — browser and Store alike (`cat_display()` wraps the strip in
`psp/store.c`). Selecting the switch is enough; O/START save and leave,
SELECT falls back to the debug Settings screen.

The E2E caught a bug that predates this page: every nested screen used to
start its pad loop with `prev = 0`, so the button *still held* from the
transition re-fired instantly inside the new screen — the SELECT that
opened Settings could cancel it in its first frame (and a held Triangle
would ping-pong Utilities ↔ Settings). Both entry points now seed `prev`
with one fresh pad sample. Verified in PPSSPP end to end: toggle OFF →
raw names + inert START, cold boot still OFF, re-enable → names stripped
and popup live again without a restart.
