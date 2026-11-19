CC ?= cc
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -Isrc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
THREAD_FLAGS = -pthread

CORE = src/http.c src/net.c src/proxy.c src/static.c
HEADERS = src/http.h src/net.h src/proxy.h src/static.h
SOURCES = src/server.c $(CORE)
SANITIZER = -fsanitize=address,undefined -fno-omit-frame-pointer
DETECT_LEAKS = $(shell if [ "$$(uname -s)" = Darwin ]; then echo 0; else echo 1; fi)

.PHONY: all test sanitize clean
all: webserver

webserver: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(SOURCES) $(THREAD_FLAGS) $(LDLIBS)

tests/unit: tests/unit.c $(CORE) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/unit.c $(CORE) $(THREAD_FLAGS) $(LDLIBS)

test: webserver tests/unit
	./tests/unit
	python3 tests/integration.py ./webserver

webserver-sanitize: $(SOURCES) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 $(SANITIZER) $(LDFLAGS) -o $@ $(SOURCES) $(THREAD_FLAGS) $(LDLIBS)

tests/unit-sanitize: tests/unit.c $(CORE) $(HEADERS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -O1 $(SANITIZER) $(LDFLAGS) -o $@ tests/unit.c $(CORE) $(THREAD_FLAGS) $(LDLIBS)

sanitize: webserver-sanitize tests/unit-sanitize
	ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS) UBSAN_OPTIONS=halt_on_error=1 ./tests/unit-sanitize
	ASAN_OPTIONS=detect_leaks=$(DETECT_LEAKS) UBSAN_OPTIONS=halt_on_error=1 python3 tests/integration.py ./webserver-sanitize

clean:
	rm -f webserver webserver-sanitize tests/unit tests/unit-sanitize
