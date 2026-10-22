# PS Linker

Cross-platform LAN game streaming and downloading for **PS3** and **PSP**, with first-class **Linux** support.

Two components:

| Component | Target | What it does |
|---|---|---|
| `server/` | Linux / NAS (host) | Streams games to PS3 via the ps3netsrv protocol, and serves files over HTTP for PSP download |
| `psp/` | PSP (homebrew) | `NAS Downloader` app: connects to Wi-Fi, browses/downloads files from the PS Linker server to the Memory Stick |

> Intended for homebrew and legally-owned content. This project does not provide or encourage piracy.

## Server (Linux / NAS)

Uses the open-source `ps3netsrv--` build (dirkvdb) for PS3 streaming, plus a simple HTTP share for the PSP app.

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

### PS3 setup

1. Install **webMAN MOD** (requires CFW/HEN).
2. `webMAN MOD -> Setup -> Network Servers` → add your Linux box's IP.
3. Games appear on the XMB and stream over LAN — no copying needed.

## PSP app (NAS Downloader)

Requires the [pspdev toolchain](https://github.com/pspdev/psptoolchain) and a PSP with CFW (ARK-4 / PRO), or PPSSPP for testing.

```bash
cd psp
make
# Copy EBOOT.PBP to ms0:/PSP/GAME/NASDL/
```

Edit the server address (`SERVER`) / file list in `psp/main.c` to point at your NAS's HTTP share.

## Roadmap

- [x] Repo scaffold + Linux server setup
- [ ] PSP: interactive NAS browser (parse directory listing, on-device file picker)
- [ ] PSP: unzip downloads with zlib
- [ ] Server: single binary bundling ps3netsrv + HTTP share
- [ ] GTK/Qt GUI front-end for Linux
- [ ] Progress bars + HTTP Range resume in PSP app

## License

MIT. Uses third-party components under their own licenses (see `server/`).
