CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -Werror

.PHONY: test
test:
	$(CC) $(CFLAGS) -o /tmp/store_logic_test ../store_logic.c store_logic_test.c -lm
	/tmp/store_logic_test
