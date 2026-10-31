#!/usr/bin/env python3
"""PS Linker HTTP share — games library for the PSP/PS3 apps, plus the web GUI.

One port serves three things, in this routing order:

1. ``/api/*`` — the web GUI's JSON endpoints (``GET /api/catalog``,
   ``POST /api/upload``, ``POST /api/upload/file``, the library
   management set ``DELETE /api/item``, ``POST /api/item/rename``,
   ``POST /api/item/move``, ``PUT /api/item/meta``, and the cover trio
   ``POST /api/item/cover`` (raw image body), ``…/cover/extract``,
   ``…/cover/clear``), and the Store endpoints
   ``GET/POST/DELETE /api/store/queue``, ``GET /api/store/status``,
   ``POST /api/store/run``). Both uploads record the
   entry in the catalog and leave a ``<name>.meta.json`` sidecar, so the
   platform and prettified title survive the Store's catalog regenerations.
2. ``/gui/`` — the web GUI, with its assets at ``/gui/<asset>`` (``/ui/*`` is
    kept as an alias). It sits under a prefix on purpose: ``/`` has to stay the
    library listing, because that is the first thing the NASDL apps request.
3. everything else — the games library, which is what the NASDL apps talk to:
   directory listings in the same simple HTML the stock ``http.server`` emits
   (both apps parse ``href`` entries, trailing ``/`` meaning a directory), and
   files with ``Range``/``206`` support so an interrupted download resumes from
   its partial file instead of restarting. Anchors additionally carry the
   entry's ``data-size`` (files), ``data-cat`` (catalog category: top-level
   folder, root files ``Uncategorized``) and ``data-platform`` (the catalog's
   "PSP"/"PS3" label, so each app can hide the other console's files)
   *after* the quoted href — invisible
   to the apps' ``href="`` scan and to foreign parsers, and the apps render
   the old plain rows whenever a share omits them.

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
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

try:                              # works as a script and as an import
    import cover_art
except ImportError:               # same folder not on sys.path
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import cover_art

RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")
# Characters that must never appear in an uploaded filename (basename is
# already stripped, this is the belt to that braces).
BAD_NAME_RE = re.compile(r"[\x00-\x1f\x7f/\\]")
ALLOWED_EXT = {".iso", ".cso", ".pbp", ".zip", ".7z", ".rar", ".pkg", ".rap",
               ".bin", ".cue"}
# .part = interrupted download; .meta.json/.sha256 = catalog metadata
# sidecars (catalog.json itself is skipped separately). Like the catalog,
# they are metadata, not library entries — listing them would put a
# sidecar on screen even where the console filter hides its file.
LISTING_SKIP_SUFFIX = (".part", ".meta.json", ".sha256")
CHUNK = 256 * 1024


def title_from_filename(name: str) -> str:
    """'God_of_War-Chains [USA][v1.05].iso' -> 'God of War Chains'."""
    s = os.path.splitext(os.path.basename(name))[0]
    s = re.sub(r"\[[^]]*\]|\([^)]*\)", " ", s)
    s = re.sub(r"\b(PSP|PS3|USA|EUR|JPN|DISC\s*\d+)\b", " ", s, flags=re.I)
    s = re.sub(r"[_\.]+", " ", s)
    return re.sub(r"\s+", " ", s).strip() or name


def parse_query(q: str) -> dict:
    """Query parameters, percent-decoded but *not* '+'-decoded: game
    filenames contain literal '+', which parse_qs would turn into spaces."""
    out = {}
    for token in q.split("&"):
        if not token:
            continue
        key, _, value = token.partition("=")
        out[unquote(key)] = unquote(value)
    return out


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


# url -> platform label, read from the generated catalog and kept until the
# file changes (stat signature). Built lazily per listing; (key, map) pair.
_CATALOG_PLATFORMS: tuple = ()


def catalog_platforms(path: str) -> dict:
    """{catalog url: platform} for entries that carry a platform label.

    Directory listings use this to send each file a ``data-platform``
    attribute, so the PSP/PS3 apps can hide files the other console owns.
    Anything unreadable yields an empty map: listings then omit the
    attribute and the apps skip filtering — the foreign-share fallback.
    """
    global _CATALOG_PLATFORMS
    try:
        st = os.stat(path)
        key = (path, st.st_mtime_ns, st.st_size)
    except OSError:
        return {}
    if _CATALOG_PLATFORMS and _CATALOG_PLATFORMS[0] == key:
        return _CATALOG_PLATFORMS[1]
    platforms = {}
    for item in catalog_load_items(path):
        if not isinstance(item, dict):
            continue
        url, plat = item.get("url"), item.get("platform")
        if isinstance(url, str) and url and isinstance(plat, str) and plat:
            platforms[url] = plat
    _CATALOG_PLATFORMS = (key, platforms)
    return platforms


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


def catalog_upsert(catalog_path: str, entry: dict, drop=()) -> list:
    """Merge one entry into the catalog list by its library path, and save.

    The key is ``url`` first (then ``path``/``name``/``id``): every shape a
    catalog takes — ``generate_catalog.py``'s entries and upload entries —
    carries a ``url``, so one uploaded file merges into its generated entry
    instead of sitting beside it. Keying on fields only uploads have used to
    collapse every generated entry onto one ``None`` key, dropping the
    library from the catalog until the next regeneration.

    ``drop`` names fields the merged entry must NOT have: a merge keeps keys
    the incoming entry is silent about, so a field cleared to null (meta
    edit) disappears only when it is dropped explicitly.
    """
    items = catalog_load_items(catalog_path)
    key = _entry_key(entry)
    merged, replaced = [], False
    for it in items:
        if not replaced and _entry_key(it) == key:
            new = {**it, **entry}
            for field in drop:
                new.pop(field, None)
            merged.append(new)
            replaced = True
        else:
            merged.append(it)
    if not replaced:
        merged.append({k: v for k, v in entry.items() if k not in drop})
    catalog_save_items(catalog_path, merged)
    return merged


def _entry_key(it: dict):
    return it.get("url") or it.get("path") or it.get("name") or it.get("id")


def catalog_remove(catalog_path: str, key) -> list:
    """Drop the entry with this key (if any), save, and return the items."""
    items = [it for it in catalog_load_items(catalog_path)
             if _entry_key(it) != key]
    catalog_save_items(catalog_path, items)
    return items


def library_target(games_dir: str, url: str):
    """A catalog-style url ('/PSP/game.iso') resolved inside the library,
    or None for anything that escapes it (percent-decoded, then realpath,
    then prefix-checked — the same discipline as the file routes)."""
    if not url or not url.startswith("/"):
        return None
    base = os.path.realpath(games_dir)
    fs = os.path.realpath(os.path.join(base, url.lstrip("/")))
    if not (fs == base or fs.startswith(base + os.sep)):
        return None
    return fs


def managed_file(fs_path: str) -> bool:
    """Whether this resolved path is a game the management endpoints may
    touch: a regular file that is not the catalog, a hidden file, an
    in-flight .part, or a sidecar (sidecars ride along with their game;
    they are never managed directly)."""
    name = os.path.basename(fs_path)
    return (os.path.isfile(fs_path) and name != "catalog.json"
            and not name.startswith(".")
            and not name.endswith((".part", ".meta.json", ".sha256")))


def upload_name_ok(safe: str) -> bool:
    """Both upload endpoints gate on this: a plain visible file name with a
    known game extension. Dot-prefixed names and ``.part`` names would slip
    through the listing/catalog skips invisibly or collide with the resume
    files, so they are rejected at the gate."""
    return (bool(safe) and not safe.startswith(".")
            and not BAD_NAME_RE.search(safe)
            and os.path.splitext(safe)[1].lower() in ALLOWED_EXT)


def upload_entry(rel: str, size: int, platform: str) -> dict:
    """The catalog entry for an uploaded file, in the shape
    ``generate_catalog.py`` produces (plus ``platform``): ``id`` underscores
    the library-relative path, ``category`` is its top-level folder
    (``Uncategorized`` at the root), so uploads and regenerated entries
    merge on the same fields."""
    name = os.path.basename(rel)
    head = rel.split("/")[0] if "/" in rel else ""
    return {"id": rel.replace("/", "_"), "name": name,
            "title": title_from_filename(name),
            "category": head or "Uncategorized", "platform": platform,
            "path": rel, "url": "/" + rel, "size": size}


def write_meta_sidecar(games_dir: str, rel: str, platform: str,
                      title: str) -> None:
    """Merge platform/title into the game's ``<name>.meta.json`` sidecar.
    ``generate_catalog.py`` reads sidecars and passes those fields through,
    which is the only way upload metadata survives a regeneration; an
    existing sidecar's own keys (description, version, rap) are kept."""
    path = os.path.join(games_dir, rel + ".meta.json")
    meta = {}
    try:
        with open(path, encoding="utf8") as fh:
            loaded = json.load(fh)
        meta = loaded if isinstance(loaded, dict) else {}
    except (OSError, ValueError):
        meta = {}
    meta["platform"] = platform
    meta["title"] = title
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf8") as fh:
        json.dump(meta, fh, indent=2)
    os.replace(tmp, path)


def record_upload(games_dir: str, entry: dict) -> list:
    """Sidecar + catalog entry for a file already written into the library;
    returns the merged catalog items. License (.rap) files carry no sidecar:
    their whole name is their metadata, and attachment is recorded on the
    *game's* sidecar, not on their own."""
    rel = entry["path"]
    if os.path.splitext(rel)[1].lower() != ".rap":
        write_meta_sidecar(games_dir, rel, entry["platform"], entry["title"])
    return catalog_upsert(os.path.join(games_dir, "catalog.json"), entry)


def repoint_rap_references(folder: str, old_name: str, new_name: str) -> None:
    """After a .rap is renamed, keep the sidecars of games in the same
    folder pointing at its new name — an attachment must not dangle."""
    try:
        names = os.listdir(folder)
    except OSError:
        return
    for fname in names:
        if not fname.endswith(".meta.json"):
            continue
        path = os.path.join(folder, fname)
        try:
            with open(path, encoding="utf8") as fh:
                meta = json.load(fh)
        except (OSError, ValueError):
            continue
        if isinstance(meta, dict) and meta.get("rap") == old_name:
            meta["rap"] = new_name
            tmp = path + ".part"
            try:
                with open(tmp, "w", encoding="utf8") as fh:
                    json.dump(meta, fh, indent=2)
                os.replace(tmp, path)
            except OSError:
                pass


def sidecar_sha256(fs_path: str) -> str:
    """Digest from a '<name>.sha256' sidecar, or '' when there is none."""
    try:
        with open(fs_path + ".sha256", encoding="ascii") as fh:
            token = fh.read().strip().split()[0].lower()
    except (OSError, IndexError):
        return ""
    return token if len(token) == 64 else ""


# ---- cover sidecars (the '.<stem>.icon0.<ext>' family cover_art.py keeps) --

COVER_EXTS = (".png", ".jpg", ".jpeg", ".bmp")   # what find_icon accepts
MAX_COVER_BYTES = 8 * 1024 * 1024
COVER_MAGIC = ((b"\x89PNG\r\n\x1a\n", ".png"), (b"\xff\xd8\xff", ".jpg"))


def cover_sidecar_paths(fs_path: str) -> list:
    """Existing hidden cover sidecars belonging to a game file."""
    folder, base = os.path.split(fs_path)
    stem = os.path.splitext(base)[0]
    prefix = "." + stem + ".icon0."
    try:
        names = os.listdir(folder)
    except OSError:
        return []
    return [os.path.join(folder, n) for n in names
            if n.startswith(prefix)
            and os.path.splitext(n)[1].lower() in COVER_EXTS]


def rename_cover_sidecars(fs_path: str, new_name: str) -> str:
    """After a rename to *new_name* (game file already moved), re-derive the
    hidden sidecar names from the new stem. Returns the new sidecar's path,
    or '' when there is no cover or the stem did not change (the sidecar
    name is still correct). A generic folder image (a PS3 disc's ICON0.PNG)
    is not stem-bound and simply stays where it is."""
    old_stem = os.path.splitext(os.path.basename(fs_path))[0]
    new_stem = os.path.splitext(new_name)[0]
    if old_stem == new_stem:
        return ""
    folder = os.path.dirname(fs_path)
    moved = ""
    for cov in cover_sidecar_paths(fs_path):
        ext = os.path.splitext(cov)[1]
        newp = os.path.join(folder, "." + new_stem + ".icon0" + ext)
        if os.path.exists(newp):
            continue
        try:
            os.rename(cov, newp)
        except OSError:
            continue
        moved = newp
    return moved


def rap_referents(folder: str, rap_name: str, exclude=None) -> list:
    """File names in *folder* whose sidecar pairs them with *rap_name*
    (*exclude* names the game being moved, so it does not block itself)."""
    try:
        names = os.listdir(folder)
    except OSError:
        return []
    out = []
    for fname in names:
        if not fname.endswith(".meta.json"):
            continue
        game = fname[:-len(".meta.json")]
        if exclude is not None and game == exclude:
            continue
        try:
            with open(os.path.join(folder, fname), encoding="utf8") as fh:
                meta = json.load(fh)
        except (OSError, ValueError):
            continue
        if isinstance(meta, dict) and meta.get("rap") == rap_name:
            out.append(game)
    return out


# ---- Store queue helpers (endpoints below; the fetcher claims the file) --

def store_category_ok(cat) -> bool:
    """A queue category is exactly one visible path component — the same
    rule store_fetch.py enforces before it mkdir's the destination."""
    return (isinstance(cat, str) and 0 < len(cat) <= 64
            and cat == os.path.basename(cat)
            and not cat.startswith(".") and cat not in (".", "..")
            and not BAD_NAME_RE.search(cat))


def store_url_ok(url) -> bool:
    # BAD_NAME_RE is the *filename* guard (slashes legal here); a URL only
    # needs the scheme, a sane length, and no control characters.
    return (isinstance(url, str) and len(url) <= 2000
            and url.startswith(("http://", "https://"))
            and not re.search(r"[\x00-\x1f\x7f]", url))


def store_queue_read(path: str) -> list:
    """The pending queue as a list; anything unreadable reads as empty
    (while a claim lives, the pending file legitimately may not exist)."""
    try:
        with open(path, encoding="utf8") as fh:
            data = json.load(fh)
        return data if isinstance(data, list) else []
    except (OSError, ValueError):
        return []


def store_queue_write(path: str, items: list) -> None:
    """Atomic rewrite; the fetcher claims the queue with its own atomic
    rename, so a half-written queue can never be claimed."""
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf8") as fh:
        json.dump(items, fh, indent=2)
    os.replace(tmp, path)


def make_handler(games_dir: str):
    games_dir = os.path.realpath(games_dir)
    catalog_path = os.path.join(games_dir, "catalog.json")
    ui_dir = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "ui"))
    # Store queue trio: the GUI reads/writes the pending file under this
    # lock (ThreadingHTTPServer), while the fetcher claims it with an
    # atomic rename that no in-process lock could match anyway.
    queue_path = os.path.join(games_dir, ".store-queue.json")
    claim_path = queue_path + ".processing"
    status_path = os.path.join(games_dir, ".store-status.json")
    fetcher_py = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "store_fetch.py")
    queue_lock = threading.Lock()

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
            # Catalog model, same as generate_catalog.py: an entry's category
            # is the top-level folder it lives under (a root folder is its own
            # category; a root file is "Uncategorized"). Anchors carry it, a
            # file's exact size, and the catalog's platform label ("PSP",
            # "PS3"), as data-* attributes so the device browsers can draw
            # the store's row chrome and hide files the other console owns
            # (each app filters with store_platform_ok). Compatibility is by
            # layout: the parsers scan href="..." and stop at the quote, so
            # attributes after it change nothing for them, and foreign shares
            # (python -m http.server, nginx autoindex) simply never send the
            # attributes — the apps then render exactly the old rows,
            # unfiltered.
            top = "" if rel == "." else rel.split("/")[0]
            platforms = catalog_platforms(catalog_path)
            rows = []
            if rel != ".":
                rows.append('<li><a href="../">../</a></li>')
            for name in entries:
                if name.startswith(".") or name.endswith(LISTING_SKIP_SUFFIX):
                    continue
                if name == "catalog.json":
                    continue          # metadata, not a library entry
                full = os.path.join(dir_path, name)
                is_dir = os.path.isdir(full)
                attrs = ""
                if is_dir:
                    cat = name if rel == "." else top
                else:
                    try:
                        attrs += (f' data-size='
                                  f'"{os.path.getsize(full)}"')
                    except OSError:
                        pass          # vanished mid-listing: name only
                    cat = top or "Uncategorized"
                    # Console the catalog says this file belongs to, if it
                    # labelled one; folders never carry it (they are open to
                    # both apps).
                    url = name if rel == "." else f"{rel}/{name}"
                    plat = platforms.get("/" + url)
                    if plat:
                        attrs += (f' data-platform='
                                  f'"{html.escape(plat, quote=True)}"')
                if cat:
                    attrs += f' data-cat="{html.escape(cat, quote=True)}"'
                suffix = "/" if is_dir else ""
                rows.append(f'<li><a href="{html.escape(name)}{suffix}"'
                            f'{attrs}>{html.escape(name)}{suffix}</a></li>')
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
            if path == "/api/store/status":
                self.handle_store_status()
                return
            if path == "/api/store/queue":
                self.handle_store_queue_get()
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
            path = urlparse(self.path).path
            if path == "/api/store/queue":
                self.handle_store_queue_add()
                return
            if path == "/api/store/run":
                self.handle_store_run()
                return
            if path == "/api/upload/file":
                self.handle_raw_upload()
                return
            if path == "/api/item/rename":
                self.handle_item_rename()
                return
            if path == "/api/item/move":
                self.handle_item_move()
                return
            if path == "/api/item/cover":
                self.handle_cover_upload()
                return
            if path == "/api/item/cover/extract":
                self.handle_cover_extract()
                return
            if path == "/api/item/cover/clear":
                self.handle_cover_clear()
                return
            if path != "/api/upload":
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

            uploaded = []
            for raw, content in fields:
                safe = os.path.basename(unquote(raw))
                if not upload_name_ok(safe):
                    self.send_json(400, {"error": f"invalid filename: {safe!r}"})
                    return
                tmp = os.path.join(games_dir, safe + ".part")
                with open(tmp, "wb") as fh:
                    fh.write(content)
                os.replace(tmp, os.path.join(games_dir, safe))   # atomic
                entry = upload_entry(safe, len(content), platform)
                record_upload(games_dir, entry)
                uploaded.append(entry)

            self.send_json(201, {"uploaded": uploaded,
                                 "items": catalog_load_items(
                                     os.path.join(games_dir, "catalog.json"))})

        def handle_raw_upload(self) -> None:
            """POST /api/upload/file?name=<urlencoded>&platform=<psp|ps3>

            One file per request, body is the raw bytes, streamed to
            ``<name>.part`` and renamed in atomically — the web GUI's Add
            Games panel uses this so a multi-GB ISO never sits in memory the
            way the multipart endpoint's whole-body ``read()`` would leave
            it. Chunked bodies are refused: BaseHTTPRequestHandler cannot
            decode them and there is no Content-Length to trust."""
            params = parse_query(urlparse(self.path).query)
            raw_name = params.get("name", "")
            safe = os.path.basename(raw_name)   # params were unquoted once
            if not upload_name_ok(safe):
                # The body is still unread; keep-alive would parse it as the
                # next request, so this connection ends with the response.
                self.close_connection = True
                self.send_json(400, {"error": f"invalid filename: {raw_name!r}"})
                return
            platform = params.get("platform", "").strip().lower()
            platform = platform if platform in ("psp", "ps3") else "psp"
            dest_dir = games_dir
            dir_param = params.get("dir", "").strip()
            if dir_param:
                # Optional destination folder inside the library — used to
                # drop a license .rap beside its game. A missing or escaping
                # dir is refused before the body is read.
                if not dir_param.startswith("/"):
                    dir_param = "/" + dir_param
                resolved = library_target(games_dir, dir_param)
                if resolved is None or not os.path.isdir(resolved):
                    self.close_connection = True
                    self.send_json(400, {"error": f"unknown dir: {dir_param!r}"})
                    return
                dest_dir = resolved
            if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
                self.close_connection = True
                self.send_json(411, {"error": "chunked upload unsupported"})
                return
            length = self.headers.get("Content-Length", "")
            if not length.isdigit():
                self.close_connection = True
                self.send_json(411, {"error": "Content-Length required"})
                return
            length = int(length)

            tmp = os.path.join(dest_dir, safe + ".part")
            try:
                remaining, received = length, 0
                with open(tmp, "wb") as fh:
                    while remaining > 0:
                        chunk = self.rfile.read(min(CHUNK, remaining))
                        if not chunk:
                            break
                        fh.write(chunk)
                        received += len(chunk)
                        remaining -= len(chunk)
                if remaining:
                    raise OSError(f"client sent {received} of {length} bytes")
                os.replace(tmp, os.path.join(dest_dir, safe))   # atomic
                rel = os.path.relpath(os.path.join(dest_dir, safe),
                                       games_dir).replace(os.sep, "/")
                entry = upload_entry(rel, length, platform)
                items = record_upload(games_dir, entry)
            except OSError as exc:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                self.send_json(500, {"error": str(exc)})
                return
            self.send_json(201, {"uploaded": entry, "items": items})

        # ---- Store queue (web GUI) ---------------------------------------
        #
        # The queue file itself is the contract with store_fetch.py: this
        # process only ever reads or atomically rewrites the *pending* file;
        # a running fetcher holds the claim (``.processing``), so an item
        # added mid-run waits for the next pass, and deleting a claimed URL
        # is refused rather than raced.

        def _claim_has(self, url) -> bool:
            return (os.path.exists(claim_path)
                    and any(isinstance(it, dict) and it.get("url") == url
                            for it in store_queue_read(claim_path)))

        def handle_store_status(self) -> None:
            try:
                with open(status_path, encoding="utf8") as fh:
                    status = json.load(fh)
                if not isinstance(status, dict):
                    status = {}
            except (OSError, ValueError):
                status = {}
            hist = status.get("history")
            status["history"] = hist if isinstance(hist, list) else []
            processing = os.path.exists(claim_path)
            # A crashed run leaves "downloading" behind with no claim; the
            # claim file is the only proof a download is really live.
            if not processing and status.get("phase") != "idle":
                status["phase"], status["current"] = "idle", None
            status.setdefault("phase", "idle")
            status["processing"] = processing
            status["pending"] = len(store_queue_read(queue_path))
            self.send_json(200, status)

        def handle_store_queue_get(self) -> None:
            running = (store_queue_read(claim_path)
                       if os.path.exists(claim_path) else None)
            self.send_json(200, {"queue": store_queue_read(queue_path),
                                 "running": running})

        def handle_store_queue_add(self) -> None:
            data, err = self.read_json()
            if err:
                self.send_json(err[0], {"error": err[1]})
                return
            url = str(data.get("url", "")).strip()
            if not store_url_ok(url):
                self.send_json(400, {"error": "url must be http(s)"})
                return
            title = str(data.get("title") or "").strip()[:200]
            if not title:
                base = os.path.basename(url.split("?")[0])
                title = os.path.splitext(base)[0] or base or url[:80]
            category = str(data.get("category") or "Homebrew").strip()
            if not store_category_ok(category):
                self.send_json(400, {"error": "category must be one visible"
                                              " folder name, no path"})
                return
            with queue_lock:
                if self._claim_has(url):
                    self.send_json(409, {"error": "that URL is downloading"
                                                  " right now"})
                    return
                q = store_queue_read(queue_path)
                if any(isinstance(it, dict) and it.get("url") == url
                       for it in q):
                    self.send_json(409, {"error": "already queued"})
                    return
                q.append({"title": title, "category": category, "url": url,
                          "added": time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                                 time.gmtime())})
                try:
                    store_queue_write(queue_path, q)
                except OSError as exc:
                    self.send_json(500, {"error": str(exc)})
                    return
            self.send_json(201, {"queue": q})

        def handle_store_queue_del(self, url) -> None:
            with queue_lock:
                if self._claim_has(url):
                    self.send_json(409, {"error": "that URL is downloading"
                                                  " right now"})
                    return
                q = store_queue_read(queue_path)
                rest = [it for it in q
                        if not (isinstance(it, dict) and it.get("url") == url)]
                if len(rest) == len(q):
                    self.send_json(404, {"error": "not in the queue"})
                    return
                try:
                    store_queue_write(queue_path, rest)
                except OSError as exc:
                    self.send_json(500, {"error": str(exc)})
                    return
            self.send_json(200, {"queue": rest})

        def handle_store_run(self) -> None:
            """Spawn the fetcher detached, so Download now does not wait on
            the entrypoint's STORE_INTERVAL. Its stdout/stderr stay attached
            to the server's, i.e. the container log. Two overlapping runs
            are harmless: only one rename can claim the queue."""
            if os.path.exists(claim_path):
                self.send_json(409, {"error": "a download is already running"})
                return
            with queue_lock:
                if not store_queue_read(queue_path):
                    self.send_json(400, {"error": "queue is empty"})
                    return
                try:
                    subprocess.Popen([sys.executable, fetcher_py,
                                      games_dir, queue_path],
                                     start_new_session=True)
                except OSError as exc:
                    self.send_json(500, {"error": str(exc)})
                    return
            self.send_json(202, {"started": True})

        # ---- library management (web GUI) ---------------------------------

        def read_json(self):
            """Parse a small JSON-object request body.
            Returns (data, None) or (None, (code, message)). The body cannot
            be consumed without a trustworthy Content-Length, so that path
            ends the connection."""
            length = self.headers.get("Content-Length", "")
            if not length.isdigit() or int(length) > 65536:
                self.close_connection = True
                return None, (400, "JSON body with Content-Length required")
            try:
                data = json.loads(self.rfile.read(int(length)))
            except ValueError:
                return None, (400, "invalid JSON")
            if not isinstance(data, dict):
                return None, (400, "JSON object required")
            return data, None

        def resolve_item(self, url):
            """Resolved path for a management request's url, or None. A
            directory answers 405 itself (the caller cannot ask for one by
            accident); anything else unmanaged answers 404 without revealing
            whether it exists."""
            fs_path = library_target(games_dir, str(url or ""))
            if fs_path is None:
                self.send_json(404, {"error": "not in the library"})
                return None
            if os.path.isdir(fs_path):
                self.send_json(405, {"error": "directories are not managed here"})
                return None
            if not managed_file(fs_path):
                self.send_json(404, {"error": "not a managed library file"})
                return None
            return fs_path

        def do_DELETE(self) -> None:
            path = urlparse(self.path).path
            if path == "/api/store/queue":
                url = parse_query(urlparse(self.path).query).get("url", "")
                self.handle_store_queue_del(url)
                return
            if path != "/api/item":
                self.send_error(404)
                return
            url = parse_query(urlparse(self.path).query).get("url", "")
            fs_path = self.resolve_item(url)
            if fs_path is None:
                return
            rel = "/" + os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            try:
                os.remove(fs_path)
                for side in (fs_path + ".meta.json", fs_path + ".sha256"):
                    if os.path.isfile(side):
                        os.remove(side)
                for cov in cover_sidecar_paths(fs_path):
                    os.remove(cov)
            except OSError as exc:
                self.send_json(500, {"error": str(exc)})
                return
            self.send_json(200, {"deleted": rel,
                                 "items": catalog_remove(catalog_path, rel)})

        def handle_item_rename(self) -> None:
            data, err = self.read_json()
            if err:
                self.send_json(err[0], {"error": err[1]})
                return
            fs_path = self.resolve_item(data.get("url"))
            if fs_path is None:
                return
            name = str(data.get("name", ""))
            if not upload_name_ok(name):
                self.send_json(400, {"error": f"invalid name: {name!r}"})
                return
            target = os.path.join(os.path.dirname(fs_path), name)
            if os.path.exists(target):
                self.send_json(409, {"error": "name already in use"})
                return
            old_key = "/" + os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            try:
                os.rename(fs_path, target)
                for suffix in (".meta.json", ".sha256"):
                    if os.path.isfile(fs_path + suffix):
                        os.rename(fs_path + suffix, target + suffix)
                moved_cover = rename_cover_sidecars(fs_path, name)
            except OSError as exc:
                self.send_json(500, {"error": str(exc)})
                return
            new_rel = os.path.relpath(target, games_dir).replace(os.sep, "/")
            old_name = os.path.basename(fs_path)
            if old_name.lower().endswith(".rap"):
                # A renamed license keeps its games: rewrite the sidecars
                # and catalog entries in the same folder that point at it.
                repoint_rap_references(os.path.dirname(target), old_name, name)
                folder_rel = os.path.dirname(new_rel)
                items = catalog_load_items(catalog_path)
                touched = False
                for it in items:
                    u = str(it.get("url") or "")
                    if (u.startswith("/") and it.get("rap") == old_name
                            and os.path.dirname(u[1:]) == folder_rel):
                        it["rap"] = name
                        touched = True
                if touched:
                    catalog_save_items(catalog_path, items)
            old_entry = next((it for it in catalog_load_items(catalog_path)
                              if _entry_key(it) == old_key), {})
            catalog_remove(catalog_path, old_key)
            fresh = {"id": new_rel.replace("/", "_"), "name": name,
                     "path": new_rel, "url": "/" + new_rel}
            entry = {**{"title": title_from_filename(name)}, **old_entry, **fresh}
            # A prettified title only survives a regeneration through the
            # sidecar, so the renamed catalog title must agree with it.
            sidecar_title = str(self.read_sidecar(target).get("title") or "")
            entry["title"] = sidecar_title or title_from_filename(name)
            if moved_cover:
                entry["icon"] = "/" + os.path.relpath(moved_cover, games_dir) \
                    .replace(os.sep, "/")
            self.send_json(200, {"renamed": entry,
                                 "items": catalog_upsert(catalog_path, entry)})

        # ---- move + covers ------------------------------------------------
        #
        # Rename pins the file's folder, so moving a game to another
        # category needs its own verb; covers reuse the hidden
        # '.<stem>.icon0.<ext>' sidecar convention cover_art.py established
        # — manual art and extracted art are the same kind of file, so the
        # listing/catalog skips already cover both and no metadata model
        # grows.

        def entry_for(self, fs_path: str) -> dict:
            """Catalog entry for a library file: fresh path fields merged
            over any entry the catalog already carries under the same url
            (title/description/size ride along; a missing entry gets a
            complete fresh one)."""
            rel = os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            name = os.path.basename(fs_path)
            top = rel.split("/")[0] if "/" in rel else ""
            fresh = {"id": rel.replace("/", "_"), "name": name, "path": rel,
                     "url": "/" + rel, "category": top or "Uncategorized"}
            old = next((it for it in catalog_load_items(catalog_path)
                        if _entry_key(it) == fresh["url"]), {})
            entry = {**old, **fresh}
            entry.setdefault("title", title_from_filename(name))
            return entry

        def handle_item_move(self) -> None:
            """POST /api/item/move {url, category} — the game (and its
            sidecars) go to another top-level category folder. The category
            is the library's organizing axis, and a .rap must not be pulled
            out from under games that still reference it: the license
            travels only with its last user, otherwise the pairing clears
            rather than dangling across folders."""
            data, err = self.read_json()
            if err:
                self.send_json(err[0], {"error": err[1]})
                return
            fs_path = self.resolve_item(data.get("url"))
            if fs_path is None:
                return
            category = str(data.get("category", "")).strip()
            if not store_category_ok(category) or category == "catalog.json":
                self.send_json(400, {"error": f"invalid category: {category!r}"})
                return
            name = os.path.basename(fs_path)
            old_rel = os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            new_rel = category + "/" + name
            if new_rel == old_rel:
                self.send_json(400, {"error": "already in that category"})
                return
            target_dir = os.path.join(games_dir, category)
            target = os.path.join(target_dir, name)
            if os.path.exists(target):
                self.send_json(409, {"error": "that category holds the name"})
                return
            src_dir = os.path.dirname(fs_path)
            meta = self.read_sidecar(fs_path)
            rap_name = str(meta.get("rap") or "")
            rap_src = os.path.join(src_dir, rap_name) if rap_name else ""
            rap_follows = rap_orphans = False
            drop = []
            if name.lower().endswith(".rap"):
                if rap_referents(src_dir, name):
                    self.send_json(409, {
                        "error": f"{name} is attached to games there — "
                                 "move those games (the license travels)"})
                    return
            elif rap_name and os.path.isfile(rap_src):
                if rap_referents(src_dir, rap_name, exclude=name):
                    rap_orphans = True          # it stays for the others
                elif os.path.exists(os.path.join(target_dir, rap_name)):
                    self.send_json(409, {"error":
                        f"license {rap_name} already exists in the target"})
                    return
                else:
                    rap_follows = True
            try:
                os.makedirs(target_dir, exist_ok=True)
                os.rename(fs_path, target)
                for suffix in (".meta.json", ".sha256"):
                    if os.path.isfile(fs_path + suffix):
                        os.rename(fs_path + suffix, target + suffix)
                if rap_follows:
                    os.rename(rap_src, os.path.join(target_dir, rap_name))
                moved_cover = ""
                for cov in cover_sidecar_paths(fs_path):
                    newp = os.path.join(target_dir, os.path.basename(cov))
                    if not os.path.exists(newp):
                        os.rename(cov, newp)
                        moved_cover = newp
            except OSError as exc:
                self.send_json(500, {"error": str(exc)})
                return
            if rap_orphans:
                # The pairing cannot follow; say so on disk instead of
                # leaving a cross-folder pointer the meta endpoint would
                # refuse to make in the first place.
                side = target + ".meta.json"
                moved_meta = self.read_sidecar(target)
                if "rap" in moved_meta:
                    moved_meta.pop("rap")
                    drop.append("rap")
                    try:
                        if moved_meta:
                            tmp = side + ".part"
                            with open(tmp, "w", encoding="utf8") as fh:
                                json.dump(moved_meta, fh, indent=2)
                            os.replace(tmp, side)
                        else:
                            os.remove(side)
                    except OSError:
                        pass
            old_key = "/" + old_rel
            old_entry = next((it for it in catalog_load_items(catalog_path)
                              if _entry_key(it) == old_key), {})
            catalog_remove(catalog_path, old_key)
            fresh = {"id": new_rel.replace("/", "_"), "name": name,
                     "path": new_rel, "url": "/" + new_rel,
                     "category": category}
            entry = {**{"title": title_from_filename(name)}, **old_entry,
                     **fresh}
            entry["title"] = str(self.read_sidecar(target).get("title")
                                 or title_from_filename(name))
            if moved_cover:
                entry["icon"] = "/" + os.path.relpath(moved_cover, games_dir) \
                    .replace(os.sep, "/")
            elif entry.pop("icon", None) is not None:
                # Whatever picture the entry carried belonged to the old
                # folder's neighbourhood; the next regeneration re-decides.
                drop.append("icon")
            for field in drop:
                entry.pop(field, None)
            self.send_json(200, {"moved": entry,
                                 "items": catalog_upsert(catalog_path, entry,
                                                         tuple(drop))})

        def handle_cover_upload(self) -> None:
            """POST /api/item/cover?url=<library url>, body = raw PNG/JPEG
            bytes. Replaces the game's hidden cover sidecar; the whole body
            is small by construction (MAX_COVER_BYTES), so no streaming
            dance is needed, but a refusal before the read still ends the
            connection so its bytes are never parsed as a second request."""
            url = parse_query(urlparse(self.path).query).get("url", "")
            self.close_connection = True
            fs_path = self.resolve_item(url)
            if fs_path is None:
                return
            if os.path.basename(fs_path).lower().endswith(".rap"):
                self.send_json(400, {"error": "licenses do not get covers"})
                return
            if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
                self.send_json(411, {"error": "chunked upload unsupported"})
                return
            length = self.headers.get("Content-Length", "")
            if not length.isdigit():
                self.send_json(411, {"error": "Content-Length required"})
                return
            length = int(length)
            if length == 0 or length > MAX_COVER_BYTES:
                self.send_json(413, {"error": "cover must be 1..8 MiB"})
                return
            blob = self.rfile.read(length)
            ext = next((e for magic, e in COVER_MAGIC
                        if blob.startswith(magic)), "")
            if not ext:
                self.send_json(400, {"error": "not a PNG or JPEG"})
                return
            folder = os.path.dirname(fs_path)
            stem = os.path.splitext(os.path.basename(fs_path))[0]
            out = os.path.join(folder, "." + stem + ".icon0" + ext)
            tmp = out + ".part"
            try:
                with open(tmp, "wb") as fh:
                    fh.write(blob)
                os.replace(tmp, out)
                cover_art.prune_other_covers(out)   # one cover per game
            except OSError as exc:
                try:
                    os.remove(tmp)
                except OSError:
                    pass
                self.send_json(500, {"error": str(exc)})
                return
            entry = self.entry_for(fs_path)
            entry["icon"] = "/" + os.path.relpath(out, games_dir) \
                .replace(os.sep, "/")
            self.send_json(200, {"cover": entry["icon"],
                                 "items": catalog_upsert(catalog_path, entry)})

        def handle_cover_extract(self) -> None:
            """POST /api/item/cover/extract {url} — force a re-run of the
            embedded-cover extraction (ICON0 inside ISO/CSO/PBP). A failure
            leaves the current cover untouched; only success re-publishes."""
            data, err = self.read_json()
            if err:
                self.send_json(err[0], {"error": err[1]})
                return
            fs_path = self.resolve_item(data.get("url"))
            if fs_path is None:
                return
            rel = os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            if os.path.splitext(rel)[1].lower() not in (".iso", ".cso", ".pbp"):
                self.send_json(400, {
                    "error": "only ISO/CSO/PBP files embed cover art"})
                return
            icon = cover_art.ensure_cover(games_dir, rel, force=True)
            if not icon:
                self.send_json(409, {"error": "no ICON0 inside this file"})
                return
            entry = self.entry_for(fs_path)
            entry["icon"] = icon
            self.send_json(200, {"cover": icon,
                                 "items": catalog_upsert(catalog_path, entry)})

        def handle_cover_clear(self) -> None:
            """POST /api/item/cover/clear {url} — drop the hidden sidecars
            and the entry's icon; the tile goes back to its placeholder."""
            data, err = self.read_json()
            if err:
                self.send_json(err[0], {"error": err[1]})
                return
            fs_path = self.resolve_item(data.get("url"))
            if fs_path is None:
                return
            removed = 0
            for cov in cover_sidecar_paths(fs_path):
                try:
                    os.remove(cov)
                    removed += 1
                except OSError as exc:
                    self.send_json(500, {"error": str(exc)})
                    return
            rel = "/" + os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            items = catalog_load_items(catalog_path)
            if any(_entry_key(it) == rel for it in items):
                items = catalog_upsert(catalog_path, {"url": rel}, ("icon",))
            self.send_json(200, {"cleared": removed, "items": items})

        def do_PUT(self) -> None:
            if urlparse(self.path).path != "/api/item/meta":
                self.send_error(404)
                return
            self.handle_item_meta()

        def handle_item_meta(self) -> None:
            data, err = self.read_json()
            if err:
                self.send_json(err[0], {"error": err[1]})
                return
            fs_path = self.resolve_item(data.get("url"))
            if fs_path is None:
                return
            patch = {}
            for field in ("title", "version", "description", "platform"):
                if field not in data:
                    continue
                value = data[field]
                if value is None:
                    patch[field] = None
                    continue
                if not isinstance(value, str) or len(value) > 500:
                    self.send_json(400, {"error": f"bad {field}"})
                    return
                value = value.strip()
                if field == "platform":
                    value = value.lower()
                    if value and value not in ("psp", "ps3"):
                        self.send_json(400, {"error": "platform is psp or ps3"})
                        return
                patch[field] = value or None
            # The license link is special-cased: it must be a .rap that
            # actually sits in the game's own folder (co-location is the
            # whole point — webMAN-style consumers look next to the game).
            if "rap" in data:
                value = data["rap"]
                same_folder = os.path.join(os.path.dirname(fs_path),
                                           str(value or ""))
                if value is None or (isinstance(value, str) and not value.strip()):
                    patch["rap"] = None
                elif (isinstance(value, str) and len(value) <= 200
                        and value == os.path.basename(value)
                        and value.lower().endswith(".rap")
                        and os.path.isfile(same_folder)):
                    patch["rap"] = value
                else:
                    self.send_json(400, {
                        "error": "rap must be a .rap file in the same folder"})
                    return
            if not patch:
                self.send_json(400, {"error": "nothing to change"})
                return

            # Merge into the sidecar; an all-empty sidecar is deleted so the
            # disk stays honest about what the generator will re-read.
            meta = self.read_sidecar(fs_path)
            for field, value in patch.items():
                if value is None:
                    meta.pop(field, None)
                else:
                    meta[field] = value
            side_path = fs_path + ".meta.json"
            try:
                if meta:
                    tmp = side_path + ".part"
                    with open(tmp, "w", encoding="utf8") as fh:
                        json.dump(meta, fh, indent=2)
                    os.replace(tmp, side_path)
                elif os.path.isfile(side_path):
                    os.remove(side_path)
            except OSError as exc:
                self.send_json(500, {"error": str(exc)})
                return

            rel = "/" + os.path.relpath(fs_path, games_dir).replace(os.sep, "/")
            old_entry = next((it for it in catalog_load_items(catalog_path)
                              if _entry_key(it) == rel), {})
            base = os.path.basename(fs_path)
            entry = {"id": rel[1:].replace("/", "_"), "name": base,
                     "path": rel[1:], "url": rel,
                     "title": title_from_filename(base)}
            entry = {**entry, **old_entry}
            # A cleared field must leave the catalog entry too, or the GUI
            # would keep showing metadata the sidecar no longer has — the
            # merge keeps silent keys, so cleared fields go through ``drop``.
            cleared = tuple(k for k, v in patch.items() if v is None)
            entry.update({k: v for k, v in patch.items() if v is not None})
            self.send_json(200, {"meta": meta,
                                 "items": catalog_upsert(catalog_path, entry,
                                                         cleared)})

        @staticmethod
        def read_sidecar(fs_path: str) -> dict:
            try:
                with open(fs_path + ".meta.json", encoding="utf8") as fh:
                    loaded = json.load(fh)
                return loaded if isinstance(loaded, dict) else {}
            except (OSError, ValueError):
                return {}

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
