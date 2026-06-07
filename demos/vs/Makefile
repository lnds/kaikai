# Comparative demos in target syntax (vs Rust, Go, Scala, Kotlin,
# Elixir, Python, Ruby, Java). See README.md.
#
# Standalone Makefile, parallel to demos/Makefile and demos/9d9l/Makefile.

KAIC2   := ../../stage2/kaic2
# Hanga Roa: --prelude is retired; the core module set (core/*, protocols,
# effects, array) is loaded automatically by the compiler.
PRELUDES :=
STDLIB  := ../../stdlib
# These demos are built by kaic2 (stage 2), whose emitted C uses the
# stage-2 runtime (Koka tagged-Int, kai_intf, reuse-token, borrow).
# Resolve `#include "runtime.h"` to stage2/runtime.h — `-I ../../stage2`
# first, `-I ../../stage0` kept after for any stage0-only headers. Without
# this, kaic2-emitted `kai_intf(...)` is undeclared against the old
# stage0/runtime.h.
RT_INC  := -I ../../stage2 -I ../../stage0
CC      := cc
CFLAGS  := -std=c99 -O2 -Wno-unused-function -Wno-unused-variable
# issue #343 — math/real libm bindings; Linux glibc needs explicit -lm.
LDLIBS_M := -lm

BUILD   := build
DEMOS   := $(patsubst %/,%,$(dir $(wildcard */main.kai)))

.PHONY: all verify list clean $(DEMOS)

all: verify

list:
	@echo "comparative demos:"
	@for d in $(DEMOS); do echo "  $$d"; done

verify:
	@mkdir -p $(BUILD)
	@for d in $(DEMOS); do \
	  rm -f $(BUILD)/$$d.status $(BUILD)/$$d.err $(BUILD)/$$d.out $(BUILD)/$$d.c $(BUILD)/$$d-bin; \
	  if ! $(KAIC2) $(PRELUDES) --path $(STDLIB) $$d/main.kai > $(BUILD)/$$d.c 2> $(BUILD)/$$d.err; then \
	    err=$$(head -1 $(BUILD)/$$d.err 2>/dev/null | sed 's/^[[:space:]]*//'); \
	    [ -z "$$err" ] && err="kaikai compile error"; \
	    echo "FAIL kaikai: $$err" > $(BUILD)/$$d.status; \
	    continue; \
	  fi; \
	  if ! $(CC) $(CFLAGS) $(RT_INC) $(BUILD)/$$d.c -o $(BUILD)/$$d-bin $(LDLIBS_M) 2>> $(BUILD)/$$d.err; then \
	    echo "FAIL cc: see $(BUILD)/$$d.err" > $(BUILD)/$$d.status; \
	    continue; \
	  fi; \
	  if ! $(BUILD)/$$d-bin > $(BUILD)/$$d.out 2>> $(BUILD)/$$d.err; then \
	    err=$$(tail -1 $(BUILD)/$$d.err 2>/dev/null | sed 's/^[[:space:]]*//'); \
	    [ -z "$$err" ] && err="runtime error"; \
	    echo "FAIL runtime: $$err" > $(BUILD)/$$d.status; \
	    continue; \
	  fi; \
	  if [ -f $$d/main.out.expected ]; then \
	    if diff -q $$d/main.out.expected $(BUILD)/$$d.out > /dev/null 2>&1; then \
	      echo "OK" > $(BUILD)/$$d.status; \
	    else \
	      echo "DIFF" > $(BUILD)/$$d.status; \
	    fi; \
	  else \
	    echo "PASS (no golden)" > $(BUILD)/$$d.status; \
	  fi; \
	done
	@echo ""
	@printf "  %-12s %s\n" "DEMO" "STATUS"
	@printf "  %-12s %s\n" "----" "------"
	@pass=0; fail=0; diffc=0; nogolden=0; \
	for d in $(DEMOS); do \
	  st=$$(cat $(BUILD)/$$d.status 2>/dev/null || echo "?"); \
	  printf "  %-12s %s\n" "$$d" "$$st"; \
	  case "$$st" in \
	    OK)                 pass=$$((pass+1)) ;; \
	    "PASS (no golden)") nogolden=$$((nogolden+1)) ;; \
	    DIFF)               diffc=$$((diffc+1)) ;; \
	    *)                  fail=$$((fail+1)) ;; \
	  esac; \
	done; \
	echo ""; \
	echo "  $$pass OK, $$nogolden ran (no golden), $$diffc diff, $$fail failed"

$(DEMOS):
	@$(MAKE) --no-print-directory verify DEMOS=$@ 2>/dev/null | tail -10

clean:
	rm -rf $(BUILD)
