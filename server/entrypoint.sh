#!/bin/sh
# Start the two services PS Linker needs:
#
#   * the netiso server on :38008 for webMAN MOD / PS3 game streaming
#   * an HTTP share on :8000 for the PSP/PS3 NASDL apps
#
# and keep the server-side Store in sync: process the download queue into the
# library and regenerate catalog.json, so SMB uploads and Store downloads show
# up in one client view.
#
# NETSRV picks the netiso implementation: "python" (default) runs the server in
# server/netsrv/ps3netsrv.py; "ps3netsrv++" runs the upstream C++ binary, which
# the Python server cannot replace for folder games (/***PS3***/).
#
#   GAMES_DIR         library to share            (default /games)
#   NETSRV            python | ps3netsrv++        (default python)
#   NETSRV_WHITELIST  IPv4 pattern, e.g. 192.168.1.*   (default: everyone)
#   NETSRV_WRITE      set to 1 to let clients write    (default: read-only)

set -e

GAMES_DIR="${GAMES_DIR:-/games}"
NETSRV="${NETSRV:-python}"
STORE_INTERVAL="${STORE_INTERVAL:-300}"

# Built-in defaults of each server, kept here so they are visible in one place.
NETSRV_PORT="${NETSRV_PORT:-38008}"
HTTP_PORT="${HTTP_PORT:-8000}"

if [ "$NETSRV" = "ps3netsrv++" ] && ! command -v ps3netsrv++ >/dev/null 2>&1; then
	echo "[ps-linker] NETSRV=ps3netsrv++ but no such binary in the image" \
		"(it was built with NETSRV=python); falling back to the python server" >&2
	NETSRV=python
fi

if [ "$NETSRV" = "ps3netsrv++" ]; then
	# No -d here: it daemonizes, which would leave this script waiting on a
	# process that exited on purpose and take the container down with it.
	echo "[ps-linker] starting ps3netsrv++ on :${NETSRV_PORT} (library: ${GAMES_DIR})"
	ps3netsrv++ -p "${NETSRV_PORT}" "${GAMES_DIR}" &
	NETSRV_PID=$!
else
	WRITE_FLAG=""
	if [ "${NETSRV_WRITE}" = "1" ]; then WRITE_FLAG="--write"; fi
	readonly_flag="read-only"
	if [ -n "$WRITE_FLAG" ]; then readonly_flag="writable"; fi
	echo "[ps-linker] starting ps3netsrv (python) on :${NETSRV_PORT}" \
		"(library: ${GAMES_DIR}, clients: ${NETSRV_WHITELIST:-any}, ${readonly_flag})"
	python3 /opt/netsrv/ps3netsrv.py "${GAMES_DIR}" "${NETSRV_PORT}" \
		"${NETSRV_WHITELIST:-}" ${WRITE_FLAG} &
	NETSRV_PID=$!
fi

echo "[ps-linker] starting HTTP share on :${HTTP_PORT} (library: ${GAMES_DIR})"
python3 /opt/store/http_server.py "${GAMES_DIR}" "${HTTP_PORT}" &
HTTP_PID=$!

# Server-side Store loop: fetch queued CDN titles into the library, then
# regenerate the catalog. Runs immediately, then every STORE_INTERVAL seconds.
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
