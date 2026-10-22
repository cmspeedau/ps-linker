# PS Linker

Cross-platform LAN game streaming and downloading for **PS3** and **PSP**, with first-class **Linux** support.

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Docker / Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, and serves files over HTTP for PSP/PS3 download |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: connects to Wi-Fi, browses/downloads files from the PS Linker server to the Memory Stick |
| `ps3/` | PS3 (homebrew, PSL1GHT) | `nasdl` app: browses/downloads files from the PS Linker server to the HDD |

> Intended for homebrew and legally-owned content. This project does not provide or encourage piracy.

## Server (Docker — recommended)

The server runs both services in one container:
- **ps3netsrv++** on port `38008` (PS3 game streaming)
- **HTTP share** on port `8000` (PSP/PS3 NASDL app downloads)

### Quick start

```bash
cd server
mkdir games                       # put your games here (or edit compose path)
docker compose up -d --build
```

Then:
- **PS3 (webMAN MOD):** `webMAN MOD -> Setup -> Network Servers` → add your Docker host's IP.
- **PSP/PS3 NASDL app:** set the NAS URL to `http://<docker-host-ip>:8000`.

### Manual build (without compose)

```bash
docker build -t ps-linker-server server/
docker run -d --name ps-linker \
  -p 38008:38008 -p 8000:8000 \
  -v /path/to/games:/games:ro \
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

Edit the server address (`SERVER`) / file list in `psp/main.c` to point at the Docker host's HTTP share (`http://<ip>:8000`).

## PS3 app (nasdl)

Requires the [PSL1GHT toolchain](https://github.com/PS3Dev/ps3toolchain) and a PS3 with CFW/HEN.

```bash
cd ps3
make          # produces nasdl.self
make pkg      # optional: installable .pkg
```

See [docs/UI-REFERENCE.md](docs/UI-REFERENCE.md) for the app UI design reference.

## Roadmap

- [x] Repo scaffold + Linux server setup
- [x] PSP: interactive NAS browser (parse directory listing, on-device file picker)
- [x] PSP: Media Go-style UI
- [x] UI reference doc (`docs/UI-REFERENCE.md`)
- [x] PS3 port (`ps3/` — PSL1GHT, socket HTTP, same browse/download flow)
- [x] Server: Docker image + docker-compose (ps3netsrv++ + HTTP share, one command)
- [x] PS3: subdirectory navigation (enter dirs with Cross, Circle goes up; root Circle quits)
- [x] PSP: subdirectory navigation (parity with PS3 app — X enters dirs, O goes up)
- [ ] PSP/PS3: unzip downloads with zlib
- [ ] GTK/Qt GUI front-end for Linux
- [ ] HTTP Range resume in PSP/PS3 apps
- [ ] PS3: RSX pixel-graphics renderer (currently ANSI console output)
- [ ] Both apps: functional Settings view (on-device NAS URL editing)
- [ ] Compile-verify both builds on real toolchains (pspdev / PSL1GHT)

## License

MIT. Uses third-party components under their own licenses (see `server/`).
