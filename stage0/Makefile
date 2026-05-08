CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -g -O0
# issue #343 — math/real libm bindings live in stage0/runtime.h.
# Linux glibc requires explicit `-lm` after the .c inputs; macOS
# libSystem already exposes those symbols. Trailed at the end of
# every $(CC) compile-and-link line that consumes runtime.h.
LDLIBS_M := -lm
SRCS    := main.c lexer.c ast.c parser.c check.c emit.c
OBJS    := $(SRCS:.c=.o)
TARGET  := kaic0

.PHONY: all clean test test-run test-blocks

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf build

build:
	@mkdir -p build

# End-to-end: compile each example to a native binary and run it.
test-run: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  echo "=== run $$name ==="; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) build/$$name.c -I . -o build/$$name $(LDLIBS_M); \
	  ./build/$$name; \
	done

# Compile each example with --test and run its test blocks.
test-blocks: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  echo "=== tests $$name ==="; \
	  ./$(TARGET) --test $$f > build/$$name-t.c; \
	  $(CC) build/$$name-t.c -I . -o build/$$name-t $(LDLIBS_M); \
	  ./build/$$name-t; \
	done

# Full MVP verification: both targets above.
test: test-run test-blocks
