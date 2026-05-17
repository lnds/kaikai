# Demos for kaikai — programs written in *target syntax*, used as probes
# to measure how far the current `kaic2` is from the language we are
# designing. Some compile today, some don't. That is the point.
#
# Run manually:
#   make -C demos verify    # try every demo, print PASS/FAIL/DIFF table
#   make -C demos <topic>   # try a single demo (still graceful)
#   make -C demos list      # list discovered demos
#   make -C demos clean
#
# Uses kaic2 directly (stage 2: full effects, m7a/m7b, --path, --prelude).
# bin/kai wraps kaic1 (stage 1, no effects-in-types) and is unsuitable here.

KAIC2     := ../stage2/kaic2
# Match `bin/kai`'s prelude chain: core (basic prelude) + protocols
# (Show / Eq / Ord / Hash / Serialize for primitives) + effects
# (Stdin / Stdout / Stderr / File / Env declarations + the
# `Console` / `Io` aliases). The earlier note about `cmp` shadowing
# is retired with the m12.8 scope-aware rename pass — local
# parameters whose name matches a registered op are no longer
# rewritten to the dispatcher.
# Hanga Roa: core (core/*, protocols, effects, array) is loaded
# automatically by the compiler. Demos that need random must
# declare `import random` themselves.
CORE_PRELUDES :=
PRELUDES  :=
STDLIB    := ../stdlib
RT_INC    := -I ../stage0
CC        := cc
CFLAGS    := -std=c99 -O2 -Wno-unused-function -Wno-unused-variable -DKAI_STDLIB_PATH=\"$(abspath ../stdlib)\"
# issue #343 — math/real libm bindings live in stage0/runtime.h.
# Linux glibc requires explicit `-lm` after the .c inputs; macOS
# libSystem already exposes those symbols.
LDLIBS_M  := -lm

BUILD     := build
DEMOS     := $(patsubst %/,%,$(dir $(wildcard */main.kai)))

.PHONY: all verify list clean $(DEMOS)

all: verify

list:
	@echo "discovered demos:"
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
	@printf "  %-20s %s\n" "DEMO" "STATUS"
	@printf "  %-20s %s\n" "----" "------"
	@pass=0; fail=0; diffc=0; nogolden=0; \
	for d in $(DEMOS); do \
	  st=$$(cat $(BUILD)/$$d.status 2>/dev/null || echo "?"); \
	  printf "  %-20s %s\n" "$$d" "$$st"; \
	  case "$$st" in \
	    OK)                 pass=$$((pass+1)) ;; \
	    "PASS (no golden)") nogolden=$$((nogolden+1)) ;; \
	    DIFF)               diffc=$$((diffc+1)) ;; \
	    *)                  fail=$$((fail+1)) ;; \
	  esac; \
	done; \
	echo ""; \
	echo "  $$pass OK, $$nogolden ran (no golden), $$diffc diff, $$fail failed"

# Single-demo target.
$(DEMOS):
	@$(MAKE) --no-print-directory verify DEMOS=$@ 2>/dev/null | tail -10

# m12.8.x post-Core REOPEN — Eric level 3: no-regression gate.
# `baseline.txt` stores the minimum OK + PASS count that the demo
# probe set must produce. Drops below the baseline fail the build;
# the file is updated by hand when a demo legitimately leaves the
# OK/PASS column (e.g., the language gains a feature that requires
# a previously-passing demo to be rewritten in target syntax that
# is not yet implemented). The intent is to prevent silent
# regressions where a compiler change knocks a demo out of the OK
# column; the user must explicitly acknowledge the regression by
# editing the baseline.
no-regression: verify
	@expected=$${BASELINE:-$$(cat baseline.txt 2>/dev/null || echo 0)}; \
	pass=0; nogolden=0; \
	for d in $(DEMOS); do \
	  st=$$(cat $(BUILD)/$$d.status 2>/dev/null || echo "?"); \
	  case "$$st" in \
	    OK)                 pass=$$((pass+1)) ;; \
	    "PASS (no golden)") nogolden=$$((nogolden+1)) ;; \
	  esac; \
	done; \
	got=$$((pass + nogolden)); \
	if [ $$got -lt $$expected ]; then \
	  echo ""; \
	  echo "*** demos REGRESSION: expected at least $$expected demos passing (OK+PASS), got $$got ***"; \
	  echo "*** edit demos/baseline.txt only when a demo legitimately becomes blocked ***"; \
	  exit 1; \
	else \
	  echo ""; \
	  echo "demos no-regression OK ($$got passing, baseline $$expected)"; \
	fi

clean:
	rm -rf $(BUILD)
