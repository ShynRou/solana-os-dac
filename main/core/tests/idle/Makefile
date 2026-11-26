# Host test for the auto-dim idle logic (no ESP-IDF needed): make test
CC ?= cc
test: build/test_idle
	./build/test_idle
build/test_idle: test_idle.c ../../os_idle.c ../../os_idle.h
	mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -fsanitize=address,undefined -o $@ test_idle.c ../../os_idle.c
clean:
	rm -rf build
.PHONY: test clean
