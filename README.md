# PS Linker

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Docker / Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, serves files over HTTP for PSP/PS3 download (with Range/resume), and runs the server-side Store (downloads queued CDN titles into the same unified library) |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: connects to Wi-Fi, browses/downloads files from the PS Linker server to the Memory Stick (resumable downloads) |
| `ps3/` | PS3 (homebrew, PSL1GHT) | `nasdl` app: browses/downloads files from the PS Linker server to the HDD (resumable downloads) |

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
- [x] Client resume: self-contained `resume.c/h` modules for both apps —
      send `Range: bytes=<size>-` when a partial file exists, append on 206,
      restart cleanly on 200; wired into both Makefiles
- [x] Compile-verify both device builds on real toolchains (pspdev / PSL1GHT):
      `psp/EBOOT.PBP`, `ps3/nasdl.self` and `ps3/nasdl.pkg` compile successfully
- [x] **Server web GUI (Sony Media Go style)** — browser-based UI served by the
      HTTP server: dark multi-pane layout, cover grid, detail/download pane,
      Store queue management, and download activity view
- [x] Web GUI library sections: All Games / Local PSP / PS3 Games filters over
      the unified catalog, plus an **Add Games** upload feature
      (multi-file upload with platform selection, drag-and-drop, `POST /api/upload`)
- [x] **Cover Flow UI (web GUI)** — 3D-style horizontally scrolling cover carousel
      (centered title enlarged/in focus), top menu bar with all categories
      (All Games | PSP Games | PS3 Games | Recently Added | Favorites | Downloads |
      Settings) filtering the flow, metadata panel with favorite toggle below the
      carousel, and a grid ⇄ cover-flow view toggle. Arrow keys to flip,
      Enter/click to select

### In progress
- [ ] **Cover Flow UI on the PSP/PS3 device apps** — port the carousel + category
      menu bar interaction from the web GUI to the on-device UIs
- [ ] Real cover art: extract ICON0.PNG from PSP ISO/CSO and PS3 game folders
      into the catalog so tiles show real covers instead of placeholders
- [ ] Test Store flow end-to-end with a real queue file + local HTTP CDN
      (including a Range-resume transfer against the container share)

### Planned
- [ ] Rewrite ps3netsrv in-house and integrate it into our own Docker image

### Next
- [ ] Optional minimal Store view on PSP/PS3 (render `catalog.json` with
      titles/categories/icons — client stays thin, server does the work)

### Later
- [ ] Store queue interface: simple web UI endpoint to queue CDN downloads
      (watched `.store-queue.json` works today)
- [ ] GTK/Qt GUI front-end for Linux (manage games + edit catalog metadata)
- [ ] PS3: RSX pixel-graphics renderer (currently ANSI console output)
- [ ] HTTPS support on the HTTP share

## License

MIT. Uses third-party components under their own licenses (see `server/`).
