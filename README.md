# jungle-webserver (SK)

> 최소 기능의 웹서버에서 시작해, 공부한 만큼 기능을 늘려간 C 웹서버 학습 기록

- 언어: C (C11). 표준 C 라이브러리와 POSIX API만 사용합니다 (외부 HTTP·이벤트 라이브러리 없음).
- 브랜치: `dev-SK`가 MVP 기준점입니다. 공부하면서 고치는 기능은 `feat/(기능)-SK` 브랜치에서 작업합니다.
- MVP 범위: 필수 기능을 처음부터 끝까지 모두 갖추되, 기능마다 가장 단순한 형태로 구현했습니다.

---

## 1. 빠른 실행

```bash
make                                   # 서버 + CGI 예제 빌드
./webserver -p 8080 -r ./www           # 서버 실행 (포트 8080, 정적 파일 루트 ./www)
curl http://localhost:8080/            # 다른 터미널에서 확인
```

**전체 옵션**

```bash
./webserver -p 8080 -r ./www -c ./cgi-bin -t 8 \
  -R /api=127.0.0.1:8001,127.0.0.1:8002 \
  -F
```

| 옵션 | 의미 | 기본값 |
|---|---|---|
| `-p` | 듣기 포트 | `8080` |
| `-r` | 정적 파일 루트 | `./www` |
| `-c` | CGI 프로그램 디렉토리 | `./cgi-bin` |
| `-t` | 작업 스레드 수 | `8` |
| `-R` | Reverse Proxy 규칙 `경로접두사=호스트:포트,...` (여러 번 지정 가능) | 없음 |
| `-F` | Forward Proxy 켜기 | 꺼짐 |

**Reverse Proxy 데모** (터미널 3개)

```bash
./webserver -p 8001 -r backends/b1
```
```bash
./webserver -p 8002 -r backends/b2
```
```bash
./webserver -p 8080 -R /api=127.0.0.1:8001,127.0.0.1:8002
```
```bash
for i in 1 2 3 4; do curl -s localhost:8080/api/hello.json; done   # backend-1, 2, 1, 2 순서
```

**테스트 명령**

| 명령 | 내용 |
|---|---|
| `make test` | 단위 테스트 |
| `make functional` | 기능 테스트 (서버를 실제로 띄우고 curl로 확인) |
| `make asan` | AddressSanitizer + UBSan 빌드로 단위·기능 테스트 |
| `make load` | `ab` 부하 테스트 (`apt install apache2-utils` 필요) |

---

## 2. 처음 구현 규칙

1. 하나의 함수에는 하나의 기능만 넣는다.
2. 여러 기능을 넣지 말고 최소한의 기능만 넣는다.
3. 공부를 하면서 기능을 수정하고 변경한다.
4. 함수 위에 주석 2줄을 단다: 무엇을 하는지, 관련 개념(CSAPP 절)
5. 시스템 콜 반환값은 반드시 확인한다.
6. 버퍼 크기는 상수로 정하고, 그 크기를 넘지 않게 쓴다. (`src/config.h`)
7. 함수 이름만 보고 역할을 알 수 있게 짓는다.

> MVP 점검 결과: 함수 84개 모두 주석이 있고, 가장 긴 함수는 29줄(`server_handle_connection`)입니다.

---

## 3. 구조 그림

### 요청 하나의 흐름

```
 클라이언트
    │  TCP 연결 (3-way handshake)
    ▼
┌──────────────────────── 메인 스레드 ────────────────────────┐
│ socket → bind → listen → accept ──► 작업 큐(원형 버퍼)에 fd 넣기 │   server.c / net.c
└───────────────────────────────────────────┬───────────────┘
                                            │ (큐가 꽉 차면 대기)
┌──────────────────── 작업 스레드 N개 ────────▼───────────────┐
│ 큐에서 fd 꺼냄                                              │   threadpool.c
│   └─ 반복 (keep-alive):                                     │   server.c
│        요청 읽기·파싱 ── 오류면 4xx/5xx 응답 후 연결 종료        │   http.c
│        │                                                    │
│        ▼  server_dispatch: 요청 종류에 따라 처리기 선택          │
│        ├─ 절대 URI (http://...)  → Forward Proxy  ──► 원 서버   │   proxy.c
│        ├─ -R 규칙 경로 (/api/...) → Reverse Proxy ──► 백엔드 1..N │   proxy.c
│        ├─ /cgi-bin/...           → CGI (fork + execve)        │   cgi.c
│        └─ 그 외                  → 정적 파일 + MIME            │   static_file.c / mime.c
│        응답 보내기 (상태 라인 + 헤더 + 본문)                      │   response.c
└─────────────────────────────────────────────────────────────┘
```

### 파일 구성

```
src/
  main.c          실행 인자 해석, 시그널 설정, 서버 시작
  server.c/h      accept 루프, 연결 처리(keep-alive), 요청 분기
  threadpool.c/h  작업 스레드 + fd 큐 (생산자-소비자)
  net.c/h         듣기 소켓 열기, 타임아웃 connect, 소켓 옵션
  http.c/h        요청 읽기 + 요청 라인·헤더·본문 파싱
  response.c/h    상태 라인·헤더·오류 페이지 보내기
  static_file.c/h 경로 해석(보안 검사) + 파일 전송
  mime.c/h        확장자 → Content-Type
  cgi.c/h         CGI 프로그램 실행
  proxy.c/h       Reverse / Forward Proxy
  config.h        버퍼 크기·한계값 상수
cgi-src/          CGI 예제 (adder: GET, echo: POST)
www/              정적 파일 루트 (html, css, js, png)
backends/         Reverse Proxy 데모용 백엔드 루트 2개
tests/            단위 테스트, 기능 테스트, 부하 테스트 스크립트
```

---

## 4. 기능별 설명

> 기능을 수정하거나 추가할 때마다 해당 칸을 고치고, 새 기능은 칸을 하나씩 추가합니다.

### 4-1. TCP 소켓 서버 — `net.c`, `server.c`
- **무엇을**: `socket → bind → listen → accept` 표준 흐름으로 연결을 받습니다.
- **공부할 개념**: CSAPP 11.4 (소켓 인터페이스, `open_listenfd`), 3-way handshake, `SO_REUSEADDR`
- **핵심 포인트**
  - `SO_REUSEADDR`: 서버를 껐다 바로 켜도 `bind` 실패(Address already in use)가 나지 않게 합니다.
  - 모든 fd에 `FD_CLOEXEC`를 켭니다. CGI로 `fork/exec`할 때 다른 연결의 소켓이 자식 프로세스에 새지 않게 하기 위해서입니다.

### 4-2. HTTP 요청 파싱 — `http.c`
- **무엇을**: 요청 라인(메서드, URI, 버전), 헤더(`Host`, `Content-Length`, `Connection`), POST 본문을 파싱합니다.
- **공부할 개념**: CSAPP 11.5 (HTTP), RFC 7230 (메시지 형식, keep-alive)
- **핵심 포인트**
  - 헤더의 끝은 빈 줄 `\r\n\r\n`이고, 본문 길이는 `Content-Length`로 압니다.
  - 수신 버퍼(`http_conn`)를 연결 단위로 둡니다. keep-alive에서는 다음 요청의 앞부분이 버퍼에 남아 있을 수 있기 때문입니다.
  - 오류 코드: 400(형식 오류), 413(본문 1MB 초과), 414(URI 2KB 초과), 431(헤더 8KB 초과), 501(GET·POST 외 메서드, chunked), 505(HTTP/1.0·1.1 외 버전)

### 4-3. HTTP 응답 생성 — `response.c`
- **무엇을**: 상태 라인과 `Content-Type`, `Content-Length`, `Connection` 헤더를 만들어 보냅니다.
- **공부할 개념**: CSAPP 11.5.4 (HTTP 응답), 10.5 (`rio_writen`: 짧은 쓰기 처리)
- **핵심 포인트**: `send`는 일부만 보내고 돌아올 수 있어서, `send_all`이 끝까지 반복해서 보냅니다.

### 4-4. 정적 파일 서빙 — `static_file.c`, `mime.c`
- **무엇을**: 루트 디렉토리에서 파일을 읽어 응답합니다. 디렉토리 요청은 `index.html`로 바꿉니다.
- **공부할 개념**: CSAPP 11.6 (Tiny `serve_static`), MIME 타입
- **핵심 포인트**
  - 보안: 경로에 `..` 구간이 있으면 403을 보냅니다. `realpath`로 정규화한 뒤 루트 밖이면 403을 보냅니다 (심볼릭 링크 탈출 차단).
  - 파일은 8KB 버퍼로 나눠 읽어서 보냅니다 (Tiny의 `mmap` 대신).

### 4-5. 동적 요청 처리 (CGI) — `cgi.c`, `cgi-src/`
- **무엇을**: `/cgi-bin/이름` 요청이 오면 프로그램을 실행하고, 그 출력을 응답으로 보냅니다.
- **공부할 개념**: CSAPP 11.5.4 (CGI), 11.6 (`serve_dynamic`), 8.4 (`fork`, `execve`, `waitpid`)
- **핵심 포인트**
  - 인자 전달: GET 쿼리는 `QUERY_STRING`, POST 본문은 표준입력(임시 파일)으로, 길이는 `CONTENT_LENGTH`로 넘깁니다.
  - 서버가 CGI 출력 전체를 받아 `Content-Length`를 계산합니다. 그래서 CGI 응답 뒤에도 keep-alive를 유지할 수 있습니다.
  - 환경변수는 `fork` 전에 만듭니다. 멀티스레드 프로그램에서 `fork`한 자식은 `exec`만 하는 게 안전하기 때문입니다.

### 4-6. 동시 요청 처리 (스레드 풀) — `threadpool.c`
- **무엇을**: 작업 스레드 N개를 미리 만들어 두고, 메인 스레드가 `accept`한 fd를 큐로 넘깁니다.
- **공부할 개념**: CSAPP 12.5.4~12.5.5 (생산자-소비자, prethreaded 서버), mutex와 condition variable
- **핵심 포인트**
  - keep-alive 연결이 스레드를 독점하지 않도록, **큐에 기다리는 연결이 있으면 응답 후 연결을 닫습니다** (5번 ②의 부하 테스트에서 발견한 문제를 수정).
  - 유휴 keep-alive 연결은 5초 뒤에 닫힙니다 (`SO_RCVTIMEO`).

### 4-7. Reverse Proxy — `proxy.c`
- **무엇을**: `-R /api=백엔드1,백엔드2` 규칙과 맞는 경로를 백엔드로 넘깁니다. 백엔드는 Round-Robin으로 고릅니다.
- **공부할 개념**: Reverse Proxy(서버를 대신함), 로드 밸런싱, hop-by-hop 헤더(RFC 7230 6.1)
- **핵심 포인트**
  - 가장 긴 접두사가 우선이고, 경로 구간 단위로 비교합니다 (`/api`는 `/apix`와 맞지 않음).
  - 백엔드에는 `Connection: close`로 요청하고, 응답을 EOF까지 그대로 중계합니다.
  - 백엔드 연결 실패는 502, 3초 안에 응답이 없으면 504를 보냅니다 (논블로킹 `connect` + `poll`).

### 4-8. Forward Proxy — `proxy.c`
- **무엇을**: `-F`를 켜면 `GET http://호스트/경로` 형태의 요청을 받아 원 서버로 대신 연결합니다.
- **공부할 개념**: Forward Proxy(클라이언트를 대신함), 절대 URI(RFC 7230 5.3.2), DNS(`getaddrinfo`, CSAPP 11.3)
- **주의**: 아무 주소로나 연결할 수 있는 열린 프록시라서, 데모용으로만 켜야 합니다.

### 4-9. 시그널 / 종료 처리 — `main.c`, `threadpool.c`
- **무엇을**: `SIGINT`/`SIGTERM`이 오면 새 연결 받기를 멈추고, 처리 중인 요청을 마친 뒤 스레드를 정리하고 종료합니다. `SIGPIPE`는 무시합니다.
- **공부할 개념**: CSAPP 8.5 (시그널, 안전한 핸들러), 12.7 (스레드와 시그널)
- **핵심 포인트**
  - 핸들러는 `volatile sig_atomic_t` 플래그만 세웁니다.
  - `SA_RESTART`를 빼서 `accept`가 `EINTR`로 깨어나게 합니다.
  - 작업 스레드는 종료 시그널을 막아 두어, 시그널이 메인 스레드로 가게 합니다.

---

## 5. 테스트 / 검증 결과 모음

### ① 기준점: MVP 처음 구현 결과 (2026-11-19)

**실행 환경**: Linux 컨테이너 (4코어), gcc 13.3, `-Wall -Wextra -Werror`

| 종류 | 명령 | 결과 |
|---|---|---|
| 단위 테스트 | `make test` | 5개 모듈, 108개 검사 모두 통과 |
| 기능 테스트 | `make functional` | 34개 항목 모두 통과 |
| ASan + UBSan | `make asan` | 단위·기능 테스트 모두 통과, sanitizer 오류 0건 |
| Valgrind | 정적·CGI·POST·프록시·오류·동시 요청 섞어서 실행 후 SIGINT 종료 | `All heap blocks were freed -- no leaks are possible`, 오류 0건 |

**단위 테스트 범위**

| 모듈 | 검사 수 | 내용 |
|---|---|---|
| `test_http` | 36 | 정상 GET/POST, Content-Length 형식·크기, keep-alive 기본값, 잘못된 메서드·버전·토큰, Host 누락, 긴 URI, 헤더 개수 초과 |
| `test_mime` | 9 | html/css/js/png/jpg, 대소문자, 확장자 없음, 디렉토리 이름의 점 |
| `test_static` | 14 | `..` 판별, 루트 안/밖 판별, index.html, 404, 403 |
| `test_proxy` | 34 | 라우팅 규칙 파싱·오류, 가장 긴 접두사, Round-Robin 순서, 절대 URI, 요청 재작성(hop-by-hop 제거, 버퍼 부족) |
| `test_cgi` | 15 | CGI 요청 판별, 프로그램 경로 보안 검사, 출력 헤더/본문 분리 |

**기능 테스트 항목** (`tests/functional.sh`, 서버와 백엔드를 실제로 띄워서 확인)

| 분류 | 확인한 것 |
|---|---|
| 정적 파일 | 200, html/css/js/png의 Content-Type, png 바이너리 내용 일치 |
| 오류 응답 | 404, 403(`../` 탈출), 405(정적 파일에 POST), 501(DELETE), 505(HTTP/2.0), 400(깨진 요청 라인, Host 누락), 414(URI 10KB), 431(헤더 10KB), 413(본문 2MB) |
| CGI | GET adder, POST echo, 266KB 본문 왕복 일치, 없는 CGI 404 |
| keep-alive | 한 연결로 요청 2개 처리, `Connection: close` 응답 |
| Reverse Proxy | Round-Robin 1→2→1→2, 꺼진 백엔드 502, 응답 없는 백엔드 504, 규칙 밖 경로는 직접 서빙 |
| Forward Proxy | `curl -x`로 원 서버 중계(서버 로그로 경유 확인), 원 서버 없음 502 |
| 동시성 / 비정상 종료 | 200요청 50병렬 모두 200, 중간에 끊긴 연결 40개 후에도 정상 응답 |
| 종료 처리 | SIGTERM 후 exit 0, 종료 로그, sanitizer 오류 없음 |

**부하 테스트** (`make load`, `ab -n 20000`)

| 스레드 | 동시 접속 | keep-alive | 경로 | req/sec | ms/req | 실패 |
|---|---|---|---|---|---|---|
| 1 | 50 | 끔 | `/index.html` | 29,695 | 1.68 | 0 |
| 8 | 50 | 끔 | `/index.html` | 25,832 | 1.94 | 0 |
| 8 | 50 | 켬 | `/index.html` | 26,404 | 1.89 | 0 |
| 8 | 200 | 켬 | `/index.html` | 23,560 | 8.49 | 0 |
| 8 | 50 | 켬 | `/cgi-bin/adder` | 1,698 | 29.44 | 0 |

- 작은 정적 파일에서는 스레드 1개와 8개의 차이가 거의 없습니다. 클라이언트(`ab`)와 서버가 같은 4코어 머신에서 돌아서, `ab` 자체가 병목일 가능성이 큽니다. 이 결과만으로 스레드 수의 효과를 판단하기는 어렵습니다.
- CGI는 요청마다 `fork + exec`를 하므로 정적 파일보다 약 15배 느립니다.

**MVP를 만들면서 테스트로 발견하고 고친 문제**

| 발견 | 원인 | 수정 |
|---|---|---|
| keep-alive + 동시 접속 50에서 **160 req/s, 실패 24건** | 스레드 8개가 keep-alive 연결을 붙잡고 있어서, 큐의 나머지 연결이 타임아웃(5초)까지 굶음 | 큐에 대기 연결이 있으면 응답 후 연결을 닫음 → **26,404 req/s, 실패 0** |
| `make asan`에서만 컴파일 오류 | 명령줄로 `CFLAGS`를 넘기면 Makefile의 `CFLAGS +=`(POSIX 정의)가 무시됨 | POSIX 정의를 `DEFS` 변수로 분리 |
| Forward Proxy 테스트가 거짓 통과 | 환경변수 `no_proxy`에 `127.0.0.1`이 있어서 curl이 `-x`를 무시하고 직접 연결함 | `--noproxy ''`를 붙이고, 서버 로그로 경유했는지 확인 |

**확인하지 못한 것 / 알려진 한계** (공부하면서 고칠 후보)
- macOS에서는 빌드하거나 실행해 보지 않았습니다 (POSIX 범위로 작성만 함).
- keep-alive는 동시 접속이 스레드 수보다 많으면 사실상 꺼집니다. 연결이 많아도 스레드를 붙잡지 않으려면 이벤트 루프(`epoll`/`kqueue`)가 필요합니다.
- 헤더를 아주 천천히 보내는 클라이언트(slowloris)는 recv 한 번마다 5초씩 버틸 수 있습니다. 요청 전체에 대한 시간 제한은 없습니다.
- CGI 프로그램이 끝나지 않으면 해당 스레드가 계속 기다립니다 (CGI 실행 시간 제한 없음).
- chunked 요청 본문은 지원하지 않고 501을 보냅니다. URI의 `%xx` 디코딩도 하지 않습니다.
- Forward Proxy는 HTTP만 지원합니다 (HTTPS `CONNECT` 없음, 캐싱 없음).

### ② 기능 추가 결과

> 이후 기능을 수정하거나 추가할 때마다, 위 기준점과 비교한 결과를 여기에 덧붙입니다.
