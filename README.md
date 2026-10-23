# PS Linker

Cross-platform LAN game streaming and downloading for **PS3** and **PSP**, with first-class **Linux** support.

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

`.zip` downloads are **extracted on the server** (with zip-slip protection) and the archive is deleted, so clients download ready-to-run files and never need on-device unzip support.

### Catalog metadata (optional)

Each `catalog.json` entry includes `title`, `category`, `url`, and `size`. Two optional extras are picked up automatically:

- **Cover art:** drop an image named `ICON0.png`, `cover.png`/`.jpg`, `boxart.*`, or `thumb.*` next to the game file — the entry gains an `"icon"` URL clients can display.
- **Version / description:** drop a sidecar `<gamename>.meta.json` next to the game file:
  ```json
  {"version": "1.2", "description": "Optional blurb shown in the client"}
  ```

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

# 3. HTTP share for the PSP/PS3 apps (Range/resume capable)
python3 server/store/http_server.py /path/to/games 8000
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

Downloads are **resumable**: if a transfer is interrupted, re-selecting the file resumes from where it left off (`Range` request; the server must support 206 responses, which the PS Linker share does).

## PS3 app (nasdl)

Requires the [PSL1GHT toolchain](https://github.com/PS3Dev/ps3toolchain) and a PS3 with CFW/HEN.

```bash
cd ps3
make          # produces nasdl.self
make pkg      # optional: installable .pkg
```

On RPCS3, boot `nasdl.self` or install the generated `.pkg`; RPCS3 rejects the loose `nasdl.elf`.

Same Settings view as the PSP app (**Select** in the browser); the URL persists to `/dev_hdd0/NASDL/settings.txt`. Downloads are resumable the same way as on PSP.

See [docs/UI-REFERENCE.md](docs/UI-REFERENCE.md) for the app UI design reference.

## Build verification

Both device builds are verified against real toolchains:

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
- [x] Both device builds verified against real toolchains

### In progress
- [ ] Test Store flow end-to-end with a real queue file + local HTTP CDN

### Planned
- [ ] **Server web GUI (Sony Media Go style)** — a browser-based desktop-class UI served by the HTTP server, modeled on Sony's Media Go:
  - Dark multi-pane layout: library sidebar (categories/covers), game grid with cover art, and a detail/download pane
  - Library view of the unified games folder (`catalog.json`), with cover art and metadata (version/description)
  - Store management: view/paste CDN URLs, edit and submit the `.store-queue.json` queue, watch fetch progress, trigger catalog refresh
  - Download activity view (recent transfers, resume events) driven by the existing HTTP access log
  - Read/write stays scoped to the games volume; no auth changes to the existing share (optional simple access key for the admin actions)
  - Reuse the Media Go visual language already established in `docs/UI-REFERENCE.md` (same palette, spacing, and pane structure as the on-console apps)

## License

MIT. Uses third-party components under their own licenses (see `server/`).
