#!/bin/sh
# P16.2 regression — docroot static responses carry validators, answer 304
# on a conditional hit, and route HEAD. Parity target: agent-httpd's
# src/http/static.c (weak ETag W/"<size>-<mtime>", Cache-Control: no-cache,
# If-None-Match taking precedence over If-Modified-Since).
#
# Wired into `make test`. Self-contained: it builds its own docroot and .lume
# server in a private temp dir on port 18893, so it can run under `make -j4
# test` next to docroot-test (18892) without stealing its listener.
#
# Assertions come in 200/304 pairs on purpose: a blinded status/header
# matcher cannot turn this gate green, because the opposite outcome is
# asserted a few lines later. The matcher itself is self-checked too.

set -eu

cd "$(dirname "$0")/.."

BIN=./bin/lume-core
if [ ! -x "${BIN}" ]; then BIN=./bin/lume-core.exe; fi
if [ ! -x "${BIN}" ]; then
    echo "FAIL static-cache: build the binary first (make all)"
    exit 1
fi

PORT=${LUME_STATIC_CACHE_PORT:-18893}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/lume-static-cache.XXXXXX")
SRV=""
cleanup() {
    if [ -n "${SRV}" ]; then
        kill "${SRV}" 2>/dev/null || :
        wait "${SRV}" 2>/dev/null || :
    fi
    if [ -n "${TMP}" ] && [ -d "${TMP}" ]; then rm -rf "${TMP}"; fi
}
trap cleanup 0 1 2 15

mkdir -p "${TMP}/root"
LUME_TMP_ROOT="${TMP}/root" python3 -S -c '
import os
p = os.path.join(os.environ["LUME_TMP_ROOT"], "app.js")
open(p, "w").write("// static-cache fixture v1\n" + "x" * 204800)
'
FILE="${TMP}/root/app.js"
SIZE=$(wc -c < "${FILE}" | tr -d ' ')
URL="http://127.0.0.1:${PORT}"

cat > "${TMP}/srv.lume" <<EOF
server {
  port = ${PORT};
  docroot = "${TMP}/root";
}
run();
EOF

expect_eq() {
    want=$1; got=$2; label=$3
    if [ "${want}" != "${got}" ]; then
        echo "FAIL static-cache ${label}: got '${got}', want '${want}'"
        exit 1
    fi
    echo "ok   static-cache ${label}"
}

# Self-check 1: a deliberately wrong expectation must be rejected. A matcher
# that always succeeds would let every assertion below pass vacuously.
if ( expect_eq 200 304 matcher-selftest >/dev/null 2>&1 ); then
    echo "FAIL static-cache: assertion matcher is blind"
    exit 1
fi

# Self-check 2: this bridge has NO Range implementation. Advertising
# "Accept-Ranges: bytes" would make clients retry ranges we ignore.
if grep -q 'Accept-Ranges: bytes' src/bridge_serve.c; then
    echo "FAIL static-cache: bridge_serve advertises Range support it lacks"
    exit 1
fi

# Independent expectations, computed without the C date/etag code under test.
EXPECT_LM=$(python3 -S -c '
import os, sys, time
g = time.gmtime(int(os.stat(sys.argv[1]).st_mtime))
d = ["Mon","Tue","Wed","Thu","Fri","Sat","Sun"]
m = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"]
print("%s, %02d %s %04d %02d:%02d:%02d GMT" %
      (d[g.tm_wday], g.tm_mday, m[g.tm_mon-1], g.tm_year,
       g.tm_hour, g.tm_min, g.tm_sec))
' "${FILE}")
EXPECT_ETAG=$(python3 -S -c '
import os, sys
s = os.stat(sys.argv[1])
print("W/\"%x-%x\"" % (s.st_size, int(s.st_mtime)))
' "${FILE}")

# --- curl plumbing -------------------------------------------------------
# -D captures raw headers, -o the body. curl does not create/truncate the -o
# file for a bodyless 304, so pre-create it and read the byte count from it.
req() {
    tag=$1; shift
    : > "${TMP}/${tag}.body"
    : > "${TMP}/${tag}.hdr"
    curl -s --noproxy '*' -D "${TMP}/${tag}.hdr" -o "${TMP}/${tag}.body" \
         -w '%{size_download}' "$@" > "${TMP}/${tag}.size" \
         2>"${TMP}/${tag}.err" || :
}
# curl -I writes the header block into the -o file, so body emptiness must be
# measured with %{size_download} (the actual wire body bytes), not that file.
req_head() {
    tag=$1; shift
    : > "${TMP}/${tag}.hdr"
    curl -s --noproxy '*' -I -D "${TMP}/${tag}.hdr" -o /dev/null \
         -w '%{size_download}' "$@" > "${TMP}/${tag}.size" \
         2>"${TMP}/${tag}.err" || :
}
code()    { awk 'NR==1 {print $2}' "$1"; }
hdr_get() {
    grep -i "^$2:" "$1" | head -1 | cut -d: -f2- | \
        sed 's/^[[:space:]]*//' | tr -d '\r'
}
has_hdr() { grep -qi "^$2:" "$1"; }
bsize()   { wc -c < "$1" | tr -d ' '; }
no_hdr() {
    if has_hdr "$1" "$2"; then
        echo "FAIL static-cache $3: unexpected $2 header"
        exit 1
    fi
    echo "ok   static-cache $3"
}

"${BIN}" "${TMP}/srv.lume" > "${TMP}/srv.log" 2>&1 &
SRV=$!
i=0
while [ "$i" -lt 60 ]; do
    if curl -s --noproxy '*' -o /dev/null "${URL}/app.js"; then break; fi
    i=$((i + 1))
    sleep 0.1
done
if [ "$i" -ge 60 ]; then
    echo "FAIL static-cache: server did not start on ${PORT}"
    cat "${TMP}/srv.log"
    exit 1
fi

# --- 1. plain 200 with validators ---------------------------------------
req get "${URL}/app.js"
expect_eq 200 "$(code "${TMP}/get.hdr")" "plain GET is 200"
expect_eq "${SIZE}" "$(bsize "${TMP}/get.body")" "GET body is the whole file"
ETAG=$(hdr_get "${TMP}/get.hdr" etag)
LM=$(hdr_get "${TMP}/get.hdr" last-modified)
CL=$(hdr_get "${TMP}/get.hdr" content-length)
expect_eq "${EXPECT_ETAG}" "${ETAG}" "ETag is W/\"size-mtime\" in hex"
expect_eq "${EXPECT_LM}" "${LM}" "Last-Modified is UTC IMF-fixdate"
expect_eq "${SIZE}" "${CL}" "Content-Length matches the file"
expect_eq "no-cache" "$(hdr_get "${TMP}/get.hdr" cache-control)" \
          "Cache-Control: no-cache"
expect_eq "none" "$(hdr_get "${TMP}/get.hdr" accept-ranges)" \
          "Accept-Ranges: none (Range is NOT implemented)"
has_hdr "${TMP}/get.hdr" x-frame-options || {
    echo "FAIL static-cache: security headers missing on 200"; exit 1; }
echo "ok   static-cache security headers on 200"

# --- 2. conditional GET -> 304, bodyless, validators retained -----------
req inm -H "If-None-Match: ${ETAG}" "${URL}/app.js"
expect_eq 304 "$(code "${TMP}/inm.hdr")" "If-None-Match replay is 304"
expect_eq 0 "$(cat "${TMP}/inm.size")" "304 has an empty body"
expect_eq "${ETAG}" "$(hdr_get "${TMP}/inm.hdr" etag)" "304 repeats ETag"
expect_eq "no-cache" "$(hdr_get "${TMP}/inm.hdr" cache-control)" \
          "304 repeats Cache-Control"
has_hdr "${TMP}/inm.hdr" x-frame-options || {
    echo "FAIL static-cache: security headers missing on 304"; exit 1; }
echo "ok   static-cache security headers on 304"
no_hdr "${TMP}/inm.hdr" content-length "304 omits Content-Length"
no_hdr "${TMP}/inm.hdr" content-type "304 omits Content-Type"

req star -H 'If-None-Match: *' "${URL}/app.js"
expect_eq 304 "$(code "${TMP}/star.hdr")" "If-None-Match: * is 304"

BARE=${ETAG#W/}
req list -H "If-None-Match: \"nope\", W/${BARE}, \"other\"" "${URL}/app.js"
expect_eq 304 "$(code "${TMP}/list.hdr")" "weak ETag inside a list matches"

# --- 3. If-None-Match wins over If-Modified-Since (RFC 9110 13.2.2) -----
req mismatch -H 'If-None-Match: "bogus"' \
             -H "If-Modified-Since: ${LM}" "${URL}/app.js"
expect_eq 200 "$(code "${TMP}/mismatch.hdr")" \
          "unmatched If-None-Match suppresses If-Modified-Since"
expect_eq "${SIZE}" "$(bsize "${TMP}/mismatch.body")" \
          "suppressed-IMS response still carries the body"

req ims -H "If-Modified-Since: ${LM}" "${URL}/app.js"
expect_eq 304 "$(code "${TMP}/ims.hdr")" "If-Modified-Since replay is 304"
expect_eq 0 "$(cat "${TMP}/ims.size")" "IMS 304 has an empty body"

req imsold -H 'If-Modified-Since: Sun, 06 Nov 1994 08:49:37 GMT' \
           "${URL}/app.js"
expect_eq 200 "$(code "${TMP}/imsold.hdr")" "older If-Modified-Since is 200"

req imsbad -H 'If-Modified-Since: not-a-date' "${URL}/app.js"
expect_eq 200 "$(code "${TMP}/imsbad.hdr")" "malformed IMS is ignored (200)"

# --- 4. HEAD ------------------------------------------------------------
req_head head "${URL}/app.js"
expect_eq 200 "$(code "${TMP}/head.hdr")" "HEAD is 200 (no longer 404)"
expect_eq 0 "$(cat "${TMP}/head.size")" "HEAD has an empty body"
expect_eq "${CL}" "$(hdr_get "${TMP}/head.hdr" content-length)" \
          "HEAD Content-Length equals the GET body"
expect_eq "${ETAG}" "$(hdr_get "${TMP}/head.hdr" etag)" \
          "HEAD carries the same ETag"

req_head head304 -H "If-None-Match: ${ETAG}" "${URL}/app.js"
expect_eq 304 "$(code "${TMP}/head304.hdr")" "conditional HEAD is 304"
expect_eq 0 "$(cat "${TMP}/head304.size")" "conditional HEAD has no body"

req_head head404 "${URL}/missing.js"
expect_eq 404 "$(code "${TMP}/head404.hdr")" "HEAD on a missing file is 404"
expect_eq 0 "$(cat "${TMP}/head404.size")" "HEAD 404 has no body"

# --- 5. errors stay validator-free and traversal stays blocked ----------
req get404 "${URL}/missing.js"
expect_eq 404 "$(code "${TMP}/get404.hdr")" "GET on a missing file is 404"
no_hdr "${TMP}/get404.hdr" etag "404 has no ETag"
req trav --path-as-is "${URL}/../secret.txt"
expect_eq 404 "$(code "${TMP}/trav.hdr")" "path traversal still 404"

# --- 6. a changed file invalidates the old validator --------------------
printf 'more-bytes\n' >> "${FILE}"
req changed "${URL}/app.js"
NEW_ETAG=$(hdr_get "${TMP}/changed.hdr" etag)
expect_eq 200 "$(code "${TMP}/changed.hdr")" "changed file still serves 200"
if [ "${NEW_ETAG}" = "${ETAG}" ]; then
    echo "FAIL static-cache: ETag did not change after the file changed"
    exit 1
fi
echo "ok   static-cache ETag changes with the file"
req stale -H "If-None-Match: ${ETAG}" "${URL}/app.js"
expect_eq 200 "$(code "${TMP}/stale.hdr")" "stale ETag no longer gets a 304"

echo "==> static-cache 单测通过 (200/304/HEAD/validators/Range honesty)"
