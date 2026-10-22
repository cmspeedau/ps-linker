#!/bin/sh
# Start ps3netsrv++ in the background and serve /games over HTTP
# on port 8000 for the PSP/PS3 NASDL apps. Also runs the server-side
# Store: processes the download queue into /games and keeps
# catalog.json in sync, so SMB uploads and Store downloads appear
# in one unified client view.

set -e

GAMES_DIR="${GAMES_DIR:-/games}"
STORE_INTERVAL="${STORE_INTERVAL:-300}"

echo "[ps-linker] starting ps3netsrv++ on :38008 (library: ${GAMES_DIR})"
ps3netsrv++ -d "${GAMES_DIR}" &
NETSRV_PID=$!

echo "[ps-linker] starting HTTP share on :8000 (library: ${GAMES_DIR})"
cd "${GAMES_DIR}"
python3 -m http.server 8000 --bind 0.0.0.0 &
HTTP_PID=$!

# Server-side Store loop: fetch queued CDN titles into the library,
# then regenerate the catalog. Runs immediately, then every
# STORE_INTERVAL seconds.
(
  while true; do
    python3 /opt/store/store_fetch.py "${GAMES_DIR}" "${GAMES_DIR}/.store-queue.json" || true
    python3 /opt/store/generate_catalog.py "${GAMES_DIR}" "${GAMES_DIR}/catalog.json" || true
    sleep "${STORE_INTERVAL}"
  done
) &
STORE_PID=$!

trap 'kill $NETSRV_PID $HTTP_PID $STORE_PID 2>/dev/null' TERM INT
wait -n
echo "[ps-linker] a service exited, shutting down"
kill $NETSRV_PID $HTTP_PID $STORE_PID 2>/dev/null
wait
