# minihttp — tiny HTTP/1.1 server

CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS  += -std=c11 -D_GNU_SOURCE -pthread -MMD -MP \
                    -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
override LDFLAGS += -pthread

BIN       := minihttp
SRCS      := $(wildcard src/*.c)
OBJS      := $(SRCS:src/%.c=build/%.o)
UNIT_OBJS := build/http.o build/mime.o

.PHONY: all run test unit integration sanitize tsan clean

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/test_http: tests/test_http.c $(UNIT_OBJS) | build
	$(CC) $(CFLAGS) -Isrc -o $@ $^ $(LDFLAGS)

build:
	mkdir -p $@

run: $(BIN)
	./$(BIN) --port 8080 --root ./public

test: unit integration

unit: build/test_http
	./build/test_http

integration: $(BIN)
	bash tests/integration.sh

# Rebuild everything with sanitizers and run the full test suite.
# Run `make clean` afterwards to get a normal build back.
sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined" \
	             LDFLAGS="-fsanitize=address,undefined"

tsan:
	$(MAKE) clean
	$(MAKE) test CFLAGS="-O1 -g -fsanitize=thread" LDFLAGS="-fsanitize=thread"

clean:
	rm -rf build $(BIN)

-include $(OBJS:.o=.d) build/test_http.d
