# 9d9l challenges in kaikai — see README.md.
#
# Standalone Makefile, parallel to demos/Makefile. Same graceful
# reporting; same kaic2-direct invocation.

KAIC2   := ../../stage2/kaic2
PRELUDES := $(patsubst %,--prelude %,$(sort $(wildcard ../../stdlib/core/*.kai)))
STDLIB  := ../../stdlib
RT_INC  := -I ../../stage0
CC      := cc
CFLAGS  := -std=c99 -O2 -Wno-unused-function -Wno-unused-variable

BUILD   := build
DEMOS   := $(patsubst %/,%,$(dir $(wildcard */main.kai)))

.PHONY: all verify list clean $(DEMOS)

all: verify

list:
	@echo "9d9l challenges:"
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
	  if ! $(CC) $(CFLAGS) $(RT_INC) $(BUILD)/$$d.c -o $(BUILD)/$$d-bin 2>> $(BUILD)/$$d.err; then \
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

$(DEMOS):
	@$(MAKE) --no-print-directory verify DEMOS=$@ 2>/dev/null | tail -10

clean:
	rm -rf $(BUILD)
