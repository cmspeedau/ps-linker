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
#                         NETSRV_WRITE=1. Everything a PS3 sends except virtual
#                         ISOs — see docs/PS3NETSRV-PROTOCOL.md §9.2.
#   NETSRV=ps3netsrv++    dirkvdb's C++ binary, compiled during the build. Only
#                         worth it if the library has folder games, which are
#                         served through /***PS3***/ and are not implemented by
#                         the Python server. It is slower in total across five
#                         consoles, and five clients that send it nothing can
#                         take it down (docs/PS3NETSRV-PROTOCOL.md §8).
#
# Pick one at build time:
#
#   docker build -t ps-linker .                          # python
#   docker build -t ps-linker --build-arg NETSRV=ps3netsrv++ .

FROM alpine:3.20

RUN apk add --no-cache python3 busybox-extras

# netiso server for the PS3, and the Store scripts
COPY netsrv/ps3netsrv.py /opt/netsrv/ps3netsrv.py
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
