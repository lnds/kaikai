CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0
KAIC1   := ../stage1/kaic1
TARGET  := kaic2
SRC     := compiler.kai

# kaic1/kaic2 run recursive passes (lex_loop and others) that stage 0's
# emitter doesn't TCO. On stage 2's source (~9 k lines) those recurse
# past the default 8 MB thread stack. Prepend this to any recipe that
# feeds $(SRC) to kaic1 or kaic2. Harmless where the shell refuses.
STACK := ulimit -s 32768 2>/dev/null || true;

.PHONY: all clean test test-fast test-tokens test-ast test-check test-run test-blocks test-holes test-effects test-effect-runtime test-modules test-modules-path test-sugars test-loop selfhost

all: $(TARGET)

$(KAIC1):
	$(MAKE) -C ../stage1 kaic1

build:
	@mkdir -p build

build/stage2.c: $(SRC) $(KAIC1) | build
	$(STACK) $(KAIC1) $(SRC) > $@

$(TARGET): build/stage2.c
	$(CC) $(CFLAGS) -I ../stage0 $< -o $@

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
	  grep -q "unknown op unknown on effect Io" build/s2-$$name.err \
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
	  grep -q "_ev.tick = &_kai_clause_" build/s2-$$name.c \
	    && grep -q "_ev.get = &_kai_clause_" build/s2-$$name.c \
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
	  grep -q "kai_default_console_print" build/s2-$$name.c \
	    && grep -q "kai_evidence_push(&_kai_default_node_console" build/s2-$$name.c \
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
	  grep -q "kai_default_console_print" build/s2-$$name.c \
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
	         ../examples/effects/m7b_14_writer_helper.kai; do \
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
	         ../examples/effects/m7b_11_followup_resume_type.kai; do \
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

test: test-tokens test-ast test-types test-env test-infer test-infer-check test-dump-typed test-dump-mono test-check test-run test-blocks test-llvm test-modules test-modules-path test-holes test-effects test-effect-runtime test-sugars test-loop test-reader test-writer

# Fast subset for iterative development: skips every subtarget that
# runs kaic2 (or kaic1) against the compiler's own source. Each of
# those takes ~80-90s per invocation and they dominate `make test`
# (~8-9 min of its ~10-12 min runtime). `test-fast` leaves in the
# smoke tests that actually catch regressions on minimal examples,
# modules, and holes — covers the common feature-work cases.
# Use `make test` for the end-of-block gate.
test-fast: test-infer test-infer-check test-dump-typed test-dump-mono test-run test-blocks test-llvm test-modules test-modules-path test-holes test-effects test-effect-runtime test-sugars test-loop test-reader test-writer

clean:
	rm -rf build $(TARGET)
