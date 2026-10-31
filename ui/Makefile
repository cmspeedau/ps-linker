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

test: $(TEST_BIN) /tmp/store_logic_test /tmp/catalog_test /tmp/browser_model_test
	$(TEST_BIN)
	/tmp/store_logic_test
	/tmp/catalog_test
	/tmp/browser_model_test

$(TEST_BIN): coverflow.c coverflow.h coverflow_test.c
	$(CC) $(CFLAGS) -o $@ coverflow.c coverflow_test.c

# Browser-to-coverflow bridge (tabs, skip lists, Downloads) — no device
# headers, host-checked like the store logic.
/tmp/browser_model_test: browser_model.c browser_model.h coverflow.c \
                         store_logic.c store_logic.h browser_model_test.c
	$(CC) $(CFLAGS) -o $@ browser_model.c coverflow.c store_logic.c \
	    browser_model_test.c -lm

# Store catalog logic (shared by both device Stores) and the catalog
# parser, host-tested since they moved out of psp/ (the psp/tests runner
# moved here with them).
/tmp/store_logic_test: store_logic.c store_logic.h store_logic_test.c
	$(CC) $(CFLAGS) -o $@ store_logic.c store_logic_test.c -lm

/tmp/catalog_test: catalog.c catalog.h store_logic.c store_logic.h catalog_test.c
	$(CC) $(CFLAGS) -o $@ catalog.c store_logic.c catalog_test.c -lm

test-store: /tmp/store_logic_test /tmp/catalog_test
	/tmp/store_logic_test
	/tmp/catalog_test

test-asan: coverflow.c coverflow.h coverflow_test.c
	$(CC) $(CFLAGS) -fsanitize=address,undefined -g \
	    -o /tmp/coverflow_test_asan coverflow.c coverflow_test.c
	/tmp/coverflow_test_asan

frames: $(TEST_BIN)
	$(TEST_BIN) --frames

clean:
	rm -f $(TEST_BIN) /tmp/coverflow_test_asan

.PHONY: test test-asan frames clean
