# PS Linker server
#
# Runs both services needed by PS Linker:
#   - ps3netsrv++  (PS3 game streaming, port 38008)
#   - HTTP file share (PSP/PS3 NASDL app downloads, port 8000)
# plus the server-side Store (CDN fetcher + catalog generator).
#
# Mount your game library at /games (read-write for Store downloads).

FROM alpine:3.20 AS builder

RUN apk add --no-cache build-base git linux-headers

# Build the open-source ps3netsrv++ server
RUN git clone --depth 1 https://github.com/dirkvdb/ps3netsrv-- /src \
    && make -C /src -j"$(nproc)"

FROM alpine:3.20

RUN apk add --no-cache python3 busybox-extras

COPY --from=builder /src/ps3netsrv++ /usr/local/bin/ps3netsrv++

# Server-side Store scripts (queue fetcher + catalog generator)
COPY store/ /opt/store/

# Game library mount point
RUN mkdir -p /games
VOLUME /games

# 38008/tcp = ps3netsrv, 8000/tcp = HTTP share for PSP/PS3 app
EXPOSE 38008 8000

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
