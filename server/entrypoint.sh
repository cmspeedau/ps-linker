#!/bin/sh
# Start ps3netsrv++ in the background and serve /games over HTTP
# on port 8000 for the PSP/PS3 NASDL apps.

set -e

GAMES_DIR="${GAMES_DIR:-/games}"

echo "[ps-linker] starting ps3netsrv++ on :38008 (library: ${GAMES_DIR})"
ps3netsrv++ -d "${GAMES_DIR}" &
NETSRV_PID=$!

echo "[ps-linker] starting HTTP share on :8000 (library: ${GAMES_DIR})"
cd "${GAMES_DIR}"
python3 -m http.server 8000 --bind 0.0.0.0 &
HTTP_PID=$!

trap 'kill $NETSRV_PID $HTTP_PID 2>/dev/null' TERM INT
wait -n
echo "[ps-linker] a service exited, shutting down"
kill $NETSRV_PID $HTTP_PID 2>/dev/null
wait
