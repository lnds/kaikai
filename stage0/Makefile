CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -Wpedantic -g -O0
# issue #343 — math/real libm bindings live in stage0/runtime.h.
# Linux glibc requires explicit `-lm` after the .c inputs; macOS
# libSystem already exposes those symbols. Trailed at the end of
# every $(CC) compile-and-link line that consumes runtime.h.
LDLIBS_M := -lm
SRCS    := main.c lexer.c ast.c parser.c parser_expr.c check.c emit.c
OBJS    := $(SRCS:.c=.o)
TARGET  := kaic0
# Every header the sources above include. Listed explicitly rather than
# wildcarded so a new header has to be declared, and coarsely rather than
# per-object because the whole stage builds at -O0 in about a second.
# runtime.h is absent on purpose: it is emitted into generated programs,
# never compiled into kaic0.
HDRS    := ast.h check.h emit.h lexer.h parser.h parser_internal.h

.PHONY: all clean test test-run test-blocks

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf build

build:
	@mkdir -p build

# Attribute fixtures in minimal syntax: multi-line #[...] must be
# skipped whole by the lexer. They live outside examples/minimal/
# because stage2 tokenizes attributes (token parity would break).
ATTR_LEX_EXAMPLES := ../examples/attributes/attr_doc_multiline.kai \
                     ../examples/attributes/attr_multiline_ascii.kai

# End-to-end: compile each example to a native binary and run it.
test-run: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai $(ATTR_LEX_EXAMPLES); do \
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
