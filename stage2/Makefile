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

# issue #343 — math/real libm bindings (sqrt, sin, log, …) live in
# stage0/runtime.h. Linux glibc requires explicit `-lm` AFTER the
# .c/.o inputs (strict-order ld); macOS libSystem already exposes
# those symbols so the flag is a no-op on Darwin. Trailed at the end
# of every $(CC) compile-and-link line that consumes runtime.h.
LDLIBS_M := -lm

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

.PHONY: all clean test test-fast test-tokens test-ast test-check test-run test-blocks test-holes test-driver-holes test-effects test-effect-runtime test-link-runtime test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-sugars test-pipes test-match test-literals test-string test-loop test-records test-diagnostics test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-stdlib-core-intrinsic test-stdlib-crypto-asan test-stdlib-regex-predicate-asan test-http-client-asan test-env-mutate-asan test-securerandom-asan test-match-pbind-catchall-asan test-protocols test-demos-core test-aspirational test-unbox-phase2 test-unbox-llvm-bench test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-trace test-trace-asan test-issue-501 test-foreign-prelude test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-318-include test-issue-335 test-log-asan test-ffi-extern-c test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-issue-315-no-double-slash test-issue-248-loop-sugars test-perceus-issue118 test-perceus-issue118-asan test-perceus-issue298 test-perceus-issue298-asan test-perceus-issue350 test-perceus-issue350-asan test-unions test-aliases test-sequence test-ufcs test-rc-trace-callsite selfhost test-tco-rule3-basic test-tco-rule3-no-misfire test-violations

all: $(TARGET)

$(KAIC1):
	$(MAKE) -C ../stage1 kaic1

build:
	@mkdir -p build

build/stage2.c: $(SRC) $(KAIC1) | build
	$(STACK) $(KAIC1) $(SRC) > $@

$(TARGET): build/stage2.c
	$(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 $< -o $@ $(LDLIBS_M)

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
	  clang -w -I ../stage0 build/$$name.ll ../stage0/runtime_llvm.c -o build/$$name-llvm $(LDLIBS_M) ; \
	  ./build/$$name-llvm > build/$$name-llvm.out; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name-c $(LDLIBS_M) ; \
	  ./build/$$name-c > build/$$name-c.out; \
	  diff -q build/$$name-llvm.out build/$$name-c.out > /dev/null \
	    && echo "llvm OK $$name" \
	    || { echo "llvm DIFF $$name"; diff build/$$name-llvm.out build/$$name-c.out | head; exit 1; }; \
	done; \
	for name in $(LLVM_EXAMPLES); do \
	  f=../examples/llvm/$$name.kai; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name.ll; \
	  clang -w -I ../stage0 build/$$name.ll ../stage0/runtime_llvm.c -o build/$$name-llvm $(LDLIBS_M) ; \
	  ./build/$$name-llvm > build/$$name-llvm.out; \
	  ./$(TARGET) $$f > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name-c $(LDLIBS_M) ; \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  ../stage1/kaic1 $$f > build/$$name-s1.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-s1.c -o build/$$name-s1 $(LDLIBS_M) ; \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t.c -o build/$$name-t $(LDLIBS_M) ; \
	  ./build/$$name-t 2> build/$$name-t.err || true; \
	  ../stage1/kaic1 --test $$f > build/$$name-t-s1.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-t-s1.c -o build/$$name-t-s1 $(LDLIBS_M) ; \
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
	    $(CC) $(CFLAGS) -I ../stage0 build/cov-$$d-$$name.c -o build/cov-$$d-$$name-c 2>/dev/null $(LDLIBS_M) \
	      || { echo "coverage SKIP $$tag (C compile failed)"; skip=$$((skip+1)); continue; }; \
	    ./build/cov-$$d-$$name-c > build/cov-$$d-$$name-c.out 2>/dev/null \
	      || { echo "coverage SKIP $$tag (C run errored)"; skip=$$((skip+1)); continue; }; \
	    ./$(TARGET) --emit=llvm $$f > build/cov-$$d-$$name.ll 2>/dev/null \
	      || { echo "coverage SKIP $$tag (kaic2 --emit=llvm errored)"; skip=$$((skip+1)); continue; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/cov-$$d-$$name.ll ../stage0/runtime_llvm.c -o build/cov-$$d-$$name-llvm 2>/dev/null $(LDLIBS_M) \
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
	$(CC) $(CFLAGS) -I ../stage0 build/modules.c -o build/modules-c $(LDLIBS_M) ; \
	./build/modules-c > build/modules-c.out; \
	./$(TARGET) --emit=llvm ../examples/modules/main.kai > build/modules.ll; \
	clang -w -I ../stage0 build/modules.ll ../stage0/runtime_llvm.c -o build/modules-llvm $(LDLIBS_M) ; \
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
QUALIFIED_DIRS := basic alias selective local_shadow two_modules two_modules_same_name record_projection qualified_type_in_signature qualified_constructor_disambig
QUALIFIED_NEG_DIRS := unknown_export qualified_type_negative

test-modules-qualified: $(TARGET) build
	@set -e; \
	for d in $(QUALIFIED_DIRS); do \
	  src=../examples/modules-qualified/$$d/main.kai; \
	  exp=../examples/modules-qualified/$$d/main.out.expected; \
	  ./$(TARGET) $$src > build/mq-$$d.c \
	    || { echo "modules-qualified FAIL $$d (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/mq-$$d.c -o build/mq-$$d-c $(LDLIBS_M) \
	    || { echo "modules-qualified FAIL $$d (cc)"; exit 1; }; \
	  ./build/mq-$$d-c > build/mq-$$d-c.out \
	    || { echo "modules-qualified FAIL $$d (run C)"; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$src > build/mq-$$d.ll \
	    || { echo "modules-qualified FAIL $$d (kaic2 --emit=llvm)"; exit 1; }; \
	  clang -w -I ../stage0 build/mq-$$d.ll ../stage0/runtime_llvm.c -o build/mq-$$d-llvm $(LDLIBS_M) \
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

# Issue #502: constructor-name collision with built-in Result/Option
# variants. Two positive multi-file fixtures verify a `pub type` whose
# variant name does NOT collide still exports correctly when declared
# alongside another `pub type` (Nota) — the original bug-report shape.
# A negative single-file fixture verifies the diagnostic when a user
# variant DOES collide with `Result.Ok`.
COLLISION_DIRS := export_constructor_collision export_constructor_collision_reverse
test-modules-collision: $(TARGET) build
	@set -e; \
	for d in $(COLLISION_DIRS); do \
	  src=../examples/modules/$$d/main.kai; \
	  exp=../examples/modules/$$d/main.out.expected; \
	  ./$(TARGET) --path ../examples/modules/$$d $$src > build/mc-$$d.c \
	    || { echo "modules-collision FAIL $$d (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/mc-$$d.c -o build/mc-$$d-c $(LDLIBS_M) \
	    || { echo "modules-collision FAIL $$d (cc)"; exit 1; }; \
	  ./build/mc-$$d-c > build/mc-$$d-c.out \
	    || { echo "modules-collision FAIL $$d (run C)"; exit 1; }; \
	  diff -q $$exp build/mc-$$d-c.out > /dev/null \
	    || { echo "modules-collision DIFF $$d (C vs golden)"; diff $$exp build/mc-$$d-c.out; exit 1; }; \
	  echo "modules-collision OK $$d"; \
	done; \
	src=../examples/modules/export_ambiguous_constructor.kai; \
	exp=../examples/modules/export_ambiguous_constructor.err.expected; \
	rc=0; ./$(TARGET) $$src > /dev/null 2> build/mc-neg.err || rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "modules-collision-neg FAIL (expected error, got success)"; exit 1; \
	fi; \
	needle=$$(cat $$exp | head -1); \
	grep -qF "$$needle" build/mc-neg.err \
	  || { echo "modules-collision-neg FAIL (substring '$$needle' not in stderr)"; \
	       cat build/mc-neg.err; exit 1; }; \
	echo "modules-collision-neg OK"

# Issue #503: `#derive(...)` annotation on a `pub type` previously
# hid the type from the module export table because the export
# collector only matched bare `DType` nodes, not the `DDerive`
# wrapper that the parser produces for annotated decls. Three
# positive multi-file fixtures cover the three relevant shapes:
# derived record, derived sum (constructors must also export),
# and multiple derives on the same record.
DERIVE_EXPORT_DIRS := export_derived_record export_derived_sum export_multiple_derives
test-modules-derive-export: $(TARGET) build
	@set -e; \
	for d in $(DERIVE_EXPORT_DIRS); do \
	  src=../examples/modules/$$d/main.kai; \
	  exp=../examples/modules/$$d/main.out.expected; \
	  ./$(TARGET) --path ../examples/modules/$$d --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) $$src > build/mde-$$d.c \
	    || { echo "modules-derive-export FAIL $$d (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/mde-$$d.c -o build/mde-$$d-c $(LDLIBS_M) \
	    || { echo "modules-derive-export FAIL $$d (cc)"; exit 1; }; \
	  ./build/mde-$$d-c > build/mde-$$d-c.out \
	    || { echo "modules-derive-export FAIL $$d (run C)"; exit 1; }; \
	  diff -q $$exp build/mde-$$d-c.out > /dev/null \
	    || { echo "modules-derive-export DIFF $$d (C vs golden)"; diff $$exp build/mde-$$d-c.out; exit 1; }; \
	  echo "modules-derive-export OK $$d"; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/modules-path.c -o build/modules-path-c $(LDLIBS_M) ; \
	./build/modules-path-c > build/modules-path-c.out; \
	./$(TARGET) --path $$LIB --emit=llvm $$MAIN > build/modules-path.ll; \
	clang -w -I ../stage0 build/modules-path.ll ../stage0/runtime_llvm.c -o build/modules-path-llvm $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/loop.c -o build/loop-c $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/reader.c -o build/reader-c $(LDLIBS_M) ; \
	./build/reader-c > build/reader.out; \
	diff -q ../examples/reader/main.out.expected build/reader.out > /dev/null \
	  && echo "reader OK" \
	  || { echo "reader DIFF"; diff ../examples/reader/main.out.expected build/reader.out; exit 1; }

test-writer: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/writer/main.kai > build/writer.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/writer.c -o build/writer-c $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/foreign_prelude.c -o build/foreign_prelude-c $(LDLIBS_M) ; \
	./build/foreign_prelude-c > build/foreign_prelude.out; \
	diff -q ../examples/effects/foreign_prelude.out.expected build/foreign_prelude.out > /dev/null \
	  && echo "foreign-prelude OK" \
	  || { echo "foreign-prelude DIFF"; diff ../examples/effects/foreign_prelude.out.expected build/foreign_prelude.out; exit 1; }

# m12.7.x FFI v1 regression — `[<extern_c>]` axiom lowers to a
# direct `extern "C"` call. Compiles + runs the fixture on the C
# backend and the LLVM backend so a regression on either path
# (parser, lower_axiom_one, perceus skip, emit_fn_body detection,
# llvm_emit_ffi_shim) shows up under tier1. The fixture stays
# libc-only so it links without extra `cc` flags.
test-ffi-extern-c: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/effects/ffi_extern_c_basic.kai > build/ffi_extern_c_basic.c; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage0 build/ffi_extern_c_basic.c -o build/ffi_extern_c_basic-c $(LDLIBS_M) ; \
	./build/ffi_extern_c_basic-c > build/ffi_extern_c_basic.out; \
	diff -q ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic.out > /dev/null \
	  && echo "ffi-extern-c OK (C backend)" \
	  || { echo "ffi-extern-c DIFF (C backend)"; diff ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic.out; exit 1; }; \
	./$(TARGET) --emit=llvm ../examples/effects/ffi_extern_c_basic.kai > build/ffi_extern_c_basic.ll; \
	clang -w -I ../stage0 build/ffi_extern_c_basic.ll ../stage0/runtime_llvm.c -o build/ffi_extern_c_basic-llvm $(LDLIBS_M) ; \
	./build/ffi_extern_c_basic-llvm > build/ffi_extern_c_basic-llvm.out; \
	diff -q ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic-llvm.out > /dev/null \
	  && echo "ffi-extern-c OK (LLVM backend)" \
	  || { echo "ffi-extern-c DIFF (LLVM backend)"; diff ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic-llvm.out; exit 1; }

# Issue #315 fixture — the `//` integer-division operator was
# retired in favour of plain `/` (which already truncates on `Int`
# operands via the `Div[Int]` primitive impl, post-#246). Source
# still using `//` must be rejected at lex time with a migration
# message pointing at `/`.
test-issue-315-no-double-slash: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/aspirational/issue_315_no_double_slash.err.kai \
	    > build/issue_315_no_double_slash.out 2>&1 \
	  && { echo "issue_315 DID NOT FAIL — `//` should be rejected"; exit 1; } \
	  || true; \
	grep -F "$$(head -1 ../examples/aspirational/issue_315_no_double_slash.err.expected)" \
	     build/issue_315_no_double_slash.out > /dev/null \
	  && echo "issue_315 OK (rejection diagnostic matches)" \
	  || { echo "issue_315 DIFF — expected migration message not in output"; \
	       cat build/issue_315_no_double_slash.out; exit 1; }

# Issue #260 / #261 fixtures — the spec-aligned `extern "C" fn`
# syntax and the optional `("real_symbol")` name override. The basic
# fixture exercises the new parse path + libc puts; the override
# fixture binds `c_strlen` to the C symbol `strlen`; the negative
# fixture confirms the legacy `[<extern_c>] axiom` form is rejected
# with a migration error pointing at the new syntax.
test-issue-260-261-extern-c-fn: $(TARGET) build
	@set -e; \
	for f in extern_c_basic extern_c_name_override; do \
	  src=../examples/ffi/$$f.kai; \
	  exp=../examples/ffi/$$f.out.expected; \
	  ./$(TARGET) $$src > build/$$f.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) ; \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    && echo "$$f OK (C backend)" \
	    || { echo "$$f DIFF (C backend)"; diff $$exp build/$$f.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$src > build/$$f.ll; \
	  clang -w -I ../stage0 build/$$f.ll ../stage0/runtime_llvm.c -o build/$$f-llvm $(LDLIBS_M) ; \
	  ./build/$$f-llvm > build/$$f.out.llvm; \
	  diff -q $$exp build/$$f.out.llvm > /dev/null \
	    && echo "$$f OK (LLVM backend)" \
	    || { echo "$$f DIFF (LLVM backend)"; diff $$exp build/$$f.out.llvm; exit 1; }; \
	done; \
	./$(TARGET) ../examples/ffi/extern_c_old_syntax.err.kai > build/extern_c_old_syntax.out 2>&1 \
	  && { echo "extern_c_old_syntax DID NOT FAIL — legacy syntax should be rejected"; exit 1; } \
	  || true; \
	grep -F "$$(head -1 ../examples/ffi/extern_c_old_syntax.err.expected)" build/extern_c_old_syntax.out > /dev/null \
	  && echo "extern_c_old_syntax OK (rejection diagnostic matches)" \
	  || { echo "extern_c_old_syntax DIFF — expected migration message not in output"; cat build/extern_c_old_syntax.out; exit 1; }

# Issue #244 fixture — `pub axiom` wrapper symbol export across
# modules. The defining module's `[<extern_c>] pub axiom abs(...)`
# is imported by `main.kai` via `import myffi`; the call site emits
# `kai_myffi__abs(...)` and the linker only finds it because the
# lowered FFI shim's `module_origin` was stamped pre-lower (issue
# #244 fix). Without the fix the link step fails with an undefined
# `kai_myffi__abs` reference. Two-file fixture compiled with `--path`
# pointed at the fixture directory so the resolver finds `myffi.kai`
# next to `main.kai`.
test-ffi-pub-axiom-cross-module: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../examples/effects/ffi_pub_axiom_cross_module \
	  ../examples/effects/ffi_pub_axiom_cross_module/main.kai \
	  > build/ffi_pub_axiom_cross_module.c; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage0 \
	  build/ffi_pub_axiom_cross_module.c -o build/ffi_pub_axiom_cross_module-c $(LDLIBS_M); \
	./build/ffi_pub_axiom_cross_module-c > build/ffi_pub_axiom_cross_module.out; \
	diff -q ../examples/effects/ffi_pub_axiom_cross_module/main.out.expected \
	  build/ffi_pub_axiom_cross_module.out > /dev/null \
	  && echo "ffi-pub-axiom-cross-module OK (C backend)" \
	  || { echo "ffi-pub-axiom-cross-module DIFF (C backend)"; \
	       diff ../examples/effects/ffi_pub_axiom_cross_module/main.out.expected \
	            build/ffi_pub_axiom_cross_module.out; exit 1; }; \
	./$(TARGET) --emit=llvm --path ../examples/effects/ffi_pub_axiom_cross_module \
	  ../examples/effects/ffi_pub_axiom_cross_module/main.kai \
	  > build/ffi_pub_axiom_cross_module.ll; \
	clang -w -I ../stage0 build/ffi_pub_axiom_cross_module.ll \
	  ../stage0/runtime_llvm.c -o build/ffi_pub_axiom_cross_module-llvm $(LDLIBS_M); \
	./build/ffi_pub_axiom_cross_module-llvm > build/ffi_pub_axiom_cross_module-llvm.out; \
	diff -q ../examples/effects/ffi_pub_axiom_cross_module/main.out.expected \
	  build/ffi_pub_axiom_cross_module-llvm.out > /dev/null \
	  && echo "ffi-pub-axiom-cross-module OK (LLVM backend)" \
	  || { echo "ffi-pub-axiom-cross-module DIFF (LLVM backend)"; \
	       diff ../examples/effects/ffi_pub_axiom_cross_module/main.out.expected \
	            build/ffi_pub_axiom_cross_module-llvm.out; exit 1; }

# Issue #248 fixtures — stdlib `loop` helpers under the m7b §5
# double-trailing-lambda sugar. Three positive fixtures exercise
# `until { cond } { body }`, `while { cond } { body }`, and the
# `repeat(n) { body }` single-trailing form. Each compiles with
# `--path ../stdlib` so `import loop` resolves; the compile + run
# is the regression — the bug closed here was a typer fault that
# made `import loop` itself fail to typecheck whenever any `repeat`
# from the auto-loaded core preludes (list / string) was a
# candidate. Output diffed against the golden.
test-issue-248-loop-sugars: $(TARGET) build
	@set -e; \
	for f in m7b_5_until_basic m7b_5_while_var m7b_5_repeat_basic; do \
	  src=../examples/sugars/loop/$$f.kai; \
	  exp=../examples/sugars/loop/$$f.out.expected; \
	  ./$(TARGET) --path ../stdlib $$src > build/$$f.c \
	    || { echo "$$f FAIL (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) \
	    || { echo "$$f FAIL (cc)"; exit 1; }; \
	  ./build/$$f-c > build/$$f.out \
	    || { echo "$$f FAIL (run)"; exit 1; }; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    && echo "$$f OK" \
	    || { echo "$$f DIFF"; diff $$exp build/$$f.out; exit 1; }; \
	done

# Tier 1-ASAN counterpart: same fixture, but the linked binary is
# instrumented with AddressSanitizer + UndefinedBehaviorSanitizer
# so any memory misuse in the FFI shim (un-balanced kai_decref, off-
# by-one extraction of the bytes pointer, etc.) trips the sanitizer
# at the exact site instead of corrupting heap state silently.
test-ffi-extern-c-asan: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/effects/ffi_extern_c_basic.kai > build/ffi_extern_c_basic-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable -Wno-absolute-value \
	      -I ../stage0 build/ffi_extern_c_basic-asan.c -o build/ffi_extern_c_basic-c-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/ffi_extern_c_basic-c-asan > build/ffi_extern_c_basic-asan.out 2>&1; \
	diff -q ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic-asan.out > /dev/null \
	  && echo "ffi-extern-c-asan OK (C backend, ASAN+UBSan clean)" \
	  || { echo "ffi-extern-c-asan DIFF (C backend, ASAN+UBSan)"; cat build/ffi_extern_c_basic-asan.out; exit 1; }

# Issue #216 regression — qualified-call resolver inside a
# `--prelude`-loaded file. The helper `qualified_call_in_prelude_lib`
# defines bodies like `list.nth(xs, 1)`; before the fix the prelude
# parse tree skipped the EField->EModCall rewrite and the typer
# rejected the call as "undefined name `list`". Compiled with the
# core stdlib preludes plus the helper file as an extra `--prelude`.
test-prelude-qualified-call: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	            --prelude ../examples/stdlib/qualified_call_in_prelude_lib.kai \
	            ../examples/stdlib/qualified_call_in_prelude.kai \
	            > build/qualified_call_in_prelude.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/qualified_call_in_prelude.c -o build/qualified_call_in_prelude $(LDLIBS_M) ; \
	./build/qualified_call_in_prelude > build/qualified_call_in_prelude.out; \
	diff -q ../examples/stdlib/qualified_call_in_prelude.out.expected build/qualified_call_in_prelude.out > /dev/null \
	  && echo "prelude-qualified-call OK (issue #216)" \
	  || { echo "prelude-qualified-call DIFF"; diff ../examples/stdlib/qualified_call_in_prelude.out.expected build/qualified_call_in_prelude.out; exit 1; }

# Issue #219 — typer EModCall lookup must filter by qualifier so two
# modules exporting the same bare name resolve unambiguously to
# their declaring module. The fixture pairs `list.repeat(0, 3)`
# (core stdlib) with a helper module that also exports
# `repeat(String, Int) -> String`. Pre-fix the typer's flat
# `ty_env_lookup` returned the helper's scheme for `list.repeat(0,
# 3)` and rejected it as `expected: (String, Int) -> String, found:
# (Int, Int) -> ?t`.
test-issue-219-modcall: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	            --prelude ../examples/stdlib/issue_219_typer_modcall_lib.kai \
	            ../examples/stdlib/issue_219_typer_modcall.kai \
	            > build/issue_219_typer_modcall.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/issue_219_typer_modcall.c -o build/issue_219_typer_modcall $(LDLIBS_M) ; \
	./build/issue_219_typer_modcall > build/issue_219_typer_modcall.out; \
	diff -q ../examples/stdlib/issue_219_typer_modcall.out.expected build/issue_219_typer_modcall.out > /dev/null \
	  && echo "issue-219-modcall OK" \
	  || { echo "issue-219-modcall DIFF"; diff ../examples/stdlib/issue_219_typer_modcall.out.expected build/issue_219_typer_modcall.out; exit 1; }

# Issue #318 — `kai test <file>` reports only the file's own test
# blocks. The fixture is a tiny user-style program with two test
# blocks; compiled with --test against the full core prelude set,
# the runner output must list exactly those two and no stdlib tests.
# The opt-in `--include-prelude-tests` re-enables the legacy
# behaviour, exercised by the `-include` companion target which
# greps for stdlib test names instead of pinning a full diff (the
# count drifts as new stdlib tests land).
test-issue-318: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	            --test ../examples/stdlib/issue_318_kai_test_prelude_scope.kai \
	            > build/issue_318_kai_test_prelude_scope.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/issue_318_kai_test_prelude_scope.c -o build/issue_318_kai_test_prelude_scope $(LDLIBS_M) ; \
	./build/issue_318_kai_test_prelude_scope > build/issue_318_kai_test_prelude_scope.out 2>&1; \
	diff -q ../examples/stdlib/issue_318_kai_test_prelude_scope.out.expected build/issue_318_kai_test_prelude_scope.out > /dev/null \
	  && echo "issue-318 OK (user-only tests)" \
	  || { echo "issue-318 DIFF"; diff ../examples/stdlib/issue_318_kai_test_prelude_scope.out.expected build/issue_318_kai_test_prelude_scope.out; exit 1; }

# Issue #318 companion — verify `--include-prelude-tests` reaches
# the prelude's tests. We don't pin a full diff (the stdlib test
# count moves), but we assert a known prelude test name (`is_digit`
# from char.kai) appears in the output and the user's tests still
# pass too.
test-issue-318-include: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	            --include-prelude-tests --test ../examples/stdlib/issue_318_kai_test_prelude_scope.kai \
	            > build/issue_318_include.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/issue_318_include.c -o build/issue_318_include $(LDLIBS_M) ; \
	./build/issue_318_include > build/issue_318_include.out 2>&1; \
	grep -q "issue 318 — first user test runs" build/issue_318_include.out \
	  || { echo "issue-318-include FAIL (missing user test)"; cat build/issue_318_include.out; exit 1; }; \
	grep -q "is_digit" build/issue_318_include.out \
	  || { echo "issue-318-include FAIL (missing prelude test 'is_digit')"; cat build/issue_318_include.out; exit 1; }; \
	echo "issue-318-include OK (prelude tests reachable via opt-in)"

# Issue #335 — file-local precedence for bare-name lookup. Both mod_a
# and mod_b export `foo` with different signatures; tests inside each
# file call `foo(...)` bare and must resolve to that file's own `foo`.
# Pre-fix the resolver picked a sibling module's `foo` because
# `emit_test_fn` did not rotate the EFn table the way `emit_fn_body`
# does for DFn bodies. Run each module as the --test target with the
# other as a --prelude to exercise the cross-module ambiguity.
test-issue-335: $(TARGET) build
	@set -e; \
	for pair in "mod_a:mod_b" "mod_b:mod_a"; do \
	  tgt=$${pair%:*}; sib=$${pair#*:}; \
	  ./$(TARGET) --prelude ../examples/modules-qualified/file_local_wins/$$sib.kai \
	              --test ../examples/modules-qualified/file_local_wins/$$tgt.kai \
	              > build/issue_335_$$tgt.c \
	    || { echo "issue-335 FAIL $$tgt (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/issue_335_$$tgt.c -o build/issue_335_$$tgt $(LDLIBS_M) \
	    || { echo "issue-335 FAIL $$tgt (cc)"; exit 1; }; \
	  ./build/issue_335_$$tgt > build/issue_335_$$tgt.out 2>&1 \
	    || { echo "issue-335 FAIL $$tgt (run)"; cat build/issue_335_$$tgt.out; exit 1; }; \
	  awk '/^[0-9]+\/[0-9]+ tests passed$$/ { split($$1, p, "/"); \
	         if (p[1] > 0 && p[1] == p[2]) { ok = 1 } } \
	       END { exit ok ? 0 : 1 }' build/issue_335_$$tgt.out \
	    || { echo "issue-335 FAIL $$tgt (no all-pass tests-passed line)"; \
	         cat build/issue_335_$$tgt.out; exit 1; }; \
	  echo "issue-335 OK $$tgt (file-local foo resolved correctly)"; \
	done

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
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 build/$$name.c -o build/$$name-c $(LDLIBS_M) ; \
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
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 build/$$name.c -o build/$$name-c $(LDLIBS_M) ; \
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

# Issue #501 — handler that does not call `resume`, on a `Nothing`-
# returning op, with the handle stmt-expr's value consumed by an
# outer let. Compiled with `-O2` so the optimiser is allowed to
# hoist the discard slot read across the longjmp. Without the
# `volatile` qualifier on `_discard` (emit_handle in compiler.kai)
# the binary segfaults — the bug reported in the issue. Three
# shapes: Int let, String let, no-let.
ISSUE_501_FIXTURES := issue_501_handler_no_resume_nothing_op_int_let \
                      issue_501_handler_no_resume_string \
                      issue_501_handler_no_resume_unused

test-issue-501: $(TARGET) build
	@set -e; \
	for name in $(ISSUE_501_FIXTURES); do \
	  ./$(TARGET) ../examples/effects/$$name.kai > build/$$name.c; \
	  $(CC) -std=c99 -Wno-unused-function -Wno-unused-variable -O2 \
	        $(LDFLAGS_STACK) -I ../stage0 build/$$name.c \
	        -o build/$$name-c $(LDLIBS_M); \
	  ./build/$$name-c > build/$$name.out; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "issue-501 OK $$name" \
	    || { echo "issue-501 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
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

# Issue #141 — ASAN+UBSan sibling for the Log default-handler
# fixture. The default handler chains `clock_gettime` +
# `gmtime_r` + `strftime` + `fputs(stderr)` + `fflush(stderr)` per
# call; ASan catches a UAF on the message KaiValue's `as.s.bytes`
# (e.g. a botched consume edit elsewhere returning a dropped
# string into the op call), and UBSan catches signed-overflow /
# misaligned-load regressions in the strftime path. Wired into
# the top-level `tier1-asan` rule so it runs alongside the
# trace-asan and signal-trap-asan gates.
LOG_DEFAULT_FIXTURES := issue_141_log_default

test-log-asan: $(TARGET) build
	@set -e; \
	for name in $(LOG_DEFAULT_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "log-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    || { echo "log-asan FAIL $$name (stdout mismatch)"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] DEBUG default-d$$' build/$$name-asan.err \
	    || { echo "log-asan FAIL $$name (DEBUG line missing or malformed)"; cat build/$$name-asan.err; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] INFO  default-i$$' build/$$name-asan.err \
	    || { echo "log-asan FAIL $$name (INFO line missing or malformed)"; cat build/$$name-asan.err; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] WARN  default-w$$' build/$$name-asan.err \
	    || { echo "log-asan FAIL $$name (WARN line missing or malformed)"; cat build/$$name-asan.err; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] ERROR default-e$$' build/$$name-asan.err \
	    || { echo "log-asan FAIL $$name (ERROR line missing or malformed)"; cat build/$$name-asan.err; exit 1; }; \
	  echo "log-asan OK $$name"; \
	done

# Issue #140 — ASAN+UBSan sibling for the SecureRandom basic
# fixture. The default handler heap-allocates a buffer per draw,
# fills it via getrandom / arc4random_buf, walks back to build a
# `[Int]` cons list, then frees. ASan catches a UAF / botched
# free if the buffer lifetime ever broke; UBSan catches signed
# overflow in the int_range modulo arithmetic. Wired into the
# top-level `tier1-asan` rule alongside the other ASAN gates.
SECURERANDOM_FIXTURES := random_secure_basic

test-securerandom-asan: $(TARGET) build
	@set -e; \
	for name in $(SECURERANDOM_FIXTURES); do \
	  ./$(TARGET) $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	    ../examples/stdlib/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "securerandom-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/stdlib/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "securerandom-asan OK $$name" \
	    || { echo "securerandom-asan DIFF $$name"; diff ../examples/stdlib/$$name.out.expected build/$$name-asan.out; exit 1; }; \
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

# Issue #126 — ASAN+UBSan sibling for the Process default-handler
# fixture. The handler does fork(2) + execvp(3) + waitpid(2): the
# argv-build path malloc's per-element strdup'd C strings, and the
# parent's recovery path frees them after the child has been
# spawned. ASan catches a UAF / double-free in either path; UBSan
# catches a signed-overflow regression in the WIFEXITED /
# WEXITSTATUS macros. The parent process is what is instrumented;
# /bin/echo runs as a separate process and is not under ASan, so
# the harness pattern matches the runtime-shadow-asan / signal-
# trap-asan precedents — diagnostics on the parent are what gate
# the lane. Wired into the top-level `tier1-asan` rule alongside
# the other ASAN gates.
PROCESS_BASIC_FIXTURES := process_basic

test-process-basic-asan: $(TARGET) build
	@set -e; \
	for name in $(PROCESS_BASIC_FIXTURES); do \
	  ./$(TARGET) ../examples/effects/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "process-basic-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "process-basic-asan OK $$name" \
	    || { echo "process-basic-asan DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/time.c -o build/time-c $(LDLIBS_M) ; \
	./build/time-c > build/time.out; \
	diff -q ../examples/time/main.out.expected build/time.out > /dev/null \
	  && echo "time OK" \
	  || { echo "time DIFF"; diff ../examples/time/main.out.expected build/time.out; exit 1; }

# m12.6.x #2 — interval propagation walker pinning. Only the
# `basic.kai` fixture pins `--dump-intervals` output; the other
# fixtures (alpha_equiv, operator_flip, call_site_*) target the
# call-site analysis (Issue #83) and pin `--dump-call-elision` /
# `--check` instead — see test-call-elision and test-call-elision-neg
# below.
test-intervals: $(TARGET) build
	@set -e; \
	for f in ../examples/intervals/basic.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --dump-intervals $$f > build/iv-$$name.out 2>&1 \
	    || { echo "intervals FAIL $$name (kaic2 errored)"; cat build/iv-$$name.out; exit 1; }; \
	  diff -q $$exp build/iv-$$name.out > /dev/null \
	    && echo "intervals OK $$name" \
	    || { echo "intervals DIFF $$name"; diff $$exp build/iv-$$name.out; exit 1; }; \
	done

# Issue #83 — call-site contract elision report. For each positive
# fixture, run `--dump-call-elision` and diff against the golden.
# The fixture set covers alpha-matching (alpha_equiv), operator
# canonicalisation (operator_flip), literal call-site (call_site_literal),
# refined-binding call-site (call_site_refined), and the unrefined
# fall-through (call_site_unproven). The negative fixture lives in
# test-call-elision-neg below.
test-call-elision: $(TARGET) build
	@set -e; \
	for f in ../examples/intervals/alpha_equiv.kai \
	         ../examples/intervals/operator_flip.kai \
	         ../examples/intervals/call_site_literal.kai \
	         ../examples/intervals/call_site_refined.kai \
	         ../examples/intervals/call_site_unproven.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --dump-call-elision $$f > build/ce-$$name.out 2>&1 \
	    || { echo "call-elision FAIL $$name (kaic2 errored)"; cat build/ce-$$name.out; exit 1; }; \
	  diff -q $$exp build/ce-$$name.out > /dev/null \
	    && echo "call-elision OK $$name" \
	    || { echo "call-elision DIFF $$name"; diff $$exp build/ce-$$name.out; exit 1; }; \
	done

# Issue #83 — negative call-site fixture. The caller's refinement
# is strictly weaker than the callee's pre, so the typer rejects
# it at compile time. The fixture pins stderr (and a non-zero exit)
# via `kaic2 --check`.
test-call-elision-neg: $(TARGET) build
	@f=../examples/intervals/call_site_too_loose.kai; \
	exp=$${f%.kai}.err.expected; \
	./$(TARGET) --check $$f > build/ce-too-loose.out 2> build/ce-too-loose.err; \
	rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "call-elision-neg FAIL too_loose (expected non-zero exit)"; exit 1; \
	fi; \
	diff -q $$exp build/ce-too-loose.err > /dev/null \
	  && echo "call-elision-neg OK too_loose" \
	  || { echo "call-elision-neg DIFF too_loose"; diff $$exp build/ce-too-loose.err; exit 1; }

# Issue #381 — unused-binding warning fixtures. Each `<name>.kai`
# under `examples/typer/` ships a `<name>.warn.expected` golden that
# pins the diagnostic stream emitted on stderr. Compilation must
# always succeed (warnings never fail the build); the gate is the
# stderr diff. An empty `.warn.expected` proves the silent cases
# (`_x` prefix, unused param, `__internal`) emit nothing.
test-unused-bindings: $(TARGET) build
	@set -e; \
	for f in ../examples/typer/*.kai; do \
	  [ -e $$f ] || continue; \
	  name=$$(basename $$f .kai); \
	  exp=../examples/typer/$$name.warn.expected; \
	  rc=0; ./$(TARGET) $$f > build/typer-$$name.c 2> build/typer-$$name.err || rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "unused-bindings FAIL $$name (kaic2 exit=$$rc)"; cat build/typer-$$name.err; exit 1; \
	  fi; \
	  if [ ! -f $$exp ]; then \
	    echo "unused-bindings FAIL $$name (missing $$exp)"; exit 1; \
	  fi; \
	  diff -q $$exp build/typer-$$name.err > /dev/null \
	    && echo "unused-bindings OK $$name" \
	    || { echo "unused-bindings DIFF $$name"; diff $$exp build/typer-$$name.err; exit 1; }; \
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
                       --prelude ../stdlib/array.kai \
                       --prelude ../stdlib/random.kai \
                       --prelude ../stdlib/random_secure.kai \
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
                       --prelude ../stdlib/math/complex.kai \
                       --prelude ../stdlib/decimal.kai \
                       --prelude ../stdlib/money.kai \
                       --prelude ../stdlib/fx.kai \
                       --prelude ../stdlib/uuid.kai \
                       --prelude ../stdlib/regexp.kai \
                       --prelude ../stdlib/path.kai \
                       --prelude ../stdlib/os/env.kai \
                       --prelude ../stdlib/os/args.kai \
                       --prelude ../stdlib/os/process.kai \
                       --prelude ../stdlib/time.kai \
                       --prelude ../stdlib/net/http.kai \
                       --prelude ../stdlib/crypto/hash.kai \
                       --prelude ../stdlib/crypto/mac.kai

test-stdlib: $(TARGET) build
	@set -e; \
	for f in ../examples/stdlib/*.kai; do \
	  name=$$(basename $$f .kai); \
	  case "$$name" in bench_*|check_*|*_lib|qualified_call_in_prelude|issue_219_typer_modcall|issue_318_kai_test_prelude_scope) continue ;; esac; \
	  exp_err=../examples/stdlib/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) $$f \
	      > /dev/null 2> build/stdlib-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "stdlib FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/stdlib-$$name.err \
	        || { echo "stdlib FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/stdlib-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "stdlib OK $$name (rejected as expected)"; \
	    continue; \
	  fi; \
	  ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) $$f > build/stdlib-$$name.c \
	    || { echo "stdlib FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/stdlib-$$name.c -o build/stdlib-$$name $(LDLIBS_M) \
	    || { echo "stdlib FAIL $$name (C compile failed)"; exit 1; }; \
	  in=../examples/stdlib/$$name.in; \
	  if [ -f $$in ]; then \
	    ./build/stdlib-$$name < $$in > build/stdlib-$$name.out \
	      || { echo "stdlib FAIL $$name (run errored)"; cat build/stdlib-$$name.out; exit 1; }; \
	  else \
	    ./build/stdlib-$$name > build/stdlib-$$name.out \
	      || { echo "stdlib FAIL $$name (run errored)"; cat build/stdlib-$$name.out; exit 1; }; \
	  fi; \
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

# Issue #170 phase 1 — intrinsic `test "..." { ... }` blocks living
# directly inside stdlib/core/*.kai. Gated by
# `.github/workflows/tier1-stdlib-core.yml` (paths: stdlib/core/**),
# so PRs that don't touch stdlib/core pay zero seconds for this.
#
# Cross-file dependencies inside stdlib/core (e.g. string.kai calls
# `ch_is_space` from char.kai) require the *other* core modules to
# be present as preludes when compiling the module under test. The
# module under test must NOT be in its own prelude set — otherwise
# its source is parsed twice (once as prelude, once as the test
# source) and every test block runs twice with duplicated definitions.
# Solution: invoke kaic2 directly with the prelude set computed as
# (CORE − module-under-test), then drive --test through the regular
# C-emit + cc + run pipeline.
test-stdlib-core-intrinsic: $(TARGET) build
	@set -e; \
	for mod in option result string char tuple list; do \
	  out=build/stdlib-core-$$mod.test.out; \
	  cfile=build/stdlib-core-$$mod.test.c; \
	  binfile=build/stdlib-core-$$mod.test.bin; \
	  preludes=""; \
	  for core_file in $(sort $(wildcard ../stdlib/core/*.kai)); do \
	    case $$core_file in *core/$$mod.kai) ;; *) preludes="$$preludes --prelude $$core_file" ;; esac; \
	  done; \
	  ./$(TARGET) --path ../stdlib $$preludes --test ../stdlib/core/$$mod.kai > $$cfile 2> $$out \
	    || { echo "stdlib-core-intrinsic FAIL $$mod (kaic2 errored)"; cat $$out; exit 1; }; \
	  $(CC) -std=c99 -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 $$cfile -o $$binfile \
	    || { echo "stdlib-core-intrinsic FAIL $$mod (cc errored)"; exit 1; }; \
	  ./$$binfile > $$out 2>&1 \
	    || { echo "stdlib-core-intrinsic FAIL $$mod (test binary errored)"; cat $$out; exit 1; }; \
	  awk '/^[0-9]+\/[0-9]+ tests passed$$/ { split($$1, p, "/"); \
	         if (p[1] > 0 && p[1] == p[2]) { ok = 1 } } \
	       END { exit ok ? 0 : 1 }' $$out \
	    || { echo "stdlib-core-intrinsic FAIL $$mod (no all-pass tests-passed line)"; cat $$out; exit 1; }; \
	  echo "stdlib-core-intrinsic OK $$mod"; \
	done

# Issue #139 — crypto_hash_basic under ASAN+UBSan. SHA-2 + HMAC are
# heavy on byte-buffer churn (per-block 64-element message schedules,
# repeated `list_append` of byte chunks for padding). The runtime has
# no Perceus discipline yet (cf. `project_emitter_no_rc_discipline`),
# so a regression that double-frees a digest buffer or leaks an
# intermediate window would surface here first. UBSan additionally
# guards the 64-bit modular arithmetic in SHA-512 — the
# decompose-and-recombine path inside `sha_add64` / `sha_shl64` /
# `sha_combine64` exists specifically to avoid the signed-overflow
# UB that direct `int64_t << k` and `int64_t + int64_t` would trip.
test-stdlib-crypto-asan: $(TARGET) build
	@set -e; \
	name=crypto_hash_basic; \
	./$(TARGET) $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	  ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	  || { echo "stdlib-crypto-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
	  || { echo "stdlib-crypto-asan FAIL $$name (cc errored)"; exit 1; }; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/stdlib-$$name-asan-c > build/stdlib-$$name-asan.out 2> build/stdlib-$$name-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/stdlib-$$name-asan.err; then \
	  echo "stdlib-crypto-asan FAIL $$name — sanitizer diagnostic:"; \
	  cat build/stdlib-$$name-asan.err; exit 1; \
	fi; \
	diff -q ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out > /dev/null \
	  && echo "stdlib-crypto-asan OK $$name" \
	  || { echo "stdlib-crypto-asan DIFF $$name"; diff ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out; exit 1; }

# Issue #85 / #157 — regex predicate fixtures under ASAN+UBSan. The
# refinement-side regex path crosses three RC-sensitive shapes per
# fixture: the `~r/.../` sigil's compile-time-built pattern string,
# the `Regex` carrier value crossing the `matches` call boundary,
# and the synthesised `__ref_pred_<Type>` fn dispatching through a
# match-arm narrowing guard. The runtime has no Perceus discipline
# yet (cf. `project_emitter_no_rc_discipline`), so a regression that
# double-frees the pattern string or leaks the compiled NFA would
# surface here first.
#
# regex_predicate_basic gates the v1 sigil + matches predicate
# (issue #85). regex_subsume_basic / _alpha gate the typer-side
# subsumption check (issue #157) — when the requires assert is
# proven redundant the runtime path drops it but the
# refinement-side check from match-arm narrowing still fires; ASAN
# verifies the surviving path doesn't leak after the drop.
test-stdlib-regex-predicate-asan: $(TARGET) build
	@set -e; \
	for name in regex_predicate_basic regex_subsume_basic regex_subsume_alpha; do \
	  ./$(TARGET) $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) \
	    ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	    || { echo "stdlib-regex-predicate-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
	    || { echo "stdlib-regex-predicate-asan FAIL $$name (cc errored)"; exit 1; }; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/stdlib-$$name-asan-c > build/stdlib-$$name-asan.out 2> build/stdlib-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/stdlib-$$name-asan.err; then \
	    echo "stdlib-regex-predicate-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/stdlib-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out > /dev/null \
	    && echo "stdlib-regex-predicate-asan OK $$name" \
	    || { echo "stdlib-regex-predicate-asan DIFF $$name"; diff ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out; exit 1; }; \
	done

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
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name-holes.c -o build/$$name-holes-c $(LDLIBS_M) ; \
	  crc=0; ./build/$$name-holes-c > build/$$name-holes-c.out 2>&1 || crc=$$?; \
	  if [ $$crc -ne 0 ] && ! grep -q "unfilled hole:" build/$$name-holes-c.out; then \
	    echo "holes FAIL $$name (C backend aborted without hole message)"; cat build/$$name-holes-c.out; exit 1; \
	  fi; \
	  ./$(TARGET) --emit=llvm $$f > build/$$name-holes.ll; \
	  clang -w -I ../stage0 build/$$name-holes.ll ../stage0/runtime_llvm.c -o build/$$name-holes-llvm $(LDLIBS_M) ; \
	  lrc=0; ./build/$$name-holes-llvm > build/$$name-holes-llvm.out 2>&1 || lrc=$$?; \
	  if [ $$lrc -ne 0 ] && ! grep -q "unfilled hole:" build/$$name-holes-llvm.out; then \
	    echo "holes FAIL $$name (LLVM backend aborted without hole message)"; cat build/$$name-holes-llvm.out; exit 1; \
	  fi; \
	  echo "holes OK $$name"; \
	done

# Issue #477: end-to-end check that `kai build --holes` and
# `kai build --holes-json` (the driver-level surface) wire through to
# kaic2's typed-hole reporting passes. test-holes above exercises the
# compiler in isolation (no stdlib auto-load); these fixtures go
# through the driver with the full stdlib preludes loaded so the
# scope-resolution path that an LLM agent will actually hit is what
# we measure.
#
# Fixtures live in examples/typed_holes/. Each .kai file declares one
# or more holes whose expected_type is known up front; the assertions
# match docs/typed-holes.md §Output formats:
#   * pretty: `<file>:<line>:<col>: type hole` and `expected: <Ty>`
#     headers must appear once per hole.
#   * JSON: validate_holes_json.py confirms the stable schema; in
#     addition we check the right number of entries.
test-driver-holes: $(TARGET) build
	@set -e; \
	KAI=../bin/kai; \
	pretty=../examples/typed_holes/hole_pretty.kai; \
	$$KAI build --holes $$pretty > build/driver-hole-pretty.out; \
	grep -q "hole_pretty.kai:3:23: type hole ?answer" build/driver-hole-pretty.out \
	  || { echo "driver-holes FAIL hole_pretty (header missing)"; cat build/driver-hole-pretty.out; exit 1; }; \
	grep -q "expected: String" build/driver-hole-pretty.out \
	  || { echo "driver-holes FAIL hole_pretty (expected type missing)"; cat build/driver-hole-pretty.out; exit 1; }; \
	echo "driver-holes OK hole_pretty (--holes pretty)"; \
	json=../examples/typed_holes/hole_json.kai; \
	$$KAI build --holes-json $$json > build/driver-hole-json.out; \
	python3 ../scripts/validate_holes_json.py build/driver-hole-json.out \
	  || { echo "driver-holes FAIL hole_json (schema)"; cat build/driver-hole-json.out; exit 1; }; \
	python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert len(d)==1, d; r=d[0]; assert r['name']=='formula', r; assert r['expected_type']=='Real', r; assert r['line']==1, r" \
	  build/driver-hole-json.out \
	  || { echo "driver-holes FAIL hole_json (content)"; cat build/driver-hole-json.out; exit 1; }; \
	echo "driver-holes OK hole_json (--holes-json single)"; \
	multi=../examples/typed_holes/hole_multiple.kai; \
	$$KAI build --holes $$multi > build/driver-hole-multi.out; \
	for h in "?head" "?tail" "?count"; do \
	  grep -q "type hole $$h" build/driver-hole-multi.out \
	    || { echo "driver-holes FAIL hole_multiple (missing $$h)"; cat build/driver-hole-multi.out; exit 1; }; \
	done; \
	$$KAI build --holes-json $$multi > build/driver-hole-multi.json; \
	python3 ../scripts/validate_holes_json.py build/driver-hole-multi.json \
	  || { echo "driver-holes FAIL hole_multiple (schema)"; cat build/driver-hole-multi.json; exit 1; }; \
	python3 -c "import json,sys; d=json.load(open(sys.argv[1])); assert len(d)==3, d; names=sorted(r['name'] for r in d); assert names==['count','head','tail'], names" \
	  build/driver-hole-multi.json \
	  || { echo "driver-holes FAIL hole_multiple (content)"; cat build/driver-hole-multi.json; exit 1; }; \
	echo "driver-holes OK hole_multiple (--holes + --holes-json multi)"; \
	echo "driver-holes OK"

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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (one-shot resume runs through KaiCont)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6d_double_resume.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (discard, discard+return, no-discard all pass)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6f_clause_typed.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  clang -w -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	      || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/s2-$$name > build/s2-$$name.out \
	      || { echo "effects FAIL $$name (run errored)"; cat build/s2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/s2-$$name.out > /dev/null \
	        && echo "effects OK $$name (C)" \
	        || { echo "effects DIFF $$name (C)"; diff $$exp_out build/s2-$$name.out; exit 1; }; \
	      ./$(TARGET) --emit=llvm $$f > build/s2-$$name.ll \
	        || { echo "effects FAIL $$name (LLVM errored)"; exit 1; }; \
	      $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm 2>/dev/null $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  $(CC) $(CFLAGS) ../tests/sigharness.c -o build/s2-$$name.harness $(LDLIBS_M) \
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
	done; \
	for f in ../examples/effects/process_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_process_start" build/s2-$$name.c \
	    && grep -q "kai_default_process_wait"  build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_process" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Process wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (Process.start + wait: /bin/echo hello → Exited(0))" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/issue_141_log_default.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.stderr \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.stderr; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "effects FAIL $$name (stdout mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] DEBUG default-d$$' build/s2-$$name.stderr \
	    || { echo "effects FAIL $$name (DEBUG line missing or malformed)"; cat build/s2-$$name.stderr; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] INFO  default-i$$' build/s2-$$name.stderr \
	    || { echo "effects FAIL $$name (INFO line missing or malformed)"; cat build/s2-$$name.stderr; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] WARN  default-w$$' build/s2-$$name.stderr \
	    || { echo "effects FAIL $$name (WARN line missing or malformed)"; cat build/s2-$$name.stderr; exit 1; }; \
	  grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] ERROR default-e$$' build/s2-$$name.stderr \
	    || { echo "effects FAIL $$name (ERROR line missing or malformed)"; cat build/s2-$$name.stderr; exit 1; }; \
	  echo "effects OK $$name (issue #141: default Log handler → ISO 8601 + level + msg on stderr, all 4 levels)"; \
	done; \
	for f in ../examples/effects/array_mutable_explicit.kai \
	         ../examples/effects/array_read_only_no_mutable.kai \
	         ../examples/effects/array_local_masks_mutable.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (issue #251 + #252 Mutable provenance + masking)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/array_mutable_required.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=$${f%.kai}.err.expected; \
	  rc=0; ./$(TARGET) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "effects FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "effects FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "effects OK $$name (issue #251 parameter-mutation rejected)"; \
	done; \
	for f in ../examples/effects/issue_308_user_log_effect.kai \
	         ../examples/effects/issue_308_stdlib_log_unchanged.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (issue #308: user effect shadows stdlib)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm --path ../stdlib $$f > build/s2-$$name.ll 2> build/s2-$$name-llvm.err \
	    || { echo "effects FAIL $$name (LLVM emit errored)"; cat build/s2-$$name-llvm.err; exit 1; }; \
	  clang -w -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (LLVM compile failed)"; exit 1; }; \
	  ./build/s2-$$name-llvm > build/s2-$$name-llvm.out 2>&1 \
	    || { echo "effects FAIL $$name (LLVM binary exit non-zero)"; cat build/s2-$$name-llvm.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name-llvm.out > /dev/null \
	    && echo "effects OK $$name (issue #308: LLVM backend matches)" \
	    || { echo "effects FAIL $$name (LLVM output mismatch)"; diff $$exp build/s2-$$name-llvm.out; exit 1; }; \
	done; \
	for f in ../examples/actors/dual_actor_request_reply.kai \
	         ../examples/actors/dual_actor_send_only.kai \
	         ../examples/actors/dual_actor_receive_only.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/actors/$$name.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "actors FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "actors FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "actors FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -u $$exp_out build/s2-$$name.out \
	    && echo "actors OK $$name (issue #472 Actor[T] dual instantiation)" \
	    || { echo "actors FAIL $$name (output diff)"; exit 1; }; \
	done; \
	for f in ../examples/actors/dual_actor_missing_one.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/actors/$$name.err.expected; \
	  rc=0; ./$(TARGET) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "actors FAIL $$name (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    grep -qF "$$line" build/s2-$$name.err \
	      || { echo "actors FAIL $$name (missing diagnostic: $$line)"; \
	           cat build/s2-$$name.err; exit 1; }; \
	  done < $$exp_err; \
	  echo "actors OK $$name (issue #472 missing parametric label still diagnosed)"; \
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
	  case "$$name" in \
	    complex_literal_*) extra_flags="--path ../stdlib --prelude ../stdlib/protocols.kai --prelude ../stdlib/math/numeric.kai --prelude ../stdlib/math/real.kai --prelude ../stdlib/math/complex.kai" ;; \
	    *) extra_flags="" ;; \
	  esac; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$extra_flags $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $$extra_flags $$f > build/$$name.c \
	      || { echo "sugars FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
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

# Issue #326 — record-spread sugar (`T { ...src, x: 10 }`). Each .kai
# under `examples/records/` either compiles + runs to a known stdout
# (positive cases — diffed against `<name>.out.expected`) or fails
# with a specific diagnostic (negative cases — every line of
# `<name>.err.expected` must be a substring of the compiler's stderr).
# Lives in its own target so a regression in spread desugaring shows
# up under a clearly named gate.
test-records: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/records/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/records/$$name.out.expected; \
	  exp_err=../examples/records/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/r-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "records FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/r-$$name.err \
	        || { echo "records FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/r-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "records OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/$$name.c \
	      || { echo "records FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
	      || { echo "records FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/$$name > build/$$name.out \
	      || { echo "records FAIL $$name (run errored)"; cat build/$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/$$name.out > /dev/null \
	        && echo "records OK $$name" \
	        || { echo "records DIFF $$name"; diff $$exp_out build/$$name.out; exit 1; }; \
	    else \
	      echo "records OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# Issue #445 — m11 v1 diagnostic templates. Same `.err.expected`
# substring discipline as `test-sugars` / `test-records`. Negative
# fixtures must reject; every line of the golden must be a substring
# of stderr. Positive fixtures (no `.err.expected`) must compile +
# run cleanly — they exist to pin regression cases like a wildcard
# `_` arm suppressing the non-exhaustive diagnostic.
test-diagnostics: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/diagnostics/*.kai; do \
	  [ -e $$f ] || continue; \
	  name=$$(basename $$f .kai); \
	  exp_err=../examples/diagnostics/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/diag-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "diagnostics FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/diag-$$name.err \
	        || { echo "diagnostics FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/diag-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "diagnostics OK $$name (m11 v1 template emitted)"; \
	  else \
	    ./$(TARGET) $$f > build/diag-$$name.c \
	      || { echo "diagnostics FAIL $$name (kaic2 errored unexpectedly)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/diag-$$name.c -o build/diag-$$name $(LDLIBS_M) \
	      || { echo "diagnostics FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/diag-$$name > /dev/null \
	      || { echo "diagnostics FAIL $$name (run errored)"; exit 1; }; \
	    echo "diagnostics OK $$name (positive regression — accepted)"; \
	  fi; \
	done

# Issue #129 — multi-arg `match` sugar. Each .kai under
# `examples/match/` either compiles + runs to a known stdout
# (positive cases — diffed against `<name>.out.expected`) or fails
# with a specific diagnostic (negative cases — every line of
# `<name>.err.expected` must be a substring of the compiler's stderr).
# Mirrors `test-sugars` mechanically; lives in its own target so a
# regression in the multi-arg desugaring shows up under a clearly
# named gate rather than buried in the sugars suite.
test-match: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/match/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/match/$$name.out.expected; \
	  exp_err=../examples/match/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/m2-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "match FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/m2-$$name.err \
	        || { echo "match FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/m2-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "match OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/m2-$$name.c \
	      || { echo "match FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/m2-$$name.c -o build/m2-$$name $(LDLIBS_M) \
	      || { echo "match FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/m2-$$name > build/m2-$$name.out \
	      || { echo "match FAIL $$name (run errored)"; cat build/m2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/m2-$$name.out > /dev/null \
	        && echo "match OK $$name" \
	        || { echo "match DIFF $$name"; diff $$exp_out build/m2-$$name.out; exit 1; }; \
	    else \
	      echo "match OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# issue #156 — hex (`0x[0-9a-fA-F]+`) and binary (`0b[01]+`) integer
# literal fixtures. Same diff / `.err.expected` discipline as test-sugars.
# Positive cases verify round-trip values up to signed 64-bit max;
# negative cases assert the lexer's malformed-literal diagnostic.
test-literals: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/literals/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/literals/$$name.out.expected; \
	  exp_err=../examples/literals/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/lit-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "literals FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/lit-$$name.err \
	        || { echo "literals FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/lit-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "literals OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/lit-$$name.c \
	      || { echo "literals FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/lit-$$name.c -o build/lit-$$name $(LDLIBS_M) \
	      || { echo "literals FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/lit-$$name > build/lit-$$name.out \
	      || { echo "literals FAIL $$name (run errored)"; cat build/lit-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/lit-$$name.out > /dev/null \
	        && echo "literals OK $$name" \
	        || { echo "literals DIFF $$name"; diff $$exp_out build/lit-$$name.out; exit 1; }; \
	    else \
	      echo "literals OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# issue #332 — string stdlib public-wrapper fixtures. Each .kai
# under `examples/string/` exercises one of the four wrappers
# added in `stdlib/core/string.kai` (`length`, `concat`, `slice`,
# `to_int`) plus a regression guard that the qualified surface
# and the flat PRELUDE primitive agree byte-for-byte. Loads the
# core preludes (so `string.length` etc. resolve) — same shape
# as `test-stdlib`. Diff / `.err.expected` discipline mirrors
# `test-literals`.
test-string: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/string/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/string/$$name.out.expected; \
	  exp_err=../examples/string/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) $$f \
	      > /dev/null 2> build/str-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "string FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/str-$$name.err \
	        || { echo "string FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/str-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "string OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) $$f \
	      > build/str-$$name.c \
	      || { echo "string FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 build/str-$$name.c -o build/str-$$name $(LDLIBS_M) \
	      || { echo "string FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/str-$$name > build/str-$$name.out \
	      || { echo "string FAIL $$name (run errored)"; cat build/str-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/str-$$name.out > /dev/null \
	        && echo "string OK $$name" \
	        || { echo "string DIFF $$name"; diff $$exp_out build/str-$$name.out; exit 1; }; \
	    else \
	      echo "string OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# issue #130 — leading-pipe regression fixtures. Each .kai under
# `examples/pipes/` exercises the apply (`|>`) and map (`|`) pipe
# operators in leading-line position; one fixture re-checks the
# pre-existing trailing-pipe shape so the relaxation in
# `parse_pipe_rest` does not regress it. Same diff/golden discipline
# as test-sugars.
test-pipes: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/pipes/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/pipes/$$name.out.expected; \
	  exp_err=../examples/pipes/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/p2-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "pipes FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/p2-$$name.err \
	        || { echo "pipes FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/p2-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "pipes OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/$$name.c \
	      || { echo "pipes FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
	      || { echo "pipes FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/$$name > build/$$name.out \
	      || { echo "pipes FAIL $$name (run errored)"; cat build/$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/$$name.out > /dev/null \
	        && echo "pipes OK $$name" \
	        || { echo "pipes DIFF $$name"; diff $$exp_out build/$$name.out; exit 1; }; \
	    else \
	      echo "pipes OK $$name (no golden, ran clean)"; \
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
	    $(CC) $(CFLAGS) -I ../stage0 build/p2-$$name.c -o build/p2-$$name $(LDLIBS_M) \
	      || { echo "protocols FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/p2-$$name > build/p2-$$name.out \
	      || { echo "protocols FAIL $$name (run errored)"; cat build/p2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/p2-$$name.out > /dev/null \
	        && echo "protocols OK $$name (C)" \
	        || { echo "protocols DIFF $$name (C)"; diff $$exp_out build/p2-$$name.out; exit 1; }; \
	      ./$(TARGET) --emit=llvm $$f > build/p2-$$name.ll \
	        || { echo "protocols FAIL $$name (kaic2 --emit=llvm errored)"; exit 1; }; \
	      $(CC) $(CFLAGS) -I ../stage0 build/p2-$$name.ll ../stage0/runtime_llvm.c -o build/p2-$$name-llvm 2>/dev/null $(LDLIBS_M) \
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
	@$(CC) $(CFLAGS) -I ../stage0 tests/effect_runtime_test.c -o build/effect_runtime_test $(LDLIBS_M)
	@./build/effect_runtime_test \
	  && echo "effect-runtime OK" \
	  || { echo "effect-runtime FAIL"; exit 1; }

# Phase 5: smoke test for the Link runtime registry. Exercises
# kai_link_add_bidirectional + kai_link_propagate_terminate on
# bare KaiFiber structs without going through scheduler dispatch.
# A kaikai-level link-cascade demo defers to Phase 5.5+ (needs
# spawn_actor / pid handoff machinery the v1 actor surface lacks).
test-link-runtime: build
	@$(CC) $(CFLAGS) -I ../stage0 tests/link_runtime_test.c -o build/link_runtime_test $(LDLIBS_M)
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
	  $(CC) $(CFLAGS) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "m4c FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out \
	    || { echo "m4c FAIL $$name (C binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$f > build/s2-$$name.ll 2> build/s2-$$name-llvm.err \
	    || { echo "m4c FAIL $$name (LLVM emit errored)"; cat build/s2-$$name-llvm.err; exit 1; }; \
	  clang -w -I ../stage0 build/s2-$$name.ll ../stage0/runtime_llvm.c -o build/s2-$$name-llvm $(LDLIBS_M) \
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
	$(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage0 build/kaic2b.c -o build/kaic2b $(LDLIBS_M) ; \
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
	clang -w -I ../stage0 build/kaic2-s1.ll ../stage0/runtime_llvm.c -o build/kaic2-llvm-s1 $(LDLIBS_M) ; \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/demo-$$name.c -o build/demo-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/demo-$$name.ll ../stage0/runtime_llvm.c -o build/demo-$$name-llvm 2>/dev/null $(LDLIBS_M) \
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
ASPIRATIONAL_DEMOS := event_logger audit_handler pipeline_reorder_smoke

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
	  $(CC) $(CFLAGS) -I ../stage0 build/asp-$$name.c -o build/asp-$$name $(LDLIBS_M) \
	    || { echo "aspirational FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/asp-$$name > build/asp-$$name.out \
	    || { echo "aspirational FAIL $$name (run errored)"; cat build/asp-$$name.out; exit 1; }; \
	  diff -q $$exp build/asp-$$name.out > /dev/null \
	    && echo "aspirational OK $$name (C)" \
	    || { echo "aspirational DIFF $$name (C)"; diff $$exp build/asp-$$name.out; exit 1; }; \
	  ./$(TARGET) --emit=llvm $$preludes $$src > build/asp-$$name.ll \
	    || { echo "aspirational FAIL $$name (kaic2 --emit=llvm errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/asp-$$name.ll ../stage0/runtime_llvm.c -o build/asp-$$name-llvm 2>/dev/null $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) -I ../stage0 build/perceus-$$name.c -o build/perceus-$$name $(LDLIBS_M) \
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
	@# Issue #90 — `^` (integer power) and integer division on raw
	@# operands must inline. The C emit unrolls `b ^ n` into
	@# `(b)*(b)*…` and lowers `a / b` (Int / Int) to native `/`. Diff
	@# against the expected output, then grep the inner-loop body to
	@# assert no `kai_op_pow_int(` / `kai_op_div(` calls survived.
	@# (Post-#315 the dedicated `//` operator is gone; `Int / Int`
	@# already produces truncating division via the `Div[Int]` impl,
	@# and the unbox pass routes raw-Int `/` to native C `/` directly.)
	@diff -q ../examples/perceus/unbox_phase2_pow_idiv.out.expected \
	        build/perceus-unbox_phase2_pow_idiv.out > /dev/null \
	  || { echo "unbox FAIL pow_idiv (output mismatch)"; \
	       diff ../examples/perceus/unbox_phase2_pow_idiv.out.expected \
	            build/perceus-unbox_phase2_pow_idiv.out; exit 1; }
	@grep -F -q 'int64_t kair_b' build/perceus-unbox_phase2_pow_idiv.c \
	  || { echo "unbox FAIL pow_idiv (kair_b not raw; ^ stayed boxed)"; exit 1; }
	@grep -F -q 'int64_t kair_c' build/perceus-unbox_phase2_pow_idiv.c \
	  || { echo "unbox FAIL pow_idiv (kair_c not raw; / stayed boxed)"; exit 1; }
	@if grep -E 'kai_pow_idiv_inner_loop\(void\) \{[^}]*kai_op_pow_int\(' build/perceus-unbox_phase2_pow_idiv.c; then \
	  echo "unbox FAIL pow_idiv (kai_op_pow_int survived inside body)"; exit 1; \
	fi
	@if grep -E 'kai_pow_idiv_inner_loop\(void\) \{[^}]*kai_op_div\(' build/perceus-unbox_phase2_pow_idiv.c; then \
	  echo "unbox FAIL pow_idiv (kai_op_div survived inside body)"; exit 1; \
	fi
	@# Issue #91 — PBind catch-all in the M5 fast path. The fixture
	@# lives outside the `unbox_phase2_*.kai` glob (its scope is
	@# match-emit, not the broader unboxing fixtures), so it gets an
	@# explicit compile + run + grep here. The body of a PBind
	@# catch-all must bind the raw scrutinee as `int64_t kair_n` in
	@# the `default:` block, and the body must use `kair_n` (raw)
	@# inside `n + 1` instead of routing through `kai_op_eq` /
	@# `kai_op_add`. The boxed match path's per-arm `kai_op_eq` chain
	@# and `kai_decref(_scr)` cleanup must not appear inside the body.
	./$(TARGET) ../examples/perceus/match_pbind_catchall_raw.kai > build/perceus-match_pbind_catchall_raw.c \
	  || { echo "unbox FAIL match_pbind_catchall_raw (kaic2 errored)"; exit 1; }
	$(CC) $(CFLAGS) -I ../stage0 build/perceus-match_pbind_catchall_raw.c -o build/perceus-match_pbind_catchall_raw $(LDLIBS_M) \
	  || { echo "unbox FAIL match_pbind_catchall_raw (cc errored)"; exit 1; }
	./build/perceus-match_pbind_catchall_raw > build/perceus-match_pbind_catchall_raw.out \
	  || { echo "unbox FAIL match_pbind_catchall_raw (run errored)"; exit 1; }
	@diff -q ../examples/perceus/match_pbind_catchall_raw.out.expected \
	        build/perceus-match_pbind_catchall_raw.out > /dev/null \
	  || { echo "unbox FAIL match_pbind_catchall_raw (output mismatch)"; \
	       diff ../examples/perceus/match_pbind_catchall_raw.out.expected \
	            build/perceus-match_pbind_catchall_raw.out; exit 1; }
	@grep -F -q 'switch (_scr)' build/perceus-match_pbind_catchall_raw.c \
	  || { echo "unbox FAIL match_pbind_catchall_raw (no raw switch found)"; exit 1; }
	@grep -F -q 'int64_t kair_n = _scr' build/perceus-match_pbind_catchall_raw.c \
	  || { echo "unbox FAIL match_pbind_catchall_raw (kair_n binding missing in default block)"; exit 1; }
	@if grep -E 'kai_classify_raw_pure\(void\) \{[^}]*kai_op_eq\(' build/perceus-match_pbind_catchall_raw.c; then \
	  echo "unbox FAIL match_pbind_catchall_raw (kai_op_eq survived inside body — fast path missed)"; exit 1; \
	fi
	@if grep -E 'kai_classify_raw_pure\(void\) \{[^}]*kai_decref\(_scr\)' build/perceus-match_pbind_catchall_raw.c; then \
	  echo "unbox FAIL match_pbind_catchall_raw (boxed _scr alias survived inside body)"; exit 1; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/tco.c -o build/tco $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/tco-nth.c -o build/tco-nth $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) -I ../stage0 build/tco-zero.c -o build/tco-zero $(LDLIBS_M) ; \
	./build/tco-zero > build/tco-zero.out; \
	diff -q ../examples/perceus/r12_zero_arg_recursive.out.expected build/tco-zero.out > /dev/null \
	  && echo "tco-zero-arg OK (issue #102 zero-arg self-tail-call compiles + runs to completion)" \
	  || { echo "tco-zero-arg DIFF"; diff ../examples/perceus/r12_zero_arg_recursive.out.expected build/tco-zero.out; exit 1; }

# Issue #309 — match-arm tail tcrec sentinel + _scr leak fixture.
# `count_down` here uses `match` (not `if`); the recursive call sits
# in the tail of an arm body. Pre-fix, every iteration leaked one
# `int` because `emit_match_default`'s synthetic `_scr =
# kai_internal_dup(scrut)` alias was decrefed AFTER the do-while,
# but the tcrec goto bypassed that decref by jumping out of the
# enclosing statement-expression. The fix injects
# `kai_decref(_scr)` inside the arm body's stmt-expr (before the
# goto fires) when every tail leaf of the body is a sentinel call
# (`tcrec_tail_always_sentinel` predicate). The fixture validates
# both sides: 1M-deep tail recursion runs in O(1) C-stack (TCO
# preserved) AND under `KAI_TRACE_RC=1` the `int` tag's `live`
# count at exit is 0 (leak closed).
test-tco-309: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/tco/issue_309_countdown.kai > build/tco-309.c; \
	grep -F -q '_kai_count_down_entry:;' build/tco-309.c \
	  || { echo "tco-309 FAIL (no _kai_count_down_entry label in emitted C)"; exit 1; }; \
	grep -F -q 'goto _kai_count_down_entry;' build/tco-309.c \
	  || { echo "tco-309 FAIL (no goto _kai_count_down_entry in emitted C)"; exit 1; }; \
	if grep -F -q 'int64_t _scr =' build/tco-309.c; then \
	  : "Phase 3 unboxing — raw int64_t scrutinee carries no RC; pre-goto decref is unnecessary"; \
	elif grep -F -q 'kai_decref(_scr); ({' build/tco-309.c; then \
	  : "boxed scrutinee path — pre-goto _scr decref injected as before"; \
	else \
	  echo "tco-309 FAIL (boxed scrut without pre-goto kai_decref(_scr) injection)"; exit 1; \
	fi; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco-309.c -o build/tco-309 $(LDLIBS_M) ; \
	./build/tco-309 > build/tco-309.out; \
	diff -q ../examples/tco/issue_309_countdown.out.expected build/tco-309.out > /dev/null \
	  && echo "tco-309 OK (issue #309 match-arm tail sentinel; 1M iters; raw or boxed scrut path validated; leak closed)" \
	  || { echo "tco-309 DIFF"; diff ../examples/tco/issue_309_countdown.out.expected build/tco-309.out; exit 1; }

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
	$(CC) $(CFLAGS) -I ../stage0 build/tco-perceus.c -o build/tco-perceus $(LDLIBS_M) ; \
	./build/tco-perceus > build/tco-perceus.out; \
	diff -q ../examples/perceus/unbox_bench.out.expected build/tco-perceus.out > /dev/null \
	  && echo "tco-perceus-wrap OK (issue #89 1M-deep self-tail-call through perceus wrap; goto label emitted; runs to completion)" \
	  || { echo "tco-perceus-wrap DIFF"; diff ../examples/perceus/unbox_bench.out.expected build/tco-perceus.out; exit 1; }

# Issue #92 — rule 3 fires. The `walk` fn matches on `xs` (single
# scrutinee read, count==1) and recurses with `(t, acc + h)`. `xs`
# does not appear anywhere in the recursive call's args, so rule 3
# (LUAt + count==1 + p NOT in args) drops `xs` in the goto block.
# The C-side assertion is the explicit `kai_internal_drop(kai_xs)`
# inside the goto-block emission; without rule 3 the drop is absent
# and a cons cell leaks per iteration. The runtime check confirms
# the program still completes with the expected sum.
test-tco-rule3-basic: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/tco/rule3_basic.kai > build/tco-r3b.c; \
	grep -F -q 'goto _kai_walk_entry' build/tco-r3b.c \
	  || { echo "tco-rule3-basic FAIL (no walk goto label)"; exit 1; }; \
	if awk '/_kai_walk_entry/,/__pcs_ret/' build/tco-r3b.c | grep -F -q 'kai_internal_drop(kai_xs)'; then \
	  echo "tco-rule3-basic FAIL (rule 3 misfired on match scrutinee xs; would double-free under perceus tcrec-tail-always-sentinel early-decref)"; exit 1; \
	fi; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco-r3b.c -o build/tco-r3b $(LDLIBS_M) ; \
	./build/tco-r3b > build/tco-r3b.out; \
	diff -q ../examples/tco/rule3_basic.out.expected build/tco-r3b.out > /dev/null \
	  && echo "tco-rule3-basic OK (issue #92: rule 3 correctly skips match-scrutinee xs; perceus early-decref closes the leak; sum matches)" \
	  || { echo "tco-rule3-basic DIFF"; diff ../examples/tco/rule3_basic.out.expected build/tco-r3b.out; exit 1; }

# Issue #92 — rule 3 does NOT misfire. The `id_walk` fn reads `xs`
# exactly once — as the recursive call's first arg — so count==1 and
# p IS in args. Rule 3 must NOT fire (otherwise the goto-block drop
# would double-free the head cell evaluated by the arg). The C-side
# assertion is the absence of `kai_internal_drop(kai_xs)` inside the
# `id_walk` goto block. The runtime check confirms the program runs
# to completion under glibc strict-malloc without abort.
test-tco-rule3-no-misfire: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/tco/rule3_no_misfire.kai > build/tco-r3n.c; \
	grep -F -q 'goto _kai_id_walk_entry' build/tco-r3n.c \
	  || { echo "tco-rule3-no-misfire FAIL (no id_walk goto label)"; exit 1; }; \
	if awk '/_kai_id_walk_entry/,/__pcs_ret/' build/tco-r3n.c | grep -F -q 'kai_internal_drop(kai_xs)'; then \
	  echo "tco-rule3-no-misfire FAIL (rule 3 misfired: kai_internal_drop(kai_xs) present in id_walk goto)"; exit 1; \
	fi; \
	$(CC) $(CFLAGS) -I ../stage0 build/tco-r3n.c -o build/tco-r3n $(LDLIBS_M) ; \
	./build/tco-r3n > build/tco-r3n.out; \
	diff -q ../examples/tco/rule3_no_misfire.out.expected build/tco-r3n.out > /dev/null \
	  && echo "tco-rule3-no-misfire OK (issue #92 rule 3 correctly skips xs when in args; runs without abort)" \
	  || { echo "tco-rule3-no-misfire DIFF"; diff ../examples/tco/rule3_no_misfire.out.expected build/tco-r3n.out; exit 1; }

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
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
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

# Issue #298 — closure capture lifecycle. Pre-fix `kai_apply` borrowed
# its closure argument and the emitter at every callsite handed it the
# binding raw; for a let-bound closure invoked exactly once Perceus
# transferred the original ref but no path ever decref'd it, leaking
# 27.97% of all closure allocations during kaic2 self-compile. The
# fixture exercises the four shapes that drove that leak: single-use
# let-bound closure, multi-use let-bound closure, chained higher-order
# helpers (map / filter), and recursive churn (fresh closure per
# iteration). The golden assertions are functional outputs; under
# `-DKAI_TRACE_RC=1` the closure tag's `live` count holds at single
# digits.
PERCEUS_ISSUE298_FIXTURES := closure_lifecycle_298

test-perceus-issue298: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE298_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-issue298 OK $$name (closure capture lifecycle balanced)" \
	    || { echo "perceus-issue298 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# Issue #350 — Gap 3 of three rb-tree leak gaps. Pattern bindings
# introduced by match arms with multi-use reads inside the arm body
# now receive an exit `__perceus_drop` (mirror of the SLet exit-drop
# pass from #342). The fix reduces rb_tree benchmark `leaked` from
# 21.96 M to ~3.6 M and RSS from 1929 MB to ~325 MB at 1 M inserts.
# Fixtures cover the variant, record-shaped, and minimal-balance
# rotation shapes that drive the leak.
PERCEUS_ISSUE350_FIXTURES := gap3_tail_rebuild_variant \
                              gap3_tail_rebuild_record \
                              gap3_balance_minimal

test-perceus-issue350: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE350_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-issue350 OK $$name (arm-bound multi-use drop emitted)" \
	    || { echo "perceus-issue350 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling for the issue #350 arm-binding-drop fixtures.
# The new exit-drop sits between the arm body and its tail; ASan
# catches a regression that double-frees the binding (e.g. an emitter
# that drops a name while it is still live in the tail), UBSan
# catches a missed kai_incref that lets a freed cell escape.
test-perceus-issue350-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE350_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-issue350-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-issue350-asan OK $$name" \
	    || { echo "perceus-issue350-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# ASAN+UBSan sibling for the issue #298 closure-lifecycle fixture.
# The fix changes the kai_apply contract from borrow to consume; ASan
# catches a regression that double-frees a closure (e.g. an emitter
# that lost the eager-dup wrapper around a multi-use closure binding).
test-perceus-issue298-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE298_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-issue298-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-issue298-asan OK $$name" \
	    || { echo "perceus-issue298-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #118 — Perceus reuse-in-place (Anga Roa wave v1).
# Fixtures cover:
#   - reuse_cons_basic       cons recogniser fires on the textbook
#                            `[h+1, ...recur(t)]` shape (positive).
#   - reuse_alias_aborts     aliased input ⇒ `kai_check_unique`
#                            blocks reuse, fallback to `kai_cons`
#                            (negative-ish; ASAN catches a missed
#                            guard as UAF).
#   - reuse_pass_ordering    chained reuse calls; the `kai_incref`
#                            inside `kai_reuse_or_alloc_cons`
#                            balances the match-exit `kai_decref`
#                            (RC discipline check).
#   - reuse_record_basic     issue #210 — record recogniser fires on
#                            `{ x, y, z } -> Point { x: f(x), y, z }`
#                            shape; typer-aware predicate confirms
#                            same-TyCon scrutinee and rebuild.
#   - reuse_variant_basic    issue #210 — variant recogniser fires on
#                            `TNode(...) -> TNode(...)` rebuild over
#                            same-TyCon scrutinee.
#   - reuse_variant_no_misfire
#                            issue #210 — typer-aware shape predicate
#                            rejects `Some(d) -> Some(false)` over a
#                            `Maybe[Int]` scrutinee (would rebuild a
#                            `Maybe[Bool]`, distinct mangle); the
#                            sibling `Some(d) -> Some(d+1)` arm DOES
#                            fire. Pins the regression that the
#                            pre-#210 lexical predicate hit.
#   - tco_double_incref_repro
#                            issue #212 — TCO + Perceus dup
#                            interaction on `[n, ...acc]`. Pre-fix
#                            the spread tail emit double-incref'd
#                            the accumulator. Wired here so any
#                            future regression of the same shape
#                            lands in tier1 + tier1-asan.
PERCEUS_ISSUE118_FIXTURES := reuse_cons_basic reuse_alias_aborts \
                              reuse_pass_ordering reuse_record_basic \
                              reuse_variant_basic \
                              reuse_variant_no_misfire \
                              tco_double_incref_repro \
                              nullary_variant_singleton

test-perceus-issue118: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE118_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-issue118 OK $$name" \
	    || { echo "perceus-issue118 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling — required by issue #118 (RC-discipline-critical;
# the runtime additions add a new alloc-vs-reuse branch on every
# constructor-after-deconsume site, and a missed `kai_incref` in the
# unique branch lands as a use-after-free at the next match exit).
test-perceus-issue118-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE118_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-issue118-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-issue118-asan OK $$name" \
	    || { echo "perceus-issue118-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #91 — PBind catch-all in the M5 fast path. ASAN+UBSan
# sibling for `match_pbind_catchall_raw.kai`. Match emit + raw
# locals is exactly the surface where RC discipline gaps
# manifest: a raw local treated as boxed leaks a `kair_<n>`
# scalar wrapped in a `kai_int` that's never decrefed; a boxed
# local treated as raw segfaults under ASan when the runtime
# tries to chase a pointer through what is in fact an int64.
test-match-pbind-catchall-asan: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/perceus/match_pbind_catchall_raw.kai > build/match_pbind_catchall_raw-asan.c \
	  || { echo "match-pbind-catchall-asan FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage0 build/match_pbind_catchall_raw-asan.c -o build/match_pbind_catchall_raw-asan-c \
	  || { echo "match-pbind-catchall-asan FAIL (C compile failed)"; exit 1; }; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/match_pbind_catchall_raw-asan-c > build/match_pbind_catchall_raw-asan.out 2> build/match_pbind_catchall_raw-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/match_pbind_catchall_raw-asan.err; then \
	  echo "match-pbind-catchall-asan FAIL — sanitizer diagnostic:"; \
	  cat build/match_pbind_catchall_raw-asan.err; exit 1; \
	fi; \
	diff -q ../examples/perceus/match_pbind_catchall_raw.out.expected build/match_pbind_catchall_raw-asan.out > /dev/null \
	  && echo "match-pbind-catchall-asan OK" \
	  || { echo "match-pbind-catchall-asan DIFF"; diff ../examples/perceus/match_pbind_catchall_raw.out.expected build/match_pbind_catchall_raw-asan.out; exit 1; }

# Tier S1 lane #3 — net.http client. The pure helpers
# (`http_serialize_request`, `http_parse_response`, `http_parse_url`)
# manipulate Strings and HttpHeader records, and the
# `drain_client` recv loop in the fixture allocates a [Int]
# accumulator across many short recvs. Both shapes have surfaced
# RC-discipline regressions in past stdlib lanes (issue #82
# match-scrutinee, issue #115 default-handler typed dispatch).
# This ASAN sibling exercises the fixture under ASan+UBSan so a
# botched decref / consume edit lands as a sanitizer diagnostic
# instead of a quiet leak. Wired into the root `tier1-asan`
# alongside the runtime-shadow / Signal / trap-exit gates.
HTTP_CLIENT_FIXTURES := http_client_basic

test-http-client-asan: $(TARGET) build
	@set -e; \
	for name in $(HTTP_CLIENT_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	    || { echo "http-client-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
	    || { echo "http-client-asan FAIL $$name (C compile failed)"; exit 1; }; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/stdlib-$$name-asan-c > build/stdlib-$$name-asan.out 2> build/stdlib-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/stdlib-$$name-asan.err; then \
	    echo "http-client-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/stdlib-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out > /dev/null \
	    && echo "http-client-asan OK $$name" \
	    || { echo "http-client-asan DIFF $$name"; diff ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out; exit 1; }; \
	done

# Issue #127 — `Env.set_var` / `Env.unset_var` / `Env.vars` round
# trip under ASAN+UBSan. POSIX `setenv` / `unsetenv` have a long
# history of buffer-ownership footguns on macOS (libsystem holds
# the original pointer, kaikai's local stack-buffer copy plus the
# heap-duplicated value buffer line up with that contract) and on
# glibc (older versions leak on overwrite). The fixture exercises
# set / overwrite / `vars()` walk / unset / unset-of-absent in one
# pass so a botched lifetime lands as a sanitizer diagnostic
# instead of a silent leak. Wired into the root `tier1-asan`
# alongside the http-client / runtime-shadow / Signal gates.
ENV_MUTATE_FIXTURES := os_env_mutate

test-env-mutate-asan: $(TARGET) build
	@set -e; \
	for name in $(ENV_MUTATE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $(EXTRA_PRELUDE_FLAGS) ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	    || { echo "env-mutate-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
	    || { echo "env-mutate-asan FAIL $$name (C compile failed)"; exit 1; }; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/stdlib-$$name-asan-c > build/stdlib-$$name-asan.out 2> build/stdlib-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/stdlib-$$name-asan.err; then \
	    echo "env-mutate-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/stdlib-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out > /dev/null \
	    && echo "env-mutate-asan OK $$name" \
	    || { echo "env-mutate-asan DIFF $$name"; diff ../examples/stdlib/$$name.out.expected build/stdlib-$$name-asan.out; exit 1; }; \
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
	clang -w -I ../stage0 build/unbox-bench.ll ../stage0/runtime_llvm.c -o build/unbox-bench-llvm $(LDLIBS_M) \
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

# issue #187 phase 2 — fixtures for unified-algebraic-types resolver.
# Each .kai under `examples/unions/` either compiles + runs to a known
# stdout (positive cases — diffed against `<name>.out.expected`) or
# fails with a specific diagnostic (negative cases — every line of
# `<name>.err.expected` must be a substring of the compiler's stderr).
# Mirrors `test-sugars` / `test-match` mechanically; lives in its own
# target so a regression in the union resolver / D2 collision check
# shows up under a clearly named gate.
# issue #201 — fixtures for the `|` map-pipe + `||` flat-map-pipe
# operators with naming-convention dispatch. Mirrors `test-unions`
# mechanically: each `.kai` either compiles + runs to a known stdout
# (positive — diffed against `<name>.out.expected`) or fails with a
# specific diagnostic (negative — every line of `<name>.err.expected`
# must be a substring of the compiler's stderr). Lives in its own
# target so a regression in the head-type-to-module dispatch path or
# the rejection diagnostics shows up under a clearly-named gate.
test-sequence: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/sequence/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/sequence/$$name.out.expected; \
	  exp_err=../examples/sequence/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/seq-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "sequence FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/seq-$$name.err \
	        || { echo "sequence FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/seq-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "sequence OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/seq-$$name.c \
	      || { echo "sequence FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/seq-$$name.c -o build/seq-$$name $(LDLIBS_M) \
	      || { echo "sequence FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/seq-$$name > build/seq-$$name.out \
	      || { echo "sequence FAIL $$name (run errored)"; cat build/seq-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/seq-$$name.out > /dev/null \
	        && echo "sequence OK $$name" \
	        || { echo "sequence DIFF $$name"; diff $$exp_out build/seq-$$name.out; exit 1; }; \
	    else \
	      echo "sequence OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

test-ufcs: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/ufcs/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/ufcs/$$name.out.expected; \
	  exp_err=../examples/ufcs/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $$f \
	      > /dev/null 2> build/ufcs-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "ufcs FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/ufcs-$$name.err \
	        || { echo "ufcs FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/ufcs-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "ufcs OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) --path ../stdlib $(CORE_PRELUDE_FLAGS) $$f > build/ufcs-$$name.c \
	      || { echo "ufcs FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/ufcs-$$name.c -o build/ufcs-$$name $(LDLIBS_M) \
	      || { echo "ufcs FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/ufcs-$$name > build/ufcs-$$name.out \
	      || { echo "ufcs FAIL $$name (run errored)"; cat build/ufcs-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/ufcs-$$name.out > /dev/null \
	        && echo "ufcs OK $$name" \
	        || { echo "ufcs DIFF $$name"; diff $$exp_out build/ufcs-$$name.out; exit 1; }; \
	    else \
	      echo "ufcs OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

test-unions: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/unions/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/unions/$$name.out.expected; \
	  exp_err=../examples/unions/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/u2-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "unions FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/u2-$$name.err \
	        || { echo "unions FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/u2-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "unions OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/u2-$$name.c \
	      || { echo "unions FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/u2-$$name.c -o build/u2-$$name $(LDLIBS_M) \
	      || { echo "unions FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/u2-$$name > build/u2-$$name.out \
	      || { echo "unions FAIL $$name (run errored)"; cat build/u2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/u2-$$name.out > /dev/null \
	        && echo "unions OK $$name" \
	        || { echo "unions DIFF $$name"; diff $$exp_out build/u2-$$name.out; exit 1; }; \
	    else \
	      echo "unions OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# issue #376 — fixtures for transparent type aliases. `type Foo = T`
# (single-component, no `|`) restores the structural-alias contract
# from `docs/kaikai-minimal.md` lines 194-195. Mirrors `test-unions`:
# each `.kai` either compiles + runs to a known stdout (positive —
# diffed against `<name>.out.expected`) or fails with a specific
# diagnostic (negative — every line of `<name>.err.expected` must be
# a substring of the compiler's stderr). The
# `union_two_components_unchanged` fixture is the regression guard
# for issue #187 (multi-component nominal-union semantics).
test-aliases: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/types/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/types/$$name.out.expected; \
	  exp_err=../examples/types/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $$f > /dev/null 2> build/ta-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "aliases FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -q "$$line" build/ta-$$name.err \
	        || { echo "aliases FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/ta-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "aliases OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) $$f > build/ta-$$name.c \
	      || { echo "aliases FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) -I ../stage0 build/ta-$$name.c -o build/ta-$$name $(LDLIBS_M) \
	      || { echo "aliases FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/ta-$$name > build/ta-$$name.out \
	      || { echo "aliases FAIL $$name (run errored)"; cat build/ta-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/ta-$$name.out > /dev/null \
	        && echo "aliases OK $$name" \
	        || { echo "aliases DIFF $$name"; diff $$exp_out build/ta-$$name.out; exit 1; }; \
	    else \
	      echo "aliases OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# issue #296 — per-call-site leak attribution. Compiles the fixture
# under `-DKAI_TRACE_RC=1`, verifies the program prints the expected
# stdout AND the runtime emits a `top sites by leak` line on stderr
# with a non-empty `aslr_slide=` and at least one `site` row. Vanilla
# tier1 only verifies the stdout golden via test-run; this target is
# the gate that catches regressions in the per-site instrumentation.
test-rc-trace-callsite: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/effects/rc_trace_callsite.kai > build/rc_trace_callsite.c; \
	$(CC) $(CFLAGS) -DKAI_TRACE_RC=1 -I ../stage0 build/rc_trace_callsite.c -o build/rc_trace_callsite-c $(LDLIBS_M) ; \
	./build/rc_trace_callsite-c > build/rc_trace_callsite.out 2> build/rc_trace_callsite.err; \
	diff -q ../examples/effects/rc_trace_callsite.out.expected build/rc_trace_callsite.out > /dev/null \
	  || { echo "rc-trace-callsite STDOUT DIFF"; diff ../examples/effects/rc_trace_callsite.out.expected build/rc_trace_callsite.out; exit 1; }; \
	grep -q "top sites by leak" build/rc_trace_callsite.err \
	  || { echo "rc-trace-callsite MISSING top-sites line"; cat build/rc_trace_callsite.err; exit 1; }; \
	grep -q "aslr_slide=" build/rc_trace_callsite.err \
	  || { echo "rc-trace-callsite MISSING aslr_slide line"; cat build/rc_trace_callsite.err; exit 1; }; \
	grep -q "^\[KAI_TRACE_RC\] site " build/rc_trace_callsite.err \
	  || { echo "rc-trace-callsite NO site rows"; cat build/rc_trace_callsite.err; exit 1; }; \
	echo "rc-trace-callsite OK"

# Issue #86 v2 — structured violation context for contract panics.
# Each fixture under examples/refinements/ either panics at runtime
# with the exact bytes in `<name>.run.err.expected` (negative cases)
# or runs cleanly with stdout matching `<name>.out.expected` (the
# happy-path companion). Keeps the violation message format byte-
# pinned so a regression in build_violation_msg_span trips a clear
# test rather than a downstream user surprise.
test-violations: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/refinements/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_run_err=../examples/refinements/$$name.run.err.expected; \
	  exp_out=../examples/refinements/$$name.out.expected; \
	  ./$(TARGET) $$f > build/viol-$$name.c \
	    || { echo "violations FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) -I ../stage0 build/viol-$$name.c -o build/viol-$$name $(LDLIBS_M) \
	    || { echo "violations FAIL $$name (C compile failed)"; exit 1; }; \
	  rc=0; ./build/viol-$$name > build/viol-$$name.out 2> build/viol-$$name.err || rc=$$?; \
	  if [ -f $$exp_run_err ]; then \
	    if [ $$rc -eq 0 ]; then \
	      echo "violations FAIL $$name (expected runtime panic, got success)"; exit 1; \
	    fi; \
	    diff -q $$exp_run_err build/viol-$$name.err > /dev/null \
	      && echo "violations OK $$name (panic matches)" \
	      || { echo "violations DIFF $$name"; diff $$exp_run_err build/viol-$$name.err; exit 1; }; \
	  elif [ -f $$exp_out ]; then \
	    if [ $$rc -ne 0 ]; then \
	      echo "violations FAIL $$name (unexpected runtime error)"; cat build/viol-$$name.err; exit 1; \
	    fi; \
	    diff -q $$exp_out build/viol-$$name.out > /dev/null \
	      && echo "violations OK $$name" \
	      || { echo "violations DIFF $$name"; diff $$exp_out build/viol-$$name.out; exit 1; }; \
	  else \
	    echo "violations SKIP $$name (no golden)"; \
	  fi; \
	done

test: test-tokens test-ast test-types test-env test-infer test-infer-check test-dump-typed test-dump-mono test-check test-run test-blocks test-llvm test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-holes test-driver-holes test-effects test-effect-runtime test-link-runtime test-sugars test-pipes test-match test-literals test-string test-loop test-records test-diagnostics test-reader test-writer test-trace test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-protocols test-demos-core test-aspirational test-m4c test-unbox-phase2 test-unbox-llvm-bench test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-runtime-shadow test-issue-501 test-foreign-prelude test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-318-include test-issue-335 test-perceus-issue82 test-perceus-issue118 test-perceus-issue298 test-perceus-issue350 test-unions test-aliases test-sequence test-ufcs test-ffi-extern-c test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-issue-315-no-double-slash test-issue-248-loop-sugars test-rc-trace-callsite test-tco-rule3-basic test-tco-rule3-no-misfire test-violations test-unused-bindings

# Fast subset for iterative development: skips every subtarget that
# runs kaic2 (or kaic1) against the compiler's own source. Each of
# those takes ~80-90s per invocation and they dominate `make test`
# (~8-9 min of its ~10-12 min runtime). `test-fast` leaves in the
# smoke tests that actually catch regressions on minimal examples,
# modules, and holes — covers the common feature-work cases.
# Use `make test` for the end-of-block gate.
test-fast: test-infer test-infer-check test-dump-typed test-dump-mono test-run test-blocks test-llvm test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-holes test-driver-holes test-effects test-effect-runtime test-sugars test-pipes test-match test-literals test-string test-loop test-diagnostics test-reader test-writer test-trace test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-protocols test-demos-core test-aspirational test-unbox-llvm-bench test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-foreign-prelude test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-318-include test-issue-335 test-unions test-aliases test-sequence test-ufcs test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-issue-315-no-double-slash test-issue-248-loop-sugars test-tco-rule3-basic test-tco-rule3-no-misfire test-unused-bindings

clean:
	rm -rf build $(TARGET)
