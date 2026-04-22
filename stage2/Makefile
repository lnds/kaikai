CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0
KAIC1   := ../stage1/kaic1
TARGET  := kaic2
SRC     := compiler.kai

.PHONY: all clean test test-tokens test-ast test-check test-run test-blocks test-holes selfhost

all: $(TARGET)

$(KAIC1):
	$(MAKE) -C ../stage1 kaic1

build:
	@mkdir -p build

build/stage2.c: $(SRC) $(KAIC1) | build
	$(KAIC1) $(SRC) > $@

$(TARGET): build/stage2.c
	$(CC) $(CFLAGS) -I ../stage0 $< -o $@

# Token stream must match stage 1 (which already matches stage 0).
test-tokens: $(TARGET) build
	@set -e; \
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
	@set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --ast $$f > build/s2-$$name.ast && echo "ast OK $$name" \
	    || { echo "ast FAIL $$name"; exit 1; }; \
	done

# --types smoke test: every top-level decl gets a signature dumped.
test-types: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai $(SRC); do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --types $$f > build/s2-$$name.types && echo "types OK $$name" \
	    || { echo "types FAIL $$name"; exit 1; }; \
	done

# --env smoke test: full type environment (prelude + variants + user decls).
test-env: $(TARGET) build
	@set -e; \
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
	@set -e; \
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

# Self-hosting: kaic2 compiles its own source, and the round-trip reaches
# a fixed point.
selfhost: $(TARGET) build
	@set -e; \
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
	@set -e; \
	./$(TARGET) --emit=llvm $(SRC) > build/kaic2-s1.ll; \
	clang -w -I ../stage0 build/kaic2-s1.ll ../stage0/runtime_llvm.c -o build/kaic2-llvm-s1; \
	./build/kaic2-llvm-s1 --emit=llvm $(SRC) > build/kaic2-s2.ll; \
	diff -q build/kaic2-s1.ll build/kaic2-s2.ll > /dev/null \
	  && echo "llvm self-hosting fixed point: OK" \
	  || { echo "llvm self-hosting fixed point: DIFF"; exit 1; }

test: test-tokens test-ast test-types test-env test-infer test-infer-check test-dump-typed test-dump-mono test-check test-run test-blocks test-llvm test-modules test-holes

clean:
	rm -rf build $(TARGET)
