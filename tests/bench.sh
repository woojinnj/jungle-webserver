#!/usr/bin/env bash
# Load test with wrk:  bash tests/bench.sh build/webserver
cd "$(dirname "$0")/.." || exit 2
BIN=${1:-build/webserver}
P=18090; A=18011; B=18012
pids=(); TMP=$(mktemp -d)
trap 'for p in "${pids[@]}"; do kill $p 2>/dev/null; done; rm -rf "$TMP"' EXIT
cp -r www "$TMP/www"   # native fs: /mnt/c (9p) would dominate the numbers

python3 tests/backend.py $A A & pids+=($!)
python3 tests/backend.py $B B & pids+=($!)
"$BIN" -q -p $P -r "$TMP/www" -w 64 -R /api=127.0.0.1:$A,127.0.0.1:$B & pids+=($!)
sleep 1

run() { echo "--- $1  (wrk -t4 -c$2 -d5s)"; wrk -t4 -c"$2" -d5s "http://127.0.0.1:$P$3" | grep -E "Requests/sec|Latency|Socket|Non-2xx"; }
run "static index.html, keep-alive" 50 /
run "static index.html, keep-alive" 200 /
run "reverse proxy -> python backends" 50 /api/x
echo "--- ab, no keep-alive: 20000 requests, 100 concurrent"
ab -q -n 20000 -c 100 http://127.0.0.1:$P/ 2>&1 | grep -E "Requests per second|Failed requests|Non-2xx"
