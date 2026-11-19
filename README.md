# cwebd — C로 밑바닥부터 만든 웹 서버 + Reverse Proxy

외부 HTTP 라이브러리 없이 POSIX 소켓 API만으로 만든 HTTP/1.1 서버입니다.
정적 파일 서빙, 동적 요청 처리(백엔드 전달), 경로 기반 Reverse Proxy(Round-Robin, failover)를 지원합니다.

```
브라우저 ─▶ cwebd :8080 ─┬─ /            정적 파일 (./www)
                         ├─ POST /echo   내장 동적 핸들러
                         └─ /api/*  ─▶ 백엔드 A :8001 / 백엔드 B :8002 (Round-Robin)
```

## 빌드와 실행

Linux / macOS / WSL2 (gcc 또는 clang, make).

```sh
make                       # build/webserver
build/webserver -p 8080 -r ./www -R /api=127.0.0.1:8001,127.0.0.1:8002
build/webserver -c routes.conf
```

| 옵션 | 의미 |
|---|---|
| `-p PORT` | 포트 (기본 8080) |
| `-r DIR` | 정적 파일 루트 (기본 `./www`) |
| `-w N` | 워커 스레드 수 (기본 32) |
| `-t SEC` | 백엔드 응답 타임아웃 (기본 10) |
| `-R /prefix=host:port[,host:port...]` | 해당 경로를 백엔드들로 전달 (반복 가능) |
| `-c FILE` | 설정 파일 (`port`/`root`/`threads`/`queue`/`timeout`/`route`, 예: `routes.conf`) |
| `-q` | 액세스 로그 끄기 |

라우팅은 **가장 긴 prefix**가 우선입니다. `/api`는 `/api`, `/api/x`와 매칭되고 `/apix`와는 매칭되지 않습니다.
백엔드가 여러 개면 요청마다 번갈아 선택하고, 연결에 실패하면 다음 백엔드로 넘어갑니다.
전부 실패하면 `502`, 연결/응답 타임아웃이면 `504`를 반환합니다.

## 구조

| 파일 | 역할 |
|---|---|
| `src/server.c` | `socket/bind/listen/accept`, 스레드 풀, keep-alive 루프, 시그널 종료 |
| `src/http.c` | 요청 파서(요청 라인/헤더/`Content-Length`), 응답 생성 |
| `src/static.c`, `src/mime.c` | 정적 파일(경로 탈출/심볼릭 링크 차단), MIME 매핑 |
| `src/router.c`, `src/proxy.c` | prefix 라우팅, Round-Robin, 요청 중계, hop-by-hop 헤더 제거, `X-Forwarded-*` |
| `src/config.c`, `src/net.c`, `src/main.c` | 설정, 소켓 유틸(타임아웃 connect/recv), CLI |

### 설계 선택

- **동시성: 스레드 풀 + blocking I/O.** 이벤트 루프(epoll/kqueue)는 C10K에는 유리하지만 프록시까지 non-blocking 상태기계로 짜야 하고 epoll은 macOS에서 동작하지 않습니다.
  스레드 풀은 이식성이 좋고 코드가 단순합니다.
  - 대가: keep-alive 연결 하나가 워커 하나를 점유합니다. 대기 중인 연결이 있으면 keep-alive를 끊어 기아(starvation)를 막고, 큐가 가득 차면 `503`으로 부하를 차단합니다.
- **본문 경계**: `Content-Length`로만 판단합니다. `Transfer-Encoding`을 요청에서 받으면 `501`, 둘이 함께 오면 `400`입니다(request smuggling 방지).
- **프록시 응답**: `Content-Length`가 있으면 정확히 그 길이만 중계하며 keep-alive를 유지합니다. chunked/close-delimited 응답은 원본 그대로 중계하고 클라이언트 연결은 닫습니다.
- **Slowloris 대응**: 헤더+본문 전체 15초 deadline, idle keep-alive 5초.
- **종료**: `SIGINT/SIGTERM`이면 accept를 멈추고 진행 중인 요청을 마친 뒤 종료합니다. `SIGPIPE`는 무시합니다.
- **에러 응답 후 close**: 클라이언트가 계속 보내는 중에 close하면 RST로 응답이 사라지므로 half-close 후 잠깐 drain합니다.

### Proxy vs Reverse Proxy

- **Forward Proxy**는 *클라이언트*를 대신합니다(클라이언트 → Proxy → 임의의 원 서버). 접근 제어, 캐싱, 익명화에 씁니다.
- **Reverse Proxy**는 *서버*를 대신합니다(클라이언트 → Proxy → 내부 백엔드 N개). 로드 밸런싱, 백엔드 은닉, 경로별 라우팅에 씁니다. **이 프로젝트는 이쪽을 구현합니다.**

## 테스트

```sh
make test         # 단위 테스트: HTTP 파서(엣지 케이스), MIME, 라우터/설정, 정적 경로 해석/서빙
make test-asan    # 같은 테스트를 AddressSanitizer + UBSan(+LeakSanitizer)으로
make itest        # 통합: 실제 서버 + Python 백엔드 2대 + curl/raw TCP (52개 검사)
bash tests/bench.sh build/webserver   # wrk / ab 부하 테스트
```

통합 테스트가 다루는 것: 정적 응답/MIME/404/403/301, 경로 탈출(`../`, `%2e%2e`), HEAD, POST, 405/413,
잘못된 요청(400/414/431/501/505), 클라이언트 중도 종료, keep-alive, pipelining,
프록시(Round-Robin, 헤더, 본문 1MB, chunked/무길이 응답, 백엔드 다운 502, 타임아웃 504, failover),
동시 300/200 요청, SIGTERM 정상 종료. ASan 빌드로 통합 테스트까지 돌렸을 때 오류와 누수는 없었습니다.

## 부하 테스트 결과

WSL2 Ubuntu, 같은 머신에서 `wrk`/`ab`를 실행한 값이라 절대 수치보다 경향을 보세요. 정적 루트는 WSL 내부 파일시스템입니다.
(`/mnt/c` 위에서는 `realpath/stat`이 느려 약 2k req/s까지 떨어졌습니다.)

| 시나리오 | 결과 |
|---|---|
| 정적 파일, keep-alive, 동시 50 | 약 42,600 req/s, 평균 4.0 ms |
| 정적 파일, keep-alive, 동시 200 (워커 64개) | 약 48,000 req/s, 소켓 오류 0 |
| `ab` keep-alive 없음, 동시 100, 20,000건 | 약 13,200 req/s, 실패 0 |
| Reverse Proxy → Python `http.server` 2대, 동시 50 | 약 1,800 req/s (병목은 Python 백엔드) |

## 한계 (구현하지 않은 것)

- TLS, HTTP/2, 요청 본문 chunked, Range, 캐싱 헤더(`ETag`/`Last-Modified`), gzip
- Forward Proxy(`CONNECT` 포함), CGI, 백엔드 헬스체크(연결 실패 시 그 요청만 다음 백엔드로 우회)
- IPv4 리슨만 지원하고, 업스트림 연결은 keep-alive 없이 요청마다 새로 맺습니다
- 디렉터리 목록은 제공하지 않습니다(`index.html`이 없으면 403)
- macOS는 코드상 분기(`MSG_NOSIGNAL`, `SO_NOSIGPIPE`)만 처리했고 실제로 빌드해 보지는 않았습니다
