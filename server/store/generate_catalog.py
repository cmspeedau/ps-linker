#!/usr/bin/env python3
"""PS Linker catalog generator (server-side only).

Scans the games library and regenerates catalog.json, which the HTTP
share serves at /catalog.json. Top-level folder names become categories,
so SMB uploads and Store downloads appear in one unified listing.

Usage: generate_catalog.py <games_dir> <output_path>
"""
import json
import os
import socket
import sys

SKIP = {"catalog.json", ".store-queue.json"}


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
            if name in SKIP or name.startswith(".") or name.endswith(".part"):
                continue
            rel_path = os.path.normpath(os.path.join(rel, name)).replace(os.sep, "/")
            entries.append({
                "id": rel_path.replace("/", "_"),
                "title": os.path.splitext(name)[0],
                "category": category,
                "url": "/" + rel_path,
                "size": os.path.getsize(os.path.join(root, name)),
            })

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
