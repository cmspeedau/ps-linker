# PS Linker

Cross-platform LAN game streaming and downloading for **PS3** and **PSP**, with first-class **Linux** support.

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Docker / Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, serves files over HTTP for PSP/PS3 download, and runs the server-side Store (downloads queued CDN titles into the same unified library) |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: connects to Wi-Fi, browses/downloads files from the PS Linker server to the Memory Stick |
| `ps3/` | PS3 (homebrew, PSL1GHT) | `nasdl` app: browses/downloads files from the PS Linker server to the HDD |

> Intended for homebrew and legally-owned content. This project does not provide or encourage piracy.

## Server (Docker — recommended)

The server runs both services in one container:
- **ps3netsrv++** on port `38008` (PS3 game streaming)
- **HTTP share** on port `8000` (PSP/PS3 NASDL app downloads)
- **Server-side Store**: fetches queued CDN titles into the same games library and regenerates `catalog.json` (served at `/catalog.json`) every `STORE_INTERVAL` seconds

### Quick start

```bash
cd server
mkdir games                       # put your games here (or edit compose path)
docker compose up -d --build
```

Then:
- **PS3 (webMAN MOD):** `webMAN MOD -> Setup -> Network Servers` → add your Docker host's IP.
- **PSP/PS3 NASDL app:** set the NAS URL to `http://<docker-host-ip>:8000` — editable on-device in the Settings view (no recompile needed).

### Store (server-side only)

Uploads and Store downloads land in the **same** games folder, so clients see one unified library — no client changes required. To queue a download, drop a `.store-queue.json` into the games directory:

```json
[
  {"title": "Example Homebrew", "category": "Homebrew",
   "url": "https://example.cdn/game.zip"}
]
```

The container fetches each URL into `games/<category>/` (atomic temp-file writes, so partial downloads are never visible), then removes the queue file. The catalog regenerates automatically.

### Manual build (without compose)

```bash
docker build -t ps-linker-server server/
docker run -d --name ps-linker \
  -p 38008:38008 -p 8000:8000 \
  -v /path/to/games:/games \
  ps-linker-server
```

Note: the games volume is read-write so the Store can save downloads into it.

## Server (bare-metal Linux, alternative)

### Quick start

```bash
# 1. Build ps3netsrv++
git clone https://github.com/dirkvdb/ps3netsrv-- server/ps3netsrv--
cd server/ps3netsrv-- && make

# 2. Point it at your game library
./ps3netsrv++ -d /path/to/games        # PS3 streaming (port 38008)

# 3. HTTP share for the PSP app
python3 -m http.server 8000 --directory /path/to/games
```

### Run at boot (systemd)

Copy `server/ps-linker.service` to `/etc/systemd/system/`, edit the paths, then:

```bash
sudo systemctl enable --now ps-linker
```

## PSP app (NAS Downloader)

Requires the [pspdev toolchain](https://github.com/pspdev/psptoolchain) and a PSP with CFW (ARK-4 / PRO), or PPSSPP for testing.

```bash
cd psp
make
# Copy EBOOT.PBP to ms0:/PSP/GAME/NASDL/
```

The server address defaults to `http://192.168.1.50:8000` and can be changed on-device: press **Select** in the browser to open the Settings view, edit the URL with the character picker, then press **Start** to save. The URL persists to `ms0:/PSP/GAME/NASDL/settings.txt`.

## PS3 app (nasdl)

Requires the [PSL1GHT toolchain](https://github.com/PS3Dev/ps3toolchain) and a PS3 with CFW/HEN.

```bash
cd ps3
make          # produces nasdl.self
make pkg      # optional: installable .pkg
```

Same Settings view as the PSP app (**Select** in the browser); the URL persists to `/dev_hdd0/NASDL/settings.txt`.

See [docs/UI-REFERENCE.md](docs/UI-REFERENCE.md) for the app UI design reference.

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

### In progress
- [ ] Compile-verify both device builds on real toolchains (pspdev / PSL1GHT)
- [ ] Test Store flow end-to-end with a real queue file + local HTTP CDN

### Next
- [ ] Optional minimal Store view on PSP/PS3 (render `catalog.json` with
      titles/categories — client stays thin, server does the work)
- [ ] Catalog niceties: cover art (`icon` field), version display,
      categories derived from folder structure
- [ ] PSP/PS3: unzip downloads with zlib
- [ ] HTTP Range resume in PSP/PS3 apps

### Later
- [ ] Store queue interface: simple web UI endpoint to queue CDN downloads
      (watched `.store-queue.json` works today)
- [ ] GTK/Qt GUI front-end for Linux (manage games + edit catalog metadata)
- [ ] PS3: RSX pixel-graphics renderer (currently ANSI console output)
- [ ] HTTPS support on the HTTP share

## License

MIT. Uses third-party components under their own licenses (see `server/`).
