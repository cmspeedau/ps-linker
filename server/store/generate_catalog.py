#!/usr/bin/env python3
"""PS Linker catalog generator (server-side only).

Scans the games library and regenerates catalog.json, which the HTTP
share serves at /catalog.json. Top-level folder names become categories,
so SMB uploads and Store downloads appear in one unified listing.

Per-entry fields:
  id, title, category, url, size  (always present)
  icon                            (optional: URL of a cover/icon image
                                   found next to the game file — e.g.
                                   Game/ICON0.PNG, cover.png, boxart.jpg)
  version, description            (optional: from a sidecar metadata file
                                   <name>.meta.json next to the game file,
                                   e.g. {"version": "1.2", "description": "..."})
  title, platform                 (also from the sidecar; the web GUI's
                                   uploads write them there so a prettified
                                   title and PSP/PS3 tab membership survive
                                   regeneration)
  rap                             (also from the sidecar: the file name of
                                   the game's attached .rap license, which
                                   the web GUI records next to the game)

Sidecar files themselves (.meta.json, .sha256) are metadata and never
become catalog entries.

Usage: generate_catalog.py <games_dir> <output_path>
"""
import json
import os
import socket
import sys

SKIP = {"catalog.json", ".store-queue.json"}

# Hidden files, in-flight downloads, and per-file sidecars are not games.
SKIP_SUFFIX = (".part", ".meta.json", ".sha256")

# File names (case-insensitive, extension-insensitive) treated as cover art.
ICON_NAMES = {"icon0", "cover", "boxart", "thumb"}
ICON_EXTS = {".png", ".jpg", ".jpeg", ".bmp"}


def find_icon(folder: str, name: str):
    """Return the relative URL of a cover image sitting next to `name`."""
    for fname in os.listdir(folder):
        base, ext = os.path.splitext(fname)
        if ext.lower() in ICON_EXTS and base.lower() in ICON_NAMES:
            return "/" + fname
    return None


def load_meta(folder: str, name: str) -> dict:
    """Load optional sidecar metadata (<name>.meta.json) for a game file."""
    meta_path = os.path.join(folder, name + ".meta.json")
    if not os.path.isfile(meta_path):
        return {}
    try:
        with open(meta_path) as fh:
            meta = json.load(fh)
        return meta if isinstance(meta, dict) else {}
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[catalog] warning: bad metadata file {meta_path}: {exc}",
              file=sys.stderr)
        return {}


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    games_dir, out_path = sys.argv[1], sys.argv[2]

    entries = []
    for root, dirs, files in os.walk(games_dir):
        rel = os.path.relpath(root, games_dir)
        category = rel.split(os.sep)[0] if rel != "." else "Uncategorized"
        for name in sorted(files):
            if (name in SKIP or name.startswith(".")
                    or name.endswith(SKIP_SUFFIX)):
                continue
            rel_path = os.path.normpath(os.path.join(rel, name)).replace(os.sep, "/")
            entry = {
                "id": rel_path.replace("/", "_"),
                "title": os.path.splitext(name)[0],
                "category": category,
                "url": "/" + rel_path,
                "size": os.path.getsize(os.path.join(root, name)),
            }
            icon = find_icon(root, name)
            if icon:
                entry["icon"] = icon
            meta = load_meta(root, name)
            if meta.get("title"):
                entry["title"] = str(meta["title"])
            if meta.get("platform"):
                entry["platform"] = str(meta["platform"])
            if meta.get("rap"):
                entry["rap"] = str(meta["rap"])
            if meta.get("version"):
                entry["version"] = str(meta["version"])
            if meta.get("description"):
                entry["description"] = str(meta["description"])
            entries.append(entry)

    entries.sort(key=lambda e: (e["category"], e["title"]))
    catalog = {"generated_by": socket.gethostname(), "entries": entries}

    # Atomic write so the HTTP share never serves a half-written catalog.
    tmp = out_path + ".tmp"
    with open(tmp, "w") as fh:
        json.dump(catalog, fh, indent=2)
    os.replace(tmp, out_path)
    print(f"[catalog] wrote {len(entries)} entries to {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
