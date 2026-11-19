#!/usr/bin/env bash
# 부하 테스트: ab(Apache Bench)로 스레드 수 / 동시 접속 수에 따른 처리량(Requests/sec)을 잰다.
# 사용: make load   (ab 필요: apt install apache2-utils)
set -u
cd "$(dirname "$0")/.."
PORT=18180
N=${N:-20000}

command -v ab >/dev/null || { echo "ab 가 없습니다 (apt install apache2-utils)"; exit 1; }

run() {   # run <threads> <concurrency> <keepalive:-k 또는 빈 값> <path>
  ./webserver -p $PORT -t "$1" 2>/dev/null & local pid=$!
  sleep 0.3
  local out; out=$(ab -q -n "$N" -c "$2" $3 "http://127.0.0.1:$PORT$4" 2>&1)
  kill -INT $pid; wait $pid 2>/dev/null
  printf "| %2s | %4s | %-3s | %-22s | %9s | %7s | %6s |\n" "$1" "$2" "${3:-no}" "$4" \
    "$(grep 'Requests per second' <<<"$out" | awk '{print $4}')" \
    "$(grep 'Time per request' <<<"$out" | head -1 | awk '{print $4}')" \
    "$(grep 'Failed requests' <<<"$out" | awk '{print $3}')"
}

echo "| threads | concurrency | keep-alive | path | req/sec | ms/req | failed |"
echo "|---|---|---|---|---|---|---|"
run 1  50  ""  /index.html
run 8  50  ""  /index.html
run 8  50  -k  /index.html
run 8  200 -k  /index.html
run 8  50  -k  "/cgi-bin/adder?a=1&b=2"
