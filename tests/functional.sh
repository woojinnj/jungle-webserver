#!/usr/bin/env bash
# 기능 테스트: 실제 서버 + 백엔드 2개를 띄우고 curl / 원시 소켓으로 확인한다.
# 사용: make functional   (저장소 루트에서 실행)
set -u
cd "$(dirname "$0")/.."

PORT=18080; B1=18001; B2=18002; SLOW=18003; DEAD=18999
LOGDIR=$(mktemp -d)
PASS=0; FAIL=0
PIDS=()

ok()   { PASS=$((PASS+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
fail() { FAIL=$((FAIL+1)); printf '  \033[31mFAIL\033[0m %s (got: %s)\n' "$1" "$2"; }

# 기대한 값과 같으면 PASS
expect() { if [ "$2" = "$3" ]; then ok "$1"; else fail "$1" "$2"; fi; }

# 상태코드만 가져오기
status() { curl -s -o /dev/null -w '%{http_code}' "$@"; }

# 원시 바이트를 보내고 응답 첫 줄의 상태코드를 돌려준다 (curl 이 못 만드는 잘못된 요청용)
raw_status() {
  python3 - "$PORT" "$1" <<'EOF'
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=5)
s.sendall(sys.argv[2].encode().decode("unicode_escape").encode("latin-1"))
data = s.recv(4096).decode("latin-1")
print(data.split(" ")[1] if data.startswith("HTTP/") else "none")
EOF
}

wait_port() { for _ in $(seq 50); do (exec 3<>/dev/tcp/127.0.0.1/$1) 2>/dev/null && return 0; sleep 0.1; done; return 1; }

cleanup() { for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done; wait 2>/dev/null; }
trap cleanup EXIT

# --- 서버 기동 ---------------------------------------------------------------
./webserver -p $B1 -r backends/b1 2>"$LOGDIR/b1.log" & PIDS+=($!)
./webserver -p $B2 -r backends/b2 2>"$LOGDIR/b2.log" & PIDS+=($!)
# 응답하지 않는 백엔드 (504 확인용): 연결만 받고 아무것도 보내지 않는다
python3 -c "
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('127.0.0.1',$SLOW)); s.listen(16); conns=[]
while True: c,_=s.accept(); conns.append(c)
" & PIDS+=($!)
./webserver -p $PORT -r www -c cgi-bin -t 8 -F \
  -R /api=127.0.0.1:$B1,127.0.0.1:$B2 -R /dead=127.0.0.1:$DEAD -R /slow=127.0.0.1:$SLOW \
  2>"$LOGDIR/main.log" &
MAIN=$!
for p in $PORT $B1 $B2 $SLOW; do wait_port $p || { echo "port $p not ready"; exit 1; }; done
URL=http://127.0.0.1:$PORT

echo "== 정적 파일 / MIME"
expect "GET / → 200"                 "$(status $URL/)" 200
expect "index.html Content-Type"     "$(curl -s -o /dev/null -w '%{content_type}' $URL/)" "text/html; charset=utf-8"
expect "css Content-Type"            "$(curl -s -o /dev/null -w '%{content_type}' $URL/css/style.css)" "text/css; charset=utf-8"
expect "js Content-Type"             "$(curl -s -o /dev/null -w '%{content_type}' $URL/js/app.js)" "application/javascript; charset=utf-8"
expect "png Content-Type"            "$(curl -s -o /dev/null -w '%{content_type}' $URL/img/logo.png)" "image/png"
curl -s $URL/img/logo.png -o "$LOGDIR/logo.png"
expect "png 바이너리 내용 일치"        "$(cmp -s "$LOGDIR/logo.png" www/img/logo.png && echo same)" same

echo "== 오류 응답"
expect "없는 파일 → 404"              "$(status $URL/nope.html)" 404
expect "../ 경로 탈출 → 403"          "$(status --path-as-is $URL/../Makefile)" 403
expect "정적 파일에 POST → 405"       "$(status -d x=1 $URL/index.html)" 405
expect "DELETE 메서드 → 501"          "$(status -X DELETE $URL/)" 501
expect "HTTP/2.0 버전 → 505"          "$(raw_status 'GET / HTTP/2.0\r\nHost: a\r\n\r\n')" 505
expect "깨진 요청 라인 → 400"          "$(raw_status 'HELLO\r\n\r\n')" 400
expect "Host 없는 HTTP/1.1 → 400"     "$(raw_status 'GET / HTTP/1.1\r\n\r\n')" 400
expect "아주 긴 URI(10KB) → 414"      "$(status $URL/$(head -c 10000 /dev/zero | tr '\0' a))" 414
expect "아주 큰 헤더(10KB) → 431"     "$(status -H "X-Big: $(head -c 10000 /dev/zero | tr '\0' b)" $URL/)" 431
head -c 2000000 /dev/zero > "$LOGDIR/big.bin"
expect "아주 큰 본문(2MB) → 413"      "$(status -H 'Expect:' --data-binary @"$LOGDIR/big.bin" $URL/cgi-bin/echo)" 413

echo "== 동적 처리 (CGI)"
expect "GET CGI adder"               "$(curl -s "$URL/cgi-bin/adder?a=3&b=4")" "3 + 4 = 7"
expect "POST CGI echo 본문"           "$(curl -s -d 'msg=hello' $URL/cgi-bin/echo | tail -1)" "msg=hello"
head -c 200000 /dev/urandom | base64 -w0 > "$LOGDIR/post.txt"
expect "POST CGI 큰 본문(266KB) 왕복"  "$(curl -s -H 'Expect:' --data-binary @"$LOGDIR/post.txt" $URL/cgi-bin/echo | tail -1 | cmp -s - "$LOGDIR/post.txt" && echo same)" same
expect "없는 CGI → 404"               "$(status $URL/cgi-bin/nope)" 404

echo "== keep-alive"
reuse=$(curl -sv $URL/ $URL/css/style.css -o /dev/null -o /dev/null 2>&1 | grep -c -i 're-using\|reusing')
expect "한 연결로 요청 2개 처리"        "$([ "$reuse" -ge 1 ] && echo yes)" yes
expect "Connection: close 응답 헤더"   "$(curl -s -D - -o /dev/null -H 'Connection: close' $URL/ | grep -i '^connection' | tr -d '\r')" "Connection: close"

echo "== Reverse Proxy"
rr=$(for i in 1 2 3 4; do curl -s $URL/api/hello.json; done | tr -d '\n')
expect "Round-Robin 분배 (1→2→1→2)"   "$rr" '{"from":"backend-1"}{"from":"backend-2"}{"from":"backend-1"}{"from":"backend-2"}'
expect "백엔드 꺼짐 → 502"            "$(status $URL/dead/x)" 502
expect "백엔드 응답 없음 → 504"        "$(status $URL/slow/x)" 504
expect "라우팅 규칙 밖은 직접 서빙"     "$(status $URL/index.html)" 200

echo "== Forward Proxy"
# --noproxy '': 환경변수 no_proxy 에 127.0.0.1 이 있으면 curl 이 -x 를 무시하므로 강제로 프록시 사용
expect "curl -x 로 원 서버 중계"       "$(curl -s --noproxy '' -x $URL http://127.0.0.1:$B1/)" "backend-1"
expect "프록시 서버 로그에 기록됨"      "$(grep -c "GET http://127.0.0.1:$B1/ -> 200" "$LOGDIR/main.log")" 1
expect "원 서버 없음 → 502"           "$(status --noproxy '' -x $URL http://127.0.0.1:$DEAD/)" 502

echo "== 동시 접속 / 비정상 종료"
codes=$(seq 200 | xargs -P 50 -I{} curl -s -o /dev/null -w '%{http_code}\n' $URL/ | sort | uniq -c | tr -s ' ')
expect "동시 200요청(50병렬) 모두 200"  "$codes" " 200 200"
python3 -c "
import socket
for _ in range(20):
    s=socket.create_connection(('127.0.0.1',$PORT)); s.sendall(b'GET / HTTP/1.1\r\nHo'); s.close()
for _ in range(20):
    s=socket.create_connection(('127.0.0.1',$PORT)); s.sendall(b'GET /img/logo.png HTTP/1.1\r\nHost: a\r\n\r\n'); s.close()
"
expect "갑자기 끊긴 연결 40개 후에도 정상" "$(status $URL/)" 200

echo "== 종료 처리"
kill -TERM $MAIN
for _ in $(seq 100); do kill -0 $MAIN 2>/dev/null || break; sleep 0.1; done
wait $MAIN; rc=$?
expect "SIGTERM → 정상 종료(exit 0)"   "$rc" 0
expect "종료 로그 출력"                "$(grep -c 'shutting down' "$LOGDIR/main.log")" 1
san=$(cat "$LOGDIR"/*.log | grep -c -E 'AddressSanitizer|LeakSanitizer|runtime error')
expect "sanitizer 오류 없음"           "$san" 0

echo
echo "기능 테스트: $PASS passed, $FAIL failed  (logs: $LOGDIR)"
[ $FAIL -eq 0 ]
