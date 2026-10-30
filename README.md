# PS Linker

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Docker / Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, serves files over HTTP for PSP/PS3 download (with Range/resume), and runs the server-side Store (downloads queued CDN titles into the same unified library) |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: connects to Wi-Fi, browses/downloads files from the PS Linker server to the Memory Stick (resumable downloads); the Store view is a pixel (sceGu) GUI |
| `ps3/` | PS3 (homebrew, PSL1GHT) | `nasdl` app: browses/downloads files from the PS Linker server to the HDD (resumable downloads) |
| `ui/` | shared source | Cover Flow renderer + key state machine both device apps can build against (ANSI only, no GPU); see `ui/INTEGRATION.md` |

> Intended for homebrew and legally-owned content. This project does not provide or encourage piracy.

## Server (Docker — recommended)

The server runs both services in one container:
- **netiso server** on port `38008` (PS3 game streaming) — `server/netsrv/ps3netsrv.py`,
  our own implementation of the ps3netsrv protocol. Stdlib Python 3, nothing to
  compile, no third-party binary in the image.
- **HTTP share** on port `8000` (PSP/PS3 NASDL app downloads; supports `Range`/resume)
- **Server-side Store**: fetches queued CDN titles into the same games library and regenerates `catalog.json` (served at `/catalog.json`) every `STORE_INTERVAL` seconds

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
and delete files. If the library has *folder* games (a directory with a
`PS3_GAME/` inside, which webMAN mounts through `/***PS3***/`), those are not
served yet — build the fallback instead:

```bash
docker build -t ps-linker --build-arg NETSRV=ps3netsrv++ server/
```

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

The PSP app browses the HTTP share, shows optional catalog icons/metadata, and downloads with resume support.

```bash
cd psp
make
# Copy EBOOT.PBP to ms0:/PSP/GAME/NASDL/
```

## PS3 app (nasdl)

The PS3 app browses the HTTP share and downloads to the HDD with resume support. It can be run in RPCS3 or installed on a jailbroken PS3.

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
      with `server/netsrv/netsrv_diff.py` — 48 identical answers, 15 declared
      deviations, 0 undeclared — plus 62 tests in `netsrv_test.py`; spec and
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

### In progress
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
- [ ] Cover Flow on the PS3 app — `ps3/main.c` writes ANSI to the TTY so
      `has_colors=1` applies directly, but its browser has no size/category to
      show until the share reports per-entry metadata (listed under Later)
- [ ] Real cover art: extract ICON0.PNG from PSP ISO/CSO and PS3 game folders
      into the catalog so tiles show real covers instead of placeholders
- [ ] Test Store flow end-to-end through the Docker container with a real queue
      file + local HTTP CDN. Share-level resume is automated now
      (`server/store/check_share.sh --with-dl`); what remains is the container
      path — entrypoint → fetcher → catalog regeneration → app download against
      port 8000 in the built image

### Planned
- [ ] Validate the netiso server on a real PS3 with webMAN MOD: the open questions
      at the end of `docs/PS3NETSRV-PROTOCOL.md` §8 (which opcodes a real client
      sends, whether it holds one connection or several, what it makes of the
      sector probe's 848 MiB ceiling)
- [ ] Virtual ISO: serve folder games through `/***PS3***/`, which means
      synthesising an ISO 9660 filesystem over a directory tree. Until then the
      Docker image keeps `--build-arg NETSRV=ps3netsrv++` as the fallback, and a
      library with folder games needs it

### Later
- [ ] Web GUI: Store queue management and download-activity panels (the shell
      renders the catalog today; watched `.store-queue.json` works)
- [ ] Browser metadata: have the share report size and category per entry so the
      flat file list can carry the same chrome as the store view
- [ ] Store queue interface: simple web UI endpoint to queue CDN downloads
      (watched `.store-queue.json` works today)
- [ ] GTK/Qt GUI front-end for Linux (manage games + edit catalog metadata)
- [ ] PS3: RSX pixel-graphics renderer (currently ANSI console output)
- [ ] PS4 support: platform-aware share (ps4 alongside psp/ps3 in upload tags,
      metadata, and GUI sections) organizing PKG titles and the user's own
      .rap licenses like everything else, plus docs/PS4.md for the real
      consumers (old-firmware browser on /gui/, PS4 Linux payload curl with
      Range/resume, exFAT USB installs) — a signed native client is
      exploit-dependent and stays out of scope
- [ ] HTTPS support on the HTTP share

## License

MIT. Uses third-party components under their own licenses (see `server/`).
