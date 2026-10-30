#!/usr/bin/env python3
"""Throughput for the ps3netsrv read path, measured over loopback.

``READ_FILE_CRITICAL`` is where a PS3 spends its life, so it is the number worth
knowing. This streams a file in ``BUFFER_SIZE`` requests the way a client does,
once on its own and then with ``MAX_CLIENTS`` at once — the case an event-loop
server has to survive and a thread-per-client one does not choose.

    python3 server/netsrv/netsrv_bench.py --file BIG_FILE.iso [--reference BIN]

The concurrent measurement uses one loopback address per client on purpose: the
server hands a slot per *IP* and lets a reconnecting client take its own slot
back (§1.1), so five connections from one address would be five clients kicking
each other out, not five clients streaming.

Loopback is faster than any home network, so read these as "how much the server
can push", never as "what a PS3 will see": a gigabit link tops out near 119
MiB/s and the console's own HDD is the tighter limit well before that.
"""
from __future__ import annotations

import argparse
import asyncio
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import netsrv_client as nc
import ps3netsrv as ns


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def loopback(i: int) -> str:
    """A distinct address on the loopback network, for one client each."""
    return f"127.0.{i // 256}.{i % 256 + 1}"


async def stream(port: int, name: bytes, total: int, chunk: int,
                 source: str | None = None) -> float:
    """Stream the first *total* bytes of *name*; return MiB/s."""
    client = nc.Netiso("127.0.0.1", port, timeout=120, local_addr=(source, 0)
                       if source else None)
    try:
        await client.connect()
        await client.open_file(name)
        done = 0
        started = time.perf_counter()
        while done < total:
            count = min(chunk, total - done)
            client.request_bulk_read(done, count)
            done += await client.drain_bulk(count)
        elapsed = time.perf_counter() - started
        return (done / 1048576) / elapsed if elapsed else float("inf")
    finally:
        client.close()


async def many(port: int, name: bytes, total: int, chunk: int, count: int) -> float:
    """``count`` clients streaming at once, each on its own IP; combined MiB/s."""
    rates = await asyncio.gather(*[stream(port, name, total, chunk, loopback(i))
                                   for i in range(count)])
    return sum(rates)


async def measure(port: int, name: bytes, size: int, chunk: int, clients: int) -> dict:
    single = await stream(port, name, min(size, 128 << 20), chunk)
    total = await many(port, name, min(size, 64 << 20), chunk, clients)
    return {"single": single, f"{clients} clients": total}


def stage(file: Path) -> tuple[Path, Path]:
    """Hardlink *file* into a private share directory, and return (dir, name).

    A benchmark server shares a whole directory, and the file's own directory is
    usually not something to hand to a network server for the duration — so the
    file is linked into a temporary one that holds nothing else. A hardlink costs
    nothing and keeps the measurement honest: same blocks, same page cache. The
    temporary directory is tried next to the file first, because a hardlink
    cannot cross filesystems, and then in the system temporary directory.
    """
    for parent in (file.parent, None):
        tmp = Path(tempfile.mkdtemp(prefix=".netsrv-bench-", dir=parent))
        try:
            os.link(file, tmp / file.name)
            return tmp, tmp / file.name
        except OSError:
            shutil.rmtree(tmp, ignore_errors=True)
    raise SystemExit(f"[bench] cannot hardlink {file} into a private share "
                     f"directory; the filesystem has to hold a second name for it")


def start_reference(binary: str, root: Path, port: int):
    """Upstream's own command lines; it binds every interface, so stage first."""
    args = ([binary, "-p", str(port), str(root)]
            if Path(binary).name.startswith("ps3netsrv++")
            else [binary, str(root), str(port)])
    return subprocess.Popen(args, stdout=subprocess.DEVNULL,
                            stderr=subprocess.STDOUT)


async def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="ps3netsrv throughput")
    parser.add_argument("--file", required=True, help="file to stream")
    parser.add_argument("--reference", default="", help="upstream binary to compare")
    parser.add_argument("--chunk", type=int, default=ns.BUFFER_SIZE)
    parser.add_argument("--clients", type=int, default=ns.MAX_CLIENTS)
    args = parser.parse_args(argv)

    path = Path(args.file)
    if not path.is_file():
        parser.error(f"no such file: {path}")
    size = path.stat().st_size
    tmp, staged = stage(path)
    name = ("/" + staged.name).encode("utf8", "surrogateescape")
    print(f"[bench] {path.name}, {size / 1048576:.0f} MiB, "
          f"{args.chunk // 1048576} MiB requests, loopback\n")

    rows = {}
    try:
        server = ns.NetisoServer(ns.Share(tmp), writable=False)
        srv = await asyncio.start_server(server.connection, "127.0.0.1", 0, backlog=1)
        ours = srv.sockets[0].getsockname()[1]
        try:
            rows["ours"] = await measure(ours, name, size, args.chunk, args.clients)
        finally:
            await server.shutdown()
            srv.close()

        if args.reference:
            port = free_port()
            proc = start_reference(args.reference, tmp, port)
            try:
                await asyncio.sleep(0.6)
                rows[Path(args.reference).name] = await measure(
                    port, name, size, args.chunk, args.clients)
            finally:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    width = max(len(k) for k in rows) + 2
    print(f"  {'server'.ljust(width)}{'single client':>17}{'all clients':>17}")
    for label, row in rows.items():
        total = row[f"{args.clients} clients"]
        print(f"  {label.ljust(width)}{row['single']:>14.0f} MiB/s"
              f"{total:>12.0f} MiB/s")
    print(f"\n[bench] '{args.clients} clients' is the combined rate of "
          f"{args.clients} simultaneous streams; a gigabit link caps out near "
          f"119 MiB/s")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
