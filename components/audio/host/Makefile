# Host tests for the audio core (no ESP-IDF needed).
#   make test            build and run the tests
#   make golden          re-record golden.txt after an intended change
#   make render          the event-script renderer: build/render <script> <out.wav>
#   make fixtures        regenerate fixtures/ with ffmpeg

CC ?= cc
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wshadow -D_DEFAULT_SOURCE
SAN ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CORE := $(wildcard ../core/*.c)
HDRS := $(wildcard ../core/*.h) harness.h

all: build/run_tests build/render

build:
	mkdir -p build

build/run_tests: test_audio.c harness.c $(CORE) $(HDRS) | build
	$(CC) $(CFLAGS) $(SAN) -o $@ test_audio.c harness.c $(CORE) -lm

build/render: render.c harness.c $(CORE) $(HDRS) | build
	$(CC) $(CFLAGS) -o $@ render.c harness.c $(CORE) -lm

test: build/run_tests
	./build/run_tests

golden: build/run_tests
	./build/run_tests --update-golden

render: build/render

fixtures:
	./make_fixtures.sh

clean:
	rm -rf build

.PHONY: all test golden render fixtures clean
