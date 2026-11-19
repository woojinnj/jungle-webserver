#!/usr/bin/env bash
# End-to-end test: real server, two Python backends, curl and raw TCP.
#   bash tests/integration.sh build/webserver
cd "$(dirname "$0")/.." || exit 2
BIN=${1:-build/webserver}
P=18080; A=18001; B=18002; D=18999
TMP=$(mktemp -d)
pass=0; fails=0
pids=()

cleanup() { for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

ok()   { pass=$((pass + 1)); }
bad()  { fails=$((fails + 1)); echo "FAIL: $*"; }
check() { if [ "$2" == "$3" ]; then ok; else bad "$1: want '$2' got '$3'"; fi; }
has()   { case "$3" in *"$2"*) ok ;; *) bad "$1: '$3' lacks '$2'" ;; esac; }

URL=http://127.0.0.1:$P
code() { curl -s -o /dev/null -w '%{http_code}' --max-time 20 "$@"; }
# raw TCP: send $1 (printf %b escapes), print the reply's first line
raw() {
    exec 3<>/dev/tcp/127.0.0.1/$P || return 1
    printf '%b' "$1" >&3
    local line; IFS= read -r -t 5 line <&3
    exec 3<&- 3>&-
    printf '%s' "${line%$'\r'}"
}
# raw TCP: print the whole reply until the server closes
raw_all() {
    exec 3<>/dev/tcp/127.0.0.1/$P || return 1
    printf '%b' "$1" >&3
    timeout 3 cat <&3
    exec 3<&- 3>&-
}

[ -x "$BIN" ] || { echo "no binary $BIN (run make)"; exit 2; }

# site = ./www plus a 5MB file
cp -r www "$TMP/site"
head -c 5242880 /dev/urandom > "$TMP/site/big.bin"
big_sum=$(sha256sum < "$TMP/site/big.bin")

python3 tests/backend.py $A A & pids+=($!)
python3 tests/backend.py $B B & pids+=($!)
BPID=${pids[1]}
"$BIN" -p $P -r "$TMP/site" -w 16 -t 2 \
    -R /api=127.0.0.1:$A,127.0.0.1:$B -R /dead=127.0.0.1:$D \
    -R /slow=127.0.0.1:$A -R /chunked=127.0.0.1:$A -R /noclen=127.0.0.1:$A \
    2> "$TMP/server.log" & SPID=$!; pids+=($SPID)

for _ in $(seq 50); do curl -s -o /dev/null "$URL/" && break; sleep 0.1; done
for _ in $(seq 50); do curl -s -o /dev/null "http://127.0.0.1:$A/" && break; sleep 0.1; done

echo "== static files"
check "GET /" 200 "$(code $URL/)"
has "index body" "cwebd" "$(curl -s $URL/)"
has "html type" "text/html" "$(curl -sI $URL/ | grep -i '^content-type')"
has "css type" "text/css" "$(curl -sI $URL/style.css | grep -i '^content-type')"
has "js type" "text/javascript" "$(curl -sI $URL/app.js | grep -i '^content-type')"
check "404" 404 "$(code $URL/nope)"
check "traversal" 403 "$(code --path-as-is $URL/../etc/passwd)"
check "encoded traversal" 403 "$(code --path-as-is $URL/%2e%2e/etc/passwd)"
check "bad percent" 400 "$(code --path-as-is $URL/%zz)"
check "dir redirect" 301 "$(code $URL/docs)"
has "redirect location" "Location: /docs/" "$(curl -sI $URL/docs)"
check "dir index" 200 "$(code $URL/docs/)"
check "HEAD" 200 "$(code -I $URL/)"
check "HEAD has no body" 0 "$(curl -sI -w '%{size_download}' -o /dev/null $URL/)"
check "big file intact" "$big_sum" "$(curl -s $URL/big.bin | sha256sum)"
check "query string ignored" 200 "$(code "$URL/index.html?a=b")"

echo "== methods and bodies"
check "POST echo" "hello" "$(curl -s -d hello $URL/echo)"
check "POST echo 1MB" "$(head -c 1048576 /dev/zero | sha256sum)" \
      "$(head -c 1048576 /dev/zero | curl -s --data-binary @- $URL/echo | sha256sum)"
check "POST static" 405 "$(code -d x $URL/index.html)"
check "DELETE static" 405 "$(code -X DELETE $URL/index.html)"
check "body > limit" 413 "$(head -c 9437184 /dev/zero | code --data-binary @- $URL/echo)"

echo "== malformed requests"
check "garbage" "HTTP/1.1 400 Bad Request" "$(raw 'GARBAGE\r\n\r\n')"
check "bad version" "HTTP/1.1 505 HTTP Version Not Supported" "$(raw 'GET / HTTP/9.9\r\n\r\n')"
check "no Host" "HTTP/1.1 400 Bad Request" "$(raw 'GET / HTTP/1.1\r\n\r\n')"
check "chunked request" "HTTP/1.1 501 Not Implemented" "$(raw 'POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n')"
long=$(head -c 3000 /dev/zero | tr '\0' a)
check "long URI" "HTTP/1.1 414 URI Too Long" "$(raw "GET /$long HTTP/1.1\r\nHost: x\r\n\r\n")"
hdr=$(head -c 20000 /dev/zero | tr '\0' b)
check "huge header" "HTTP/1.1 431 Request Header Fields Too Large" "$(raw "GET / HTTP/1.1\r\nHost: x\r\nX: $hdr\r\n\r\n")"
check "unknown method" "HTTP/1.1 405 Method Not Allowed" "$(raw 'BREW /pot HTTP/1.1\r\nHost: x\r\n\r\n')"
# client sends half a request and vanishes
raw 'GET /ind' > /dev/null 2>&1 &
wait $! 2>/dev/null
(exec 3<>/dev/tcp/127.0.0.1/$P; printf 'POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: 100\r\n\r\nabc' >&3; exec 3<&-)
check "alive after aborts" 200 "$(code $URL/)"

echo "== keep-alive"
v=$(curl -sv -o /dev/null $URL/ -o /dev/null $URL/index.html 2>&1)
check "connection reused" 1 "$(grep -ciE 're-?using existing' <<< "$v")"
has "Connection: close honoured" "Connection: close" "$(curl -sI -H 'Connection: close' $URL/)"
two=$(raw_all 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n')
check "pipelined requests" 2 "$(grep -c '^HTTP/1.1 200' <<< "$two")"

echo "== reverse proxy"
seen=""
for i in 1 2 3 4; do seen+=$(curl -s "$URL/api/x?y=1" | awk '{print $2}'); done
case "$seen" in ABAB | BABA) ok ;; *) bad "round robin: got '$seen'" ;; esac
r=$(curl -s "$URL/api/x?y=1")
has "path+query kept" "path=/api/x?y=1" "$r"
has "X-Forwarded-For" "xff=127.0.0.1" "$r"
has "Host rewritten for backend" "host=127.0.0.1:" "$r"
has "POST body forwarded" "len=3" "$(curl -s -d abc $URL/api/p)"
has "1MB POST forwarded" "len=1048576" "$(head -c 1048576 /dev/zero | curl -s --data-binary @- $URL/api/p)"
has "PUT forwarded" "path=/api/put" "$(curl -s -X PUT -d z $URL/api/put)"
check "HEAD via proxy" 200 "$(code -I $URL/api/x)"
check "dead backend" 502 "$(code $URL/dead)"
check "slow backend" 504 "$(code $URL/slow)"
c=$(curl -si $URL/chunked)
has "chunked relayed" "hello chunked" "$(curl -s $URL/chunked | tr -d '\r\n0')"
has "chunked closes client conn" "Connection: close" "$c"
check "no Content-Length reply" "until-eof" "$(curl -s $URL/noclen)"
# client gives up on a slow upstream request
timeout 0.5 curl -s $URL/slow > /dev/null; sleep 0.2
check "alive after client abort" 200 "$(code $URL/)"
# failover: kill backend B, every request must still succeed through A
kill "$BPID"; wait "$BPID" 2>/dev/null; sleep 0.3
okc=0; for i in $(seq 8); do
    [[ "$(curl -s $URL/api/x)" == "backend A "* ]] && okc=$((okc + 1)); done
check "failover to live backend" 8 "$okc"

echo "== concurrency"
n=$(seq 300 | xargs -P 60 -I{} curl -s -o /dev/null -w '%{http_code}\n' $URL/style.css | grep -c '^200$')
check "300 concurrent static" 300 "$n"
n=$(seq 200 | xargs -P 40 -I{} curl -s -o /dev/null -w '%{http_code}\n' $URL/api/x | grep -c '^200$')
check "200 concurrent proxied" 200 "$n"

echo "== shutdown"
kill -TERM "$SPID"
for _ in $(seq 30); do kill -0 "$SPID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$SPID" 2>/dev/null; then bad "server still running 3s after SIGTERM"; else ok; fi
wait "$SPID" 2>/dev/null
check "exit code" 0 "$?"
has "clean shutdown message" "bye" "$(cat "$TMP/server.log")"

echo
echo "passed: $pass  failed: $fails"
[ "$fails" -eq 0 ]
