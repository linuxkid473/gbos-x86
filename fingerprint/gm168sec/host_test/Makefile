# Host tests for the portable layers of gm168sec_hal.h.
# No sensor, no Android toolchain: host gcc + system OpenSSL.
CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
CPPFLAGS = -I.. -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L

test_gm168sec: test_gm168sec.c ../gm168sec_hal.h ../gm168sec_config.h
	$(CC) $(CFLAGS) $(CPPFLAGS) test_gm168sec.c -o $@ -lssl -lcrypto

check: test_gm168sec
	./test_gm168sec

clean:
	rm -f test_gm168sec

.PHONY: check clean
