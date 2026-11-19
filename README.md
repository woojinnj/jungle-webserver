# Jungle Web Server

크래프톤 정글 웹 서버 프로젝트입니다. C11 표준 라이브러리와 POSIX 소켓 API로 HTTP 요청 파싱, 정적 파일 서빙, 동적 POST 처리, 리버스 프록시를 구현했습니다.

## 빌드 및 실행

```sh
make
./webserver 8080 ./public
```

서버는 지정한 포트의 IPv4 인터페이스에서 연결을 받습니다. 디렉터리 요청은 해당 디렉터리의 `index.html`을 돌려줍니다. `POST /echo`는 요청 본문을 바이트 그대로 반환합니다.

```sh
curl -i http://127.0.0.1:8080/
curl -i --data-binary 'hello' http://127.0.0.1:8080/echo
```

## 리버스 프록시

각 인자를 `/prefix=HOST:PORT[,HOST:PORT...]` 형식으로 전달합니다. 경로 세그먼트를 기준으로 일치하는 가장 긴 접두사를 찾고, 같은 규칙의 백엔드는 라운드 로빈으로 선택합니다. 예를 들어 `/api`는 `/api/items`와 일치하고 `/apix`는 정적 파일 처리로 넘어갑니다. 백엔드에는 원래 요청 경로와 쿼리를 전달합니다.

두 백엔드를 실행합니다. 같은 작업 디렉터리에서 세 번째 터미널로 웹 서버를 실행하기 전에 앞의 정적 파일 서버를 종료해 포트를 비워 둡니다.

```sh
# 터미널 1
backend_a=$(mktemp -d)
printf 'backend A\n' > "$backend_a/api"
python3 -m http.server 8001 --bind 127.0.0.1 --directory "$backend_a"
```

```sh
# 터미널 2
backend_b=$(mktemp -d)
printf 'backend B\n' > "$backend_b/api"
python3 -m http.server 8002 --bind 127.0.0.1 --directory "$backend_b"
```

```sh
# 터미널 3
./webserver 8080 ./public '/api=127.0.0.1:8001,127.0.0.1:8002'
```

```sh
# 터미널 4
curl -fsS http://127.0.0.1:8080/api  # backend A
curl -fsS http://127.0.0.1:8080/api  # backend B
curl -fsS http://127.0.0.1:8080/     # 로컬 index.html
```

## 동작 범위

- `socket → bind → listen → accept` 흐름으로 리스너를 만들고 `SO_REUSEADDR`을 설정합니다. 메인 스레드는 `poll`로 리스너와 종료 신호를 기다리며, 워커 8개가 최대 128개의 대기 연결을 처리합니다. 백엔드가 느리면 워커 하나를 점유하며 대기열이 가득 차면 `503`을 반환합니다.
- `GET`과 `POST`의 HTTP/1.0·1.1 요청을 지원합니다. 요청 라인과 헤더는 CRLF로 구분하고, 본문은 `Content-Length`로 읽습니다. 요청 URI 2 KiB, 헤더 영역 16 KiB, 헤더 100개, 본문 1 MiB를 상한으로 둡니다. 요청의 `Transfer-Encoding`에는 `501`을 반환합니다.
- 연결당 요청 하나를 처리하고 `Connection: close`를 보냅니다. URI의 경로 부분은 퍼센트 디코딩하며, 정적 파일을 열 때 루트 디렉터리 기준으로 각 경로 요소를 확인합니다. 상위 경로 이동과 심볼릭 링크에 대한 요청은 `403`으로 처리합니다.
- 프록시는 설정된 백엔드에 새 TCP 연결을 만들고, 연결 구간에만 유효한 헤더를 제거한 뒤 요청을 전달합니다. 응답은 `Content-Length`, chunked 또는 연결 종료를 기준으로 스트리밍합니다. 응답 헤더를 보내기 전 감지한 백엔드 오류는 `502`, 타임아웃은 `504`로 처리합니다.
- 클라이언트 수신 5초, 백엔드 연결 2초, 백엔드 요청 송신 및 응답 수신 합계 5초, 클라이언트 응답 송신 15초의 제한 시간을 둡니다. `SIGINT`와 `SIGTERM`은 새 접속을 중단하고 대기 및 처리 중 연결을 정리합니다. `SIGPIPE`는 무시합니다.

HTTP 메시지 형식과 전달 규칙에 참고한 표준입니다. [RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html), [RFC 9110](https://www.rfc-editor.org/rfc/rfc9110.html)

## 테스트

```sh
make test
make sanitize
```

통합 테스트에는 Python 3과 로컬 TCP 연결이 필요합니다. 테스트는 분할 수신, 바이너리 POST, 잘못된 헤더와 본문 길이, 정적 파일 경로 및 MIME 타입, 라운드 로빈, 백엔드 응답 프레이밍, 타임아웃, 동시 접속과 `SIGTERM` 종료를 검사합니다. macOS ARM64에서 단위·통합 테스트와 AddressSanitizer·UndefinedBehaviorSanitizer를 통과했습니다. macOS `leaks`로 정적 파일, `POST /echo`, 프록시를 반복 호출한 실행 중 프로세스를 검사한 결과 누수는 0건이었습니다.

### 로컬 부하 테스트

macOS ARM64에서 루프백으로 정적 `GET /` 요청을 각 동시성 수준당 2,000회 보낸 결과입니다.

```sh
for concurrency in 1 8 32 64; do
  ab -n 2000 -c "$concurrency" -s 10 http://127.0.0.1:8080/
done
```

| 동시 접속 | 초당 요청 | 실패 요청 |
| ---: | ---: | ---: |
| 1 | 14,817 | 0 |
| 8 | 35,904 | 0 |
| 32 | 41,927 | 0 |
| 64 | 43,685 | 0 |
