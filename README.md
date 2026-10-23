# PS Linker

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Docker / Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, serves files over HTTP for PSP/PS3 download (with Range/resume), and runs the server-side Store (downloads queued CDN titles into the same unified library) |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: connects to Wi-Fi, browses/downloads files from the PS Linker server to the Memory Stick (resumable downloads) |
| `ps3/` | PS3 (homebrew, PSL1GHT) | `nasdl` app: browses/downloads files from the PS Linker server to the HDD (resumable downloads) |
| `ui/` | shared source | Cover Flow renderer + key state machine both device apps can build against (ANSI only, no GPU); see `ui/INTEGRATION.md` |

> Intended for homebrew and legally-owned content. This project does not provide or encourage piracy.

## Server (Docker — recommended)

The server runs both services in one container:
- **ps3netsrv++** on port `38008` (PS3 game streaming)
- **HTTP share** on port `8000` (PSP/PS3 NASDL app downloads; supports `Range`/resume)
- **Server-side Store**: fetches queued CDN titles into the same games library and regenerates `catalog.json` (served at `/catalog.json`) every `STORE_INTERVAL` seconds

### Quick start

```bash
cd server
git clone https://github.com/dirkvdb/ps3netsrv-- ps3netsrv--
docker compose up -d --build
```

Then:

- Put game files under `./games/` (or change the volume in `docker-compose.yml`)
- Open `http://<server>:8000/` to browse/download
- Open `http://<server>:8000/gui/` for the web library (Cover Flow / grid, catalog, uploads)
- Point your PS3 at `<server>:38008` for game streaming

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

`generate_catalog.py` merges these sidecars with folder-derived categories and serves the result as `/catalog.json`.

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
# 1. Build ps3netsrv++
git clone https://github.com/dirkvdb/ps3netsrv-- server/ps3netsrv--
cd server/ps3netsrv-- && make

# 2. Point it at your game library
./ps3netsrv++ -d /path/to/games        # PS3 streaming (port 38008)

# 3. HTTP share for the PSP/PS3 apps (Range/resume capable)
python3 server/store/http_server.py /path/to/games 8000
```

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
- [x] Server: Docker image + docker-compose (ps3netsrv++ + HTTP share, one command)
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
      unified catalog (`GET /api/catalog`). The upload *endpoint*
      (`POST /api/upload`) exists and is checked; the browser-side "Add Games"
      UI does not exist yet
- [x] **Cover Flow UI (web GUI)** — horizontally scrolling cover carousel
      (centered title enlarged/in focus), top menu bar with all categories
      (All Games | PSP Games | PS3 Games | Recently Added | Favorites | Downloads |
      Settings) filtering the flow, metadata panel with favorite toggle below the
      carousel, and a grid ⇄ cover-flow view toggle. Arrow keys to flip,
      Enter/click to select
- [x] Shared Cover Flow module for the device apps (`ui/coverflow.c/h`, ANSI, no
      GPU): 197 host assertions + ASan/UBSan clean, and compiles warning-free
      under `psp-gcc` and `ppu-gcc` with each app's flags (`make -C ui test`)

### In progress
- [ ] **Store view on the PSP, rendered by the Cover Flow module** — build
      `psp/store.c` with `ui/coverflow.*` as its renderer, opened with Triangle
      as `psp/store.h` already documents. Two gaps first, both read off the
      code: `store.c` parses `size_hint` and `install_path` while
      `generate_catalog.py` emits `size` (int) and no `install_path`, and
      `store_download()` needs a destination — so install paths get derived
      on-device and sizes formatted from `size`; favourites/recent need a small
      file next to `settings.txt`. `store.c` already prints through
      `pspDebugScreenPrintf`, so the module's `has_colors=0` path fits the PSP
      text screen (40 columns: tiles 8+2+14+2+8)
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
- [ ] Rewrite ps3netsrv in-house and integrate it into our own Docker image

### Later
- [ ] Web GUI: **Add Games** panel wired to `POST /api/upload` — the endpoint is
      implemented and checked, the shell has no file input yet
- [ ] Web GUI: Store queue management and download-activity panels (the shell
      renders the catalog today; watched `.store-queue.json` works)
- [ ] Browser metadata: have the share report size and category per entry so the
      flat file list can carry the same chrome as the store view
- [ ] Store queue interface: simple web UI endpoint to queue CDN downloads
      (watched `.store-queue.json` works today)
- [ ] GTK/Qt GUI front-end for Linux (manage games + edit catalog metadata)
- [ ] PS3: RSX pixel-graphics renderer (currently ANSI console output)
- [ ] HTTPS support on the HTTP share

## License

MIT. Uses third-party components under their own licenses (see `server/`).
