#!/usr/bin/env bash
# Behaviour check for the PS Linker HTTP share (server/store/http_server.py).
#
#   ./check_share.sh              # HTTP-level checks (~50 assertions, curl only)
#   ./check_share.sh --with-dl    # also compile ps3/resume.c on the host and
#                                 # run the console's real downloader against it
#
# Started as the regression test for a rewrite that resolved every path against
# the web-GUI folder (so every game file and directory listing 404'd) and whose
# double-escaped Range regex silently ignored Range headers, leaving the apps
# unable to browse and unable to resume.
#
# Needs: python3, curl, sha256sum. --with-dl also needs gcc.
set -u

PORT="${PORT:-8151}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
SERVER="$HERE/http_server.py"
W="$(mktemp -d "${TMPDIR:-/tmp}/pslinker-share-check.XXXXXX")"
SRV=""
pass=0; fail=0

cleanup() {
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    rm -rf "$W"
}
trap cleanup EXIT INT TERM

chk() {   # chk <description> <expected> <actual>
    if [ "$2" = "$3" ]; then
        printf '  ok    %-52s %s\n' "$1" "$3"; pass=$((pass + 1))
    else
        printf '  FAIL  %-52s got=%s want=%s\n' "$1" "$3" "$2"; fail=$((fail + 1))
    fi
}

BASE="http://127.0.0.1:$PORT"
code() { curl -s -o /dev/null -w '%{http_code}' "$BASE$1"; }
hdr()  { curl -s -D - -o /dev/null "$@" | tr -d '\r'; }

# ---------------------------------------------------------------- fixtures ---
mkdir -p "$W/games/PSP" "$W/games/PS3" "$W/up" "$W/out"
SIZE=5000000
head -c "$SIZE" /dev/urandom > "$W/games/PSP/test.iso"
(cd "$W/games/PSP" && sha256sum test.iso | cut -d' ' -f1 > test.iso.sha256)
GOOD="$(cat "$W/games/PSP/test.iso.sha256")"
echo keep > "$W/games/PSP/partial.bin.part"
cat > "$W/games/catalog.json" <<JSON
{"generated_by":"check_share","categories":["PSP","PS3"],
 "items":[{"id":"PSP_test.iso","name":"test.iso",
 "title":"Test","category":"PSP","url":"/PSP/test.iso","size":$SIZE}]}
JSON
head -c 100 /dev/urandom > "$W/up/demo.zip"
cp "$W/up/demo.zip" "$W/up/evil.exe"

python3 "$SERVER" "$W/games" "$PORT" > "$W/server.log" 2>&1 &
SRV=$!
sleep 1.2
if ! kill -0 "$SRV" 2>/dev/null; then
    echo "server failed to start:"; cat "$W/server.log"; exit 1
fi

# ------------------------------------------------- library: what apps depend on
echo "library (the PSP/PS3 apps' browse + download path)"
chk "GET / is the library listing, not the GUI" 200 "$(code /)"
listing="$(curl -s "$BASE/")"
chk "listing links a subdirectory as DIR/" yes \
    "$(case "$listing" in *'href="PSP/"'*) echo yes;; *) echo no;; esac)"
chk "listing links a file as a bare name" yes \
    "$(case "$(curl -s "$BASE/PSP/")" in *'href="test.iso"'*) echo yes;; *) echo no;; esac)"
chk "listing hides catalog.json" yes \
    "$(case "$listing" in *catalog.json*) echo no;; *) echo yes;; esac)"
chk "listing hides .part files" yes \
    "$(case "$listing" in *partial.bin*) echo no;; *) echo yes;; esac)"
chk "GET /PSP/ listing" 200 "$(code /PSP/)"
chk "GET a game file" 200 "$(code /PSP/test.iso)"
chk "full body length" "$SIZE" "$(curl -s "$BASE/PSP/test.iso" | wc -c)"
chk "Accept-Ranges: bytes" bytes "$(hdr "$BASE/PSP/test.iso" | sed -n 's/^Accept-Ranges: //p')"
chk "X-SHA256 from the .sha256 sidecar" "$GOOD" \
    "$(hdr "$BASE/PSP/test.iso" | sed -n 's/^X-SHA256: //p')"
chk "HEAD sends headers only" 0 "$(curl -s -I -o /dev/null -w '%{size_download}' "$BASE/PSP/test.iso")"
chk "missing file is 404" 404 "$(code /PSP/nope.iso)"

# ------------------------------------------------------------ Range / resume --
echo "Range / resume"
r="$(hdr -H 'Range: bytes=1000-2999' "$BASE/PSP/test.iso")"
chk "closed range answers 206" 206 "$(printf '%s' "$r" | sed -n '1p' | awk '{print $2}')"
chk "Content-Range echoes the slice" "bytes 1000-2999/$SIZE" \
    "$(printf '%s' "$r" | sed -n 's/^Content-Range: //p')"
chk "closed range body length" 2000 \
    "$(curl -s -H 'Range: bytes=1000-2999' "$BASE/PSP/test.iso" | wc -c)"
chk "open-ended range 'N-' (how resume asks)" $((SIZE - 4999000)) \
    "$(curl -s -H 'Range: bytes=4999000-' "$BASE/PSP/test.iso" | wc -c)"
chk "suffix range '-500'" 500 \
    "$(curl -s -H 'Range: bytes=-500' "$BASE/PSP/test.iso" | wc -c)"
chk "range past EOF is 416" 416 \
    "$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=99999999-' "$BASE/PSP/test.iso")"
chk "unparseable range falls back to the full body" 200 \
    "$(curl -s -o /dev/null -w '%{http_code}' -H 'Range: bytes=abc' "$BASE/PSP/test.iso")"
chk "slice bytes match the file at that offset" same "$(
    a=$(curl -s -H 'Range: bytes=3000000-3000999' "$BASE/PSP/test.iso" | sha256sum)
    b=$(dd if="$W/games/PSP/test.iso" bs=1 skip=3000000 count=1000 2>/dev/null | sha256sum)
    [ "$a" = "$b" ] && echo same || echo differ)"

# ------------------------------------------------------- web GUI and JSON API --
echo "web GUI and JSON API"
chk "GET /gui/ serves the shell" 200 "$(code /gui/)"
chk "GET /gui/ui.css" 200 "$(code /gui/ui.css)"
chk "ui.css keeps its content type" "text/css" \
    "$(hdr "$BASE/gui/ui.css" | sed -n 's/^Content-Type: //p')"
chk "/api/catalog answers" 200 "$(code /api/catalog)"
chk "/api/catalog uses the key ui.js reads (items)" Test "$(
    curl -s "$BASE/api/catalog" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["items"][0]["title"])')"
chk "/api/catalog passes categories through" "['PSP', 'PS3']" "$(
    curl -s "$BASE/api/catalog" | python3 -c \
    'import json,sys; print(json.load(sys.stdin).get("categories"))')"

# ------------------------------------------------------------ upload + safety --
echo "upload and path safety"
chk "multipart upload accepted" 201 "$(
    curl -s -o /dev/null -w '%{http_code}' -F 'platform=psp' \
         -F "file=@$W/up/demo.zip" "$BASE/api/upload")"
chk "uploaded file landed in the library" yes \
    "$([ -f "$W/games/demo.zip" ] && echo yes || echo no)"
chk "upload kept the catalog's own top-level shape" generated_by "$(
    python3 -c "import json;print(list(json.load(open('$W/games/catalog.json')))[0])")"
chk "upload recorded the new item" demo.zip "$(
    python3 -c "import json;d=json.load(open('$W/games/catalog.json'));
print(next(i['name'] for i in d['items'] if i.get('name')=='demo.zip'))")"
chk "disallowed extension rejected" 400 "$(
    curl -s -o /dev/null -w '%{http_code}' -F "file=@$W/up/evil.exe" "$BASE/api/upload")"
chk "encoded traversal refused" refused "$(
    case "$(code '/..%2f..%2f..%2fetc%2fpasswd')" in
        403|404) echo refused;; *) echo "$(code '/..%2f..%2f..%2fetc%2fpasswd')";; esac)"
chk "double-encoded traversal refused" refused "$(
    case "$(code '/%252e%252e%252fetc/passwd')" in
        403|404) echo refused;; *) echo "$(code '/%252e%252e%252fetc/passwd')";; esac)"

# ------------------------------------- raw streaming upload (web GUI Add Games)
echo "raw upload endpoint (streamed, one file per request)"
head -c 2000000 /dev/urandom > "$W/up/raw.iso"
chk "raw upload accepted" 201 "$(
    curl -s -o /dev/null -w '%{http_code}' -X POST \
         --data-binary "@$W/up/raw.iso" "$BASE/api/upload/file?name=raw.iso&platform=psp")"
chk "raw upload landed byte-identical" same "$(
    cmp -s "$W/games/raw.iso" "$W/up/raw.iso" && echo same || echo differ)"
chk "no .part residue" 0 "$(ls "$W/games"/*.part 2>/dev/null | wc -l | tr -d ' ')"
chk "entry carries id/category/platform" "raw.iso Uncategorized psp" "$(
    python3 -c "import json;d=json.load(open('$W/games/catalog.json'));
i=next(i for i in d['items'] if i.get('url')=='/raw.iso')
print(i['id'],i['category'],i['platform'])")"
# Regression: entries lacking name/path used to collapse onto one None key,
# dropping everything but one entry until the next catalog regeneration.
chk "upload did not collapse pre-existing entries" yes "$(
    python3 -c "import json;d=json.load(open('$W/games/catalog.json'));
print('yes' if any(i.get('url')=='/PSP/test.iso' for i in d['items']) else 'no')")"
chk "meta sidecar written for the upload" yes \
    "$([ -f "$W/games/raw.iso.meta.json" ] && echo yes || echo no)"
chk "bad extension rejected" 400 "$(
    curl -s -o /dev/null -w '%{http_code}' -X POST \
         --data-binary "@$W/up/evil.exe" "$BASE/api/upload/file?name=evil.exe")"
chk "no file behind the rejected upload" yes \
    "$([ ! -f "$W/games/evil.exe" ] && echo yes || echo no)"
chk "control-char name rejected" 400 "$(
    curl -s -o /dev/null -w '%{http_code}' -X POST \
         --data-binary "@$W/up/raw.iso" "$BASE/api/upload/file?name=t%00st.iso")"
chk "chunked body refused" 411 "$(
    curl -s -o /dev/null -w '%{http_code}' -X POST -H 'Transfer-Encoding: chunked' \
         --data-binary "@$W/up/raw.iso" "$BASE/api/upload/file?name=chunk.iso")"

# ------------------------------------------------- catalog generator hygiene --
echo "generate_catalog sidecar hygiene"
mkdir -p "$W/gen/Utilities"
head -c 100 /dev/urandom > "$W/gen/Utilities/tool.bin"
echo '{"platform":"psp","title":"Tool","version":"1.2"}' > "$W/gen/Utilities/tool.bin.meta.json"
(cd "$W/gen/Utilities" && sha256sum tool.bin | cut -d' ' -f1 > tool.bin.sha256)
python3 "$HERE/generate_catalog.py" "$W/gen" "$W/gen/catalog.json" >"$W/gen.log" 2>&1
chk "sidecars (.meta.json/.sha256) are not entries" 1 "$(
    python3 -c "import json;d=json.load(open('$W/gen/catalog.json'));
print(len(d['entries']))")"
chk "sidecar title passes through" Tool "$(
    python3 -c "import json;d=json.load(open('$W/gen/catalog.json'));
print(d['entries'][0]['title'])")"
chk "sidecar platform passes through" psp "$(
    python3 -c "import json;d=json.load(open('$W/gen/catalog.json'));
print(d['entries'][0]['platform'])")"

# --------------------------------------- the console's own downloader (opt-in) -
if [ "${1:-}" = "--with-dl" ]; then
    echo "ps3/resume.c (host-compiled) against this server"
    if ! command -v gcc >/dev/null 2>&1; then
        echo "  skip: gcc not found"
    else
        mkdir -p "$W/shim/net"; : > "$W/shim/net/net.h"   # PSL1GHT-only, unused there
        SIZE2=16777216
        head -c "$SIZE2" /dev/urandom > "$W/games/PSP/big.bin"
        (cd "$W/games/PSP" && sha256sum big.bin | cut -d' ' -f1 > big.bin.sha256)
        BIGSHA="$(cat "$W/games/PSP/big.bin.sha256")"
        cat > "$W/dl_host.c" <<'C'
#include "resume.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    if (argc > 3) { char c[256]; snprintf(c, sizeof c, "head -c %ld \"$SOURCE_FILE\" > %s",
                                          atol(argv[3]), argv[2]); if (system(c)) return 2; }
    else remove(argv[2]);
    int rc = ps3_resume_download(argv[1], argv[2], 0);
    printf("RESULT rc=%d\n", rc);
    return rc == 0 ? 0 : 1;
}
C
        if ! gcc -O1 -w -DNASDL_NETDEBUG -I"$W/shim" -I"$REPO/ps3" \
                -o "$W/dl_host" "$W/dl_host.c" "$REPO/ps3/resume.c" 2>"$W/cc.log"; then
            echo "  skip: could not build the harness"; cat "$W/cc.log"
        else
            out="$( "$W/dl_host" "$BASE/PSP/big.bin" "$W/out/big.bin" 2>"$W/dl1.log" )"
            chk "cold download rc=0" 0 "$(printf '%s' "$out" | sed -n 's/^RESULT rc=//p')"
            chk "cold download length" "$SIZE2" "$(wc -c < "$W/out/big.bin")"
            chk "digest equals the sidecar (X-SHA256 compared)" "$BIGSHA" \
                "$(sha256sum "$W/out/big.bin" | cut -d' ' -f1)"
            chk "resume from a half file rc=0" 0 "$(
                SOURCE_FILE="$W/games/PSP/big.bin" \
                "$W/dl_host" "$BASE/PSP/big.bin" "$W/out/big.bin" $((SIZE2 / 2)) \
                    2>"$W/dl2.log" | sed -n 's/^RESULT rc=//p')"
            chk "it asked for the right offset" yes \
                "$(grep -q "Range offset=$((SIZE2 / 2))" "$W/dl2.log" && echo yes || echo no)"
            chk "server answered 206, not a restart" yes \
                "$(grep -q 'status=206' "$W/dl2.log" && echo yes || echo no)"
            chk "resumed file is byte-identical" "$BIGSHA" \
                "$(sha256sum "$W/out/big.bin" | cut -d' ' -f1)"
            printf '%064d\n' 1 > "$W/games/PSP/big.bin.sha256"
            chk "a wrong server digest is refused (rc=-11)" -11 "$(
                "$W/dl_host" "$BASE/PSP/big.bin" "$W/out/big.bin" 2>/dev/null \
                    | sed -n 's/^RESULT rc=//p')"
            rm -f "$W/games/PSP/big.bin.sha256"
            chk "no sidecar still downloads" 0 "$(
                "$W/dl_host" "$BASE/PSP/big.bin" "$W/out/big.bin" 2>/dev/null \
                    | sed -n 's/^RESULT rc=//p')"
        fi
    fi
fi

echo
echo "passed=$pass failed=$fail"
[ "$fail" -eq 0 ]
