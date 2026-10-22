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

Usage: store_fetch.py <games_dir> <queue_path>
"""
import json
import os
import sys
import tempfile
import urllib.request
import zipfile


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


def fetch(item: dict, games_dir: str) -> None:
    category = item.get("category", "Homebrew")
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
    try:
        with os.fdopen(tmp_fd, "wb") as out, urllib.request.urlopen(url, timeout=60) as resp:
            while True:
                chunk = resp.read(1024 * 256)
                if not chunk:
                    break
                out.write(chunk)
        if is_zip:
            # Extract while still under the .part name so the catalog never
            # lists either the archive or a half-extracted set.
            print(f"[store] extracting {filename} into {dest_dir}")
            _extract_zip(tmp_path, dest_dir)
            os.remove(tmp_path)
        else:
            os.replace(tmp_path, dest)
    except BaseException:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    games_dir, queue_path = sys.argv[1], sys.argv[2]
    if not os.path.exists(queue_path):
        return 0

    with open(queue_path) as fh:
        queue = json.load(fh)
    if not isinstance(queue, list):
        print(f"[store] invalid queue file: {queue_path}", file=sys.stderr)
        return 1

    for item in queue:
        try:
            fetch(item, games_dir)
        except Exception as exc:
            # One bad item must not block the rest of the queue.
            print(f"[store] FAILED {item.get('url', '?')}: {exc}", file=sys.stderr)

    os.remove(queue_path)
    print(f"[store] processed {len(queue)} queued item(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
