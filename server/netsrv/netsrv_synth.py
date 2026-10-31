#!/usr/bin/env python3
"""Synthetic sector-probe ceiling test (§5, §8, §9.5).

Six sparse images straddle the probe's size window, including one-byte-either-
side cases at ``PROBE_MAX`` and real-stride files whose correct answer the
ceiling defeats. First ``detect_sector_size`` is asserted offline against every
fixture; then the same answer is read back off the wire (§5's measure_stride
trick) from our server and, when binaries are given, the upstream ones. A row
where a server's answer is not the file's truth is a row the ceiling — or a
broken probe — defeated, and only a real client can say whether it cares (§8).

The fixtures and scenarios live in ``netsrv_diff.py``; this is the focused,
re-runnable form of them, and runs without any reference binary.

    python3 server/netsrv/netsrv_synth.py
    python3 server/netsrv/netsrv_synth.py \
            --reference /tmp/opencode/ref/ps3netsrv \
            --reference /tmp/opencode/ref/ps3netsrv++
"""
from __future__ import annotations

import argparse
import asyncio
import os
import sys
from pathlib import Path

import netsrv_diff as nd
import ps3netsrv as ns


def offline_probe(root: Path) -> list[int]:
    """Assert what ``detect_sector_size`` answers for each fixture size (§5)."""
    oks = []
    for name, (_size, stride, _raw, truth, expected, _note) in \
            nd.CEILING_IMAGES.items():
        with (root / name).open("rb") as fh:
            got = ns.detect_sector_size(fh, os.path.getsize(root / name))
        oks.append(got == expected)
        verdict = "ok" if got == expected else "FAIL"
        print(f"  {name:<30} {got:>5}  expect {expected:>5}  {verdict}"
              f"{'  (default is wrong for this file)' if truth != expected else ''}")
    return oks


async def wire_probe(root: Path, refs: list[nd.Reference]) -> list[bool]:
    """The same answers as the client sees them: READ_CD_2048 read back (§5)."""
    print(f"\n  {'fixture':<30} {'truth':>6} {'ours':>12} " +
          " ".join(f"{r.name:>16}" for r in refs))
    oks = []
    for name, (_size, stride, _raw, truth, expected, _note) in \
            nd.CEILING_IMAGES.items():
        case = nd.scenario(name, nd.cd_stride("/" + name))
        ours = await nd.run_against_ours(case, root)
        theirs = [await ref.drive(case, root) for ref in refs]
        oks.append(ours == expected)
        mark = "✔" if ours == truth else "✘ ceiling"
        print(f"  {name:<30} {stride:>6} {str(ours):>10} {mark:>2}  " +
              " ".join(f"{str(t):>16}" for t in theirs))
    return oks


async def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        description="Synthetic sector-probe ceiling test")
    parser.add_argument("--reference", action="append", default=[],
                        help="upstream server binary; repeatable")
    parser.add_argument("--dir", default="/tmp/opencode/netsrv-synth",
                        help="where to build the fixtures")
    args = parser.parse_args(argv)

    root = Path(args.dir)
    root.mkdir(parents=True, exist_ok=True)
    nd.LISTING_SORTED = False  # read the wire, not our listing order (§9.3)
    for name, (size, stride, raw, *_r) in nd.CEILING_IMAGES.items():
        nd.synth_image(root / name, stride, size, raw)
    on_disk = sum(os.stat(root / n).st_blocks * 512 for n in nd.CEILING_IMAGES)
    apparent = sum(size for size, *_r in nd.CEILING_IMAGES.values())
    print(f"[synth] fixtures    {root}  "
          f"({on_disk >> 20} MiB on disk, apparent {apparent >> 20} MiB)\n")

    print("  detect_sector_size, in-process:")
    offline_ok = offline_probe(root)

    refs = [nd.Reference(p, root) for p in nd.pick_references(args.reference)]
    try:
        wire_ok = await wire_probe(root, refs)
    finally:
        for ref in refs:
            ref.stop()

    good = all(offline_ok) and all(wire_ok)
    print(f"\n[synth] {'PASS' if good else 'FAIL'} — "
          f"{sum(offline_ok)}/{len(offline_ok)} offline, "
          f"{sum(wire_ok)}/{len(wire_ok)} over the wire")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
