#ifndef CONFIG_H
#define CONFIG_H

/* 규칙 6: 모든 버퍼 크기와 한계값은 여기서 상수로 정한다. */

#define MAX_HEAD_SIZE      8192        /* 요청 라인 + 헤더 전체 최대 크기 (넘으면 431) */
#define MAX_URI_LEN        2048        /* URI 최대 길이 (넘으면 414) */
#define MAX_METHOD_LEN     16          /* 메서드 문자열 최대 길이 */
#define MAX_VERSION_LEN    16          /* 버전 문자열 최대 길이 */
#define MAX_HEADERS        64          /* 헤더 최대 개수 */
#define MAX_BODY_SIZE      (1 << 20)   /* POST 본문 최대 1MB (넘으면 413) */
#define MAX_PATH_LEN       4096        /* 파일 경로 최대 길이 */
#define IO_CHUNK_SIZE      8192        /* 파일·소켓 중계 시 한 번에 읽는 크기 */
#define MAX_CGI_OUTPUT     (1 << 20)   /* CGI 출력 최대 1MB */
#define MAX_CGI_ENV        16          /* CGI 환경변수 최대 개수 */
#define MAX_ENV_LEN        2304        /* 환경변수 한 개 최대 길이 (QUERY_STRING 포함) */

#define MAX_ROUTES         8           /* Reverse Proxy 라우팅 규칙 최대 개수 */
#define MAX_BACKENDS       8           /* 규칙 하나당 백엔드 최대 개수 */
#define MAX_HOST_LEN       256         /* 호스트 이름 최대 길이 */
#define MAX_PREFIX_LEN     128         /* 라우팅 경로 접두사 최대 길이 */

#define DEFAULT_PORT       8080
#define DEFAULT_THREADS    8
#define QUEUE_CAPACITY     256         /* 스레드 풀 작업 큐 크기 */
#define LISTEN_BACKLOG     128         /* listen() 대기열 크기 */

#define KEEPALIVE_TIMEOUT_SEC  5       /* keep-alive 연결에서 다음 요청을 기다리는 시간 */
#define BACKEND_TIMEOUT_SEC    3       /* 백엔드 connect/응답 대기 시간 (넘으면 502/504) */

#endif
