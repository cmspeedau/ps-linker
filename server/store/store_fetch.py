#!/usr/bin/env python3
"""PS Linker Store fetcher (server-side only).

Reads a queue file of CDN items, downloads each into its category folder
under the games library, then removes the queue. The catalog generator
picks the new files up on its next pass — clients see one unified library.

Zip archives are extracted in place (the archive is removed after a
successful extraction), so clients download ready-to-run files and never
need on-device unzip support.

Queue file format (default: ${GAMES_DIR}/.store-queue.json):
[
  {"title": "Example Homebrew", "category": "Homebrew",
   "url": "https://example.cdn/game.zip"}
]

The web GUI (http_server.py) edits that same file, so the fetcher never
trims it in place: it *claims* the queue first (atomic rename to
<queue>.processing), which means items the GUI appends mid-run simply
wait for the next pass instead of being deleted unprocessed. While a
claim lives, the fetcher publishes a status file the GUI polls:

${GAMES_DIR}/.store-status.json
{ "updated": "...Z", "phase": "idle|downloading|extracting",
  "current": {"title","category","url","bytes","total","started"}|null,
  "history": [{"title","url","category","status":"ok|failed",
               "bytes","seconds","error","finished"}] }   # newest first

Both files are dot-prefixed, so listings and the catalog skip them like
any hidden file, and devices never see them.

Usage: store_fetch.py <games_dir> <queue_path>
"""
import json
import os
import sys
import tempfile
import time
import urllib.request
import zipfile

STATUS_NAME = ".store-status.json"
HISTORY_CAP = 30
PROGRESS_INTERVAL = 0.4      # s between status writes inside one download


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _atomic_json(path: str, data) -> None:
    tmp = path + ".part"
    with open(tmp, "w", encoding="utf8") as fh:
        json.dump(data, fh)
    os.replace(tmp, path)


class Status:
    """${GAMES_DIR}/.store-status.json, plus the per-item progress state
    the fetch loop feeds in. History survives across runs (each entrypoint
    tick spawns a new process; the panel wants the whole recent log, not
    just this run's)."""

    def __init__(self, games_dir: str):
        self.path = os.path.join(games_dir, STATUS_NAME)
        prev = {}
        try:
            with open(self.path, encoding="utf8") as fh:
                loaded = json.load(fh)
            if isinstance(loaded, dict):
                prev = loaded
        except (OSError, ValueError):
            pass
        hist = prev.get("history")
        self.history = hist if isinstance(hist, list) else []
        self.item, self.started, self.moved = {}, 0.0, 0
        self.throttle = 0.0

    def publish(self, phase: str, current=None) -> None:
        try:
            _atomic_json(self.path, {"updated": _now(), "phase": phase,
                                     "current": current,
                                     "history": self.history})
        except OSError as exc:           # status is telemetry, never fatal
            print(f"[store] status write failed: {exc}", file=sys.stderr)

    # ---- per-item progress ----

    def start_item(self, item: dict) -> None:
        self.item = item if isinstance(item, dict) else {}
        self.started = time.time()
        self.moved = 0
        self.throttle = time.monotonic()
        self.publish("downloading", {
            "title": str(self.item.get("title") or self.item.get("url") or "?"),
            "category": str(self.item.get("category") or "Homebrew"),
            "url": str(self.item.get("url") or ""),
            "bytes": 0, "total": None, "started": _now(),
        })

    def report(self, state: str, moved: int, total) -> None:
        """fetch()'s progress callback: throttled on the way down, always
        written for the one-shot 'extracting' state."""
        self.moved = moved
        now = time.monotonic()
        if state != "extracting" and now - self.throttle < PROGRESS_INTERVAL:
            return
        self.throttle = now
        self.publish(state, {
            "title": str(self.item.get("title") or self.item.get("url") or "?"),
            "category": str(self.item.get("category") or "Homebrew"),
            "url": str(self.item.get("url") or ""),
            "bytes": int(moved),
            "total": int(total) if total else None,
            "started": _now(),
        })

    def finish(self, ok: bool, error: str = "") -> None:
        item = self.item
        self.history.insert(0, {
            "title": str(item.get("title") or item.get("url") or "?"),
            "url": str(item.get("url") or ""),
            "category": str(item.get("category") or "Homebrew"),
            "status": "ok" if ok else "failed",
            "bytes": int(self.moved),
            "seconds": round(time.time() - self.started, 1),
            "error": str(error)[:200], "finished": _now(),
        })
        del self.history[HISTORY_CAP:]


def _safe_category(cat) -> str:
    """A category is exactly one visible path component — the queue is a
    file an operator (or a bug) can edit directly, so the fetcher never
    trusts it to stay inside the library. '' falls back to Homebrew."""
    cat = str(cat or "").strip()
    if not cat:
        return "Homebrew"
    if cat != os.path.basename(cat) or cat.startswith(".") or cat in (".", ".."):
        raise ValueError(f"invalid category: {cat!r}")
    return cat


def _extract_zip(zip_path: str, dest_dir: str) -> None:
    """Extract zip_path into dest_dir, rejecting path traversal ("zip slip")."""
    with zipfile.ZipFile(zip_path) as zf:
        dest_real = os.path.realpath(dest_dir)
        for info in zf.infolist():
            # Normalize the target path and require it to stay inside dest_dir.
            target = os.path.realpath(os.path.join(dest_dir, info.filename))
            if target != dest_real and not target.startswith(dest_real + os.sep):
                raise ValueError(f"unsafe zip entry: {info.filename}")
        zf.extractall(dest_dir)


def fetch(item: dict, games_dir: str, progress=None) -> int:
    """Download (and unpack, for zips) one queue item; returns bytes moved.

    ``progress(state, moved, total)`` is called with state "downloading"
    after every chunk (total None when the CDN withholds Content-Length)
    and once with "extracting". Throttle at the caller if it is a file write.
    """
    category = _safe_category(item.get("category"))
    url = item["url"]
    filename = os.path.basename(url.split("?")[0])
    if not filename:
        raise ValueError(f"cannot derive filename from URL: {url}")
    dest_dir = os.path.join(games_dir, category)
    os.makedirs(dest_dir, exist_ok=True)
    dest = os.path.join(dest_dir, filename)
    is_zip = filename.lower().endswith(".zip")

    print(f"[store] fetching {item.get('title', filename)} -> {dest}")
    # Download to a temp file first so the catalog/browser never sees a
    # partially downloaded game, then atomically rename into place.
    tmp_fd, tmp_path = tempfile.mkstemp(dir=dest_dir, suffix=".part")
    moved = 0
    try:
        with os.fdopen(tmp_fd, "wb") as out, urllib.request.urlopen(url, timeout=60) as resp:
            raw_len = resp.headers.get("Content-Length", "")
            total = int(raw_len) if raw_len.isdigit() else None
            while True:
                chunk = resp.read(1024 * 256)
                if not chunk:
                    break
                out.write(chunk)
                moved += len(chunk)
                if progress:
                    progress("downloading", moved, total)
        if is_zip:
            # Extract while still under the .part name so the catalog never
            # lists either the archive or a half-extracted set.
            print(f"[store] extracting {filename} into {dest_dir}")
            if progress:
                progress("extracting", moved, total)
            _extract_zip(tmp_path, dest_dir)
            os.remove(tmp_path)
        else:
            os.replace(tmp_path, dest)
    except BaseException:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise
    return moved


def process_once(queue_path: str, games_dir: str, status: Status) -> int:
    """Claim and drain the queue once. Returns the item count processed,
    or -1 when there was nothing to claim (no file, or another process won
    the rename)."""
    claimed = queue_path + ".processing"
    try:
        os.rename(queue_path, claimed)
    except OSError:
        return -1
    try:
        with open(claimed, encoding="utf8") as fh:
            queue = json.load(fh)
        if not isinstance(queue, list):
            raise ValueError("queue file is not a JSON array")
    except (OSError, ValueError) as exc:
        print(f"[store] invalid queue file: {exc}", file=sys.stderr)
        try:
            os.remove(claimed)
        except OSError:
            pass
        return -1
    for item in queue:
        status.start_item(item)
        if not isinstance(item, dict) or not item.get("url"):
            status.finish(False, "invalid queue entry")
            continue
        try:
            fetch(item, games_dir, status.report)
            status.finish(True)
        except Exception as exc:
            # One bad item must not block the rest of the queue.
            print(f"[store] FAILED {item.get('url', '?')}: {exc}",
                  file=sys.stderr)
            status.finish(False, str(exc))
    os.remove(claimed)
    return len(queue)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    games_dir, queue_path = sys.argv[1], sys.argv[2]
    status = Status(games_dir)
    total_items = 0
    while True:
        n = process_once(queue_path, games_dir, status)
        if n < 0:
            break
        total_items += n
    if total_items:
        status.publish("idle")
        print(f"[store] processed {total_items} queued item(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
