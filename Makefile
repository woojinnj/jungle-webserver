CC      ?= gcc
BUILD   ?= build
SAN     ?=
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
CFLAGS  += -D_POSIX_C_SOURCE=200809L -pthread -Isrc $(SAN)
LDFLAGS += -pthread $(SAN)

LIB_SRC  := $(filter-out src/main.c,$(wildcard src/*.c))
LIB_OBJ  := $(LIB_SRC:src/%.c=$(BUILD)/%.o)
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(TEST_SRC:tests/%.c=$(BUILD)/%)

all: $(BUILD)/webserver

$(BUILD)/webserver: $(LIB_OBJ) $(BUILD)/main.o
	$(CC) -o $@ $^ $(LDFLAGS)

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/test_%: tests/test_%.c tests/testutil.h $(LIB_OBJ)
	$(CC) $(CFLAGS) -Itests -o $@ $< $(LIB_OBJ) $(LDFLAGS)

$(BUILD):
	mkdir -p $(BUILD)

# Unit tests (HTTP parser, MIME, router, static path resolution, config).
test: $(TEST_BIN)
	@for t in $(TEST_BIN); do $$t || exit 1; done

# Same build with AddressSanitizer + UBSan (also catches leaks on Linux).
asan:
	$(MAKE) BUILD=build-asan SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" all

test-asan:
	$(MAKE) BUILD=build-asan SAN="-fsanitize=address,undefined -fno-omit-frame-pointer" test

# End-to-end: curl against a running server and two Python backends.
itest: all
	bash tests/integration.sh $(BUILD)/webserver

clean:
	rm -rf build build-asan

.PHONY: all test asan test-asan itest clean
