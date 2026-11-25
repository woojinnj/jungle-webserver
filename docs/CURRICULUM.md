# cwebd 코드 읽기 커리큘럼

C 언어 기초(포인터, 구조체, 함수)만 알면 이 커리큘럼으로 cwebd의 전체 흐름과 각 함수의 역할을 이해할 수 있습니다.
총 8단계이고, 한 단계는 30분~1시간 정도를 잡았습니다. **각 단계는 "먼저 실행해 보고 → 코드를 읽고 → 직접 바꿔 보는"** 순서입니다.

실행 환경은 Linux, macOS, WSL2 중 하나이고 `make`가 필요합니다. 코드 위치는 모두 `src/`입니다.

## 0단계. 이게 뭔지 이해하기 (코드 읽기 전)

웹 서버는 "HTTP라는 텍스트 약속으로 온 요청을 읽고 응답을 돌려주는 프로그램"입니다.
cwebd는 그 일을 하는 프로그램을 라이브러리 없이 소켓 API만으로 만든 것입니다.

```
GET /index.html HTTP/1.1          ← 요청 라인 (메서드, 경로, 버전)
Host: localhost:8080              ← 헤더 (이름: 값)
                                  ← 빈 줄 = 헤더 끝
(POST라면 여기에 본문)

HTTP/1.1 200 OK                   ← 상태 라인
Content-Type: text/html           ← 응답 헤더
Content-Length: 1234              ← 본문 길이
                                  ← 빈 줄
<html>...                         ← 본문
```

cwebd가 하는 일은 세 가지입니다.

1. **정적 파일 서버**: `./www` 안의 파일을 그대로 보내 줍니다.
2. **Reverse Proxy**: `/api/...` 같은 경로의 요청을 뒤에 있는 백엔드 서버들에게 대신 전달하고, 응답을 받아 클라이언트에게 돌려줍니다. 백엔드가 여러 대면 번갈아 보냅니다(Round-Robin).
3. **동시 처리**: 여러 사용자의 요청을 스레드 풀로 동시에 처리합니다.

> Proxy와 Reverse Proxy의 차이: Forward Proxy는 *클라이언트*를 대신해 외부로 나가고, Reverse Proxy는 *서버*를 대신해 요청을 받습니다. cwebd는 Reverse Proxy입니다.

## 1단계. 먼저 실행해서 눈으로 보기

**목표**: 코드를 읽기 전에 서버가 무엇을 하는지 체험합니다.

```sh
make
build/webserver -p 8080 -r ./www          # 터미널 1: 서버 실행 (액세스 로그가 찍힙니다)
```

```sh
curl -v http://localhost:8080/            # 터미널 2: -v 로 요청/응답 원문을 확인
curl -i http://localhost:8080/nope        # 404
curl -i -d hello http://localhost:8080/echo   # POST: 본문을 그대로 돌려줌
printf 'GARBAGE\r\n\r\n' | nc localhost 8080  # 잘못된 요청 -> 400
```

**확인할 것**: 터미널 1의 로그 한 줄(`IP "메서드 경로" 상태 바이트`)과 터미널 2의 요청/응답을 짝지어 보세요.
`curl -v`에서 `>`로 시작하는 줄이 요청, `<`로 시작하는 줄이 응답입니다.

**과제**: 서버를 켠 채로 브라우저에서 `http://localhost:8080/`을 열고, 로그에 어떤 요청이 몇 개 찍히는지 세어 보세요(HTML 하나에 CSS, JS가 따라 요청됩니다).

## 2단계. 전체 흐름 지도 (큰 그림)

**목표**: 파일이 서로 어떻게 호출하는지 한 장으로 이해합니다.

```
main.c  main()
  │  인자 파싱 → config 채움 → 시그널 설정
  ▼
server.c  server_run()                    ← accept 전담 스레드(메인)
  │  listen_on(): socket → bind → listen
  │  while: poll → accept → pool_push (큐가 가득 차면 503)
  ▼
server.c  worker() × N                    ← 워커 스레드들, pool_pop으로 연결을 하나씩 꺼냄
  ▼
server.c  handle_conn()                   ← 연결 하나의 keep-alive 루프
  │  ① 헤더가 완성될 때까지 recv
  │  ② http_parse_request()               ← http.c : 문자열 → http_request 구조체
  │  ③ 본문 읽기 (read_body)
  │  ④ dispatch()
  ▼
server.c  dispatch()                      ← "이 요청을 누가 처리할까?" 결정
  ├─ router_match() 성공 ─▶ proxy_handle()        (proxy.c)   백엔드로 전달
  ├─ GET / HEAD          ─▶ static_serve()        (static.c)  파일 전송
  ├─ POST /echo          ─▶ http_send_response()  (http.c)    본문 되돌리기
  └─ 그 외               ─▶ 405
```

**읽는 요령**: 지금은 함수 안을 읽지 말고, `server.c`에서 `handle_conn`과 `dispatch`의 **전체 구조(for 루프, if 분기)만** 훑어보세요. 세부 내용은 뒤 단계에서 채웁니다.

**과제**: 종이에 위 그림을 보지 않고 직접 그려 보세요. `GET /api/x` 요청이 지나가는 함수들을 순서대로 적으면 `main → server_run → worker → handle_conn → http_parse_request → dispatch → router_match → proxy_handle`이 됩니다.

## 3단계. 바닥 도구: `net.c` (소켓 유틸)

**목표**: 다른 모든 파일이 쓰는 네트워크 도구 5개를 이해합니다. 파일이 짧습니다.

| 함수 | 역할 | 왜 필요한가 |
|---|---|---|
| `net_now_ms()` | 단조 증가 시계(ms) | 타임아웃 계산용. 시스템 시간이 바뀌어도 안전 |
| `net_tune(fd, ms)` | `TCP_NODELAY`, 전송 타임아웃 설정 | 작은 응답 지연 방지, 느린 클라이언트가 워커를 영원히 붙잡지 못하게 함 |
| `net_send_all(fd, buf, len)` | 전부 보낼 때까지 `send` 반복 | `send`는 일부만 보낼 수 있어서 루프가 필수 |
| `net_wait_readable(fd, ms, stop)` | 읽을 데이터가 올 때까지 최대 ms 대기 | 200ms 조각으로 `poll`하며 종료 플래그를 확인 |
| `net_connect(host, port, ms, &timed_out)` | 타임아웃 있는 연결 | 백엔드가 응답 없을 때 무한 대기하지 않기 위함 |

**핵심 개념**
- `send`/`recv`는 요청한 만큼 처리하지 않을 수 있다 → 반드시 반환값을 보고 루프를 돈다.
- 타임아웃이 없는 blocking 소켓은 상대가 멈추면 스레드도 같이 멈춘다 → `poll`로 기다린다.
- `net_connect`는 소켓을 잠깐 non-blocking으로 바꿔 `connect`를 시작하고 `poll`로 완료를 기다린 뒤, 다시 blocking으로 되돌립니다.

**과제**: `net_wait_readable`이 왜 한 번에 `poll(timeout_ms)`를 하지 않고 200ms씩 쪼개는지 설명해 보세요(힌트: `stop` 플래그, Ctrl+C).

## 4단계. HTTP 파서: `http.c` 앞부분

**목표**: 바이트 덩어리가 구조체로 바뀌는 과정을 이해합니다. 이 프로젝트에서 가장 중요한 부분입니다.

먼저 `http.h`의 `http_request` 구조체를 읽으세요. 문자열을 복사하지 않고 **원본 버퍼를 가리키는 포인터+길이** 쌍으로 들고 있습니다(zero-copy).
그래서 파서가 끝나도 원본 버퍼(`rbuf`)를 지우거나 덮어쓰면 안 됩니다.

`http_parse_request(buf, len, &req)`의 반환값은 세 가지입니다.

| 반환 | 의미 | 호출자(`handle_conn`)의 행동 |
|---|---|---|
| `0` | 성공 | 요청 처리 시작 |
| `-1` | 아직 덜 왔음 | `recv`로 더 받고 다시 호출 |
| `400`~`505` | 잘못된 요청, 그 상태코드로 응답 | 에러 응답 후 연결 종료 |

**읽는 순서**
1. `http_parse_request`: 전체 뼈대(요청 라인 → 헤더 반복 → 빈 줄 → 후처리)
2. `parse_request_line`: 메서드, URI, 버전을 자르고 검증(`414` URI 너무 김, `505` 버전 오류)
3. `parse_header_line`: `이름: 값` 한 줄 분해. 공백 앞의 콜론, 줄 이어쓰기 등을 거부
4. 함수 끝부분의 후처리: `Content-Length` 검증, `Host` 필수 확인, keep-alive 판정

**핵심 개념**
- **경계 식별**: 요청 라인 → 헤더들 → 빈 줄(`\r\n\r\n`) → 본문. 본문의 길이는 오직 `Content-Length`로 압니다.
- **Request smuggling 방지**: `Content-Length`가 두 개인데 값이 다르거나, `Transfer-Encoding`과 같이 오면 `400`.
- **크기 제한**: URI 2048, 헤더 한 줄 8192, 헤더 전체 16384, 헤더 개수 64 → 초과하면 `414`/`431`.
- **keep-alive 규칙**: HTTP/1.1은 기본 유지(`Connection: close`면 종료), HTTP/1.0은 기본 종료(`keep-alive`가 있어야 유지).
- 파서가 **여러 번 호출**될 수 있다는 점에 주의하세요. 데이터가 조각조각 도착하므로, 매번 처음부터 다시 파싱해도 같은 결과여야 합니다.

**과제**
1. `tests/test_http.c`의 `test_bad_requests`를 읽고, 각 줄이 어떤 방어를 검증하는지 한 줄씩 적어 보세요.
2. `printf 'GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n' | nc localhost 8080`을 보내 `400`을 확인하세요.
3. (도전) 새 메서드 `PUT`을 `http_method` enum에 추가하려면 어느 곳을 고쳐야 할까요?

## 5단계. 응답 만들기와 파일 서빙: `http.c` 뒷부분, `mime.c`, `static.c`

**목표**: 응답 한 개가 만들어져 나가는 과정을 이해합니다.

**응답 생성 (`http.c`)**

| 함수 | 역할 |
|---|---|
| `http_format_head` | `HTTP/1.1 200 OK` + 헤더들을 문자열로 조립 (`Content-Length`, `Connection`, `Date` 포함) |
| `http_send_response` | 헤더+본문 전송. 작은 본문은 한 번의 `send`로 합쳐 패킷 수를 줄임 |
| `http_send_error` | 상태코드만 주면 간단한 HTML 에러 페이지를 만들어 보냄 |
| `http_status_text` | `404` → `"Not Found"` |

**MIME (`mime.c`)**: `mime_type(path)`는 확장자를 대소문자 구분 없이 표에서 찾고, 없으면 `application/octet-stream`을 돌려줍니다. 브라우저는 이 값을 보고 CSS인지 이미지인지 판단합니다.

**정적 파일 (`static.c`)**: 보안상 가장 신경 쓴 부분입니다.

| 함수 | 역할 |
|---|---|
| `static_resolve(root, uri, ...)` | URL 경로 → 파일 경로. `?query` 제거, `%XX` 해독, `..` 세그먼트 거부(`403`) |
| `static_serve(fd, root, req, ...)` | 실제 서빙. 아래 순서로 검사 후 전송 |

`static_serve`가 파일을 보내기 전에 거치는 관문:
1. `static_resolve` → 경로 해석 (`..` 차단, 잘못된 `%` 인코딩은 `400`)
2. `realpath` → 심볼릭 링크를 풀어 **진짜 경로**를 구함. 루트 밖이면 `403`
3. `stat` → 디렉터리면 `/`로 끝나는지 확인(아니면 `301`), `index.html` 붙이기
4. 일반 파일인지 확인(FIFO, 장치 파일은 열지 않음) → `open` → 헤더 전송 → 32KB씩 `read`/`send`

**핵심 개념**
- 디코딩을 **먼저** 하고 `..`를 검사해야 `%2e%2e`로 우회하지 못합니다.
- 문자열 검사만으로는 심볼릭 링크 탈출을 못 막으므로 `realpath` 후 루트 접두사를 다시 비교합니다.
- `HEAD`는 헤더만 보내고 본문은 보내지 않습니다.

**과제**
1. `curl --path-as-is http://localhost:8080/../etc/passwd`, `.../%2e%2e/etc/passwd`가 모두 `403`인 이유를 코드에서 찾아 보세요.
2. `www`에 `ln -s /etc/passwd www/link`를 만들고 `curl -i localhost:8080/link`를 보내 `403`을 확인하세요(확인 후 링크를 지우세요).

## 6단계. 서버 심장: `server.c`

**목표**: 동시성과 연결 생애주기를 이해합니다. 가장 길고 가장 어려운 단계입니다. 두 번에 나눠 읽으세요.

### 6-1. 연결을 받는 쪽 (accept 스레드)

| 함수 | 역할 |
|---|---|
| `listen_on(port)` | `socket` → `SO_REUSEADDR` → `bind` → `listen`. 포트 충돌 시 오류 메시지 출력 |
| `server_run(cfg)` | 워커 스레드 생성 → `poll`+`accept` 루프 → 종료 시 정리 |
| `pool_push` / `pool_pop` | 수락한 연결을 담는 **고정 크기 큐**(mutex + 조건변수). 가득 차면 push 실패 → 즉시 `503` |
| `server_request_stop` | 시그널 핸들러가 호출. 전역 플래그만 세움 |

**핵심 개념**
- **생산자-소비자 패턴**: accept 스레드가 연결을 큐에 넣고(생산), 워커들이 꺼내 처리합니다(소비). `pthread_cond_wait`는 큐가 비었을 때 워커를 재우는 장치입니다.
- `SO_REUSEADDR`: 서버를 껐다 바로 켜도 `Address already in use`가 나지 않게 합니다.
- **백프레셔**: 처리 못 하면 조용히 쌓지 않고 `503 Retry-After`로 부하를 거절합니다.
- **정상 종료**: 시그널 핸들러에서는 플래그만 세웁니다(async-signal-safe). 루프가 플래그를 보고 accept를 멈추고, 큐를 닫고, 워커를 `pthread_join`으로 기다립니다.

### 6-2. 연결 하나를 처리하는 쪽 (워커)

`handle_conn`은 **keep-alive 루프**입니다. 한 연결에서 요청을 여러 개 처리할 수 있습니다.

```
for (;;) {
   헤더 읽기 루프:  버퍼에 요청이 완성됐는지 http_parse_request로 확인,
                   아니면 net_wait_readable + recv  (idle 5초, 요청 전체 15초 제한)
   파싱 오류         → 에러 응답, linger_close, 종료
   Content-Length 초과(8MB) → 413
   Expect: 100-continue     → "100 Continue" 먼저 전송
   본문 읽기 (read_body)
   dispatch() 실행 → 로그
   처리한 바이트를 버퍼에서 제거 (뒤에 pipelining된 다음 요청은 남김)
   keep-alive 아니면 종료
}
```

| 함수 | 역할 |
|---|---|
| `handle_conn` | 위 루프 전체 |
| `read_body` | 이미 버퍼에 있는 본문 + 나머지를 deadline 안에 읽음 |
| `dispatch` | 라우팅 결정(2단계 그림의 분기) |
| `linger_close` | 에러 응답 후 `shutdown(SHUT_WR)`로 반닫고 잠깐 읽어서 비움 |
| `pool_waiting` | 대기 중인 연결이 있는지 확인 |
| `log_access` | 한 줄 액세스 로그 |

**핵심 개념**
- **pipelining**: 클라이언트가 응답을 기다리지 않고 요청을 연달아 보낼 수 있어서, 한 번의 `recv`에 요청 두 개가 들어올 수 있습니다. 그래서 처리한 만큼만 `memmove`로 지우고 나머지를 다음 반복에 씁니다.
- **thread-per-connection의 한계**: keep-alive 연결이 워커를 붙잡으므로, 연결 수가 워커 수보다 많으면 일부가 굶습니다. 그래서 대기 중인 연결이 있으면 keep-alive를 끊습니다(`pool_waiting`).
- **slowloris 방어**: 헤더를 아주 천천히 보내는 공격을 막으려고 *요청 전체*에 15초 deadline을 둡니다.
- **linger_close가 필요한 이유**: 클라이언트가 아직 보내는 중에 `close`하면 커널이 RST를 보내 응답이 사라질 수 있습니다.
- **자원 회수**: `goto done` 한 곳에서 `rbuf` 해제와 `close(fd)`를 합니다. 본문 버퍼는 에러 경로마다 `free`합니다.

**과제**
1. `build/webserver -w 1`로 워커를 1개로 줄이고, 터미널 2개에서 `nc localhost 8080`을 동시에 열어 보세요. 두 번째가 어떻게 되는지, 그리고 5초 후 어떻게 되는지 관찰하세요.
2. `printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\nGET /style.css HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n' | nc localhost 8080`으로 pipelining을 보내고 응답이 두 개 오는지 확인하세요.
3. 서버 실행 중 `Ctrl+C`를 누르고 `shutting down...`, `bye`가 출력되는 흐름을 코드에서 따라가 보세요.

## 7단계. 라우팅과 Reverse Proxy: `config.c`, `router.c`, `proxy.c`

**목표**: 요청이 백엔드로 전달되어 응답이 돌아오는 과정을 이해합니다.

**설정과 라우팅**

| 함수 | 역할 |
|---|---|
| `config_init` | 기본값(포트 8080, 스레드 32, 타임아웃 10초) |
| `config_apply_line` | 설정 한 줄(`port 80`, `route /api a:1 b:2`) 해석. CLI 옵션도 이 함수로 처리 |
| `config_load_file` | 파일을 줄 단위로 읽어 위 함수에 전달, 오류에 줄 번호 표시 |
| `router_add(prefix, "a:1,b:2")` | 경로 접두사 → 백엔드 목록 등록 (`host:port`, `[::1]:80`, `http://` 허용) |
| `router_match(path)` | **가장 긴 접두사** 선택. `/api`는 `/apix`와 매칭되지 않음(경계 규칙) |
| `route_pick(route)` | Round-Robin 카운터를 원자적으로 증가 (`atomic_uint`) |

**Reverse Proxy (`proxy.c`)**: `proxy_handle`은 6단계로 나뉩니다.

1. **연결**: `route_pick`으로 시작 백엔드를 고르고, `net_connect`가 실패하면 다음 백엔드로 넘어갑니다(failover). 전부 실패하면 `502`(거부) 또는 `504`(타임아웃).
2. **요청 전달**: `build_request`가 요청을 새로 조립합니다. `Host`를 백엔드 주소로 바꾸고, `X-Forwarded-For`/`X-Forwarded-Host`를 붙이고, hop-by-hop 헤더(`Connection`, `Keep-Alive`, `Transfer-Encoding` 등)는 제거하고, `Connection: close`로 보냅니다.
3. **응답 헤더 수신**: 백엔드 응답의 헤더가 끝날 때까지 읽고(`recv_to`, 타임아웃 시 `504`), 상태 라인을 파싱합니다.
4. **헤더 재구성**: 상태와 헤더를 클라이언트용으로 다시 만들고, 본문 길이 방식(framing)을 결정합니다.
5. **본문 중계**: 방식에 따라 정확히 N바이트(`Content-Length`) 또는 EOF까지 그대로 보냅니다.
6. **연결 정리**: 백엔드 소켓을 닫고 상태코드를 반환합니다.

**핵심 개념**
- **framing 판단**: ① `HEAD`/`204`/`304`는 본문 없음 ② chunked는 원본 그대로 EOF까지 중계하고 클라이언트 연결을 닫음 ③ `Content-Length`가 있으면 정확히 그 길이만 중계하며 keep-alive 유지 ④ 둘 다 없으면 EOF까지 중계하고 연결을 닫음.
- **hop-by-hop 헤더**: 연결 한 구간에만 의미가 있는 헤더이므로 프록시가 다음 구간에 전달하면 안 됩니다.
- **에러 응답은 "응답 헤더를 클라이언트에 보내기 전"에만** 가능합니다. 이미 헤더를 보낸 뒤 백엔드가 죽으면 연결을 그냥 끊습니다(`goto fail`이 헤더 전송 전 구간에만 있는 이유).
- Round-Robin 카운터가 `atomic`인 이유는 여러 워커 스레드가 동시에 `route_pick`을 부르기 때문입니다.

**과제**
1. 터미널 3개로 확인해 보세요.
   ```sh
   python3 tests/backend.py 8001 A &
   python3 tests/backend.py 8002 B &
   build/webserver -p 8080 -r ./www -R /api=127.0.0.1:8001,127.0.0.1:8002
   ```
   `curl localhost:8080/api/x`를 여러 번 보내 `backend A`, `backend B`가 번갈아 나오는지 보고, B를 `kill`한 뒤 계속 성공하는지(failover) 확인하세요.
2. `build/webserver -t 2 -R /slow=127.0.0.1:8001`로 서버를 띄우고(`-t 2`는 백엔드 응답 대기 2초), `curl -i localhost:8080/slow`가 약 2초 뒤 `504`를 주는지 확인하세요. `tests/backend.py`는 `/slow` 요청에 30초간 답하지 않습니다.
3. (도전) 백엔드가 응답 중간에 죽으면 클라이언트는 무엇을 받게 될까요? `proxy_handle`의 5번 단계에서 근거를 찾아 보세요.

## 8단계. 테스트로 검증 방식 이해하기: `tests/`

**목표**: "어떻게 맞다고 확신하는가"를 이해합니다.

| 종류 | 위치 | 실행 | 무엇을 검증 |
|---|---|---|---|
| 단위 테스트 | `tests/test_http.c` `test_mime.c` `test_router.c` `test_static.c` | `make test` | 파서의 엣지 케이스, MIME, 라우팅 규칙, 경로 탈출 방어, 설정 파싱 |
| 통합 테스트 | `tests/integration.sh` + `tests/backend.py` | `make itest` | 진짜 서버+백엔드 2대에 curl과 raw TCP로 요청 (52개 검사) |
| 메모리 검사 | `Makefile`의 `asan`, `test-asan` | `make test-asan` | AddressSanitizer, UBSan, LeakSanitizer로 잘못된 접근과 누수 탐지 |
| 부하 테스트 | `tests/bench.sh` | `bash tests/bench.sh build/webserver` | `wrk`, `ab`로 처리량 측정 |

`tests/testutil.h`는 `CHECK(조건)` 매크로 하나로 만든 아주 작은 테스트 틀입니다. 이해하기 가장 쉬우니 먼저 읽어 보세요.

**과제 (수정 → 테스트가 잡아내는지 보기)**
1. `src/http.c`의 `parse_header_line`에서 공백 앞 콜론을 거부하는 부분(`is_tchar` 검사)을 일부러 지우고 `make test`를 돌려 어떤 테스트가 실패하는지 보세요. 확인 후 되돌리세요.
2. `src/proxy.c`의 `build_request`에서 `Connection: close` 줄을 일부러 망가뜨리고 `make itest`를 돌려 보세요. (개발 중에 실제로 이 길이 오류가 있었고, 통합 테스트가 504로 잡았습니다.)

## 마무리 과제: 기능 하나 직접 추가하기

배운 것을 종합해 아래 중 하나를 골라 구현해 보세요. 난이도는 위에서 아래로 올라갑니다.

1. **`GET /health`**가 `ok`를 반환하게 하기 → `dispatch`만 수정 (단위 이해 확인)
2. **`Last-Modified` 헤더** 추가 → `static_serve`와 `http_format_head`의 `extra` 활용
3. **접속 로그에 처리 시간(ms)** 추가 → `handle_conn`과 `log_access`
4. **백엔드 헬스체크**: 실패한 백엔드를 일정 시간 건너뛰기 → `route`/`backend` 구조체와 `proxy_handle`
5. **Forward Proxy**: 절대 URI(`GET http://host/path`)를 받아 외부 서버로 전달 → `http.c` 파서 규칙, `dispatch`, `proxy_handle` 재사용

## 용어 정리

| 용어 | 한 줄 설명 |
|---|---|
| 소켓 | 네트워크 연결의 끝점. 파일 디스크립터(fd)로 다룸 |
| keep-alive | 한 TCP 연결로 요청을 여러 번 주고받는 것 |
| pipelining | 응답을 기다리지 않고 요청을 연달아 보내는 것 |
| Content-Length | 본문의 정확한 바이트 수. 본문 끝을 아는 유일한 방법(이 프로젝트에서) |
| hop-by-hop 헤더 | 연결 한 구간에만 의미 있는 헤더. 프록시가 전달하지 않음 |
| Round-Robin | 백엔드를 순서대로 번갈아 선택하는 분산 방식 |
| failover | 한 백엔드가 실패하면 다음 백엔드로 넘어가는 것 |
| 백프레셔 | 처리 능력을 넘으면 거절해서 시스템을 보호하는 것 |
| slowloris | 요청을 아주 천천히 보내 서버 자원을 묶어 두는 공격 |
| 스레드 풀 | 미리 만들어 둔 N개의 스레드가 큐에서 일을 꺼내 처리하는 구조 |
