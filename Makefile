CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2 -g
# POSIX 함수(getaddrinfo, strtok_r 등) 선언을 켜는 정의. CFLAGS 를 바꿔도 항상 들어가도록 분리.
DEFS    := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE
LDFLAGS += -pthread

SRCS     := $(wildcard src/*.c)
LIB_SRCS := $(filter-out src/main.c, $(SRCS))
HEADERS  := $(wildcard src/*.h)
CGI_BINS := cgi-bin/adder cgi-bin/echo
TESTS    := tests/test_http tests/test_mime tests/test_static tests/test_proxy tests/test_cgi

.PHONY: all clean test functional load asan

all: webserver $(CGI_BINS)

webserver: $(SRCS) $(HEADERS)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $(SRCS) $(LDFLAGS)

cgi-bin/%: cgi-src/%.c
	@mkdir -p cgi-bin
	$(CC) $(CFLAGS) $(DEFS) -o $@ $<

tests/test_%: tests/test_%.c $(LIB_SRCS) $(HEADERS) tests/test.h
	$(CC) $(CFLAGS) $(DEFS) -Isrc -o $@ $< $(LIB_SRCS) $(LDFLAGS)

# 단위 테스트
test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

# 기능 테스트 (curl 로 실제 서버 확인)
functional: all
	./tests/functional.sh

# 부하 테스트 (ab 필요)
load: all
	./tests/load.sh

# AddressSanitizer + LeakSanitizer 빌드로 단위·기능 테스트
asan: clean
	$(MAKE) all test CFLAGS="-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" LDFLAGS="-pthread -fsanitize=address,undefined"
	./tests/functional.sh

clean:
	rm -f webserver $(CGI_BINS) $(TESTS)
