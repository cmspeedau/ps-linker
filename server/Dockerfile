# PS Linker server
#
# Runs both services needed by PS Linker:
#   - the netiso server (PS3 game streaming, port 38008)
#   - HTTP file share (PSP/PS3 NASDL app downloads, port 8000)
# plus the server-side Store (CDN fetcher + catalog generator).
#
# Mount your game library at /games (read-write for Store downloads).
#
# Port 38008 can be served by either of two implementations:
#
#   NETSRV=python         (default) our own server, stdlib Python 3, no build
#                         step: server/netsrv/ps3netsrv.py. Read-only unless
#                         NETSRV_WRITE=1. It serves everything a PS3 sends,
#                         folder games through /***PS3***/ included
#                         (docs/PS3NETSRV-PROTOCOL.md §7.4).
#   NETSRV=ps3netsrv++    dirkvdb's C++ binary, compiled during the build.
#                         Kept as a way to check our answers against the real
#                         thing. It is slower in total across five consoles,
#                         five clients that send it nothing can take it down,
#                         and it has no virtual ISO either
#                         (docs/PS3NETSRV-PROTOCOL.md §8).
#
# Pick one at build time:
#
#   docker build -t ps-linker .                          # python
#   docker build -t ps-linker --build-arg NETSRV=ps3netsrv++ .

FROM alpine:3.20

RUN apk add --no-cache python3 busybox-extras

# netiso server for the PS3 (with its virtual-ISO builder), and the Store scripts
COPY netsrv/ps3netsrv.py netsrv/virtual_iso.py /opt/netsrv/
COPY store/ /opt/store/

# The fallback C++ server, if asked for. Three things the upstream tree needs
# help with, none of them in its Makefile: the utils/ submodule; a compiler that
# exists in this image (the Makefile hardcodes clang++, which build-base does not
# provide); and #include <cstdint> at the top of ps3netsrv.cpp, which g++ 13
# requires. The toolchain is removed again in the same layer.
ARG NETSRV=python
ENV NETSRV=${NETSRV}
RUN if [ "$NETSRV" = "ps3netsrv++" ]; then \
        apk add --no-cache build-base git \
        && git clone --depth 1 --recursive https://github.com/dirkvdb/ps3netsrv-- /src \
        && sed -i '1i #include <cstdint>' /src/ps3netsrv.cpp \
        && make -C /src CXX=g++ -j"$(nproc)" \
        && cp /src/ps3netsrv++ /usr/local/bin/ps3netsrv++ \
        && rm -rf /src \
        && apk del build-base git; \
    fi

# Game library mount point
RUN mkdir -p /games
VOLUME /games

# 38008/tcp = netiso (ps3netsrv protocol), 8000/tcp = HTTP share for PSP/PS3 app
EXPOSE 38008 8000

COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
