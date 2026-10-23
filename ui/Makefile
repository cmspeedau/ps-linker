# PS Linker - shared UI module: host checks
#
# coverflow.c has no device headers in it on purpose, so its logic and its
# renderer can be checked on the build host. Run:
#
#   make test        build with -Wall -Wextra -Werror and run the assertions
#   make test-asan   same, under AddressSanitizer + UBSan
#   make frames      print rendered frames so the layout can be eyeballed
#   make clean
#
# The device builds compile the same file with their own toolchains:
#   psp-gcc  -O2 -G0 -Wall            (pspdev/pspdev)
#   ppu-gcc  -O2 -Wall -std=gnu99 -mcpu=cell -mminimal-toc   (ps3dev)

CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror

TEST_BIN := /tmp/coverflow_test

test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): coverflow.c coverflow.h coverflow_test.c
	$(CC) $(CFLAGS) -o $@ coverflow.c coverflow_test.c

test-asan: coverflow.c coverflow.h coverflow_test.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g \
	    -o /tmp/coverflow_test_asan coverflow.c coverflow_test.c
	/tmp/coverflow_test_asan

frames: $(TEST_BIN)
	$(TEST_BIN) --frames

clean:
	rm -f $(TEST_BIN) /tmp/coverflow_test_asan

.PHONY: test test-asan frames clean
