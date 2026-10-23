#!/usr/bin/env python3
"""PS Linker HTTP share — games library for the PSP/PS3 apps, plus the web GUI.

One port serves three things, in this routing order:

1. ``/api/*`` — the web GUI's JSON endpoints (``GET /api/catalog``,
   ``POST /api/upload``).
2. ``/gui/`` — the web GUI, with its assets at ``/gui/<asset>`` (``/ui/*`` is
    kept as an alias). It sits under a prefix on purpose: ``/`` has to stay the
    library listing, because that is the first thing the NASDL apps request.
3. everything else — the games library, which is what the NASDL apps talk to:
   directory listings in the same simple HTML the stock ``http.server`` emits
   (both apps parse ``href`` entries, trailing ``/`` meaning a directory), and
   files with ``Range``/``206`` support so an interrupted download resumes from
   its partial file instead of restarting.

Everything in group 3 resolves *inside the games directory* and nothing else
(no traversal). An earlier rewrite resolved every path against the GUI folder
instead, which 404'd every game file and listing, and carried double-escaped
regexes (``r'^bytes=(\\d*)-(\\d*)$'`` matches a literal backslash, not a digit),
so ``Range`` was silently ignored and the client fell back to a full restart.

If a file has a ``<name>.sha256`` sidecar next to it, its digest is sent as
``X-SHA256``: the resumable downloader in ``ps3/resume.c`` (and the PSP one)
hashes the completed file and refuses it on a mismatch.

Usage: http_server.py <games_dir> [port]
"""
import html
import json
import mimetypes
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")
# Characters that must never appear in an uploaded filename (basename is
# already stripped, this is the belt to that braces).
BAD_NAME_RE = re.compile(r"[\x00-\x1f\x7f/\\]")
ALLOWED_EXT = {".iso", ".cso", ".pbp", ".zip", ".7z", ".rar", ".pkg", ".rap",
               ".bin", ".cue"}
LISTING_SKIP_SUFFIX = (".part",)
CHUNK = 256 * 1024


def title_from_filename(name: str) -> str:
    """'God_of_War-Chains [USA][v1.05].iso' -> 'God of War Chains'."""
    s = os.path.splitext(os.path.basename(name))[0]
    s = re.sub(r"\[[^]]*\]|\([^)]*\)", " ", s)
    s = re.sub(r"\b(PSP|PS3|USA|EUR|JPN|DISC\s*\d+)\b", " ", s, flags=re.I)
    s = re.sub(r"[_\.]+", " ", s)
    return re.sub(r"\s+", " ", s).strip() or name


def catalog_read(path: str) -> dict:
    """The catalog file as a dict, whatever shape it is on disk."""
    try:
        with open(path, encoding="utf8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return {}
    if isinstance(data, list):
        return {"items": data}
    return data if isinstance(data, dict) else {}


def catalog_load_items(path: str) -> list:
    """Read a catalog regardless of which top-level key it uses."""
    data = catalog_read(path)
    for key in ("items", "entries", "games"):
        if isinstance(data.get(key), list):
            return data[key]
    return []


def catalog_save_items(path: str, items: list) -> None:
    """Write items back, keeping any other top-level keys already in the file.

    ``generate_catalog.py`` regenerates this file from disk on its own timer,
    so whatever is written here is a convenience for the GUI between cycles —
    but it must not turn into ``{"games": ...}`` while every reader is looking
    for ``items``, so the existing shape is preserved.
    """
    data = catalog_read(path)
    key = next((k for k in ("items", "entries", "games")
                if isinstance(data.get(k), list)), "items")
    data[key] = items
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf8") as fh:
        json.dump(data, fh, indent=2)
    os.replace(tmp, path)


def sidecar_sha256(fs_path: str) -> str:
    """Digest from a '<name>.sha256' sidecar, or '' when there is none."""
    try:
        with open(fs_path + ".sha256", encoding="ascii") as fh:
            token = fh.read().strip().split()[0].lower()
    except (OSError, IndexError):
        return ""
    return token if len(token) == 64 else ""


def make_handler(games_dir: str):
    games_dir = os.path.realpath(games_dir)
    catalog_path = os.path.join(games_dir, "catalog.json")
    ui_dir = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui"))

    class PSLinkerHandler(BaseHTTPRequestHandler):
        server_version = "PSLinker/1.2"

        # ---- responses -------------------------------------------------

        def send_json(self, code: int, payload) -> None:
            body = json.dumps(payload).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def send_tree(self, base: str, rel_path: str) -> None:
            """Serve a file (with Range support) or a directory listing from
            under *base*. 404 when the path is outside base or missing."""
            base = os.path.realpath(base)
            fs_path = os.path.realpath(os.path.join(base, rel_path.lstrip("/")))
            if not (fs_path == base or fs_path.startswith(base + os.sep)):
                self.send_error(403)
                return
            if os.path.isdir(fs_path):
                self.list_directory(fs_path, base)
                return
            if not os.path.isfile(fs_path):
                self.send_error(404)
                return
            self.send_file(fs_path)

        def send_file(self, fs_path: str) -> None:
            size = os.path.getsize(fs_path)
            start, end = 0, size - 1
            partial = False

            header = self.headers.get("Range")
            if header:
                m = RANGE_RE.match(header.strip())
                # Anything we do not understand (multi-range, unknown units)
                # gets the full body, which is valid per RFC 7233.
                if m and size > 0:
                    if m.group(1):
                        start = int(m.group(1))
                        end = int(m.group(2)) if m.group(2) else size - 1
                    elif m.group(2):                 # suffix: last N bytes
                        start = max(0, size - int(m.group(2)))
                    if start >= size or end < start:
                        self.send_error(416, "Requested Range Not Satisfiable")
                        return
                    end = min(end, size - 1)
                    partial = True

            if partial:
                self.send_response(206)
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            else:
                self.send_response(200)

            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type",
                             mimetypes.guess_type(fs_path)[0]
                             or "application/octet-stream")
            self.send_header("Content-Length", str(end - start + 1))
            digest = sidecar_sha256(fs_path)
            if digest:
                self.send_header("X-SHA256", digest)
            self.end_headers()

            if self.command == "HEAD":
                return
            # Streamed, not f.read(size): these are 10 GB ISOs.
            with open(fs_path, "rb") as fh:
                fh.seek(start)
                remaining = end - start + 1
                while remaining > 0:
                    chunk = fh.read(min(CHUNK, remaining))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)

        def list_directory(self, dir_path: str, base: str) -> None:
            entries = sorted(os.listdir(dir_path))
            rel = os.path.relpath(dir_path, base).replace(os.sep, "/")
            display = "Library root" if rel == "." else rel
            rows = []
            if rel != ".":
                rows.append('<li><a href="../">../</a></li>')
            for name in entries:
                if name.startswith(".") or name.endswith(LISTING_SKIP_SUFFIX):
                    continue
                if name == "catalog.json":
                    continue          # metadata, not a library entry
                suffix = "/" if os.path.isdir(os.path.join(dir_path, name)) else ""
                rows.append(f'<li><a href="{html.escape(name)}{suffix}">'
                            f'{html.escape(name)}{suffix}</a></li>')
            body = (f'<!DOCTYPE html><html><head><meta charset="utf-8">'
                    f'<title>{html.escape(display)}</title></head><body>'
                    f'<h1>PS Linker &mdash; {html.escape(display)}</h1><hr><ul>'
                    + "".join(rows) + "</ul><hr></body></html>").encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        # ---- routing ---------------------------------------------------

        def do_GET(self) -> None:
            path = unquote(urlparse(self.path).path)
            if path == "/api/catalog":
                raw = catalog_read(catalog_path)
                payload = {"items": catalog_load_items(catalog_path)}
                # The GUI builds extra menu tabs from a declared category list
                # when the catalog supplies one (generate_catalog.py does not).
                if isinstance(raw.get("categories"), list):
                    payload["categories"] = raw["categories"]
                self.send_json(200, payload)
                return
            # The web GUI lives under /gui/ so that "/" stays the library
            # listing: that is what the PSP/PS3 apps request first, and what
            # the README tells people to open for downloads.
            if path in ("/gui", "/gui/"):
                self.send_tree(ui_dir, "index.html")
                return
            if path.startswith("/gui/"):
                self.send_tree(ui_dir, path[len("/gui/"):])
                return
            if path.startswith("/ui/"):        # older alias for the assets
                self.send_tree(ui_dir, path[len("/ui/"):])
                return
            self.send_tree(games_dir, path)

        def do_HEAD(self) -> None:
            self.do_GET()

        def do_POST(self) -> None:
            if urlparse(self.path).path != "/api/upload":
                self.send_error(404)
                return
            ctype = self.headers.get("Content-Type", "")
            boundary = ctype.split("boundary=", 1)[-1].strip('"')
            if not boundary:
                self.send_json(400, {"error": "missing multipart boundary"})
                return
            length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(length)

            platform, fields = "psp", []
            for part in body.split(("--" + boundary).encode()):
                head, _, content = part.strip(b"\r\n-").partition(b"\r\n\r\n")
                text = head.decode(errors="ignore")
                name = re.search(r'name="([^"]+)"', text)
                if not name:
                    continue
                if name.group(1) == "platform":
                    platform = content.decode(errors="ignore").strip().lower()
                else:
                    filename = re.search(r'filename="([^"]*)"', text)
                    if filename and filename.group(1):
                        fields.append((filename.group(1), content))
            platform = platform if platform in ("psp", "ps3") else "psp"

            items = catalog_load_items(catalog_path)
            by_name = {it.get("name") or it.get("path"): it for it in items}
            uploaded = []
            for raw, content in fields:
                safe = os.path.basename(unquote(raw))
                if (not safe or BAD_NAME_RE.search(safe)
                        or os.path.splitext(safe)[1].lower() not in ALLOWED_EXT):
                    self.send_json(400, {"error": f"invalid filename: {safe!r}"})
                    return
                tmp = os.path.join(games_dir, safe + ".part")
                with open(tmp, "wb") as fh:
                    fh.write(content)
                os.replace(tmp, os.path.join(games_dir, safe))   # atomic
                entry = {"name": safe, "title": title_from_filename(safe),
                         "platform": platform, "path": safe,
                         "url": "/" + safe, "size": len(content)}
                by_name[safe] = {**by_name.get(safe, {}), **entry}
                uploaded.append(entry)

            catalog_save_items(catalog_path, list(by_name.values()))
            self.send_json(201, {"uploaded": uploaded,
                                 "items": list(by_name.values())})

        def log_message(self, fmt, *args) -> None:
            print("[http] %s - %s" % (self.address_string(), fmt % args))

    return PSLinkerHandler


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    games_dir = os.path.realpath(sys.argv[1])
    os.makedirs(games_dir, exist_ok=True)
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
    httpd = ThreadingHTTPServer(("0.0.0.0", port), make_handler(games_dir))
    print(f"[http] serving {games_dir} on :{port} "
          f"(listings + Range/resume, GUI at / and /api/*)")
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    sys.exit(main())
