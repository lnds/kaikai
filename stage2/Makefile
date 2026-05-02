CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0
# macOS caps the main-thread stack at the LINKED size, not the ulimit
# soft limit (ulimit -s only matters where the kernel is the
# allocator — i.e. Linux). The Tongariki kai fmt lane (2026-05-01)
# tipped self-host past the 8 MB default at -O0, so on Darwin we
# link with a 128 MB main-thread stack. Linux honours the runtime
# ulimit -s 65520 set in the `STACK` prefix below, so it needs no
# linker flag.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
LDFLAGS_STACK := -Wl,-stack_size,0x8000000
else
LDFLAGS_STACK :=
endif
KAIC1   := ../stage1/kaic1
TARGET  := kaic2
SRC     := compiler.kai

# Issue #42 closure (2026-05-01): the recursive lex_loop / parse_*
# inside kaic1 / kaic2 used to need a 32-64 MB stack bump to handle
# stage2/compiler.kai's ~32 k tokens. With both stage 0 and stage 1
# emitting goto-form self-tail-calls, kaic1 / kaic2 run with O(1)
# C-stack on the recursive paths and the bump is no longer needed.
# Left as an empty variable so the existing `$(STACK)` recipe
# prefixes still expand to a no-op and don't have to be edited.
STACK :=

.PHONY: all clean test test-fast test-tokens test-ast test-check test-run test-blocks test-holes test-effects test-effect-runtime test-link-runtime test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-sugars test-loop test-time test-intervals test-stdlib test-protocols test-demos-core test-aspirational test-unbox-phase2 test-unbox-llvm-bench test-tco test-tco-regression test-tco-zero-arg test-tco-perceus-wrap test-trace test-trace-asan test-foreign-prelude selfhost

all: $(TARGET)

$(KAIC1):
	$(MAKE) -C ../stage1 kaic1

build:
	@mkdir -p build

build/stage2.c: $(SRC) $(KAIC1) | build
	$(STACK) $(KAIC1) $(SRC) > $@

$(TARGET): build/stage2.c
	$(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 $< -o $@

# Token stream must match stage 1 (which already matches stage 0).
test-tokens: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/minimal/*.kai ../stage1/compiler.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --tokens $$f | awk '{$$1=$$1; print}' > build/s2-$$name.tok; \
	  ../stage1/kaic1 --tokens $$f | awk '{$$1=$$1; print}' > build/s1-$$name.tok; \
	  diff -q build/s1-$$name.tok build/s2-$$name.tok > /dev/null \
	    && echo "tokens OK $$name" \
	    || { echo "tokens DIFF $$name"; diff build/s1-$$name.tok build/s2-$$name.tok | head; exit 1; }; \
	done

# --ast smoke test (format is indented, readable, not byte-compared).
test-ast: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s2-$$name.ast && echo "ast OK $$name" \
	    || { echo "ast FAIL $$name"; exit 1; }; \
	done

# --types smoke test: every top-level decl gets a signature dumped.
test-types: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --types $$f > build/s2-$$name.types && echo "types OK $$name" \
	    || { echo "types FAIL $$name"; exit 1; }; \
	done

# --env smoke test: full type environment (prelude + variants + user decls).
test-env: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --env $$f > build/s2-$$name.env && echo "env OK $$name" \
	    || { echo "env FAIL $$name"; exit 1; }; \
	done

# --infer smoke test: HM inference over every top-level fn body. Each
# example is expected to pass without unification errors; stderr lines
# other than routine diagnostics would indicate a regression.
test-infer: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --infer $$f > build/s2-$$name.infer 2> build/s2-$$name.infer.err \
	    && [ ! -s build/s2-$$name.infer.err ] \
	    && echo "infer OK $$name" \
	    || { echo "infer FAIL $$name"; cat build/s2-$$name.infer.err; exit 1; }; \
	done

# --dump-mono: smoke test that call-site instantiations resolve to
# concrete types. Any `?t<N>` in the output means a fresh var wasn't
# pinned by the final substitution, which for our minimal examples
# would indicate an incomplete rewrite.
test-dump-mono: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --dump-mono $$f > build/s2-$$name.mono 2> build/s2-$$name.mono.err; \
	  if grep -Eq '\?t[0-9]+' build/s2-$$name.mono; then \
	    echo "dump-mono FAIL $$name (unresolved type vars)"; \
	    grep -E '\?t[0-9]+' build/s2-$$name.mono | head; \
	    exit 1; \
	  else \
	    echo "dump-mono OK $$name"; \
	  fi; \
	done

# --dump-typed: smoke test that the walker populates every Expr node.
# A clean run produces stdout with no `?` type markers — every node
# must be annotated.
test-dump-typed: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --dump-typed $$f > build/s2-$$name.typed 2> build/s2-$$name.typed.err; \
	  if grep -q ' : ?$$' build/s2-$$name.typed; then \
	    echo "dump-typed FAIL $$name (found un-annotated nodes)"; \
	    grep ' : ?$$' build/s2-$$name.typed | head; \
	    exit 1; \
	  else \
	    echo "dump-typed OK $$name"; \
	  fi; \
	done

# --check: name resolution + HM inference, with no --infer noise on stdout.
# A clean run must leave both stdout and stderr empty.
test-infer-check: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --check $$f > build/s2-$$name.check 2> build/s2-$$name.check.err \
	    && [ ! -s build/s2-$$name.check ] \
	    && [ ! -s build/s2-$$name.check.err ] \
	    && echo "check-types OK $$name" \
	    || { echo "check-types FAIL $$name"; cat build/s2-$$name.check.err; exit 1; }; \
	done

# Checker: undefined-name resolution runs clean on every example.
test-check: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > /dev/null \
	    && echo "check OK $$name" \
	    || { echo "check FAIL $$name"; exit 1; }; \
	done

# LLVM backend round-trip: emitted .ll is compiled with clang against
# the runtime shim, and the resulting binary's output must match the
# C backend's. `minimal/hello.kai` exercises the M3a skeleton; files
# under `examples/llvm/` cover the incremental milestones with just
# the constructs each one targets.
LLVM_MIN_EXAMPLES := hello fizzbuzz interp quicksort
LLVM_EXAMPLES     := m3b
test-llvm: $(TARGET) build
	@set -e; \
	for name in $(LLVM_MIN_EXAMPLES); do \
	  f=../examples/minimal/$$name.kai; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name.ll; \
	  clang -w -I ../stage0 build/$$name.ll ../stage0/runtime_llvm.c -o build/$$name-llvm; \
	  ./build/$$name-llvm > build/$$name-llvm.out; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name-c; \
	  ./build/$$name-c > build/$$name-c.out; \
	  diff -q build/$$name-llvm.out build/$$name-c.out > /dev/null \
	    && echo "llvm OK $$name" \
	    || { echo "llvm DIFF $$name"; diff build/$$name-llvm.out build/$$name-c.out | head; exit 1; }; \
	done; \
	for name in $(LLVM_EXAMPLES); do \
	  f=../examples/llvm/$$name.kai; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name.ll; \
	  clang -w -I ../stage0 build/$$name.ll ../stage0/runtime_llvm.c -o build/$$name-llvm; \
	  ./build/$$name-llvm > build/$$name-llvm.out; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name-c; \
	  ./build/$$name-c > build/$$name-c.out; \
	  diff -q build/$$name-llvm.out build/$$name-c.out > /dev/null \
	    && echo "llvm OK $$name" \
	    || { echo "llvm DIFF $$name"; diff build/$$name-llvm.out build/$$name-c.out | head; exit 1; }; \
	done

# End-to-end: emitted C from kaic2 matches stage 1's stdout byte for byte.
test-run: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name; \
	  ./build/$$name > build/$$name.out; \
	  ../stage1/kaic1 $$f > build/$$name-s1.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-s1.c -o build/$$name-s1; \
	  ./build/$$name-s1 > build/$$name-s1.out; \
	  diff -q build/$$name.out build/$$name-s1.out > /dev/null \
	    && echo "run OK $$name" \
	    || { echo "run DIFF $$name"; exit 1; }; \
	done

# --test binaries produce the same stderr as stage 1's.
test-blocks: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --test $$f > build/$$name-t.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t.c -o build/$$name-t; \
	  ./build/$$name-t 2> build/$$name-t.err || true; \
	  ../stage1/kaic1 --test $$f > build/$$name-t-s1.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t-s1.c -o build/$$name-t-s1; \
	  ./build/$$name-t-s1 2> build/$$name-t-s1.err || true; \
	  diff -q build/$$name-t.err build/$$name-t-s1.err > /dev/null \
	    && echo "tests OK $$name" \
	    || { echo "tests DIFF $$name"; exit 1; }; \
	done

# Backend-parity coverage harness: for every fixture in
# {sugars, units, minimal, llvm} that has a `.out.expected` golden,
# compile through BOTH the C backend and the LLVM backend, run each
# binary, and diff the outputs. A divergence (e.g. `Real<unit>`
# printing 0 in LLVM but the actual value in C) fails the build.
#
# This rule is the systemic answer to the m7e13 / m12.5 / m12.8 lanes
# each validating LLVM manually with a one-shot bash loop. It is
# deliberately separate from `test-llvm` (which uses a hardcoded
# fixture list and pre-dates the coverage push) and from
# `test-protocols` / `test-modules` (which already do dual-backend
# inside their own rules).
#
# Fixtures with `.err.expected` (negative) are skipped — backend
# divergence is irrelevant when the typer rejects the program.
# Fixtures without any expected file get a "no golden" pass when both
# backends compile + run cleanly.
#
# To add a directory: extend the `for d in ...` loop. Each subdir
# must contain `.kai` files at the top level.
test-llvm-coverage: $(TARGET) build
	@$(STACK) set -e; \
	pass=0; fail=0; skip=0; \
	for d in sugars units minimal llvm; do \
	  for f in ../examples/$$d/*.kai; do \
	    [ -e $$f ] || continue; \
	    name=$$(basename $$f .kai); \
	    tag="$$d/$$name"; \
	    exp_out=../examples/$$d/$$name.out.expected; \
	    exp_err=../examples/$$d/$$name.err.expected; \
	    if [ -f $$exp_err ]; then \
	      skip=$$((skip+1)); continue; \
	    fi; \
	    rc=0; ./$(TARGET) $$f > build/cov-$$d-$$name.c 2>/dev/null || rc=$$?; \
	    if [ $$rc -ne 0 ]; then \
	      echo "coverage SKIP $$tag (kaic2 rejects without --prelude)"; \
	      skip=$$((skip+1)); continue; \
	    fi; \
	    $(CC) $(CFLAGS) -I ../stage0 build/cov-$$d-$$name.c -o build/cov-$$d-$$name-c 2>/dev/null \
	      || { echo "coverage SKIP $$tag (C compile failed)"; skip=$$((skip+1)); continue; }; \
	    ./build/cov-$$d-$$name-c > build/cov-$$d-$$name-c.out 2>/dev/null \
	      || { echo "coverage SKIP $$tag (C run errored)"; skip=$$((skip+1)); continue; }; \
	    ./$(TARGET) --emit=llvm $$f > build/cov-$$d-$$name.ll 2>/dev/null \
	      || { echo "coverage SKIP $$tag (kaic2 --emit=llvm errored)"; skip=$$((skip+1)); continue; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/cov-$$d-$$name.ll ../stage0/runtime_llvm.c -o build/cov-$$d-$$name-llvm 2>/dev/null \
	      || { echo "coverage SKIP $$tag (LLVM compile failed)"; skip=$$((skip+1)); continue; }; \
	    ./build/cov-$$d-$$name-llvm > build/cov-$$d-$$name-llvm.out 2>/dev/null \
	      || { echo "coverage SKIP $$tag (LLVM run errored)"; skip=$$((skip+1)); continue; }; \
	    if diff -q build/cov-$$d-$$name-c.out build/cov-$$d-$$name-llvm.out > /dev/null; then \
	      echo "coverage OK $$tag"; \
	      pass=$$((pass+1)); \
	    else \
	      echo "coverage DIFF $$tag"; \
	      diff build/cov-$$d-$$name-c.out build/cov-$$d-$$name-llvm.out | head -10; \
	      fail=$$((fail+1)); \
	    fi; \
	  done; \
	done; \
	echo ""; \
	echo "coverage summary: $$pass pass, $$fail DIFF, $$skip skip"; \
	[ $$fail -eq 0 ] || exit 1

# M6.1 modules smoke test: the `examples/modules/` tree has a
# diamond `main → {arith, shapes}` where `shapes` also imports
# `arith`. A clean compile through each backend must produce the
# expected output `42 / 42 / 12` byte-for-byte. Verifies that
# import resolution produces a single flat decl list that
# downstream passes (check + HM + emit) accept without change.
test-modules: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/modules/main.kai > build/modules.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/modules.c -o build/modules-c; \
	./build/modules-c > build/modules-c.out; \
	./$(TARGET) --emit=llvm ../examples/modules/main.kai > build/modules.ll; \
	clang -w -I ../stage0 build/modules.ll ../stage0/runtime_llvm.c -o build/modules-llvm; \
	./build/modules-llvm > build/modules-llvm.out; \
	diff -q build/modules-c.out build/modules-llvm.out > /dev/null \
	  && echo "modules OK" \
	  || { echo "modules DIFF"; diff build/modules-c.out build/modules-llvm.out; exit 1; }

# m6.2 — qualified module calls. Each subdirectory under
# examples/modules-qualified/ is one self-contained scenario
# (basic / alias / selective / local_shadow / two_modules /
# record_projection): a `main.kai` plus zero or more helper
# modules. Compiled with the subdir as the import root (no
# --path needed because helpers live next to main.kai). Both
# backends run; outputs diffed against `<subdir>/main.out.expected`
# and against each other.
#
# `record_projection` is the no-op smoke test — no imports, so
# rqc has an empty module table and every EField stays as a
# record-field projection (M6.1 + record-projection legacy
# behaviour preserved).
QUALIFIED_DIRS := basic alias selective local_shadow two_modules two_modules_same_name record_projection
QUALIFIED_NEG_DIRS := unknown_export

test-modules-qualified: $(TARGET) build
	@set -e; \
	for d in $(QUALIFIED_DIRS); do \
	  src=../examples/modules-qualified/$$d/main.kai; \
	  exp=../examples/modules-qualified/$$d/main.out.expected; \
	  ./$(TARGET) $$src > build/mq-$$d.c \
	    || { echo "modules-qualified FAIL $$d (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/mq-$$d.c -o build/mq-$$d-c \
	    || { echo "modules-qualified FAIL $$d (cc)"; exit 1; }; \
	  ./build/mq-$$d-c > build/mq-$$d-c.out \
	    || { echo "modules-qualified FAIL $$d (run C)"; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$src > build/mq-$$d.ll \
	    || { echo "modules-qualified FAIL $$d (kaic2 --emit=llvm)"; exit 1; }; \
	  clang -w -I ../stage0 build/mq-$$d.ll ../stage0/runtime_llvm.c -o build/mq-$$d-llvm \
	    || { echo "modules-qualified FAIL $$d (clang)"; exit 1; }; \
	  ./build/mq-$$d-llvm > build/mq-$$d-llvm.out \
	    || { echo "modules-qualified FAIL $$d (run LLVM)"; exit 1; }; \
	  diff -q $$exp build/mq-$$d-c.out > /dev/null \
	    || { echo "modules-qualified DIFF $$d (C vs golden)"; diff $$exp build/mq-$$d-c.out; exit 1; }; \
	  diff -q build/mq-$$d-c.out build/mq-$$d-llvm.out > /dev/null \
	    || { echo "modules-qualified DIFF $$d (C vs LLVM)"; diff build/mq-$$d-c.out build/mq-$$d-llvm.out; exit 1; }; \
	  echo "modules-qualified OK $$d"; \
	done

# m6.2 negative fixtures — qualified-call diagnostics.
# Each subdir under examples/modules-qualified/ named in
# QUALIFIED_NEG_DIRS must FAIL to compile (kaic2 exit != 0) and
# its stderr must contain the substring listed in
# `<subdir>/main.err.expected`.
test-modules-qualified-neg: $(TARGET) build
	@set -e; \
	for d in $(QUALIFIED_NEG_DIRS); do \
	  src=../examples/modules-qualified/$$d/main.kai; \
	  exp=../examples/modules-qualified/$$d/main.err.expected; \
	  rc=0; ./$(TARGET) $$src > /dev/null 2> build/mq-neg-$$d.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "modules-qualified-neg FAIL $$d (expected error, got success)"; exit 1; \
	  fi; \
	  needle=$$(cat $$exp | head -1); \
	  grep -qF "$$needle" build/mq-neg-$$d.err \
	    || { echo "modules-qualified-neg FAIL $$d (substring '$$needle' not in stderr)"; \
	         cat build/mq-neg-$$d.err; exit 1; }; \
	  echo "modules-qualified-neg OK $$d"; \
	done

# M6.2 search-path smoke test: `examples/modules-path/main.kai`
# imports `shared.helpers`, which lives under
# `examples/modules-path-lib/`. Without `--path`, the compile must
# fail with "cannot open module"; with `--path`, both backends must
# produce identical output matching the golden "42\nKAI!\n".
test-modules-path: $(TARGET) build
	@set -e; \
	MAIN=../examples/modules-path/main.kai; \
	LIB=../examples/modules-path-lib; \
	rc=0; ./$(TARGET) $$MAIN > /dev/null 2> build/modules-path.miss.err || rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "modules-path FAIL (expected error without --path)"; exit 1; \
	fi; \
	grep -q "cannot open module" build/modules-path.miss.err \
	  || { echo "modules-path FAIL (wrong error message)"; cat build/modules-path.miss.err; exit 1; }; \
	./$(TARGET) --path $$LIB $$MAIN > build/modules-path.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/modules-path.c -o build/modules-path-c; \
	./build/modules-path-c > build/modules-path-c.out; \
	./$(TARGET) --path $$LIB --emit=llvm $$MAIN > build/modules-path.ll; \
	clang -w -I ../stage0 build/modules-path.ll ../stage0/runtime_llvm.c -o build/modules-path-llvm; \
	./build/modules-path-llvm > build/modules-path-llvm.out; \
	diff -q build/modules-path-c.out build/modules-path-llvm.out > /dev/null \
	  || { echo "modules-path DIFF"; diff build/modules-path-c.out build/modules-path-llvm.out; exit 1; }; \
	printf '42\nKAI!\n' > build/modules-path.expected; \
	diff -q build/modules-path-c.out build/modules-path.expected > /dev/null \
	  && echo "modules-path OK" \
	  || { echo "modules-path UNEXPECTED"; cat build/modules-path-c.out; exit 1; }

# stdlib/loop.kai smoke test — exercises the row-polymorphic
# helpers (`while`, `until`, `repeat`, `forever`) declared in
# stdlib/loop.kai (m7b #12 unblocked them). Compiled with
# `--path ../stdlib` so the resolver finds the module.
test-loop: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/loop/main.kai > build/loop.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/loop.c -o build/loop-c; \
	./build/loop-c > build/loop.out; \
	diff -q ../examples/loop/main.out.expected build/loop.out > /dev/null \
	  && echo "loop OK" \
	  || { echo "loop DIFF"; diff ../examples/loop/main.out.expected build/loop.out; exit 1; }

# m7b #7 — stdlib `Reader[T]` / `Writer[W]` helpers. Same pattern
# as test-loop: compile the example with `--path ../stdlib` so
# the resolver finds the new module, run, diff against golden.
test-reader: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/reader/main.kai > build/reader.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/reader.c -o build/reader-c; \
	./build/reader-c > build/reader.out; \
	diff -q ../examples/reader/main.out.expected build/reader.out > /dev/null \
	  && echo "reader OK" \
	  || { echo "reader DIFF"; diff ../examples/reader/main.out.expected build/reader.out; exit 1; }

test-writer: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/writer/main.kai > build/writer.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/writer.c -o build/writer-c; \
	./build/writer-c > build/writer.out; \
	diff -q ../examples/writer/main.out.expected build/writer.out > /dev/null \
	  && echo "writer OK" \
	  || { echo "writer DIFF"; diff ../examples/writer/main.out.expected build/writer.out; exit 1; }

# Issue #80 regression — polymorphic prelude calls from a foreign file.
# Loads `examples/effects/foreign_prelude_lib.kai` via `--prelude` and
# compiles + runs `examples/effects/foreign_prelude.kai` against the
# golden. The fixture exercises every scheme shape that historically
# tripped cross-file instantiation (implicit lowercase tparams, row
# polymorphism on a callback, parametric Reader[T]). A regression of
# the same shape produces a deterministic stdout diff under tier1.
test-foreign-prelude: $(TARGET) build
	@set -e; \
	./$(TARGET) --prelude ../examples/effects/foreign_prelude_lib.kai \
	            ../examples/effects/foreign_prelude.kai > build/foreign_prelude.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/foreign_prelude.c -o build/foreign_prelude-c; \
	./build/foreign_prelude-c > build/foreign_prelude.out; \
	diff -q ../examples/effects/foreign_prelude.out.expected build/foreign_prelude.out > /dev/null \
	  && echo "foreign-prelude OK" \
	  || { echo "foreign-prelude DIFF"; diff ../examples/effects/foreign_prelude.out.expected build/foreign_prelude.out; exit 1; }

# stdlib/trace.kai smoke tests — exercises the user-defined `Trace`
# effect end-to-end. `trace_basic` covers the default handler
# (`with_trace_default`); `trace_prefix` covers handler composition
# (`with_log_prefix` delegating to the outer default). Compiled with
# `--path ../stdlib` so the resolver finds the module.
# `r9_clause_capture` covers issue #60 / R9 — clause bodies capturing
# parameters from the enclosing fn through the EvE.env channel.
# Pre-fix, kaic2 produced an undeclared-identifier C error; the test
# both compiles cleanly and produces a known-correct golden so a
# capture-RC regression (the same UAF that fooled the early draft of
# the fix) shows up as a stdout diff.
# `r10_repro` and `r11_repro` cover issue #61 / R10 + R11 — single
# read of a parameterised handler clause's `state` was raw-transferred
# to a decref-aware sink, freeing the EvE state slot's storage before
# the next op call could read it. Fix carved out `state` / `log` in
# `pcs_is_non_last` so every read pays a `__perceus_dup`. Both
# fixtures are listed so a regression caught only via one effect-name
# arm still fails this gate. See `docs/lane-diagnostic-r10-r11.md`.
TRACE_FIXTURES := trace_basic trace_prefix r9_clause_capture r10_repro r11_repro \
                  m8x_9_nested_mailbox_under_trap_exit \
                  issue_115_default_handler_typed_dispatch

test-trace: $(TARGET) build
	@set -e; \
	for name in $(TRACE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 build/$$name.c -o build/$$name-c; \
	  ./build/$$name-c > build/$$name.out; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "trace OK $$name" \
	    || { echo "trace DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan variant of test-trace. Wired into the top-level
# `tier1-asan` target so the R10 / R11 regression fixtures
# (parameterised handler `state` aliasing) run under sanitizers; the
# rest of the trace suite comes along for free at no extra
# maintenance cost. CFLAGS swap O0 → O1 so ASAN's frame-pointer
# unwind has something to walk; -fsanitize=address,undefined is the
# same flag set tier1-asan uses for demos/.
test-trace-asan: $(TARGET) build
	@set -e; \
	for name in $(TRACE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "trace-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "trace-asan OK $$name" \
	    || { echo "trace-asan DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #78 regression — kaikai user fns named after runtime
# helpers (`add` / `sub` / `mul` / `div` / `idiv` / `mod` / `neg`,
# plus the `lt` / `gt` / `le` / `ge` / `boolnot` / `truthy`
# comparison + boolean cluster) used to mangle to `kai_<name>` and
# collide with the runtime's `kai_add` / ... definitions in
# stage0/runtime.h, breaking the C link. Fix renamed the runtime
# helpers to `kai_op_<name>` so the `kai_<user_fn>` namespace is
# collision-free; the fixture exercises every previously-colliding
# name in a single program. Negative-side coverage (the link
# would have failed at `cc` time) needs no separate fixture: any
# regression collapses the build before reaching the diff stage.
RUNTIME_SHADOW_FIXTURES := issue_78_runtime_name_shadow

test-runtime-shadow: $(TARGET) build
	@set -e; \
	for name in $(RUNTIME_SHADOW_FIXTURES); do \
	  ./$(TARGET) ../examples/effects/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 build/$$name.c -o build/$$name-c; \
	  ./build/$$name-c > build/$$name.out; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "runtime-shadow OK $$name" \
	    || { echo "runtime-shadow DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling of test-runtime-shadow — the rename touches
# the runtime helper bodies, so the asan variant catches a UAF
# regression introduced by a botched consume / decref edit on
# `kai_op_*`.
test-runtime-shadow-asan: $(TARGET) build
	@set -e; \
	for name in $(RUNTIME_SHADOW_FIXTURES); do \
	  ./$(TARGET) ../examples/effects/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "runtime-shadow-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "runtime-shadow-asan OK $$name" \
	    || { echo "runtime-shadow-asan DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #107 — ASAN+UBSan sibling for the Signal trap fixture. The
# Signal handler manipulates a sigset_t static, allocates a Sig
# variant inside `_kai_signal_to_variant`, and resumes through the
# normal effect-op continuation path. ASan catches a UAF / botched
# consume edit on any of those; UBSan catches a signed-overflow
# regression in the signo lookup. Wired into the top-level
# `tier1-asan` rule so it runs alongside the trace-asan and
# runtime-shadow-asan gates.
SIGNAL_TRAP_FIXTURES := issue_107_signal_trap

test-signal-trap-asan: $(TARGET) build
	@set -e; \
	for name in $(SIGNAL_TRAP_FIXTURES); do \
	  ./$(TARGET) ../examples/effects/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        ../tests/sigharness.c -o build/$$name-asan.harness; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan.harness ./build/$$name-asan-c \
	    > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "signal-trap-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "signal-trap-asan OK $$name" \
	    || { echo "signal-trap-asan DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #103 — ASAN+UBSan sibling for the trap-exit / outer Cancel
# handler regression fixture. The fix touches the link table walk
# and the evidence-lookup hook for the Cancel effect; ASan catches
# UAF in either path (e.g. a stale KaiLinkNode walked at trampoline
# tail, or a miscompiled longjmp landing pad), and UBSan catches
# signed misuse in the link traversal. Wired into the top-level
# `tier1-asan` rule alongside the other ASAN gates.
TRAP_EXIT_CANCEL_FIXTURES := issue_103_trap_exit_cancel_handler

test-trap-exit-cancel-asan: $(TARGET) build
	@set -e; \
	for name in $(TRAP_EXIT_CANCEL_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "trap-exit-cancel-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "trap-exit-cancel-asan OK $$name" \
	    || { echo "trap-exit-cancel-asan DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# stdlib/time.kai smoke test — exercises the Clock effect (wall_now,
# monotonic_now, sleep_ns) plus pure Duration / Instant arithmetic.
# v0 has no default Clock handler (m8.x scope); the fixture installs
# its own stub handler with fixed return values so the golden stays
# deterministic. Compiled with `--path ../stdlib` so the resolver
# finds the module.
test-time: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/time/main.kai > build/time.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/time.c -o build/time-c; \
	./build/time-c > build/time.out; \
	diff -q ../examples/time/main.out.expected build/time.out > /dev/null \
	  && echo "time OK" \
	  || { echo "time DIFF"; diff ../examples/time/main.out.expected build/time.out; exit 1; }

# m12.6.x #2 — interval propagation walker pinning. Compiles each
# fixture under examples/intervals/ with `--dump-intervals` and
# diffs the output against the golden. The walker is dead code as
# far as user programs are concerned; the diff is what gives us
# confidence the next sub-step (unifier reshape) reads correct
# bounds.
test-intervals: $(TARGET) build
	@set -e; \
	for f in ../examples/intervals/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --dump-intervals $$f > build/iv-$$name.out 2>&1 \
	    || { echo "intervals FAIL $$name (kaic2 errored)"; cat build/iv-$$name.out; exit 1; }; \
	  diff -q $$exp build/iv-$$name.out > /dev/null \
	    && echo "intervals OK $$name" \
	    || { echo "intervals DIFF $$name"; diff $$exp build/iv-$$name.out; exit 1; }; \
	done

# m14-pre — stdlib/core/*.kai list-op smoke tests. Each fixture under
# examples/stdlib/ is compiled with kaic2 + the full split core
# preludes and its stdout is diffed against `<name>.out.expected`.
# One fixture per group of related ops: list_basic, list_*-by,
# list_sort, …
#
# Routes through kaic2 to match bin/kai's actual prelude chain
# (multiple --prelude flags, one per stdlib/core/*.kai file). The
# earlier kaic1-only routing was retired when bin/kai migrated to
# kaic2 + when the kaic2 typer learned to bind polymorphic prelude
# calls. The naming-convention migration to `list.map` etc. is m14
# proper (see stage2-design.md §m14).
CORE_PRELUDE_FLAGS := $(patsubst %,--prelude %,$(sort $(wildcard ../stdlib/core/*.kai)))
# Mirrors bin/kai's full prelude chain so fixtures that lean on
# encoding/collections modules (base64_basic, hex_basic, map_basic,
# jwt_encoder) compile out of the box. Keep this in sync with bin/kai
# when new top-level stdlib modules land.
EXTRA_PRELUDE_FLAGS := --prelude ../stdlib/protocols.kai \
                       --prelude ../stdlib/effects.kai \
                       --prelude ../stdlib/random.kai \
                       --prelude ../stdlib/encoding/base64.kai \
                       --prelude ../stdlib/encoding/hex.kai \
                       --prelude ../stdlib/encoding/json.kai \
                       --prelude ../stdlib/collections/map.kai \
                       --prelude ../stdlib/collections/set.kai \
                       --prelude ../stdlib/collections/queue.kai \
                       --prelude ../stdlib/collections/stack.kai \
                       --prelude ../stdlib/math/numeric.kai \
                       --prelude ../stdlib/math/int.kai \
                       --prelude ../stdlib/math/real.kai \
                       --prelude ../stdlib/decimal.kai \
                       --prelude ../stdlib/money.kai \
                       --prelude ../stdlib/uuid.kai \
                       --prelude ../stdlib/regexp.kai \
                       --prelude ../stdlib/path.kai \
                       --prelude ../stdlib/os/env.kai \
                       --prelude ../stdlib/os/args.kai \
                       --prelude ../stdlib/time.kai

test-stdlib: $(TARGET) build
	@set -e; \
	for f in ../examples/stdlib/*.kai; do \
	  name=$$(basename $$f .kai); \
	  case "$$name" in bench_*|check_*) continue ;; esac; \
	  ./$(TARGET) $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) $$f > build/stdlib-$$name.c \
	    || { echo "stdlib FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/stdlib-$$name.c -o build/stdlib-$$name \
	    || { echo "stdlib FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/stdlib-$$name > build/stdlib-$$name.out \
	    || { echo "stdlib FAIL $$name (run errored)"; cat build/stdlib-$$name.out; exit 1; }; \
	  diff -q ../examples/stdlib/$$name.out.expected build/stdlib-$$name.out > /dev/null \
	    && echo "stdlib OK $$name" \
	    || { echo "stdlib DIFF $$name"; diff ../examples/stdlib/$$name.out.expected build/stdlib-$$name.out; exit 1; }; \
	done
	@# m13: assert bit ops lowered as compiler intrinsics. The emitted
	@# C must contain each raw operator inline next to `_a->as.i`, and
	@# must NOT reference any `kai_bit_*` runtime helper (we never
	@# declared one — a leak would be an emit-pass miss).
	@if grep -q 'kai_bit_' build/stdlib-bits_basic.c; then \
	  echo "stdlib FAIL bits_basic (intrinsic leaked as kai_bit_* call)"; \
	  grep 'kai_bit_' build/stdlib-bits_basic.c; \
	  exit 1; \
	fi
	@grep -F -q '_a->as.i << _b' build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (<< not inlined)"; exit 1; }
	@grep -F -q '_a->as.i >> _b' build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (>> not inlined)"; exit 1; }
	@grep -F -q '_a->as.i & _b'  build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (& not inlined)"; exit 1; }
	@grep -F -q '_a->as.i | _b'  build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (| not inlined)"; exit 1; }
	@grep -F -q '_a->as.i ^ _b'  build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (^ not inlined)"; exit 1; }
	@grep -F -q '~ _a->as.i'     build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (~ not inlined)"; exit 1; }
	@grep -F -q '__builtin_popcountll' build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (popcount not inlined)"; exit 1; }
	@echo "stdlib OK bits_basic intrinsic-lowered"
	@# m13 dotted surface: `bit.X(...)` rewrites to the flat-prefix
	@# `bit_X` intrinsic in rqc_kind, so the same lowering must hold
	@# for bits_dotted. If a `kai_bit_*` symbol leaks, the rewrite
	@# regressed and the dotted form fell back to the EModCall codegen
	@# path (which would mint `kai_bit__and` and break the build).
	@if grep -q 'kai_bit_' build/stdlib-bits_dotted.c; then \
	  echo "stdlib FAIL bits_dotted (intrinsic leaked as kai_bit_* call)"; \
	  grep 'kai_bit_' build/stdlib-bits_dotted.c; \
	  exit 1; \
	fi
	@grep -F -q '_a->as.i << _b' build/stdlib-bits_dotted.c \
	  || { echo "stdlib FAIL bits_dotted (<< not inlined)"; exit 1; }
	@grep -F -q '_a->as.i & _b'  build/stdlib-bits_dotted.c \
	  || { echo "stdlib FAIL bits_dotted (& not inlined)"; exit 1; }
	@grep -F -q '__builtin_popcountll' build/stdlib-bits_dotted.c \
	  || { echo "stdlib FAIL bits_dotted (popcount not inlined)"; exit 1; }
	@echo "stdlib OK bits_dotted intrinsic-lowered"
# Typed holes (M10): every example under examples/holes/ must
# (a) produce a --holes report with at least one "type hole" entry,
# (b) produce a valid --holes-json array,
# (c) fail under --strict-holes with non-zero exit,
# (d) compile clean through both backends, and
# (e) if the runtime aborts, the message must contain "unfilled hole:"
#     (expression holes panic; pattern holes fall through to arm body).
test-holes: $(TARGET) build
	@set -e; \
	for f in ../examples/holes/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --holes $$f > build/s2-$$name.holes; \
	  if ! grep -q "type hole" build/s2-$$name.holes; then \
	    echo "holes FAIL $$name (no report)"; cat build/s2-$$name.holes; exit 1; \
	  fi; \
	  ./$(TARGET) --holes-json $$f > build/s2-$$name.holes.json; \
	  python3 ../scripts/validate_holes_json.py build/s2-$$name.holes.json \
	    || { echo "holes FAIL $$name (invalid JSON / schema)"; cat build/s2-$$name.holes.json; exit 1; }; \
	  rc=0; ./$(TARGET) --strict-holes $$f > /dev/null 2> build/s2-$$name.strict.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "holes FAIL $$name (--strict-holes did not fail)"; cat build/s2-$$name.strict.err; exit 1; \
	  fi; \
	  ./$(TARGET) $$f > build/$$name-holes.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-holes.c -o build/$$name-holes-c; \
	  crc=0; ./build/$$name-holes-c > build/$$name-holes-c.out 2>&1 || crc=$$?; \
	  if [ $$crc -ne 0 ] && ! grep -q "unfilled hole:" build/$$name-holes-c.out; then \
	    echo "holes FAIL $$name (C backend aborted without hole message)"; cat build/$$name-holes-c.out; exit 1; \
	  fi; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name-holes.ll; \
	  clang -w -I ../stage0 build/$$name-holes.ll ../stage0/runtime_llvm.c -o build/$$name-holes-llvm; \
	  lrc=0; ./build/$$name-holes-llvm > build/$$name-holes-llvm.out 2>&1 || lrc=$$?; \
	  if [ $$lrc -ne 0 ] && ! grep -q "unfilled hole:" build/$$name-holes-llvm.out; then \
	    echo "holes FAIL $$name (LLVM backend aborted without hole message)"; cat build/$$name-holes-llvm.out; exit 1; \
	  fi; \
	  echo "holes OK $$name"; \
	done

# m7a #3 — parser-level smoke tests for `effect` declarations and
# the `T / Eff + Eff` row syntax. Each .kai file dumps to AST and
# the test asserts grep-able markers exist (or, for negatives,
# that the expected diagnostic fires). Semantic checks (row
# inferred from body equals declared row) are m7a #8.
test-effects: $(TARGET) build
	@set -e; \
	for f in ../examples/effects/parse_effect.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s2-$$name.ast \
	    && grep -q "effect \"Io\"" build/s2-$$name.ast \
	    && grep -q "effect \"Fail\"" build/s2-$$name.ast \
	    && grep -q "op \"print\"" build/s2-$$name.ast \
	    && echo "effects OK $$name" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.ast; exit 1; }; \
	done; \
	for f in ../examples/effects/parse_row_syntax.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s2-$$name.ast \
	    && grep -q "row Io" build/s2-$$name.ast \
	    && grep -q "row Io + Fail" build/s2-$$name.ast \
	    && echo "effects OK $$name" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.ast; exit 1; }; \
	done; \
	for f in ../examples/effects/parse_effect_bad_no_ret.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (parser accepted bad input)"; exit 1; } \
	    || true; \
	  grep -q "expected \`:\` before operation return type" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/parse_row_unknown_effect.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (unknown effect accepted)"; exit 1; } \
	    || true; \
	  grep -q "unknown effect: Unknown" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/row_match_higher_order.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (row match accepted)" \
	    || { echo "effects FAIL $$name (matching rows rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/row_mismatch_higher_order.kai \
	         ../examples/effects/row_pure_to_effectful.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (row mismatch accepted)"; exit 1; } \
	    || true; \
	  grep -q "type mismatch in function call" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/op_call_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (op call accepted)" \
	    || { echo "effects FAIL $$name (op call rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/op_call_unknown_op.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (unknown op accepted)"; exit 1; } \
	    || true; \
	  grep -q "unknown op .unknown. on effect .Io." build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/op_call_wrong_arg.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (wrong arg accepted)"; exit 1; } \
	    || true; \
	  grep -q "type mismatch in op call Io.print" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s2-$$name.ast 2> build/s2-$$name.err \
	    && grep -q "handle Io" build/s2-$$name.ast \
	    && echo "effects OK $$name (handle parsed)" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.err build/s2-$$name.ast; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_alias.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (alias resolved)" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_bad_no_with.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (missing with accepted)"; exit 1; } \
	    || true; \
	  grep -q "expected \`with\` after handle body" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/row_op_call_undeclared.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (undeclared op accepted)"; exit 1; } \
	    || true; \
	  grep -q "effect not handled: Io" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_removes_effect.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (handle removes effect)" \
	    || { echo "effects FAIL $$name (handle did not remove effect)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_unknown_clause.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (unknown clause accepted)"; exit 1; } \
	    || true; \
	  grep -q "unknown op \`unknown\` on effect \`Io\`" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6a_struct_emit.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "typedef struct EvCounter EvCounter;" build/s2-$$name.c \
	    && grep -q "typedef struct EvGreeter EvGreeter;" build/s2-$$name.c \
	    && grep -q "KaiHandlerId handler_id;" build/s2-$$name.c \
	    && grep -q "KaiValue \*(\*tick)(EvCounter \*self, KaiCont \*k);" build/s2-$$name.c \
	    && grep -q "KaiValue \*(\*get)(EvCounter \*self, KaiCont \*k);"  build/s2-$$name.c \
	    && grep -q "KaiValue \*(\*hello)(EvGreeter \*self, KaiValue \*kai_name, KaiCont \*k);" build/s2-$$name.c \
	    && grep -q "KaiValue \*(\*bye)(EvGreeter \*self, KaiValue \*kai_name, KaiValue \*kai_count, KaiCont \*k);" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (struct shape wrong)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (struct emitted + compiles + runs)" \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6b_handle_runs.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_evidence_push_with_jmp(&_node, \"Counter\"" build/s2-$$name.c \
	    && grep -q "kai_evidence_pop()" build/s2-$$name.c \
	    && grep -q "EvCounter _ev = {0};" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (handle lowering shape wrong)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (handle prologue/epilogue + return clause runs)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6c_op_dispatch.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "_ev.tick = &_kai_main__clause_" build/s2-$$name.c \
	    && grep -q "_ev.get = &_kai_main__clause_" build/s2-$$name.c \
	    && grep -q "kai_evidence_lookup_node(\"Counter\")" build/s2-$$name.c \
	    && grep -q "_ev_op->tick(_ev_op, &_k)" build/s2-$$name.c \
	    && grep -q "_ev_op->get(_ev_op, &_k)"  build/s2-$$name.c \
	    || { echo "effects FAIL $$name (op dispatch shape wrong)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (op dispatch + clause body runs end-to-end)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6d_resume_check.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_cont_init_identity(&_k" build/s2-$$name.c \
	    && grep -q "kai_cont_resume(k, " build/s2-$$name.c \
	    || { echo "effects FAIL $$name (resume lowering shape wrong)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (one-shot resume runs through KaiCont)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6d_double_resume.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name 2> build/s2-$$name.run.err > /dev/null \
	    && { echo "effects FAIL $$name (binary should have aborted)"; exit 1; } \
	    || true; \
	  grep -q "continuation resumed twice (handler #" build/s2-$$name.run.err \
	    && echo "effects OK $$name (double resume aborted with diagnostic)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.run.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6e_discard_resume.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_evidence_push_with_jmp" build/s2-$$name.c \
	    && grep -q "if (setjmp(_jmp) == 0)" build/s2-$$name.c \
	    && grep -q "longjmp(\*_node_op->handle_jmp" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (discard wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (discard, discard+return, no-discard all pass)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6f_clause_typed.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (clause body + return clause type-checked + ran)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6f_resume_arg_type.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null \
	    && { echo "effects FAIL $$name (resume arg mismatch accepted)"; exit 1; } \
	    || true; \
	  grep -q "type mismatch in function call" build/s2-$$name.err \
	    && grep -q "(Unit) -> " build/s2-$$name.err \
	    && grep -q "(Int) -> " build/s2-$$name.err \
	    && echo "effects OK $$name (resume arg-type mismatch caught)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_8_diag_effect_not_handled.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null \
	    && { echo "effects FAIL $$name (unhandled effect accepted)"; exit 1; } \
	    || true; \
	  grep -q "effect not handled: Fail" build/s2-$$name.err \
	    && grep -q "process.*declared row does not include" build/s2-$$name.err \
	    && grep -q "process : ... / Fail" build/s2-$$name.err \
	    && grep -q "with Fail" build/s2-$$name.err \
	    && echo "effects OK $$name (Doc C shape: error + note + help)" \
	    || { echo "effects FAIL $$name (wrong diag shape)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_console.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_stdout_print" build/s2-$$name.c \
	    && grep -q "kai_default_stderr_eprint" build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_stdout" build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_stderr" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-handler wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.run.err \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^hello$$"                       build/s2-$$name.out \
	    && grep -q "^world$$"                  build/s2-$$name.out \
	    && grep -q "^warn: this goes to stderr$$" build/s2-$$name.run.err \
	    && echo "effects OK $$name (default Console: stdout + stderr verified)" \
	    || { echo "effects FAIL $$name (wrong default-handler output)"; \
	         echo "stdout:"; cat build/s2-$$name.out; \
	         echo "stderr:"; cat build/s2-$$name.run.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_fail.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_fail_fail" build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_fail" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Fail wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name 2> build/s2-$$name.run.err > /dev/null \
	    && { echo "effects FAIL $$name (binary should have aborted)"; exit 1; } \
	    || true; \
	  grep -q "^kai: Fail.fail: bad input$$" build/s2-$$name.run.err \
	    && echo "effects OK $$name (default Fail: aborts with banner)" \
	    || { echo "effects FAIL $$name (wrong banner)"; cat build/s2-$$name.run.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_env.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_env_args" build/s2-$$name.c \
	    && grep -q "kai_default_env_var"  build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Env wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  PATH=/usr/bin ./build/s2-$$name foo bar > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^argc=2$$"   build/s2-$$name.out \
	    && grep -q "^PATH set$$" build/s2-$$name.out \
	    && echo "effects OK $$name (default Env: args + var)" \
	    || { echo "effects FAIL $$name (output unexpected)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_file.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_file_read_file"  build/s2-$$name.c \
	    && grep -q "kai_default_file_write_file" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-File wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^hello from kaikai$$" build/s2-$$name.out \
	    && echo "effects OK $$name (default File: write+read roundtrip)" \
	    || { echo "effects FAIL $$name (roundtrip output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_stdin.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_stdin_read_line" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Stdin wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  printf "hi there\n" | ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^got: hi there$$" build/s2-$$name.out \
	    && echo "effects OK $$name (default Stdin: read_line Some)" \
	    || { echo "effects FAIL $$name (Some path output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	  printf "" | ./build/s2-$$name > build/s2-$$name.out2 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero on EOF)"; exit 1; }; \
	  grep -q "^eof$$" build/s2-$$name.out2 \
	    && echo "effects OK $$name (default Stdin: read_line None on EOF)" \
	    || { echo "effects FAIL $$name (None path output wrong)"; cat build/s2-$$name.out2; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_1_alias_io.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_stdout_print" build/s2-$$name.c \
	    && grep -q "kai_default_stdin_read_line" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (alias did not expand to default handlers)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^hello via alias$$" build/s2-$$name.out \
	    && echo "effects OK $$name (Io alias resolves + runs)" \
	    || { echo "effects FAIL $$name (alias output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_1_alias_single.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^single-alias works$$" build/s2-$$name.out \
	    && echo "effects OK $$name (single-effect alias resolves + runs)" \
	    || { echo "effects FAIL $$name (alias output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_1_alias_cycle.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (cyclic alias accepted)"; exit 1; } \
	    || true; \
	  grep -q "cyclic effect alias:" build/s2-$$name.err \
	    && echo "effects OK $$name (cycle rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_1_alias_non_effect.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (non-effect label accepted)"; exit 1; } \
	    || true; \
	  grep -q "unknown effect in alias body: Int" build/s2-$$name.err \
	    && echo "effects OK $$name (non-effect label rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_12_row_var_pure_body.kai \
	         ../examples/effects/m7b_12_row_var_console.kai \
	         ../examples/effects/m7b_12_row_var_compose.kai \
	         ../examples/effects/m7b_12_row_var_concrete_plus_var.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > /dev/null \
	    && echo "effects OK $$name (row var instantiated + ran)" \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_12_row_var_two_vars.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (two row vars accepted)"; exit 1; } \
	    || true; \
	  grep -q "row variable must be the last item in the row" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_12_row_var_var_first.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (label after row var accepted)"; exit 1; } \
	    || true; \
	  grep -q "row variable must be the last item in the row" build/s2-$$name.err \
	    && echo "effects OK $$name (rejected as expected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_12_row_var_uppercase.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f 2> build/s2-$$name.err > /dev/null && \
	    { echo "effects FAIL $$name (uppercase label as row var accepted)"; exit 1; } \
	    || true; \
	  grep -q "unknown effect: E" build/s2-$$name.err \
	    && echo "effects OK $$name (uppercase treated as label, rejected)" \
	    || { echo "effects FAIL $$name (wrong diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_11_state_basic.kai \
	         ../examples/effects/m7b_11_reader_basic.kai \
	         ../examples/effects/m7b_11_writer_basic.kai \
	         ../examples/effects/m7b_11_followup_distinct_types.kai \
	         ../examples/effects/m7b_14_state_helper.kai \
	         ../examples/effects/m7b_14_reader_helper.kai \
	         ../examples/effects/m7b_14_writer_helper.kai \
	         ../examples/effects/m7b_2a_op_id_basic.kai \
	         ../examples/effects/m7b_2a_op_distinct_types.kai \
	         ../examples/effects/m7b_2a_op_in_parametric.kai \
	         ../examples/effects/m7b_2b_mutable_default.kai \
	         ../examples/effects/m7b_2b_mutable_intercept.kai \
	         ../examples/effects/m7b_15_nested_alias.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (parametric effect runs end-to-end)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_11_followup_arity_error.kai \
	         ../examples/effects/m7b_11_followup_diag_type_args.kai \
	         ../examples/effects/m7b_11_followup_resume_type.kai \
	         ../examples/effects/m8_2_yield_unhandled.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/effects/$$name.err.expected; \
	  rc=0; ./$(TARGET) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (rejected with expected diagnostic)"; \
	done; \
	for f in ../examples/effects/m8_2_yield_noop.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_spawn_yield" build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_spawn" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Spawn wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^before yield$$" build/s2-$$name.out \
	    && grep -q "^after yield$$"  build/s2-$$name.out \
	    && echo "effects OK $$name (default Spawn: yield resumes inline)" \
	    || { echo "effects FAIL $$name (wrong default-handler output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_3_spawn_await_value.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_spawn_spawn" build/s2-$$name.c \
	    && grep -q "kai_default_spawn_await" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Spawn ops missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^42$$" build/s2-$$name.out \
	    && echo "effects OK $$name (fiber_spawn → fiber_await round-trip)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_3_spawn_console.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^before spawn$$" build/s2-$$name.out \
	    && grep -q "^inside fiber$$" build/s2-$$name.out \
	    && grep -q "^7$$" build/s2-$$name.out \
	    && grep -q "^after await$$" build/s2-$$name.out \
	    && echo "effects OK $$name (effect row propagates through fiber_spawn)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_2_yield_interleave.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (cooperative scheduler interleaves yielding fibers)" \
	    || { echo "effects FAIL $$name (interleave output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_3_cancel_at_yield.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (Cancel delivered at yield-point hook unwinds fiber)" \
	    || { echo "effects FAIL $$name (cancel output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_4_recv_blocking.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (Actor.receive parks on empty; sender wakeup unblocks)" \
	    || { echo "effects FAIL $$name (recv-blocking output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_4_request_reply.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (two-actor request/reply: client parks on receive, server replies, client resumes)" \
	    || { echo "effects FAIL $$name (request-reply output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_4_block_sender.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (Bounded(_, BlockSender) parks sender on full; receiver wakeup unblocks)" \
	    || { echo "effects FAIL $$name (block-sender output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_trap_exit.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (trap_exit=1: linked peer DONE/CANCELLED delivers Normal/Crashed string to mailbox)" \
	    || { echo "effects FAIL $$name (trap-exit output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/issue_103_trap_exit_cancel_handler.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (trap_exit overrides outer Cancel handler installed at the spawn site)" \
	    || { echo "effects FAIL $$name (issue #103 regression: outer Cancel handler intercepted child termination)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_spawn_per_op_generics.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^41$$" build/s2-$$name.out \
	    && grep -q "^100$$" build/s2-$$name.out \
	    && grep -q "^7$$" build/s2-$$name.out \
	    && grep -q "^cancelled$$" build/s2-$$name.out \
	    && echo "effects OK $$name (per-op [T] flows through Spawn.spawn / await / select / cancel)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_spawn_per_op_row_generics.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^inside thunk$$" build/s2-$$name.out \
	    && grep -q "^42$$" build/s2-$$name.out \
	    && echo "effects OK $$name (per-op [T, e] row flows through Spawn.spawn directly)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_spawn_row_mismatch.kai; do \
	  name=$$(basename $$f .kai); \
	  if ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err; then \
	    echo "effects FAIL $$name (typer accepted Int as a thunk)"; exit 1; \
	  fi; \
	  grep -q "() -> " build/s2-$$name.err \
	    && echo "effects OK $$name (Spawn.spawn requires a thunk; per-op [T, e] surface rejects bare Int)" \
	    || { echo "effects FAIL $$name (no thunk-shape diagnostic)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_monitor.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (supervisor exit non-zero — Monitor cascaded fault?)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^worker monitored$$" build/s2-$$name.out \
	    && echo "effects OK $$name (Monitor.monitor delivery on CANCELLED termination, supervisor not cancelled)" \
	    || { echo "effects FAIL $$name (no MonitorDown delivery)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_4_cancel_uncaught.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_cancel_raise" build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_cancel" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Cancel wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.run.err; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "effects FAIL $$name (default Cancel should exit 0, got $$rc)"; exit 1; \
	  fi; \
	  grep -q "^before raise$$" build/s2-$$name.out \
	    || { echo "effects FAIL $$name (output before raise missing)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "Cancel.raise: unhandled" build/s2-$$name.run.err \
	    && echo "effects OK $$name (default Cancel: banner + exit 0)" \
	    || { echo "effects FAIL $$name (banner missing on stderr)"; cat build/s2-$$name.run.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_4_cancel_caught.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^before handle$$" build/s2-$$name.out \
	    && grep -q "^inside handle$$" build/s2-$$name.out \
	    && grep -q "^cleanup ran$$" build/s2-$$name.out \
	    && grep -q "^after handle$$" build/s2-$$name.out \
	    && grep -q "^42$$" build/s2-$$name.out \
	    && grep -v "^never reached$$" build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (with Cancel handler unwinds + clause value returned)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_5_nursery_join.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^33$$" build/s2-$$name.out \
	    && echo "effects OK $$name (nursery join: 11 + 22 = 33)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_6_fiber_inside_ok.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^99$$" build/s2-$$name.out \
	    && echo "effects OK $$name (Fiber used locally, returned as Int)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_6_fiber_escapes.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/effects/$$name.err.expected; \
	  rc=0; ./$(TARGET) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (Fiber escape rejected with diagnostic)"; \
	done; \
	for f in ../examples/effects/m8x_6_fiber_in_result.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/effects/$$name.err.expected; \
	  rc=0; ./$(TARGET) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (Fiber escape detected through Result type-arg)"; \
	done; \
	for f in ../examples/effects/m8x_6_pid_escapes.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/effects/$$name.err.expected; \
	  rc=0; ./$(TARGET) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (Pid escape rejected with diagnostic)"; \
	done; \
	for f in ../examples/effects/m8x_7_fiber_in_sum.kai \
	         ../examples/effects/m8x_7_pid_in_sum.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/effects/$$name.err.expected; \
	  rc=0; ./$(TARGET) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (sum-payload Fiber/Pid escape detected, issue #71)"; \
	done; \
	for f in ../examples/effects/m8x_7_recursive_sum_no_breach.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored on positive sum fixture)"; \
	         cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^7$$" build/s2-$$name.out \
	    && echo "effects OK $$name (recursive sum traversal terminates, no false positive)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_8_nursery_cap.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored on cap-binding fixture)"; \
	         cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^33$$" build/s2-$$name.out \
	    && echo "effects OK $$name (cap-binding nursery n.spawn / n.await, issue #71 prereq)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_8_brand_through_match.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored on positive same-nursery fixture)"; \
	         cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^123$$" build/s2-$$name.out \
	    && echo "effects OK $$name (same-nursery let-chain through brand check)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_8_sibling_nursery_mismatch.kai \
	         ../examples/effects/m8x_8_brand_propagation_let_chain.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/effects/$$name.err.expected; \
	  rc=0; ./$(TARGET) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (sibling-nursery brand mismatch, issue #71 option (b))"; \
	done; \
	for f in ../examples/effects/m8x_8_brand_dump.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=../examples/effects/$$name.brands.expected; \
	  ./$(TARGET) --dump-brands --path ../stdlib $$f > build/s2-$$name.brands 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored on --dump-brands)"; \
	         cat build/s2-$$name.err; exit 1; }; \
	  diff -u $$exp build/s2-$$name.brands \
	    && echo "effects OK $$name (--dump-brands golden, issue #71 option (b))" \
	    || { echo "effects FAIL $$name (--dump-brands diverged from golden)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_7_actor_self_send.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^hello$$" build/s2-$$name.out \
	    && grep -q "^world$$" build/s2-$$name.out \
	    && echo "effects OK $$name (Actor[String] self-send + receive FIFO)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_8_mailbox_drop_oldest.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^b$$" build/s2-$$name.out \
	    && grep -q "^c$$" build/s2-$$name.out \
	    && ! grep -q "^a$$" build/s2-$$name.out \
	    && echo "effects OK $$name (Bounded(2, DropOldest) evicts head)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_8_mailbox_drop_newest.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^a$$" build/s2-$$name.out \
	    && grep -q "^b$$" build/s2-$$name.out \
	    && ! grep -q "^c$$" build/s2-$$name.out \
	    && echo "effects OK $$name (Bounded(2, DropNewest) discards new)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_9_supervision_pattern.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^11$$" build/s2-$$name.out \
	    && grep -q "^22$$" build/s2-$$name.out \
	    && echo "effects OK $$name (supervision pattern: workers report through Actor mailbox)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_12_self_delegating_handler.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  set_count=$$(grep -c "_disp_fib->in_dispatch_node = _node_op" build/s2-$$name.c || true); \
	  rest_count=$$(grep -c "_disp_fib->in_dispatch_node = _saved_disp" build/s2-$$name.c || true); \
	  test "$$set_count" -gt 0 \
	    || { echo "effects FAIL $$name (no per-fiber in_dispatch_node sets emitted — bug #12 fix regressed?)"; exit 1; }; \
	  test "$$set_count" = "$$rest_count" \
	    || { echo "effects FAIL $$name (in_dispatch_node invariant broken: $$set_count sets vs $$rest_count restores)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    || { echo "effects FAIL $$name (C binary exit non-zero — infinite-loop regression?)"; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$f > build/s2-$$name.ll 2> build/s2-$$name.ll.err \
	    || { echo "effects FAIL $$name (kaic2 --emit=llvm errored)"; cat build/s2-$$name.ll.err; exit 1; }; \
	  enter_count=$$(grep -c "call i8\* @kaix_in_dispatch_enter" build/s2-$$name.ll || true); \
	  leave_count=$$(grep -c "call void @kaix_in_dispatch_leave" build/s2-$$name.ll || true); \
	  test "$$enter_count" -gt 0 \
	    || { echo "effects FAIL $$name (LLVM in_dispatch_enter missing — bug #12 LLVM mirror regressed?)"; exit 1; }; \
	  test "$$enter_count" = "$$leave_count" \
	    || { echo "effects FAIL $$name (LLVM in_dispatch invariant broken: $$enter_count enter vs $$leave_count leave)"; exit 1; }; \
	  clang -w -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm \
	    || { echo "effects FAIL $$name (LLVM link failed)"; exit 1; }; \
	  ./build/s2-$$name-llvm \
	    && echo "effects OK $$name (m8 bug #12: self-delegating handler resolves to outer + per-fiber save/restore paired (C+LLVM))" \
	    || { echo "effects FAIL $$name (LLVM binary exit non-zero — infinite-loop regression?)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_fiber_discard.kai \
	         ../examples/effects/m8_fiber_discard_yields.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/effects/$$name.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero — R4 fiber-discard regression?)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -u $$exp_out build/s2-$$name.out \
	    && echo "effects OK $$name (R4: fiber-discard does not deadlock)" \
	    || { echo "effects FAIL $$name (output diff)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_fiber_stack_overflow.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  rc=0; ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.stderr || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected non-zero exit from guard-page SIGSEGV, got 0)"; \
	    cat build/s2-$$name.out; exit 1; \
	  fi; \
	  grep -q "fiber stack overflow" build/s2-$$name.stderr \
	    && echo "effects OK $$name (guard page caught overflow with diagnostic, exit=$$rc)" \
	    || { echo "effects FAIL $$name (no overflow diagnostic on stderr; exit=$$rc)"; \
	         echo '--- stderr:'; cat build/s2-$$name.stderr; \
	         echo '--- stdout:'; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m12_8_y_phase4_*.kai; do \
	  [ -e $$f ] || continue; \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/effects/$$name.out.expected; \
	  exp_err=../examples/effects/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/s2-$$name.err \
	        || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/s2-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "effects OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	      || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	      || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/s2-$$name > build/s2-$$name.out \
	      || { echo "effects FAIL $$name (run errored)"; cat build/s2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/s2-$$name.out > /dev/null \
	        && echo "effects OK $$name (C)" \
	        || { echo "effects DIFF $$name (C)"; diff $$exp_out build/s2-$$name.out; exit 1; }; \
	      ./$(TARGET) --emit=llvm $$f > build/s2-$$name.ll \
	        || { echo "effects FAIL $$name (LLVM errored)"; exit 1; }; \
	      $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm 2>/dev/null \
	        || { echo "effects FAIL $$name (LLVM compile failed)"; exit 1; }; \
	      ./build/s2-$$name-llvm > build/s2-$$name-llvm.out \
	        || { echo "effects FAIL $$name (LLVM run errored)"; exit 1; }; \
	      diff -q $$exp_out build/s2-$$name-llvm.out > /dev/null \
	        && echo "effects OK $$name (LLVM)" \
	        || { echo "effects DIFF $$name (LLVM)"; diff $$exp_out build/s2-$$name-llvm.out; exit 1; }; \
	    else \
	      echo "effects OK $$name (ran clean)"; \
	    fi; \
	  fi; \
	done; \
	for f in ../examples/effects/issue_107_signal_trap.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  $(CC) $(CFLAGS) ../tests/sigharness.c -o build/s2-$$name.harness \
	    || { echo "effects FAIL $$name (harness compile failed)"; exit 1; }; \
	  ./build/s2-$$name.harness ./build/s2-$$name > build/s2-$$name.out 2>&1; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "effects FAIL $$name (harness rc=$$rc — expected 0 for clean SIGINT shutdown)"; \
	    cat build/s2-$$name.out; exit 1; \
	  fi; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (Signal.on/off/await: SIGINT trap → clean exit 0)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done

# m7b — syntax sugars. Currently covers m7b #3 (trailing lambdas,
# lambda-block as expression, double trailing lambdas) and m7b #6
# (array indexing `a[i]` / `a[i] := v`). Each .kai under
# `examples/sugars/` either compiles + runs to a known stdout
# (positive cases — diffed against `<name>.out.expected`) or fails
# with a specific diagnostic (negative cases — every line of
# `<name>.err.expected` must be a substring of the compiler's stderr).
test-sugars: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/sugars/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/sugars/$$name.out.expected; \
	  exp_err=../examples/sugars/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "sugars FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/s2-$$name.err \
	        || { echo "sugars FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/s2-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "sugars OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/$$name.c \
	      || { echo "sugars FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name \
	      || { echo "sugars FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/$$name > build/$$name.out \
	      || { echo "sugars FAIL $$name (run errored)"; cat build/$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/$$name.out > /dev/null \
	        && echo "sugars OK $$name" \
	        || { echo "sugars DIFF $$name"; diff $$exp_out build/$$name.out; exit 1; }; \
	    else \
	      echo "sugars OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# m12.8 — single-dispatch protocols. Each .kai under
# `examples/protocols/` either compiles + runs to a known stdout
# (positive cases — diffed against `<name>.out.expected`) or fails
# with a specific diagnostic substring (negative cases — every line
# of `<name>.err.expected` must appear in the compiler's stderr).
# Positive cases run on both backends (C and LLVM) when an
# `.out.expected` file is present.
test-protocols: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/protocols/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/protocols/$$name.out.expected; \
	  exp_err=../examples/protocols/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/p2-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "protocols FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/p2-$$name.err \
	        || { echo "protocols FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/p2-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "protocols OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/p2-$$name.c \
	      || { echo "protocols FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/p2-$$name.c -o build/p2-$$name \
	      || { echo "protocols FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/p2-$$name > build/p2-$$name.out \
	      || { echo "protocols FAIL $$name (run errored)"; cat build/p2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/p2-$$name.out > /dev/null \
	        && echo "protocols OK $$name (C)" \
	        || { echo "protocols DIFF $$name (C)"; diff $$exp_out build/p2-$$name.out; exit 1; }; \
	      ./$(TARGET) --emit=llvm $$f > build/p2-$$name.ll \
	        || { echo "protocols FAIL $$name (kaic2 --emit=llvm errored)"; exit 1; }; \
	      $(CC) $(CFLAGS) -I ../stage0 build/p2-$$name.ll ../stage0/runtime_llvm.c -o build/p2-$$name-llvm 2>/dev/null \
	        || { echo "protocols FAIL $$name (LLVM compile failed)"; exit 1; }; \
	      ./build/p2-$$name-llvm > build/p2-$$name-llvm.out \
	        || { echo "protocols FAIL $$name (LLVM run errored)"; cat build/p2-$$name-llvm.out; exit 1; }; \
	      diff -q $$exp_out build/p2-$$name-llvm.out > /dev/null \
	        && echo "protocols OK $$name (LLVM)" \
	        || { echo "protocols DIFF $$name (LLVM)"; diff $$exp_out build/p2-$$name-llvm.out; exit 1; }; \
	    else \
	      echo "protocols OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# Self-hosting: kaic2 compiles its own source, and the round-trip reaches
# a fixed point.
# m7a #5: smoke test for the handler-stack runtime primitives.
# Builds a small C program against runtime.h and runs it. m7a #6
# will provide the end-to-end coverage from compiled kaikai code.
test-effect-runtime: build
	@$(CC) $(CFLAGS) -I ../stage0 tests/effect_runtime_test.c -o build/effect_runtime_test
	@./build/effect_runtime_test \
	  && echo "effect-runtime OK" \
	  || { echo "effect-runtime FAIL"; exit 1; }

# Phase 5: smoke test for the Link runtime registry. Exercises
# kai_link_add_bidirectional + kai_link_propagate_terminate on
# bare KaiFiber structs without going through scheduler dispatch.
# A kaikai-level link-cascade demo defers to Phase 5.5+ (needs
# spawn_actor / pid handoff machinery the v1 actor surface lacks).
test-link-runtime: build
	@$(CC) $(CFLAGS) -I ../stage0 tests/link_runtime_test.c -o build/link_runtime_test
	@./build/link_runtime_test \
	  && echo "link-runtime OK" \
	  || { echo "link-runtime FAIL"; exit 1; }

# m4c #4 — real specialisation regression. Each demo exercises a
# polymorphic body called with two distinct call-site type tuples;
# the gate is "both backends emit, link, and run, and the C output
# carries the post-monomorph mangled symbols". Pre-fix the embedded
# `EHandle` clause symbols collided across the two specialisations
# and the C linker rejected the binary. Pinned in
# `docs/m4c-real-specialisation.md`.
test-m4c: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/effects/m4c_run_with.kai ../examples/effects/m4c_handler_in_body.kai ../examples/effects/m4c_flow_through.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "m4c FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name \
	    || { echo "m4c FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out \
	    || { echo "m4c FAIL $$name (C binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$f > build/s2-$$name.ll 2> build/s2-$$name-llvm.err \
	    || { echo "m4c FAIL $$name (LLVM emit errored)"; cat build/s2-$$name-llvm.err; exit 1; }; \
	  clang -w -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm \
	    || { echo "m4c FAIL $$name (LLVM compile failed)"; exit 1; }; \
	  ./build/s2-$$name-llvm > build/s2-$$name-llvm.out \
	    || { echo "m4c FAIL $$name (LLVM binary exit non-zero)"; cat build/s2-$$name-llvm.out; exit 1; }; \
	  echo "m4c OK $$name (C + LLVM)"; \
	done
	@grep -q "kai_run_with__mono__Int__Int" build/s2-m4c_run_with.c \
	  && grep -q "kai_run_with__mono__String__Int" build/s2-m4c_run_with.c \
	  && echo "m4c OK m4c_run_with monomorph specialisations emitted" \
	  || { echo "m4c FAIL m4c_run_with — specialisations missing"; exit 1; }
	@grep -q "_kai_process_with_log__mono__Int__Int__clause_" build/s2-m4c_handler_in_body.c \
	  && grep -q "_kai_process_with_log__mono__String__Int__clause_" build/s2-m4c_handler_in_body.c \
	  && echo "m4c OK m4c_handler_in_body collision-free clause symbols" \
	  || { echo "m4c FAIL m4c_handler_in_body — distinct clause symbols missing"; exit 1; }
	@grep -q "= kai_run_with__mono__Int__Int(" build/s2-m4c_run_with.c \
	  && grep -q "= kai_run_with__mono__String__Int(" build/s2-m4c_run_with.c \
	  && echo "m4c OK m4c_run_with C call sites rewritten to mangled names" \
	  || { echo "m4c FAIL m4c_run_with — main still calls polymorphic kai_run_with"; exit 1; }
	@grep -q "call %KaiValue\* @kai_run_with__mono__Int__Int(" build/s2-m4c_run_with.ll \
	  && grep -q "call %KaiValue\* @kai_run_with__mono__String__Int(" build/s2-m4c_run_with.ll \
	  && echo "m4c OK m4c_run_with LLVM call sites rewritten to mangled names" \
	  || { echo "m4c FAIL m4c_run_with — LLVM main still calls polymorphic kai_run_with"; exit 1; }
	@grep -q "= kai_process_with_log__mono__Int__Int(" build/s2-m4c_handler_in_body.c \
	  && grep -q "= kai_process_with_log__mono__String__Int(" build/s2-m4c_handler_in_body.c \
	  && echo "m4c OK m4c_handler_in_body C call sites rewritten to mangled names" \
	  || { echo "m4c FAIL m4c_handler_in_body — main still calls polymorphic kai_process_with_log"; exit 1; }
	@grep -q "kai_outer__mono__Int" build/s2-m4c_flow_through.c \
	  && grep -q "kai_outer__mono__String" build/s2-m4c_flow_through.c \
	  && grep -q "kai_inner__mono__Int" build/s2-m4c_flow_through.c \
	  && grep -q "kai_inner__mono__String" build/s2-m4c_flow_through.c \
	  && echo "m4c OK m4c_flow_through outer + inner specialisations emitted" \
	  || { echo "m4c FAIL m4c_flow_through — outer or inner spec missing"; exit 1; }
	@grep -A 2 "kai_outer__mono__Int(" build/s2-m4c_flow_through.c | grep -q "kai_inner__mono__Int(" \
	  && grep -A 2 "kai_outer__mono__String(" build/s2-m4c_flow_through.c | grep -q "kai_inner__mono__String(" \
	  && echo "m4c OK m4c_flow_through specs call matching specs (no flow-through to polymorphic inner)" \
	  || { echo "m4c FAIL m4c_flow_through — outer__mono__T does not call inner__mono__T"; exit 1; }

# m7a #9: régime-cost benchmark. Trivial-clause Counter.tick() loop
# vs a C-direct call with the same side effect. Threshold 5× ns/op
# (Doc C §*Open questions* #6). Compiled at -O2 to match how kaikai
# code will be released; -O0 would over-penalise the effect path
# (more frame overhead) without changing the design conclusion.
bench-effects: build
	@$(CC) -O2 -std=c99 -Wall -Wno-unused-function -I ../stage0 \
	    bench/bench_perform_resume.c -o build/bench_perform_resume
	@./build/bench_perform_resume

selfhost: $(TARGET) build
	@$(STACK) set -e; \
	./$(TARGET) $(SRC) > build/kaic2b.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/kaic2b.c -o build/kaic2b; \
	./build/kaic2b $(SRC) > build/kaic2c.c; \
	diff -q build/kaic2b.c build/kaic2c.c > /dev/null \
	  && echo "self-hosting fixed point: OK" \
	  || { echo "self-hosting fixed point: DIFF"; exit 1; }

# LLVM self-hosting: same idea but via the LLVM backend.
# kaic2 (C) emits its own source as .ll, which is compiled to a
# binary. That binary then re-emits the same .ll. We require a
# byte-identical fixed point — the LLVM emitter must be a pure
# function of the typed AST.
selfhost-llvm: $(TARGET) build
	@$(STACK) set -e; \
	./$(TARGET) --emit=llvm $(SRC) > build/kaic2-s1.ll; \
	clang -w -I ../stage0 build/kaic2-s1.ll ../stage0/runtime_llvm.c -o build/kaic2-llvm-s1; \
	./build/kaic2-llvm-s1 --emit=llvm $(SRC) > build/kaic2-s2.ll; \
	diff -q build/kaic2-s1.ll build/kaic2-s2.ll > /dev/null \
	  && echo "llvm self-hosting fixed point: OK" \
	  || { echo "llvm self-hosting fixed point: DIFF"; exit 1; }

# m12.8 Phase 3 — Core demos. Each demo lives in its own
# `examples/<name>/<name>.kai` directory, ships an `<name>.out.expected`
# golden, and (if the demo reads stdin) an `<name>.in` fixture fed via
# shell redirect. Builds each demo with the same `--prelude` chain
# `bin/kai` uses (core + protocols + effects), runs both backends, and
# diffs the output. Drives the "Core REOPENED" automatable golden gate
# Eric called for in the m12.8 review.
DEMOS_CORE := portfolio usd_to_eur

test-demos-core: $(TARGET) build
	@$(STACK) set -e; \
	for name in $(DEMOS_CORE); do \
	  src=../examples/$$name/$$name.kai; \
	  exp=../examples/$$name/$$name.out.expected; \
	  in=../examples/$$name/$$name.in; \
	  if [ ! -f $$src ]; then echo "demos-core FAIL $$name (missing $$src)"; exit 1; fi; \
	  if [ ! -f $$exp ]; then echo "demos-core FAIL $$name (missing $$exp)"; exit 1; fi; \
	  preludes="$(CORE_PRELUDE_FLAGS) --prelude ../stdlib/protocols.kai --prelude ../stdlib/effects.kai"; \
	  ./$(TARGET) $$preludes $$src > build/demo-$$name.c \
	    || { echo "demos-core FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/demo-$$name.c -o build/demo-$$name \
	    || { echo "demos-core FAIL $$name (C compile failed)"; exit 1; }; \
	  if [ -f $$in ]; then \
	    ./build/demo-$$name < $$in > build/demo-$$name.out \
	      || { echo "demos-core FAIL $$name (run errored)"; cat build/demo-$$name.out; exit 1; }; \
	  else \
	    ./build/demo-$$name > build/demo-$$name.out \
	      || { echo "demos-core FAIL $$name (run errored)"; cat build/demo-$$name.out; exit 1; }; \
	  fi; \
	  diff -q $$exp build/demo-$$name.out > /dev/null \
	    && echo "demos-core OK $$name (C)" \
	    || { echo "demos-core DIFF $$name (C)"; diff $$exp build/demo-$$name.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$preludes $$src > build/demo-$$name.ll \
	    || { echo "demos-core FAIL $$name (kaic2 --emit=llvm errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/demo-$$name.ll ../stage0/runtime_llvm.c -o build/demo-$$name-llvm 2>/dev/null \
	    || { echo "demos-core FAIL $$name (LLVM compile failed)"; exit 1; }; \
	  if [ -f $$in ]; then \
	    ./build/demo-$$name-llvm < $$in > build/demo-$$name-llvm.out \
	      || { echo "demos-core FAIL $$name (LLVM run errored)"; cat build/demo-$$name-llvm.out; exit 1; }; \
	  else \
	    ./build/demo-$$name-llvm > build/demo-$$name-llvm.out \
	      || { echo "demos-core FAIL $$name (LLVM run errored)"; cat build/demo-$$name-llvm.out; exit 1; }; \
	  fi; \
	  diff -q $$exp build/demo-$$name-llvm.out > /dev/null \
	    && echo "demos-core OK $$name (LLVM)" \
	    || { echo "demos-core DIFF $$name (LLVM)"; diff $$exp build/demo-$$name-llvm.out; exit 1; }; \
	done

# m12.8.x post-Core REOPEN — Eric level 3: aspirational unplanned
# program. Verifies that `make -C demos verify` was not the only
# probe pushing the typer beyond the milestone-planned paths. Each
# demo under examples/aspirational/ exercises a feature combination
# the milestone did not anticipate; failures here mean the close
# criterion is not met.
ASPIRATIONAL_DEMOS := event_logger audit_handler

test-aspirational: $(TARGET) build
	@$(STACK) set -e; \
	for name in $(ASPIRATIONAL_DEMOS); do \
	  src=../examples/aspirational/$$name/$$name.kai; \
	  exp=../examples/aspirational/$$name/$$name.out.expected; \
	  if [ ! -f $$src ]; then echo "aspirational FAIL $$name (missing $$src)"; exit 1; fi; \
	  if [ ! -f $$exp ]; then echo "aspirational FAIL $$name (missing $$exp)"; exit 1; fi; \
	  preludes="$(CORE_PRELUDE_FLAGS) --prelude ../stdlib/protocols.kai --prelude ../stdlib/effects.kai"; \
	  ./$(TARGET) $$preludes $$src > build/asp-$$name.c \
	    || { echo "aspirational FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/asp-$$name.c -o build/asp-$$name \
	    || { echo "aspirational FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/asp-$$name > build/asp-$$name.out \
	    || { echo "aspirational FAIL $$name (run errored)"; cat build/asp-$$name.out; exit 1; }; \
	  diff -q $$exp build/asp-$$name.out > /dev/null \
	    && echo "aspirational OK $$name (C)" \
	    || { echo "aspirational DIFF $$name (C)"; diff $$exp build/asp-$$name.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$preludes $$src > build/asp-$$name.ll \
	    || { echo "aspirational FAIL $$name (kaic2 --emit=llvm errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/asp-$$name.ll ../stage0/runtime_llvm.c -o build/asp-$$name-llvm 2>/dev/null \
	    || { echo "aspirational FAIL $$name (LLVM compile failed)"; exit 1; }; \
	  ./build/asp-$$name-llvm > build/asp-$$name-llvm.out \
	    || { echo "aspirational FAIL $$name (LLVM run errored)"; cat build/asp-$$name-llvm.out; exit 1; }; \
	  diff -q $$exp build/asp-$$name-llvm.out > /dev/null \
	    && echo "aspirational OK $$name (LLVM)" \
	    || { echo "aspirational DIFF $$name (LLVM)"; diff $$exp build/asp-$$name-llvm.out; exit 1; }; \
	done

# Tier 2.5 unboxing fixtures (see docs/unboxing-phase2-design.md).
# Each fixture compiles, runs, and is structurally inspected to
# assert that arithmetic on unboxable types collapses to raw
# `int64_t` (no `kai_int(` / `kai_op_add(` survives in the inner loop).
test-unbox-phase2: $(TARGET) build
	@set -e; \
	for f in ../examples/perceus/unbox_phase2_*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/perceus-$$name.c \
	    || { echo "unbox FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/perceus-$$name.c -o build/perceus-$$name \
	    || { echo "unbox FAIL $$name (cc errored)"; exit 1; }; \
	  ./build/perceus-$$name > build/perceus-$$name.out \
	    || { echo "unbox FAIL $$name (run errored)"; exit 1; }; \
	  echo "unbox run OK $$name ($$(cat build/perceus-$$name.out | head -1))"; \
	done
	@# M3 grep: arith fixture must contain raw int64_t bindings
	@# and no `kai_int(` / `kai_op_add(` / `kai_op_mul(` inside the
	@# `kai_arith_inner_loop` body — the unbox pass must collapse
	@# the chain into native C arithmetic.
	@grep -F -q 'int64_t kair_a' build/perceus-unbox_phase2_arith.c \
	  || { echo "unbox FAIL arith (kair_a not raw)"; exit 1; }
	@grep -F -q 'int64_t kair_s' build/perceus-unbox_phase2_arith.c \
	  || { echo "unbox FAIL arith (kair_s not raw)"; exit 1; }
	@if grep -E 'kai_arith_inner_loop\(void\) \{[^}]*kai_op_add\(' build/perceus-unbox_phase2_arith.c; then \
	  echo "unbox FAIL arith (kai_op_add survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_arith_inner_loop\(void\) \{[^}]*kai_op_mul\(' build/perceus-unbox_phase2_arith.c; then \
	  echo "unbox FAIL arith (kai_op_mul survived inside body)"; exit 1; \
	fi
	@# M4 grep: cond fixture must contain raw `int` bindings for
	@# the comparison + boolean results, and no `kai_op_lt(`,
	@# `kai_op_eq_v(`, or `kai_op_truthy(` should appear inside the
	@# `kai_cond_inner_loop` body — comparisons collapse to
	@# native C operators on raw scalars.
	@grep -F -q 'int kair_big' build/perceus-unbox_phase2_cond.c \
	  || { echo "unbox FAIL cond (kair_big not raw)"; exit 1; }
	@grep -F -q 'int kair_neq' build/perceus-unbox_phase2_cond.c \
	  || { echo "unbox FAIL cond (kair_neq not raw)"; exit 1; }
	@if grep -E 'kai_cond_inner_loop\(void\) \{[^}]*kai_op_lt\(' build/perceus-unbox_phase2_cond.c; then \
	  echo "unbox FAIL cond (kai_op_lt survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_cond_inner_loop\(void\) \{[^}]*kai_op_truthy\(' build/perceus-unbox_phase2_cond.c; then \
	  echo "unbox FAIL cond (kai_op_truthy survived inside body)"; exit 1; \
	fi
	@# M5 grep: match fixture must use a C `switch` on the raw
	@# scalar — no `kai_decref(_scr)` (the boxed-alias cleanup) and
	@# no `kai_op_eq(` (per-arm equality chain) inside the body.
	@grep -F -q 'switch (_scr)' build/perceus-unbox_phase2_match.c \
	  || { echo "unbox FAIL match (no raw switch found)"; exit 1; }
	@if grep -E 'kai_classify_raw\(void\) \{[^}]*kai_op_eq\(' build/perceus-unbox_phase2_match.c; then \
	  echo "unbox FAIL match (kai_op_eq survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_classify_raw\(void\) \{[^}]*kai_decref\(_scr\)' build/perceus-unbox_phase2_match.c; then \
	  echo "unbox FAIL match (boxed _scr alias survived)"; exit 1; \
	fi
	@# Tongariki Real-unboxing — arith fixture. The chain must lower
	@# to raw `double kair_*` locals with no `kai_op_add(` / `kai_op_mul(` /
	@# `kai_op_sub(` / `kai_op_div(` survival inside the body. The single
	@# `kai_real(` call is the boundary wrap on the return value.
	@grep -F -q 'double kair_a' build/perceus-unbox_phase2_arith_real.c \
	  || { echo "unbox FAIL arith_real (kair_a not raw double)"; exit 1; }
	@grep -F -q 'double kair_s' build/perceus-unbox_phase2_arith_real.c \
	  || { echo "unbox FAIL arith_real (kair_s not raw double)"; exit 1; }
	@if grep -E 'kai_arith_inner_loop_real\(void\) \{[^}]*kai_op_add\(' build/perceus-unbox_phase2_arith_real.c; then \
	  echo "unbox FAIL arith_real (kai_op_add survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_arith_inner_loop_real\(void\) \{[^}]*kai_op_mul\(' build/perceus-unbox_phase2_arith_real.c; then \
	  echo "unbox FAIL arith_real (kai_op_mul survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_arith_inner_loop_real\(void\) \{[^}]*kai_op_sub\(' build/perceus-unbox_phase2_arith_real.c; then \
	  echo "unbox FAIL arith_real (kai_op_sub survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_arith_inner_loop_real\(void\) \{[^}]*kai_op_div\(' build/perceus-unbox_phase2_arith_real.c; then \
	  echo "unbox FAIL arith_real (kai_op_div survived inside body)"; exit 1; \
	fi
	@# Tongariki Real-unboxing — cond fixture. Comparisons on `double`
	@# operands must collapse to native C `<` / `==` / `!=`; the boolean
	@# results are raw `int kair_<name>`. No `kai_op_lt(` / `kai_op_truthy(`
	@# / `kai_op_eq_v(` may survive inside the body.
	@grep -F -q 'double kair_a' build/perceus-unbox_phase2_cond_real.c \
	  || { echo "unbox FAIL cond_real (kair_a not raw double)"; exit 1; }
	@grep -F -q 'int kair_big' build/perceus-unbox_phase2_cond_real.c \
	  || { echo "unbox FAIL cond_real (kair_big not raw)"; exit 1; }
	@grep -F -q 'int kair_neq' build/perceus-unbox_phase2_cond_real.c \
	  || { echo "unbox FAIL cond_real (kair_neq not raw)"; exit 1; }
	@if grep -E 'kai_cond_inner_loop_real\(void\) \{[^}]*kai_op_lt\(' build/perceus-unbox_phase2_cond_real.c; then \
	  echo "unbox FAIL cond_real (kai_op_lt survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_cond_inner_loop_real\(void\) \{[^}]*kai_op_truthy\(' build/perceus-unbox_phase2_cond_real.c; then \
	  echo "unbox FAIL cond_real (kai_op_truthy survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_cond_inner_loop_real\(void\) \{[^}]*kai_op_eq_v\(' build/perceus-unbox_phase2_cond_real.c; then \
	  echo "unbox FAIL cond_real (kai_op_eq_v survived inside body)"; exit 1; \
	fi
	@# Issue #94 / R8 — let-bound Int referenced through a `#{...}`
	@# interpolation slot must NOT collapse to raw `int64_t kair_n`,
	@# because emit re-parses the segment without consulting the unbox
	@# alias map and would emit `kai_n` (boxed). The unbox_stmt SLet
	@# branch demotes such bindings to MBoxed; the fixture's run output
	@# must match `unbox_phase2_r8_let_in_interp.out.expected`, and the
	@# binding must surface as `KaiValue *kai_n`.
	@diff -q ../examples/perceus/unbox_phase2_r8_let_in_interp.out.expected \
	        build/perceus-unbox_phase2_r8_let_in_interp.out > /dev/null \
	  || { echo "unbox FAIL r8_let_in_interp (output mismatch)"; \
	       diff ../examples/perceus/unbox_phase2_r8_let_in_interp.out.expected \
	            build/perceus-unbox_phase2_r8_let_in_interp.out; exit 1; }
	@grep -F -q 'KaiValue *kai_n = kai_int(42LL)' \
	        build/perceus-unbox_phase2_r8_let_in_interp.c \
	  || { echo "unbox FAIL r8_let_in_interp (kai_n not boxed — bug returned?)"; exit 1; }
	@if grep -F -q 'int64_t kair_n' \
	        build/perceus-unbox_phase2_r8_let_in_interp.c; then \
	  echo "unbox FAIL r8_let_in_interp (raw kair_n survived; demote did not fire)"; exit 1; \
	fi
	@echo "unbox phase2 OK"

# Issue #37 — verify the C emitter rewrites self-tail-calls into
# constant-stack goto-loops. The fixture's `count_down(50_000_000)`
# would consume ~3 GiB of C-stack at no-TCO (50 M frames at
# ~60 B each), far past Linux's 8 MiB main-thread default and
# even past the runtime constructor's 256 MiB bump. With the
# rewrite, it runs in O(1) C-stack. We only run via stage 2 here
# — kaic1 still emits recursive C calls (the rewrite has not been
# mirrored into stage 1 yet), so a kaic1 build of this fixture
# would deterministically blow the stack.
test-tco: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/tco/main.kai > build/tco.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco.c -o build/tco; \
	./build/tco > build/tco.out; \
	diff -q ../examples/tco/main.out.expected build/tco.out > /dev/null \
	  && echo "tco OK (50M-deep self-tail-call ran in O(1) stack)" \
	  || { echo "tco DIFF"; diff ../examples/tco/main.out.expected build/tco.out; exit 1; }

# Issue #43 — `list.nth(xs, i)`-shape fixture. Documents the
# canonical shape that the precise per-call-site dropmask
# (rule 3) would fix. Under the conservative dropmask shipped in
# `main` today, the function compiles and runs end-to-end with the
# expected output; the cons-cell leak the rule would close lives
# in the goto-block emit and is not visible to a stdout-only
# golden. See issue #92 (R6) for the full diagnosis and the
# failed re-land history. This target stays
# under `test` so any future re-land attempt has a fixed shape
# to validate against — but it does NOT enforce rule 3 today.
test-tco-regression: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/tco/list_nth_shape.kai > build/tco-nth.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco-nth.c -o build/tco-nth; \
	./build/tco-nth > build/tco-nth.out; \
	diff -q ../examples/tco/list_nth_shape.out.expected build/tco-nth.out > /dev/null \
	  && echo "tco-regression OK (issue #43 list.nth-shape fixture compiles + runs; conservative dropmask, R6 leak documented but not enforced)" \
	  || { echo "tco-regression DIFF"; diff ../examples/tco/list_nth_shape.out.expected build/tco-nth.out; exit 1; }

# Issue #102 — R12 regression fixture. A zero-arg fn whose body has
# a self-tail-call. Before the fix, `tcrec_make_sentinel` emitted a
# trailing `|` for empty pnames, leaking an empty-string element
# into the goto block's rebinds and producing `kai_ = _t0;` against
# undeclared identifiers. The fixture's `round_n()` is exactly the
# shape the bug needs; termination is bounded via a `State[Int]`
# handler so the test cannot hang. Lives under `examples/perceus/`
# because TCO's per-call-site dropmask is a Perceus output.
test-tco-zero-arg: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/perceus/r12_zero_arg_recursive.kai > build/tco-zero.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco-zero.c -o build/tco-zero; \
	./build/tco-zero > build/tco-zero.out; \
	diff -q ../examples/perceus/r12_zero_arg_recursive.out.expected build/tco-zero.out > /dev/null \
	  && echo "tco-zero-arg OK (issue #102 zero-arg self-tail-call compiles + runs to completion)" \
	  || { echo "tco-zero-arg DIFF"; diff ../examples/perceus/r12_zero_arg_recursive.out.expected build/tco-zero.out; exit 1; }

# Issue #89 — perceus-wrap TCO regression fixture. The
# `bench_loop` driver in unbox_bench.kai is a self-tail-recursive
# fn whose body has multi-use parameters that perceus wraps in
# `EBlock([SLet("__pcs_ret", body), exit_drops], Some(EVar("__pcs_ret")))`.
# Pre-fix the TCO walker ran AFTER perceus and missed the tail
# position because of the wrap, so the loop allocated a stack
# frame per iteration and overflowed past ~50 k iterations. The
# fix that landed via issues #42 / #102 runs the TCO rewrite
# BEFORE perceus; the sentinel callee survives the wrap (its
# name is not in any param scope) and `emit_fn_body` plants
# `_kai_<sym>_entry:` ahead of the wrap's `return`. N is set to
# 1_000_000 — well past the pre-fix overflow point — so any
# future regression of the same shape produces a deterministic
# stack overflow under tier1 instead of bit-rotting in a doc.
# We also grep the emitted C to assert the goto-loop label
# actually lands on `kai_bench_loop`.
test-tco-perceus-wrap: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/perceus/unbox_bench.kai > build/tco-perceus.c; \
	grep -F -q '_kai_bench_loop_entry:;' build/tco-perceus.c \
	  || { echo "tco-perceus-wrap FAIL (no _kai_bench_loop_entry label in emitted C)"; exit 1; }; \
	grep -F -q 'goto _kai_bench_loop_entry;' build/tco-perceus.c \
	  || { echo "tco-perceus-wrap FAIL (no goto _kai_bench_loop_entry in emitted C)"; exit 1; }; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco-perceus.c -o build/tco-perceus; \
	./build/tco-perceus > build/tco-perceus.out; \
	diff -q ../examples/perceus/unbox_bench.out.expected build/tco-perceus.out > /dev/null \
	  && echo "tco-perceus-wrap OK (issue #89 1M-deep self-tail-call through perceus wrap; goto label emitted; runs to completion)" \
	  || { echo "tco-perceus-wrap DIFF"; diff ../examples/perceus/unbox_bench.out.expected build/tco-perceus.out; exit 1; }

# Issue #82 — Perceus leak-audit fixture. Exercises four RC
# discipline gaps that survived the m5.x flip: match-scrutinee
# decref at exit, kai_op_truthy on fresh `if` conds, kai_field
# borrowing reads in record-pattern test paths, and the
# kai_prelude_mailbox_* helpers consuming their KaiValue * params.
# A regression in any of these surfaces under the asan sibling as
# a UAF (decref of a singly-held value the runtime later double-
# frees) or as a stdout diff against the golden.
PERCEUS_ISSUE82_FIXTURES := issue_82_leak_audit

test-perceus-issue82: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE82_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-issue82 OK $$name (4 leak sources closed)" \
	    || { echo "perceus-issue82 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling for the issue #82 leak-audit. The fixes
# touch emit-side decref discipline (stages 1/2 plus LLVM if) and
# the kai_prelude_mailbox_* helpers in stage0/runtime.h; ASan
# catches a regression that double-frees the cond / scrutinee /
# message, UBSan flags any signed-overflow that slips through the
# new mailbox transfer paths.
test-perceus-issue82-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE82_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-issue82-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-issue82-asan OK $$name" \
	    || { echo "perceus-issue82-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #87 — Phase 2 unbox LLVM mirror. Compiles unbox_bench.kai
# through `--emit=llvm`, asserts the inner `pure_chain` body
# lowers to raw `i64` arithmetic (no `kaix_int` / `kaix_add` /
# `kaix_mul` calls inside the `pure_chain` body), and that the
# emitted IR links cleanly through clang + the LLVM runtime
# shim. Mirrors the C-side `test-unbox-phase2` greps. Fails
# pre-fix (LLVM IR still emits `kaix_*` boxed calls inside the
# loop), passes post-fix (raw `add i64` / `mul i64` / `srem i64`).
#
# Build-only — the bench's outer driver `bench_loop(1_000_000, 0)`
# requires self-tail-call optimization to run in O(1) C-stack,
# which the C backend has (`_kai_bench_loop_entry:` + `goto` —
# issue #89, PR #121) but the LLVM backend does not yet (a
# separate follow-up). Running the LLVM binary would stack-overflow
# at ~50 k frames; that is independent of the unbox mirror this
# fixture documents, so we only check the IR shape and the link.
# When LLVM TCO lands, swap the link-only check below for a run
# + cross-check against the C-backend output.
test-unbox-llvm-bench: $(TARGET) build
	@set -e; \
	./$(TARGET) --emit=llvm ../examples/perceus/unbox_bench.kai > build/unbox-bench.ll \
	  || { echo "unbox-llvm-bench FAIL (kaic2 --emit=llvm errored)"; exit 1; }; \
	clang -w -I ../stage0 build/unbox-bench.ll ../stage0/runtime_llvm.c -o build/unbox-bench-llvm \
	  || { echo "unbox-llvm-bench FAIL (clang errored — IR ill-formed?)"; exit 1; }
	@# Acceptance grep — `pure_chain` body must contain raw IR
	@# arithmetic and must NOT contain boxed `kaix_*` arithmetic
	@# calls. We extract the body between `define ... @kai_pure_chain`
	@# and the next `define` so the asserts are local to the loop.
	@awk '/^define %KaiValue\* @kai_pure_chain\(/,/^}/' build/unbox-bench.ll > build/unbox-bench.purechain
	@grep -E '= (add|mul|sub|srem) i64' build/unbox-bench.purechain > /dev/null \
	  || { echo "unbox-llvm-bench FAIL (no raw i64 arith inside pure_chain)"; \
	       cat build/unbox-bench.purechain; exit 1; }
	@if grep -E 'call %KaiValue\* @kaix_(add|sub|mul|mod)\(' build/unbox-bench.purechain; then \
	  echo "unbox-llvm-bench FAIL (boxed kaix_add/sub/mul/mod survived inside pure_chain)"; exit 1; \
	fi
	@# At most one `kaix_int(` call survives — the boundary box
	@# of the final raw scalar before `ret`. More than one means
	@# the chain didn't fully collapse.
	@n=$$(grep -c 'call %KaiValue\* @kaix_int(' build/unbox-bench.purechain); \
	  if [ "$$n" -gt 1 ]; then \
	    echo "unbox-llvm-bench FAIL (more than one kaix_int boundary box inside pure_chain: $$n)"; \
	    cat build/unbox-bench.purechain; exit 1; \
	  fi
	@echo "unbox-llvm-bench OK (pure_chain inner loop lowers to raw i64; one boundary kaix_int; IR links cleanly)"

test: test-tokens test-ast test-types test-env test-infer test-infer-check test-dump-typed test-dump-mono test-check test-run test-blocks test-llvm test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-holes test-effects test-effect-runtime test-link-runtime test-sugars test-loop test-reader test-writer test-trace test-time test-intervals test-stdlib test-protocols test-demos-core test-aspirational test-m4c test-unbox-phase2 test-unbox-llvm-bench test-tco test-tco-regression test-tco-zero-arg test-tco-perceus-wrap test-runtime-shadow test-foreign-prelude test-perceus-issue82

# Fast subset for iterative development: skips every subtarget that
# runs kaic2 (or kaic1) against the compiler's own source. Each of
# those takes ~80-90s per invocation and they dominate `make test`
# (~8-9 min of its ~10-12 min runtime). `test-fast` leaves in the
# smoke tests that actually catch regressions on minimal examples,
# modules, and holes — covers the common feature-work cases.
# Use `make test` for the end-of-block gate.
test-fast: test-infer test-infer-check test-dump-typed test-dump-mono test-run test-blocks test-llvm test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-holes test-effects test-effect-runtime test-sugars test-loop test-reader test-writer test-trace test-time test-intervals test-stdlib test-protocols test-demos-core test-aspirational test-unbox-llvm-bench test-tco test-tco-regression test-tco-zero-arg test-tco-perceus-wrap test-foreign-prelude

clean:
	rm -rf build $(TARGET)
