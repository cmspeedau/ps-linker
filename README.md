# PS Linker

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Docker / Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, serves files over HTTP for PSP/PS3 download (with Range/resume), and runs the server-side Store (downloads queued CDN titles into the same unified library) |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: boots into the pixel (sceGu) Cover Flow browser of the share, downloads to the Memory Stick with resume support; the Store view shares the same GUI |
| `ps3/` | PS3 (homebrew, PSL1GHT) | `nasdl` app: pixel Cover Flow browser + Store over the RSX display, downloads files from the PS Linker server to the HDD (resumable downloads) |
| `ui/` | shared source | Cover Flow state machine + browser model (listing → covers and tab rail) both pixel device apps build against; see `ui/INTEGRATION.md` |

> Intended for homebrew and legally-owned content. This project does not provide or encourage piracy.

## Server (Docker — recommended)

The server runs both services in one container:
- **netiso server** on port `38008` (PS3 game streaming) — `server/netsrv/ps3netsrv.py`,
  our own implementation of the ps3netsrv protocol. Stdlib Python 3, nothing to
  compile, no third-party binary in the image.
- **HTTP share** on port `8000` (PSP/PS3 NASDL app downloads; supports `Range`/resume)
- **Server-side Store**: fetches queued CDN titles into the same games library and regenerates `catalog.json` (served at `/catalog.json`) every `STORE_INTERVAL` seconds; the web GUI's Store panel edits the queue and shows download activity, with a *Download now* that fetches on demand

### Quick start

```bash
cd server
docker compose up -d --build
```

Then:

- Put game files under `./games/` (or change the volume in `docker-compose.yml`)
- Open `http://<server>:8000/` to browse/download
- Open `http://<server>:8000/gui/` for the web library (card grid, search,
  sections, favorites, uploads, rename/metadata/delete — served by the share)
- Point your PS3 at `<server>:38008` for game streaming

The netiso server shares `GAMES_DIR` read-only. Set `NETSRV_WHITELIST=192.168.1.*`
to accept only that subnet, or `NETSRV_WRITE=1` to let PS3 clients create, write
and delete files. *Folder* games — a directory with a `PS3_GAME/` inside, which
webMAN mounts through `/***PS3***/` — are served as well: the server builds the
ISO 9660 image around the folder as the client reads it, so a 30 GB folder game
costs a few hundred kilobytes of RAM and no start-up pause
(`docs/PS3NETSRV-PROTOCOL.md` §7.4).

### Store (server-side only)

The Store is intentionally server-side: it downloads queue entries into `/games`, so the PSP and PS3 apps remain thin clients and see one unified library.

The queue file is `/games/.store-queue.json` (inside the container; bind-mounted from the host). Example:

```json
[
  {"title": "Example Homebrew", "category": "Homebrew",
   "url": "https://example.cdn/game.zip"}
]
```

The entrypoint fetches queued titles on boot and every `STORE_INTERVAL` seconds (default `300`). Each item may also include `icon`, `version`, and `description`; these are written into `catalog.json` for clients to display. Zip downloads are extracted in place; archive paths are zip-slip protected, and `.part` files are never included in the catalog.

### Catalog metadata (optional)

For uploaded games, place `<name>.meta.json` next to the game file:

  ```json
  {"icon": "covers/example.png", "version": "1.0", "description": "Homebrew example"}
  ```

`generate_catalog.py` merges these sidecars with folder-derived categories and serves the result as `/catalog.json`; it passes `title`, `platform`, and `rap` through as well, which is how uploads from the web GUI keep their prettified title and PSP/PS3 tab membership across regenerations. The uploads themselves write `platform` and `title` into that sidecar, and the web GUI's metadata editor writes `title`/`version`/`description`/`platform` (and the license pairing `rap`, the name of a `.rap` file sitting beside the game) through `PUT /api/item/meta`, which merges rather than replaces. Sidecar files (`.meta.json`, `.sha256`) are never listed as catalog entries.

### Manual build (without compose)

```bash
docker build -t ps-linker-server server/
docker run -d --name ps-linker \
  -p 38008:38008 -p 8000:8000 \
  -v /path/to/games:/games \
  ps-linker-server
```

## Server (bare-metal Linux, alternative)

### Quick start

```bash
# 1. PS3 streaming: our netiso server. Stdlib Python 3, nothing to build.
python3 server/netsrv/ps3netsrv.py /path/to/games 38008

#    an optional third argument limits clients by address, --write allows writes
python3 server/netsrv/ps3netsrv.py /path/to/games 38008 192.168.1.*
python3 server/netsrv/ps3netsrv.py /path/to/games 38008 "" --write

# 2. HTTP share for the PSP/PS3 apps (Range/resume capable)
python3 server/store/http_server.py /path/to/games 8000
```

To check the server against the real thing instead of trusting this README, two
harnesses come with it: `python3 server/netsrv/netsrv_test.py` runs the unit and
end-to-end suite, and `python3 server/netsrv/netsrv_diff.py --reference BIN`
sends the same command script to our server and to an upstream `ps3netsrv`
binary and compares the bytes (`docs/PS3NETSRV-PROTOCOL.md` §8 has what it
found).

### Run at boot (systemd)

Install `server/ps-linker.service` (edit paths/user as needed), then:

```bash
sudo systemctl enable --now ps-linker
```

## PSP app (NAS Downloader)

The PSP app boots into the pixel Cover Flow browser (sceGu, 480x272): the
HTTP share is the carousel, the tab rail is `All | categories | Downloads`,
and `Licenses`/`PS3` entries are hidden. Cross downloads (with resume
support) or opens a folder, Triangle opens the Store. Settings — the one
remaining debug-screen view — sits behind SELECT.

```bash
cd psp
make
# Copy EBOOT.PBP to ms0:/PSP/GAME/NASDL/
```

## PS3 app (nasdl)

The PS3 app draws every screen on the RSX pixel display (`ps3/gfx.c`:
CPU-drawn frames copied into two RSX buffers, flipped on vsync — no shaders,
so RPCS3 and CFW both scan it out reliably) at a virtual 1280x720 grid. It
boots straight into the Cover Flow browser (the share listing as covers, the
tab rail `All | categories | Downloads`); Settings and the Store (Triangle)
share the same pixel look. Download to the HDD with resume support; Store
installs derive `/dev_hdd0` destinations (`.pkg` to `packages/`, disc images
to `PS3ISO`/`PSPISO`, `.rap` to `exdata/`). A game whose catalog entry names
an attached `rap` downloads that license into `exdata/` in the same action,
so the `Licenses` category has no menu on this device (hidden in both the
browser and the Store).

```bash
cd ps3
make          # produces nasdl.self
make pkg      # optional: installable .pkg
```

## PS3 / RPCS3 notes

- RPCS3 loads `nasdl.self` directly (File → Boot SELF) or via the generated `.pkg`.
- Real-hardware PS3s need the `.pkg` installed; run the app from the XMB.
- Set the server URL in the app's Settings view (persisted to `settings.txt`).

## Build verification

The repository includes a Docker-based build path for both device toolchains:

```bash
docker run --rm -v "$PWD:/src" -w /src/psp pspdev/pspdev:latest make
docker run --rm -v "$PWD:/src" -w /src/ps3 ps3dev/ps3dev:submodules make pkg
```

Verified with pspdev (GCC 15.2) and ps3toolchain/PSL1GHT (GCC 7.2).
Artifacts: `psp/EBOOT.PBP`, `ps3/nasdl.self`, and `ps3/nasdl.pkg` compile successfully

## Checks

The shared UI module and the HTTP share both have host-side checks that need no
console or emulator:

```bash
make -C ui test                 # Cover Flow module: assertions + render sweep
make -C ui test-asan            # same under AddressSanitizer + UBSan
server/store/check_share.sh     # HTTP share: listings, Range/206, GUI, uploads
server/store/check_share.sh --with-dl   # + runs ps3/resume.c against the server
```

`check_share.sh --with-dl` compiles the console's real downloader
(`ps3/resume.c`, which needs nothing but BSD sockets) and proves a cold
download, a resume from a half-file (206, byte-identical result), and that a
wrong `X-SHA256` is rejected with `rc=-11`.

## Roadmap

### Done
- [x] Repo scaffold + Linux server setup
- [x] PSP: interactive NAS browser (parse directory listing, on-device file picker)
- [x] PSP: Media Go-style UI + UI reference doc (`docs/UI-REFERENCE.md`)
- [x] PS3 port (`ps3/` — PSL1GHT, socket HTTP, same browse/download flow)
- [x] Server: Docker image + docker-compose (netiso server + HTTP share, one
      command, nothing third-party in the image)
- [x] Both apps: subdirectory navigation (X enters dirs, O goes up)
- [x] Both apps: Settings view (on-device NAS URL editing, persisted to settings.txt)
- [x] Store (server-side): Store fetcher downloads CDN titles directly into /games so
      uploaded and store-sourced games appear in one unified client view;
      `generate_catalog.py` keeps catalog.json in sync (atomic writes, skips .part files);
      wired into the container entrypoint (boot + every STORE_INTERVAL seconds)
- [x] Catalog niceties: optional `icon` (cover art next to game file),
      `version`/`description` via `<name>.meta.json` sidecars; categories
      already derived from folder structure
- [x] Server-side unzip: Store extracts `.zip` downloads in place (zip-slip
      protected) so PSP/PS3 clients never need zlib/unzip support
- [x] Server: Range-capable HTTP share (`server/store/http_server.py`) replacing
      stock http.server — `Accept-Ranges`/`206` support, path-traversal safe,
      compatible directory listings
- [x] Server: share regression fixed — the GUI rewrite routed every path into
      `ui/` (404 for all game files *and* directory listings, so neither app
      could browse) and its double-escaped Range regex silently ignored
      `Range` (200 + full body, so resume never triggered). Library serving and
      `206` restored, GUI moved to `/gui/`, `X-SHA256` now sent from a
      `<name>.sha256` sidecar, uploads atomic, response bodies streamed
      (the rewrite buffered a whole ISO per request)
- [x] Client resume: self-contained `resume.c/h` modules for both apps —
      send `Range: bytes=<size>-` when a partial file exists, append on 206,
      restart cleanly on 200; wired into both Makefiles
- [x] Checks that need no console or emulator: `make -C ui test` (Cover Flow
      module, 197 assertions + ASan/UBSan render sweep),
      `server/store/check_share.sh` (listings, `Range`/206/416, GUI routes,
      catalog key, upload, traversal); `--with-dl` compiles the PS3 app's own
      `resume.c` for the host and runs it against the share — cold 16 MiB
      download verified against the sidecar, resume from an 8,388,608-byte
      partial answered 206 with exactly the remaining bytes and a byte-identical
      file, a lying digest rejected with `rc=-11`
- [x] Compile-verify both device builds on real toolchains (pspdev / PSL1GHT):
      `psp/EBOOT.PBP`, `ps3/nasdl.self` and `ps3/nasdl.pkg` compile successfully
- [x] **Server web GUI (Sony Media Go style)** — dependency-free browser shell
      served by the HTTP share at `/gui/` (`docs/WEB-GUI.md`): dark layout,
      category menu bar, cover-flow ⇄ grid view toggle, selected-item detail
      pane, favourites kept in `localStorage`. Rendering verified against a
      running share (menu filters narrow the carousel correctly)
- [x] Web GUI library sections: All Games / PSP Games / PS3 Games / Emulators /
      Demos / Utilities / Recently Added / Favorites menu filters over the
      unified catalog (`GET /api/catalog`). The upload *endpoints*
      (`POST /api/upload`, streamed `POST /api/upload/file`) exist and are
      checked; the browser-side **Add Games** panel is Done below
- [x] **Cover Flow UI (web GUI)** — horizontally scrolling cover carousel
      (centered title enlarged/in focus), top menu bar with all categories
      (All Games | PSP Games | PS3 Games | Recently Added | Favorites | Downloads |
      Settings) filtering the flow, metadata panel with favorite toggle below the
      carousel, and a grid ⇄ cover-flow view toggle. Arrow keys to flip,
      Enter/click to select
- [x] Shared Cover Flow module for the device apps (`ui/coverflow.c/h`, ANSI, no
      GPU): 197 host assertions + ASan/UBSan clean, and compiles warning-free
      under `psp-gcc` and `ppu-gcc` with each app's flags (`make -C ui test`)
- [x] **ps3netsrv rewritten in house** (`server/netsrv/ps3netsrv.py`, stdlib
      Python 3): the netiso protocol on port 38008, no third-party binary in the
      image. Checked byte-for-byte against upstream's own binaries on one tree
      with `server/netsrv/netsrv_diff.py` — 60 identical answers, 20 declared
      deviations, 0 undeclared — plus 82 tests in `netsrv_test.py`; spec and
      findings in `docs/PS3NETSRV-PROTOCOL.md`. Ships in the image as the default,
      with `--build-arg NETSRV=ps3netsrv++` kept as the fallback
- [x] **Web GUI Add Games** — a topbar button opens an upload panel (platform
      picker, file input + drag-and-drop, per-file progress and result). Files
      go one request each to the new `POST /api/upload/file`, streamed to
      `.part` + atomic rename, so a multi-GB ISO never sits in the server's
      memory the way the multipart `POST /api/upload` (kept for curl) would
      leave it. Uploads upsert their catalog entry by `url` — fixing a bug
      where an upload after a regeneration collapsed the catalog to one
      entry — and write a `<name>.meta.json` sidecar whose `platform` and
      `title` `generate_catalog.py` now passes through, so PSP/PS3 tab
      membership survives regeneration; the generator no longer lists
      `.meta.json`/`.sha256` sidecars as games. 13 new `check_share.sh`
      assertions, and the panel plus a browser-origin upload were driven in a
      real browser (progress events, merged catalog, no entry loss)
- [x] **Web GUI redesigned as a modern library dashboard** — the Media Go/
      Cover Flow shell became a sidebar + toolbar + card-grid + detail-drawer
      app in ES modules and token-based CSS (still dependency-free, the image
      is still a file copy; Cover Flow remains the device apps' own look).
      Search (`/`), four sorts, per-folder counts, wired Recently Viewed (the
      old tab read state nothing wrote), real Settings (light/dark/auto theme,
      default sort, local-data reset), toasts, a whole-window drop overlay and
      a transfer dock; mobile sidebar and bottom sheet. Management added
      server-side: `DELETE /api/item`, `POST /api/item/rename`,
      `PUT /api/item/meta` (merge into `<name>.meta.json`; rename stays in its
      folder, carries sidecars, re-keys the catalog; delete cleans sidecars,
      refuses directories/sidecars/`catalog.json`/traversal), all driven from
      the drawer with a two-click delete confirm. 18 new `check_share.sh`
      assertions; every flow (drawer, rename with sidecar ride-along, meta
      merge, favorite, delete, theme, search, Add Games modal) was driven in
      a real browser
- [x] **PS3 license (.rap) support in the web GUI** — the game's drawer has a
      License (RAP) section: a `.rap` uploaded beside the game is offered
      with one click, or one is picked from disk and uploaded into the game's
      folder via the new `dir=` parameter on `POST /api/upload/file`; the
      pairing is the sidecar's `rap` field (validated to be a real `.rap` in
      the game's own folder through `PUT /api/item/meta`), which
      `generate_catalog.py` passes through so attachments survive
      regeneration. Attached `.rap`s lose their standalone card (the game
      carries a 🔑 badge, red when the license file went missing behind the
      share's back); loose ones stay discoverable as License cards.
      Renaming a license repoints the sidecars and entries beside it; a
      title-ID mismatch asks before attaching. 19 new `check_share.sh`
      assertions; attach/detach/missing states, the hidden-rap grid, and a
      browser-origin `dir=` upload were driven in a real browser. The share
      only stores `.rap` files the user supplies for games they own
- [x] **Virtual ISO** (`server/netsrv/virtual_iso.py`): `/***PS3***/` and
      `/***DVD***/` mount a game *folder* by synthesising its ISO 9660 image,
      built from upstream's `VIsoFile` and byte-identical to it outside the
      fields upstream itself does not repeat (the random PS3 disc-info sector,
      the volume timestamps). File contents are read at whatever offset a client
      asks for, so nothing is prebuilt or cached. §7.4
- [x] **Store view on the PSP, rendered by the Cover Flow module** —
      `psp/store.c` feeds `ui/coverflow.c` (built for the debug screen through
      `psp/coverflow_psp.c`) and `Triangle` opens it from the browser. It parses
      both the hand-written `items` catalog and `generate_catalog.py`'s
      `entries` form (integer `size`, formatted on-device), derives PSP
      EBOOT/ISO/CSO destinations locally, and keeps favourites plus the eight
      most recent installs in `store_state.txt` beside `settings.txt`. Built
      with `psp-gcc` and run in PPSSPPSDL: browse, category switch, grid toggle,
      favourite add/remove, and installs of two `EBOOT.PBP`s and a 3 MB ISO each
      landing on the memory stick with a sha256 equal to the served file. The
      evidence, and the five bugs the run found, are in `log.md` (2026-10-30).
      Real PSP hardware is still untested.
- [x] **Store categories on the PSP** — the Store sends the catalog's
      first-seen, unique categories to Cover Flow (`coverflow_set_categories`);
      L/R moves between them and each tab filters on the exact category name,
      so the tabs are the library's own folders. The shared fixed menu stays
      the default for other callers and as the empty-catalog fallback. Built
      with `psp-gcc` and validated in PPSSPPSDL: tabs rendered as `Games` and
      `Utilities`, L/R switched and clamped, and a Utilities install landed
      with a sha256 equal to the served file. Evidence in `log.md`
      (2026-10-30). Real PSP hardware is still untested.
- [x] **Store view on the PSP as a pixel GUI (sceGu)** — the debug-screen
      renderer was all the device had; the Store now draws Media Go's own look
      through `sceGu` at 480x272: orange header, tab rail with active-tab
      underline, Cover Flow cards with id-hashed gradient covers (the web GUI's
      `placeholderBG()` hash reproduced and pinned by host tests) plus initials
      and dimmed neighbours, a 4x2 grid view with favourite stars, and an
      install screen with a live progress bar. `psp/gui.{c,h}` is the renderer
      core (rects, 8x8 font atlas, textured quads) the PS3 RSX stages will
      reuse; `ui/coverflow.c` still owns all state, and `-DNASDL_TEXT_UI`
      rebuilds the original layout. Built with `psp-gcc` and driven in
      PPSSPPSDL end-to-end: browse, tab switch, cover move, favourite and
      recents persisting to `store_state.txt`, two installs landing
      byte-identical, O returning to a live browser and a clean second store
      visit; the emulator also pinned down two real GE behaviours along the
      way (sprite attribute packing, and GPU-side framebuffers outliving
      `sceGuTerm`), documented in `log.md` (2026-10-31). Real PSP hardware is
      still untested.
- [x] **PS3 pixel UI + Store view on the RSX** — the whole PS3 app moved from
      ANSI TTY output to a pixel display: `ps3/gfx.{c,h}` draws frames on the
      CPU into a main-RAM back buffer, copies each into one of two RSX display
      buffers and flips on vsync (no shaders or RSX draws), mapping a virtual
      1280x720 grid onto whatever mode the XMB reports; `ps3/ui.h` fronts two
      interchangeable app backends — `ui_pix.c` (default) and `ui_text.c`
      (`-DNASDL_TEXT_UI`, also the fallback when `gfx_init()` fails). The
      catalog parser and install-path logic moved out of `psp/` into
      `ui/catalog.{c,h}` + `store_derive_path_ps3()` (host-tested), so the PS3
      `ps3/store.c` + `ps3/store_gui.c` browse the same `catalog.json` as the
      PSP Store: Triangle opens Cover Flow (tabs, favourites, grid, install
      progress), landing `.pkg` in `/dev_hdd0/packages`, disc images in
      `PS3ISO`/`PSPISO` and `.rap` files in `exdata/`, with favourites/recents
      persisted like the PSP's. Built warning-free with `ppu-gcc` (both
      backends) and driven end-to-end in RPCS3 through injected keys: browser
      download and Store install each landing with a sha256 equal to the
      served file, tab/favourite/grid toggles on screen, and the run pinned
      three real bugs (result panels dismissed by the button *release* that
      triggered them, `//` in entered directory names, hint/status bar
      overlap) — evidence in `log.md` (2026-10-31). Real PS3 hardware is
      still untested.

- [x] **Web GUI: Store queue management + download activity** — the sidebar
      gains a Store entry (pending-count badge) opening a queue modal: add
      `url`/`title`/`category`, remove a queued item, and *Download now*
      (`POST /api/store/run`, which spawns the fetcher detached instead of
      waiting on `STORE_INTERVAL`). Live progress and a newest-first history
      render in a Store section of the now-shared transfer dock (`dock.js`,
      which Uploads moved into too), and the shell refetches the catalog
      when a finished download has changed it so new cards appear without a
      reload. The fetcher now *claims* the queue (atomic rename, so a
      mid-run append waits for the next pass rather than being wiped) and
      publishes a throttled status file, guarding its own `category` and url
      because the queue is hand-editable; a `../` category wrote outside the
      library before that guard. `check_share.sh` grows to 106 assertions —
      validation codes, hidden queue file, claimed-url `409`, stale-status
      idle, and a real run byte-matched against the served file and reaching
      `/api/catalog` — and the whole flow (add → run → live dock → extracted
      card, plus the delete path) was driven in a real browser against a
      throttled CDN; evidence in `log.md` (2026-10-31).
- [x] Browser metadata: the share's listing anchors carry each entry's
      `data-size`/`data-cat` (attributes after the quoted href — invisible to
      the apps' `href="` scan and to foreign parsers), and both browsers draw
      store-style file rows from them: size, plus the category where the path
      header doesn't already state it. Stock `http.server`/nginx shares need
      no change — no attributes, old plain rows
- [x] Real cover art: `server/store/cover_art.py` (stdlib only) reads the
      ICON0 PNG out of PSP ISO/CSO/PBP files (minimal ISO9660 + CISO block
      readers, PBP header slice — never a whole image) and caches it as a
      hidden `<name>.icon0.png` sidecar; PS3 game folders keep pointing at
      their disc-layout `ICON0.PNG`. `generate_catalog.py` publishes the
      sidecar as the entry's `icon` and the web GUI tiles render it; the
      device Store views still draw initials (no PNG codec there)
- [x] Linux front-end for managing games and catalog metadata — retired its
      GTK/Qt framing and shipped through the server web GUI: the drawer edits
      metadata, renames, moves games between categories (`POST /api/item/move`,
      sidecars and hidden covers travelling, license-pairing rules), and
      controls cover art (upload, re-extract, clear — all on the same
      `.icon0` sidecar convention, no second UI stack to build or drift)
- [x] **The browser IS the Cover Flow — both clients went pixel-only** — the
      text/list browsers are gone: boot opens the shared Cover Flow state with
      the share listing as its covers, driven by the new
      `ui/browser_model.c` (listing → cover items + tab rail: a wildcard
      `All` tab, one tab per share category in first-seen order, then
      `Downloads`, which lists the device's own download folder;
      dotfiles and `.part` resumes stay hidden). L1/R1 move tabs, Square
      toggles flow ⇄ grid, Cross opens/downloads, Circle leaves the
      Downloads view, up one level, and quits at root. `ui/coverflow.c` grew
      the empty-name wildcard category (renders `All`, matches everything)
      and `coverflow_set_items()` (swap item storage under a live state —
      the share ⇄ Downloads swap keeps the rail); `store_gui_browse()` draws
      the frame on both pixel backends with the browser's own hints and the
      cwd in the header. On the PSP, `Licenses` and `PS3` are filtered out
      of covers *and* tabs — in the browser and in the Store — because a PSP
      cannot act on them; the PS3 hides only `Licenses` (a game's `.rap`
      installs with the game, see below — its `PS3` category stays). The
      debug-screen Settings
      picker survives behind SELECT only (`gui_to_debug()` reusing the
      after-GE VRAM trick); the PS3 app deleted `ui_text.c`, the
      `TEXT_UI` build and every ANSI branch. 208 Cover Flow assertions plus
      the new `browser_model` tests (`make -C ui test`, ASan/UBSan clean),
      zero-warning device builds, and both apps driven end-to-end with
      injected keys — boot, tab switch, grid toggle and a populated
      Downloads view screenshotted in PPSSPP and RPCS3 (`log.md`
      2026-10-31). Real hardware is still untested.
- [x] **PS3 installs carry their license** — the catalog parser keeps the
      sidecar's `rap` field per item (`ui/catalog.{c,h}`, bounded to its own
      object like `platform`), and the new shared `store_license_url()`
      (`ui/store_logic.{c,h}`) resolves it next to the entry URL (folder
      entry → inside the folder, file entry → its parent). `ps3/store.c`
      installs the game first, then fetches the `.rap` into
      `/dev_hdd0/exdata/` through the same resumable downloader; a failed
      license never undoes the game install and the result panel says
      `(+ license)` / `(license failed)`. With the ride-along in place the
      `Licenses` menu is gone on the PS3: the browser skip list and the
      Store's tab filter both drop it (the `PS3` category stays visible on
      a PS3). New host assertions for the license URLs and the scoped `rap`
      parse (`make -C ui test`, ASan clean); warning-free PS3 build;
      end-to-end in RPCS3 — installing `Quake_1.08.pkg` landed the pkg in
      `packages/` *and* `Quake_1.08.rap` in `exdata/`, both md5-equal to
      the served files, with the Licenses tab absent from browser and
      Store (`log.md` 2026-10-31, screenshots `rap-*.png`).

### In progress
- [ ] Test Store flow end-to-end through the Docker container with a real queue
      file + local HTTP CDN. Share-level resume is automated now
      (`server/store/check_share.sh --with-dl`); what remains is the container
      path — entrypoint → fetcher → catalog regeneration → app download against
      port 8000 in the built image

### Planned
- [ ] Validate the netiso server on a real PS3 with webMAN MOD (the console is
      in hand): what the synthetic window harness (`server/netsrv/netsrv_synth.py`,
      2026-10-31) left for the wire — which opcodes a client sends (incl. the
      `0x24xx` range), whether it holds one connection or several, whether
      `ctime` displays as a creation time, and a 713 MB `MODE2/2352` rip
      (`Driver (Europe)`) playing through its late regions against ours and
      upstream's; the 848 MiB ceiling question of §8 is already measured closed
      (`docs/PS3NETSRV-PROTOCOL.md` §5 window table)

### Later
- [ ] PS4 support: platform-aware share (ps4 alongside psp/ps3 in upload tags,
      metadata, and GUI sections) organizing PKG titles and the user's own
      .rap licenses like everything else, plus docs/PS4.md for the real
      consumers (old-firmware browser on /gui/, PS4 Linux payload curl with
      Range/resume, exFAT USB installs) — a signed native client is
      exploit-dependent and stays out of scope
- [ ] HTTPS support on the HTTP share

## License

MIT. Uses third-party components under their own licenses (see `server/`).
