# The `kai` CLI binary. Built by invoking kaic2 directly — the bootstrap never
# goes through the CLI. bin/kai runs this on first use in a dev checkout;
# scripts/build-release.sh ships the result as libexec/kaikai/kai.
#
# Fixed flags, not $(CFLAGS): a user's CFLAGS may carry FFI shim sources or
# sanitisers meant for their program, never for the driver.

ROOT    := $(abspath ../..)
KAIC2   := $(ROOT)/stage2/kaic2
STDLIB  := $(ROOT)/stdlib
EDITION := $(shell cat $(ROOT)/EDITION)
CC      ?= cc
CLI_CFLAGS := -std=c99 -Wno-unused-function -Wno-unused-variable -O2
RT_INC  := -I $(ROOT)/stage2 -I $(ROOT)/stage0
OWNER_DEFS := -DKAI_SEPARATE_COMPILATION=1 -DKAI_RUNTIME_OWNER=1 -DKAI_PROGRAM_PROVIDES_MAIN=1

SRCS := $(wildcard *.kai)

kai: $(SRCS) cli_sys.c cli_io.c $(KAIC2) $(ROOT)/stage2/runtime.h $(ROOT)/stage2/runtime_owner_c.c
	@mkdir -p build
	KAIKAI_STDLIB_PATH=$(STDLIB) $(KAIC2) --edition $(EDITION) --path $(CURDIR) --path $(STDLIB) main.kai > build/kai.c
	$(CC) $(CLI_CFLAGS) -DKAI_SEPARATE_COMPILATION=1 $(RT_INC) -c build/kai.c -o build/kai.o
	$(CC) $(CLI_CFLAGS) -c cli_sys.c -o build/cli_sys.o
	$(CC) $(CLI_CFLAGS) -c cli_io.c -o build/cli_io.o
	$(CC) $(CLI_CFLAGS) -O0 $(OWNER_DEFS) $(RT_INC) -c $(ROOT)/stage2/runtime_owner_c.c -o build/owner.o
	$(CC) build/kai.o build/cli_sys.o build/cli_io.o build/owner.o -o build/kai.$$$$ -lm && mv -f build/kai.$$$$ $@

clean:
	rm -rf build kai

.PHONY: clean
