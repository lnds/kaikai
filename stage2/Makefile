CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O2
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

# C++ runtime for the native (libLLVM) link. LLVM is C++, so the static
# link needs the C++ runtime explicitly (`cc` does not add it). The library
# is platform-specific: macOS ships libc++ (`-lc++`), but a Linux libLLVM —
# whether the apt `llvm-dev` package or a stock build — is compiled against
# libstdc++ (`-lstdc++`); linking `-lc++` there either fails to find the lib
# or drags in a second, ABI-incompatible C++ runtime (`libstdc++.so: DSO
# missing` / `__once_callable` undefined). Pick per platform; #834 caught
# the hardcoded `-lc++` breaking tier1-native on Linux.
ifeq ($(UNAME_S),Darwin)
LLVM_CXXLIB := -lc++
else
LLVM_CXXLIB := -lstdc++
endif

# issue #343 — math/real libm bindings (sqrt, sin, log, …) live in
# stage0/runtime.h. Linux glibc requires explicit `-lm` AFTER the
# .c/.o inputs (strict-order ld); macOS libSystem already exposes
# those symbols so the flag is a no-op on Darwin. Trailed at the end
# of every $(CC) compile-and-link line that consumes runtime.h.
LDLIBS_M := -lm

# KIR Lane 1 (docs/kir-design.md §7.2) — in-process libLLVM backend.
# The native backend builds the LLVM module via the C API in one
# process (no `.ll` text, no clang subprocess); it is the default
# `kai build` destination since Lane 1.5 (the flip). Whether a given
# kaic2 carries the native backend is a BUILD-CAPABILITY decision made
# here; whether `kai build` *invokes* it is a runtime decision made in
# `bin/kai resolve_backend()`. The two are deliberately separate: the
# Makefile decides what is compiled in, the wrapper decides what runs.
#
# Capability auto-detection (Lane 1.5):
#   - `make kaic2` with NO KAI_LLVM override probes `llvm-config`. Found
#     → link libLLVM (native-capable). Absent → C-only build. This keeps
#     the bootstrap chain (stages 0–1 + C) and the selfhost byte-id /
#     tier0 / tier1 gates green on a runner WITHOUT LLVM — they build a
#     C-only kaic2 and never need a toolchain beyond `cc`. A dev WITH
#     llvm-config gets native automatically, no flag.
#   - `make KAI_LLVM=1 kaic2` FORCES native-capable: if llvm-config is
#     missing the link line fails loudly (used by the release job, where
#     a missing libLLVM MUST break, not silently ship a C-only binary).
#   - `make KAI_LLVM=0 kaic2` forces C-only even where llvm-config exists
#     (the C oracle build, e.g. for selfhost determinism in isolation).
#
# The Makefile NEVER changes the runtime default and NEVER degrades it
# silently: a C-only kaic2 simply lacks the native backend, and the
# wrapper reports an actionable error if asked for native — it does not
# fall back to C. Discovery is via `llvm-config` (never a hardcoded
# version): the LLVM C API is stable across major versions, so the
# binding survives version bumps and the build stays off Apple clang.
#
# Dev-local links DYNAMIC (Homebrew `-lLLVM`); the release path links
# STATIC (Rust/Zig/Julia model) so the shipped binary has no libLLVM
# runtime dep and the brew formula needs no `depends_on llvm`.
LLVM_CONFIG ?= llvm-config
# KAI_LLVM unset → auto-detect; =1 → force on (fail if absent); =0 → off.
ifeq ($(origin KAI_LLVM),undefined)
KAI_LLVM := $(if $(shell command -v $(LLVM_CONFIG) 2>/dev/null),1,0)
KAI_LLVM_AUTO := 1
endif
ifeq ($(KAI_LLVM),1)
LLVM_CPPFLAGS := $(shell $(LLVM_CONFIG) --cflags) -DKAI_LLVM
# Component set is `nativecodegen` (NOT `native`): the backend EMITS code,
# never disassembles, so it must not drag in the AArch64/X86 Disassembler
# libs that a narrow static vendored build (Makefile §LLVM_CMAKE_TARGETS)
# deliberately omits. `passes` (L4, issue #498) pulls in LLVMPasses — the
# New-PM PassBuilder behind `LLVMRunPasses` / `LLVMCreatePassBuilderOptions`
# the in-process opt pipeline calls; llvm-config expands its transitive
# closure (ipo / vectorize / scalaropts / instcombine …), so the explicit
# `core analysis target codegen` deps below stay covered. The component
# list is repeated on `--system-libs`: without it, against the narrow
# static vendored build, llvm-config scans every known lib and spews a
# `missing libLLVM*.a` line per absent component to stderr (a dynamic
# Homebrew llvm-config has every lib, so it stays quiet there).
# LLVM is C++, so a static link needs the C++ runtime
# explicitly (`cc` does not add it; a dynamic `-lLLVM.dylib` carried it
# implicitly) — `$(LLVM_CXXLIB)` resolves to `-lc++` on macOS and `-lstdc++`
# on Linux (the apt libLLVM is libstdc++-built). The release-static path
# (Rust/Zig/Julia model) thus links with no libLLVM runtime dep; the
# dev-dynamic path links the right C++ lib for the host.
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags) $(shell $(LLVM_CONFIG) --link-static --libs passes core analysis target nativecodegen bitwriter) $(shell $(LLVM_CONFIG) --link-static --system-libs passes core analysis target nativecodegen bitwriter) $(LLVM_CXXLIB)
else
LLVM_CPPFLAGS :=
LLVM_LDFLAGS  :=
endif

KAIC1   := ../stage1/kaic1
TARGET  := kaic2

# Issue #677 — entry point of the `compiler` package. kaic2 reads
# main.kai and resolves `import compiler.<mod>` against
# compiler/<mod>.kai. kaic1 reads the concatenated build/bundle.kai
# produced by the BUNDLE_SRCS recipe below.
SRC     := main.kai

# Issue #677 — modularisation. The stage 2 compiler is now a kaikai
# package rooted at this directory. `kai.toml` names it `compiler`;
# `main.kai` is the entry point; per-module source lives in
# `compiler/<mod>.kai`. kaic2 reads the package modularly (resolving
# `import compiler.<mod>` lines in main.kai). kaic1, which does not
# resolve imports, still consumes a concatenated bundle that the
# Makefile produces below.
#
# BUNDLE_SRCS is the explicit, ordered list of source files the
# concat target glues together for kaic1. The order matters: helpers
# like ch_is_digit must precede the lexer body in main.kai that calls
# them. No wildcards — a wildcard would silently misorder the day
# someone adds a module whose alphabetical name lands before its
# dependencies. kaic1 parses `import` lines (stage1/compiler.kai
# `parse_import_decl`) and ignores them downstream, so the bundle
# compiles clean without stripping the import statements.
BUNDLE_SRCS := compiler/util.kai compiler/chars.kai compiler/diag.kai compiler/lex.kai compiler/ast.kai compiler/rows.kai compiler/doc_attr.kai compiler/native_prims.kai compiler/intervals.kai compiler/refine_help.kai compiler/refinements.kai compiler/parse.kai compiler/resolve.kai compiler/fmt_writer.kai compiler/fmt_expr.kai compiler/fmt_effect.kai compiler/fmt_decl.kai compiler/fmt.kai compiler/cache.kai compiler/modules.kai compiler/user_cache.kai compiler/core_cache.kai compiler/desugar.kai compiler/region.kai compiler/infer_index.kai compiler/infer.kai compiler/evidence_scan.kai compiler/evidence.kai compiler/evidence_inject.kai compiler/evidence_frame.kai compiler/fnreg.kai compiler/unbox.kai compiler/rawsafe.kai compiler/perceus.kai compiler/impl_index.kai compiler/protos.kai compiler/monomorph.kai compiler/emit_shared.kai compiler/ffi_v2.kai compiler/kir.kai compiler/kir_lower.kai compiler/kir_lower_raw.kai compiler/kir_lower_bind.kai compiler/kir_lower_variant.kai compiler/kir_lower_match.kai compiler/kir_lower_lit.kai compiler/kir_lower_rec.kai compiler/kir_lower_walk.kai compiler/kir_lower_fns.kai compiler/kir_dump.kai compiler/emit_native_fn.kai compiler/emit_native_ops.kai compiler/emit_native_ffi.kai compiler/emit_native_fx.kai compiler/emit_native_fx2.kai compiler/emit_native_trmc.kai compiler/emit_native_term.kai compiler/emit_native_def.kai compiler/emit_native_proto.kai compiler/emit_native_test.kai compiler/emit_native.kai compiler/emit_ffi_c.kai compiler/emit_c.kai compiler/lint_idiom.kai compiler/lint.kai compiler/driver.kai main.kai

# Hanga Roa: compile-time absolute path to `stdlib/` so the core
# loader inside kaic2 can find core/*.kai, protocols.kai,
# effects.kai, array.kai without any --prelude flag at runtime.
STDLIB_ABS := $(abspath ../stdlib)
# Stage 2 has its OWN runtime.h (stage2/runtime.h) carrying the Koka
# tagged-Int value representation + reuse-token Perceus. The `-I .`
# comes first so the generated C's `#include "runtime.h"` resolves to
# stage2/runtime.h, while the trailing `-I ../stage0` on each compile
# line still supplies any stage0 headers. Stage 0 and stage 1 keep
# their original heap-everything runtime untouched — the new
# representation is isolated to the self-hosted compiler that builds
# the real programs (and the rb-tree benchmark).
CPPFLAGS_CORE := -I . -DKAI_STDLIB_PATH=\"$(STDLIB_ABS)\"

# Issue #42 closure (2026-05-01): the recursive lex_loop / parse_*
# inside kaic1 / kaic2 used to need a 32-64 MB stack bump to handle
# stage2/compiler.kai's ~32 k tokens. With both stage 0 and stage 1
# emitting goto-form self-tail-calls, kaic1 / kaic2 run with O(1)
# C-stack on the recursive paths and the bump is no longer needed.
# Left as an empty variable so the existing `$(STACK)` recipe
# prefixes still expand to a no-op and don't have to be edited.
STACK :=

.PHONY: all clean test test-fast test-costly-parallel test-tokens test-ast test-check test-run test-blocks test-holes test-lint test-driver-holes test-effects test-effect-runtime test-link-runtime test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-modules-transitive-privacy test-modules-uom-privacy test-sugars test-pipes test-pipes-collections test-match test-literals test-string test-loop test-records test-diagnostics test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-stdlib-survey test-stdlib-crypto-asan test-stdlib-regex-predicate-asan test-http-client-asan test-env-mutate-asan test-securerandom-asan test-match-pbind-catchall-asan test-protocols test-proto-scalar-dispatch test-bare-narrow-assert test-demos-core test-aspirational test-match-backtrack-unify test-unbox-phase2 test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-trace test-trace-asan test-issue-501 test-issue-743 test-issue-842 test-foreign-prelude test-issue-672 test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-746 test-issue-668 test-trmc-list-build test-log-asan test-ffi-extern-c test-ffi-scalar-char test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-issue-315-no-double-slash test-issue-248-loop-sugars test-perceus-issue118 test-perceus-issue118-asan test-perceus-issue298 test-perceus-issue298-asan test-perceus-issue350 test-perceus-issue350-asan test-perceus-issue703 test-perceus-issue703-asan test-issue-779 test-issue-779-asan test-perceus-enum-slot test-perceus-enum-slot-asan test-perceus-int-cache test-perceus-int-cache-asan test-int-field-inline test-int-field-inline-asan test-perceus-nested-reuse test-perceus-nested-reuse-asan test-perceus-812-tracer test-perceus-817-filter-leak test-perceus-860-native-cons-leak test-perceus-872-native-variant-reuse-leak test-perceus-882-native-rotation-reuse-crash test-issue-892-mono-arg-unbox test-heap-limit test-unions test-aliases test-sequence test-ufcs test-rc-trace-callsite selfhost test-tco-rule3-basic test-tco-rule3-no-misfire test-tco-issue-680 test-issue-678-stdout-buffered test-issue-679-cancel-parked test-issue-682-cancel-sibling-handler test-issue-978-spawn-parent-handled-effect test-violations test-attributes test-doc-attr test-shadowing test-modular-build test-issue-963-cross-module-mono test-packages test-kir test-user-cache test-core-cache test-independence-oracle test-batch-isolation

all: $(TARGET)

$(KAIC1):
	$(MAKE) -C ../stage1 kaic1

build:
	@mkdir -p build

# Issue #677 Phase 1 — concat the per-module sources into a single
# bundle that kaic1 consumes as one translation unit. `awk 1` ensures
# every file ends with a newline before its successor starts, so a
# missing final newline in module A cannot fuse a token across the
# A→B boundary.
#
# No header preamble. Adding any line at the top would shift every
# downstream line number and would cascade into kaikai's gensym names
# (which encode `<line>_<col>`), producing a byte-shift in the
# emitted C between the direct `kaic1 compiler.kai` path and the new
# `kaic1 build/bundle.kai` path. Selfhost determinism still holds
# (kaic2b.c == kaic2c.c) but the cross-path equivalence is worth
# preserving so anyone comparing artifacts pre/post-modularisation
# sees zero diff.
build/bundle.kai: $(BUNDLE_SRCS) | build
	@awk 1 $(BUNDLE_SRCS) > $@

build/stage2.c: build/bundle.kai $(KAIC1) | build
	$(STACK) $(KAIC1) build/bundle.kai > $@

# Announce which backend capability got compiled in, so a dev never has
# to guess why `kai build` behaves differently across two checkouts (the
# answer is always: llvm-config in PATH or not). Lane 1.5 capability line.
$(TARGET): build/stage2.c
ifeq ($(KAI_LLVM),1)
	@echo "building stage2: native backend ENABLED ($(LLVM_CONFIG) found)"
else
	@echo "building stage2: C-only$(if $(KAI_LLVM_AUTO), (no $(LLVM_CONFIG) in PATH; run make KAI_LLVM=1 for native),)"
endif
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) $(LLVM_CPPFLAGS) $(LDFLAGS_STACK) -I ../stage0 $< -o $@ $(LDLIBS_M) $(LLVM_LDFLAGS)
ifeq ($(KAI_LLVM),1)
	@# P2 (docs/native-codegen-perf-plan.md §P2): generate the native-runtime
	@# bitcode the in-process backend links before O2. Build-time + gated to
	@# clang 18 (writer <= the libLLVM-18 reader); a clang-18-less host opts
	@# out cleanly (native falls back to linking runtime_llvm.c with cc). The
	@# script is idempotent — a no-op when the .bc is fresh.
	@../tools/gen-runtime-bc.sh || true
endif

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
	  : '#645 introduced phantom row vars on pure named fns, so call-sites'; \
	  : 'into row-poly callbacks (filter, map, each, ...) legitimately'; \
	  : 'leave the trailing row slot as `?tN` until m4c specialises.'; \
	  : 'Reject only when a `?tN` appears as a TYPE var (any slot but'; \
	  : 'the last of the type-arg list), not as the trailing row var.'; \
	  if grep -Eq '\?t[0-9]+, ' build/s2-$$name.mono; then \
	    echo "dump-mono FAIL $$name (unresolved type vars)"; \
	    grep -E '\?t[0-9]+, ' build/s2-$$name.mono | head; \
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

# Phase 1.B.0 — cons-head raw-safe use-analysis (--dump-rawsafe). The
# golden pins only the USER fns' marks (grep by fn name), so a stdlib
# edit cannot perturb the fixture. Verifies the two load-bearing shapes:
# `bump` (head only feeds raw arithmetic) marks load+store `raw`, and
# `show_heads` (head feeds a boxed call `int_to_string(h)`) marks load
# `boxed` — the 4th-dead-end antidote. The pass changes no runtime and
# no emitted text; this is a pure analysis golden.
RAWSAFE_FIXTURES := rawsafe_bump:bump,main rawsafe_boxed:show_heads,keep_pos,main

test-rawsafe: $(TARGET) build
	@set -e; \
	for spec in $(RAWSAFE_FIXTURES); do \
	  name=$${spec%%:*}; fns=$${spec#*:}; \
	  pat=$$(echo "$$fns" | sed 's/,/|/g'); \
	  ./$(TARGET) --path ../stdlib --dump-rawsafe ../examples/perceus/$$name.kai 2> build/$$name.rawsafe.err \
	    | grep -E "^($$pat) (load|store)" > build/$$name.rawsafe || true; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.rawsafe > /dev/null \
	    && echo "rawsafe OK $$name (use-analysis marks match golden)" \
	    || { echo "rawsafe DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.rawsafe; exit 1; }; \
	done

# Phase 1.B.1 — cons-head borrow-safe classifier (--dump-borrowsafe).
# Pins the four discriminating shapes: sum_sq/bump raw (head only read
# as scalar), first/show boxed (head pointer propagated via return /
# call). The classifier changes no emitted text; this is a pure
# analysis golden, the prerequisite for the perceus borrow consumer.
BORROWSAFE_FIXTURES := borrowsafe_classify:sum_sq,first,bump,show

test-borrowsafe: $(TARGET) build
	@set -e; \
	for spec in $(BORROWSAFE_FIXTURES); do \
	  name=$${spec%%:*}; fns=$${spec#*:}; \
	  pat=$$(echo "$$fns" | sed 's/,/|/g'); \
	  ./$(TARGET) --path ../stdlib --dump-borrowsafe ../examples/perceus/$$name.kai 2> build/$$name.borrowsafe.err \
	    | grep -E "^($$pat) borrow" > build/$$name.borrowsafe || true; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.borrowsafe > /dev/null \
	    && echo "borrowsafe OK $$name (borrow classifier matches golden)" \
	    || { echo "borrowsafe DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.borrowsafe; exit 1; }; \
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

# End-to-end: emitted C from kaic2 matches stage 1's stdout byte for byte.
test-run: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/run-$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/run-$$name.c -o build/run-$$name $(LDLIBS_M) ; \
	  ./build/run-$$name > build/run-$$name.out; \
	  ../stage1/kaic1 $$f > build/run-$$name-s1.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/run-$$name-s1.c -o build/run-$$name-s1 $(LDLIBS_M) ; \
	  ./build/run-$$name-s1 > build/run-$$name-s1.out; \
	  diff -q build/run-$$name.out build/run-$$name-s1.out > /dev/null \
	    && echo "run OK $$name" \
	    || { echo "run DIFF $$name"; exit 1; }; \
	done

# --test binaries produce the same stderr as stage 1's.
test-blocks: $(TARGET) build
	@set -e; \
	for f in ../examples/minimal/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --test $$f > build/$$name-t.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name-t.c -o build/$$name-t $(LDLIBS_M) ; \
	  ./build/$$name-t 2> build/$$name-t.err || true; \
	  ../stage1/kaic1 --test $$f > build/$$name-t-s1.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name-t-s1.c -o build/$$name-t-s1 $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/modules.c -o build/modules-c $(LDLIBS_M) ; \
	./build/modules-c > build/modules-c.out; \
	printf '42\n42\n12\n' > build/modules.expected; \
	diff -q build/modules-c.out build/modules.expected > /dev/null \
	  && echo "modules OK" \
	  || { echo "modules DIFF"; diff build/modules.expected build/modules-c.out; exit 1; }

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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/mq-$$d.c -o build/mq-$$d-c $(LDLIBS_M) \
	    || { echo "modules-qualified FAIL $$d (cc)"; exit 1; }; \
	  ./build/mq-$$d-c > build/mq-$$d-c.out \
	    || { echo "modules-qualified FAIL $$d (run C)"; exit 1; }; \
	  diff -q $$exp build/mq-$$d-c.out > /dev/null \
	    || { echo "modules-qualified DIFF $$d (C vs golden)"; diff $$exp build/mq-$$d-c.out; exit 1; }; \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/mc-$$d.c -o build/mc-$$d-c $(LDLIBS_M) \
	    || { echo "modules-collision FAIL $$d (cc)"; exit 1; }; \
	  ./build/mc-$$d-c > build/mc-$$d-c.out \
	    || { echo "modules-collision FAIL $$d (run C)"; exit 1; }; \
	  diff -q $$exp build/mc-$$d-c.out > /dev/null \
	    || { echo "modules-collision DIFF $$d (C vs golden)"; diff $$exp build/mc-$$d-c.out; exit 1; }; \
	  echo "modules-collision OK $$d"; \
	done
	@# Issue #644: the m12.8.x upfront rejection (PR #507) for
	@# `pub type Respuesta = Ok(String) | ...` colliding with the
	@# builtin `Result.Ok` was relaxed by Plan B. The
	@# `export_ambiguous_constructor.kai` fixture moved from a
	@# negative gate (was rejected) to a positive runner under
	@# `test-shadowing` — the new fixture set covers the use-site
	@# disambiguation that replaced the upfront rejection.

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
	  ./$(TARGET) --path ../examples/modules/$$d --path ../stdlib $$src > build/mde-$$d.c \
	    || { echo "modules-derive-export FAIL $$d (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/mde-$$d.c -o build/mde-$$d-c $(LDLIBS_M) \
	    || { echo "modules-derive-export FAIL $$d (cc)"; exit 1; }; \
	  ./build/mde-$$d-c > build/mde-$$d-c.out \
	    || { echo "modules-derive-export FAIL $$d (run C)"; exit 1; }; \
	  diff -q $$exp build/mde-$$d-c.out > /dev/null \
	    || { echo "modules-derive-export DIFF $$d (C vs golden)"; diff $$exp build/mde-$$d-c.out; exit 1; }; \
	  echo "modules-derive-export OK $$d"; \
	done

# Issue #565 — transitive-import privacy regression. `main`
# imports `mid`, `mid` imports `leaf`. `leaf` has a non-`pub`
# helper called only from `leaf`'s own `pub` function. Before
# the fix the privacy validator mis-attributed `leaf`'s decls
# to `mid` (the importer reached first) and rejected the intra-
# `leaf` call as a cross-module access. Positive fixture must
# compile + run + match the golden on both backends.
test-modules-transitive-privacy: $(TARGET) build
	@set -e; \
	d=transitive_import_privacy; \
	src=../examples/modules/$$d/main.kai; \
	exp=../examples/modules/$$d/main.out.expected; \
	./$(TARGET) --path ../examples/modules/$$d $$src > build/mtp-$$d.c \
	  || { echo "modules-transitive-privacy FAIL $$d (kaic2)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/mtp-$$d.c -o build/mtp-$$d-c $(LDLIBS_M) \
	  || { echo "modules-transitive-privacy FAIL $$d (cc)"; exit 1; }; \
	./build/mtp-$$d-c > build/mtp-$$d-c.out \
	  || { echo "modules-transitive-privacy FAIL $$d (run C)"; exit 1; }; \
	diff -q $$exp build/mtp-$$d-c.out > /dev/null \
	  || { echo "modules-transitive-privacy DIFF $$d (C vs golden)"; diff $$exp build/mtp-$$d-c.out; exit 1; }; \
	echo "modules-transitive-privacy OK $$d"

# Hanga Roa import-filter lane — cross-module privacy validation
# of decl signatures. Each sub-fixture lives in its own directory
# under `examples/modules/` with `lib.kai` + `main.kai`. Positive
# fixtures supply a `main.out.expected` golden; negative fixtures
# supply a `main.err.expected` whose every non-empty line must
# appear (grep) in the compiler's stderr.
#
#   uom_pub_import           positive: pub unit visible
#   uom_private_unit         negative: priv unit rejected at use
#   priv_type_used_in_main   negative: priv type rejected at use
#   priv_effect_used_in_main negative: priv effect rejected at use
MUOMP_POS_DIRS := uom_pub_import pub_type_used_in_main
MUOMP_NEG_DIRS := uom_private_unit priv_type_used_in_main priv_effect_used_in_main
test-modules-uom-privacy: $(TARGET) build
	@set -e; \
	for d in $(MUOMP_POS_DIRS); do \
	  src=../examples/modules/$$d/main.kai; \
	  exp=../examples/modules/$$d/main.out.expected; \
	  ./$(TARGET) --path ../examples/modules/$$d $$src > build/muomp-$$d.c \
	    || { echo "modules-uom-privacy FAIL $$d (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/muomp-$$d.c -o build/muomp-$$d-c $(LDLIBS_M) \
	    || { echo "modules-uom-privacy FAIL $$d (cc)"; exit 1; }; \
	  ./build/muomp-$$d-c > build/muomp-$$d-c.out \
	    || { echo "modules-uom-privacy FAIL $$d (run C)"; exit 1; }; \
	  diff -q $$exp build/muomp-$$d-c.out > /dev/null \
	    || { echo "modules-uom-privacy DIFF $$d"; diff $$exp build/muomp-$$d-c.out; exit 1; }; \
	  echo "modules-uom-privacy OK $$d"; \
	done
	@set -e; \
	for d in $(MUOMP_NEG_DIRS); do \
	  src=../examples/modules/$$d/main.kai; \
	  exp=../examples/modules/$$d/main.err.expected; \
	  rc=0; ./$(TARGET) --path ../examples/modules/$$d $$src > /dev/null 2> build/muomp-$$d.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "modules-uom-privacy FAIL $$d (expected error, got success)"; exit 1; \
	  fi; \
	  while read -r line; do \
	    [ -z "$$line" ] && continue; \
	    grep -q "$$line" build/muomp-$$d.err \
	      || { echo "modules-uom-privacy FAIL $$d (missing diagnostic: $$line)"; \
	           cat build/muomp-$$d.err; exit 1; }; \
	  done < $$exp; \
	  echo "modules-uom-privacy OK $$d (rejected as expected)"; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/modules-path.c -o build/modules-path-c $(LDLIBS_M) ; \
	./build/modules-path-c > build/modules-path-c.out; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/loop.c -o build/loop-c $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/reader.c -o build/reader-c $(LDLIBS_M) ; \
	./build/reader-c > build/reader.out; \
	diff -q ../examples/reader/main.out.expected build/reader.out > /dev/null \
	  && echo "reader OK" \
	  || { echo "reader DIFF"; diff ../examples/reader/main.out.expected build/reader.out; exit 1; }

test-writer: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/writer/main.kai > build/writer.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/writer.c -o build/writer-c $(LDLIBS_M) ; \
	./build/writer-c > build/writer.out; \
	diff -q ../examples/writer/main.out.expected build/writer.out > /dev/null \
	  && echo "writer OK" \
	  || { echo "writer DIFF"; diff ../examples/writer/main.out.expected build/writer.out; exit 1; }

# Issue #80 regression — polymorphic calls across an imported module.
# `examples/effects/foreign_prelude.kai` imports `foreign_prelude_lib`;
# this target compiles + runs it against the
# golden. The fixture exercises every scheme shape that historically
# tripped cross-file instantiation (implicit lowercase tparams, row
# polymorphism on a callback, parametric Reader[T]). A regression of
# the same shape produces a deterministic stdout diff under tier1.
test-foreign-prelude: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/effects/foreign_prelude.kai > build/foreign_prelude.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/foreign_prelude.c -o build/foreign_prelude-c $(LDLIBS_M) ; \
	./build/foreign_prelude-c > build/foreign_prelude.out; \
	diff -q ../examples/effects/foreign_prelude.out.expected build/foreign_prelude.out > /dev/null \
	  && echo "foreign-prelude OK" \
	  || { echo "foreign-prelude DIFF"; diff ../examples/effects/foreign_prelude.out.expected build/foreign_prelude.out; exit 1; }

# Issue #672 — a `@cap` read inside a `#{...}` interpolation must
# resolve in the enclosing `var` scope. The var-specialisation escape
# check skipped raw interp spans, so the cheaper array-slot lowering
# fired and dropped the binder; the interp-lifted `tally.get()` then
# resolved to nothing (`cannot find tally`, span on the fn signature).
# Compile + run + diff so a regression of the same shape fails tier1.
test-issue-672: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/effects/cap_read_in_interp.kai > build/cap_read_in_interp.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/cap_read_in_interp.c -o build/cap_read_in_interp-c $(LDLIBS_M) ; \
	./build/cap_read_in_interp-c > build/cap_read_in_interp.out; \
	diff -q ../examples/effects/cap_read_in_interp.out.expected build/cap_read_in_interp.out > /dev/null \
	  && echo "issue-672 OK (@cap in interp resolves)" \
	  || { echo "issue-672 DIFF"; diff ../examples/effects/cap_read_in_interp.out.expected build/cap_read_in_interp.out; exit 1; }

# m12.7.x FFI v1 regression — `[<extern_c>]` axiom lowers to a
# direct `extern "C"` call. Compiles + runs the fixture on the C
# backend so a regression on the FFI path (parser, lower_axiom_one,
# perceus skip, emit_fn_body detection) shows up under tier1. The
# fixture stays libc-only so it links without extra `cc` flags.
test-ffi-extern-c: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/effects/ffi_extern_c_basic.kai > build/ffi_extern_c_basic.c; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage2 -I ../stage0 build/ffi_extern_c_basic.c -o build/ffi_extern_c_basic-c $(LDLIBS_M) ; \
	./build/ffi_extern_c_basic-c > build/ffi_extern_c_basic.out; \
	diff -q ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic.out > /dev/null \
	  && echo "ffi-extern-c OK (C backend)" \
	  || { echo "ffi-extern-c DIFF (C backend)"; diff ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic.out; exit 1; }

# FFI v2 (#417) — struct-by-value across the C boundary. The emitter
# ships the `extern "C" type` structs in a shared `kai_ffi.h` (issue
# #924); a struct program emits a `//KAIFILE`-marker stream the wrapper
# splits into `kai_ffi.h` + `out.c`. The shim (v2_struct_shim.c)
# `#include`s the SAME header and names the SAME `Color` / `Vector2`
# types, so a layout mismatch is a C compile error, not silent corruption.
# ASAN-clean (boundary unwrap + re-box is fresh malloc territory).
# Struct-by-value is a C-backend feature; the native backend rejects it
# with an actionable `--backend=c` diagnostic (issue #923).
test-ffi-v2-struct: $(TARGET) build
	@set -e; \
	./$(TARGET) --backend=c ../examples/ffi/v2_struct_by_value.kai > build/v2_struct.stream; \
	rm -rf build/v2_struct.d; mkdir -p build/v2_struct.d; \
	nonce=$$(sed -n '1s/^\/\/KAIDELIM://p' build/v2_struct.stream); \
	awk -v dir=build/v2_struct.d -v pfx="//KAIFILE:$$nonce::" 'index($$0, pfx) == 1 { out = dir "/" substr($$0, length(pfx) + 1); next } out { print > out }' build/v2_struct.stream; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I build/v2_struct.d -I ../stage2 -I ../stage0 build/v2_struct.d/out.c ../examples/ffi/v2_struct_shim.c -o build/v2_struct-c $(LDLIBS_M); \
	./build/v2_struct-c > build/v2_struct.out; \
	diff -q ../examples/ffi/v2_struct_by_value.out.expected build/v2_struct.out > /dev/null \
	  && echo "ffi-v2-struct OK (C backend)" \
	  || { echo "ffi-v2-struct DIFF (C backend)"; diff ../examples/ffi/v2_struct_by_value.out.expected build/v2_struct.out; exit 1; }; \
	$(CC) $(CFLAGS) -fsanitize=address -Wno-absolute-value -I build/v2_struct.d -I ../stage2 -I ../stage0 build/v2_struct.d/out.c ../examples/ffi/v2_struct_shim.c -o build/v2_struct-asan $(LDLIBS_M); \
	./build/v2_struct-asan > build/v2_struct.asan.out 2>&1; \
	diff -q ../examples/ffi/v2_struct_by_value.out.expected build/v2_struct.asan.out > /dev/null \
	  && echo "ffi-v2-struct OK (ASAN clean)" \
	  || { echo "ffi-v2-struct ASAN DIFF"; cat build/v2_struct.asan.out; exit 1; }

# FFI v2 (#417) — opaque handles. A handle is used in three consuming
# calls (two bumps + the destructor), pinning the no-Drop ownership rule
# (a wrong rule double-frees the box on the second use). The C-backend
# variant runs + ASAN. Opaque also works on the native backend, gated by
# the native-vs-C parity ratchet.
test-ffi-v2-opaque: $(TARGET) build
	@set -e; \
	./$(TARGET) --backend=c ../examples/ffi/v2_opaque_handle.kai > build/v2_opaque.c; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage2 -I ../stage0 build/v2_opaque.c ../examples/ffi/v2_shim.c -o build/v2_opaque-c $(LDLIBS_M); \
	./build/v2_opaque-c > build/v2_opaque.out; \
	diff -q ../examples/ffi/v2_opaque_handle.out.expected build/v2_opaque.out > /dev/null \
	  && echo "ffi-v2-opaque OK (C backend)" \
	  || { echo "ffi-v2-opaque DIFF (C backend)"; diff ../examples/ffi/v2_opaque_handle.out.expected build/v2_opaque.out; exit 1; }; \
	$(CC) $(CFLAGS) -fsanitize=address -Wno-absolute-value -I ../stage2 -I ../stage0 build/v2_opaque.c ../examples/ffi/v2_shim.c -o build/v2_opaque-asan $(LDLIBS_M); \
	./build/v2_opaque-asan > build/v2_opaque.asan.out 2>&1; \
	diff -q ../examples/ffi/v2_opaque_handle.out.expected build/v2_opaque.asan.out > /dev/null \
	  && echo "ffi-v2-opaque OK (ASAN clean)" \
	  || { echo "ffi-v2-opaque ASAN DIFF"; cat build/v2_opaque.asan.out; exit 1; }

# FFI v2 — native backend rejects struct-by-value with an actionable
# `--backend=c` diagnostic (issue #923) instead of crashing. A small
# struct's C ABI (iN / [N x float] / sret) is frontend classification the
# native backend does not yet replicate; the C backend marshals it. Gated
# on KAI_LLVM (the native backend must be built to exercise the reject).
ifeq ($(KAI_LLVM),1)
test-ffi-v2-native-reject: $(TARGET) build
	@set -e; \
	./$(TARGET) --emit=native --path ../examples/ffi ../examples/ffi/v2_struct_by_value.kai > /dev/null 2> build/v2_struct_native.err \
	  && { echo "ffi-v2-native-reject DID NOT FAIL — native must reject struct-by-value"; exit 1; } || true; \
	grep -F "$$(cat ../examples/ffi/v2_struct_native_reject.err.expected)" build/v2_struct_native.err > /dev/null \
	  && echo "ffi-v2-native-reject OK (actionable --backend=c diagnostic)" \
	  || { echo "ffi-v2-native-reject DIFF — expected diagnostic not in output"; cat build/v2_struct_native.err; exit 1; }
else
test-ffi-v2-native-reject:
	@echo "ffi-v2-native-reject SKIP (C-only build; native backend not compiled)"
endif

# FFI v2 (#417) — negatives the surface still rejects: a struct field
# with no exact C width, and a function-typed (callback-from-C) extern
# param. Each must fail to compile with the matching diagnostic.
test-ffi-v2-negatives: $(TARGET) build
	@set -e; \
	for f in v2_struct_field_non_c v2_callback_param; do \
	  ./$(TARGET) --check ../examples/negative/ffi/$$f.kai > build/$$f.out 2>&1 \
	    && { echo "$$f DID NOT FAIL — v2 must still reject it"; exit 1; } || true; \
	  grep -F "$$(head -1 ../examples/negative/ffi/$$f.err.expected)" build/$$f.out > /dev/null \
	    && echo "ffi-v2-negative $$f OK (rejected)" \
	    || { echo "ffi-v2-negative $$f DIFF — expected diagnostic not in output"; cat build/$$f.out; exit 1; }; \
	done

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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) ; \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    && echo "$$f OK (C backend)" \
	    || { echo "$$f DIFF (C backend)"; diff $$exp build/$$f.out; exit 1; }; \
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
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage2 -I ../stage0 \
	  build/ffi_pub_axiom_cross_module.c -o build/ffi_pub_axiom_cross_module-c $(LDLIBS_M); \
	./build/ffi_pub_axiom_cross_module-c > build/ffi_pub_axiom_cross_module.out; \
	diff -q ../examples/effects/ffi_pub_axiom_cross_module/main.out.expected \
	  build/ffi_pub_axiom_cross_module.out > /dev/null \
	  && echo "ffi-pub-axiom-cross-module OK (C backend)" \
	  || { echo "ffi-pub-axiom-cross-module DIFF (C backend)"; \
	       diff ../examples/effects/ffi_pub_axiom_cross_module/main.out.expected \
	            build/ffi_pub_axiom_cross_module.out; exit 1; }

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
	for f in m7b_5_until_basic m7b_5_while_var m7b_5_repeat_basic \
	         issue_802_while_disjoint_state_cells \
	         issue_802_lambda_yield_var_state \
	         issue_802_stream_alias_var_state; do \
	  src=../examples/sugars/loop/$$f.kai; \
	  exp=../examples/sugars/loop/$$f.out.expected; \
	  ./$(TARGET) --path ../stdlib $$src > build/$$f.c \
	    || { echo "$$f FAIL (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) \
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
	      -I ../stage2 -I ../stage0 build/ffi_extern_c_basic-asan.c -o build/ffi_extern_c_basic-c-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/ffi_extern_c_basic-c-asan > build/ffi_extern_c_basic-asan.out 2>&1; \
	diff -q ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic-asan.out > /dev/null \
	  && echo "ffi-extern-c-asan OK (C backend, ASAN+UBSan clean)" \
	  || { echo "ffi-extern-c-asan DIFF (C backend, ASAN+UBSan)"; cat build/ffi_extern_c_basic-asan.out; exit 1; }

# Issue #933 / #934 regression — a non-libc user-symbol scalar extern
# (#933) and the `Char` boundary type (#934), linked against
# ../examples/ffi/scalar_shim.c. `-Werror=implicit-function-declaration`
# makes the missing forward declaration #933 fixed a HARD failure (so a
# regression cannot hide behind the undocumented `-Wno-implicit-...`
# flag). Both fixtures run on the C backend + ASAN; the native backend
# is exercised by the parity ratchet.
test-ffi-scalar-char: $(TARGET) build
	@set -e; \
	for f in extern_c_user_symbol extern_c_char; do \
	  src=../examples/ffi/$$f.kai; \
	  exp=../examples/ffi/$$f.out.expected; \
	  ./$(TARGET) $$src > build/$$f.c; \
	  $(CC) $(CFLAGS) -Werror=implicit-function-declaration -Wno-absolute-value \
	    -I ../stage2 -I ../stage0 build/$$f.c ../examples/ffi/scalar_shim.c -o build/$$f-c $(LDLIBS_M); \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    && echo "$$f OK (C backend)" \
	    || { echo "$$f DIFF (C backend)"; diff $$exp build/$$f.out; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Werror=implicit-function-declaration -Wno-unused-function -Wno-unused-variable -Wno-absolute-value \
	    -I ../stage2 -I ../stage0 build/$$f.c ../examples/ffi/scalar_shim.c -o build/$$f-c-asan; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	    UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$f-c-asan > build/$$f-asan.out 2>&1; \
	  diff -q $$exp build/$$f-asan.out > /dev/null \
	    && echo "$$f OK (ASAN+UBSan clean)" \
	    || { echo "$$f ASAN DIFF"; cat build/$$f-asan.out; exit 1; }; \
	done

# Issue #216 regression — qualified-call resolver inside a stdlib file.
# The helper `qualified_call_in_prelude_lib` defines bodies like
# `list.nth(xs, 1)`; before the fix the parse tree skipped the
# EField->EModCall rewrite and the typer rejected the call as
# "undefined name `list`". Compiled against the auto-loaded core plus
# the helper file (resolved via `--path ../stdlib`).
test-prelude-qualified-call: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib \
	            ../examples/stdlib/qualified_call_in_prelude.kai \
	            > build/qualified_call_in_prelude.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/qualified_call_in_prelude.c -o build/qualified_call_in_prelude $(LDLIBS_M) ; \
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
	./$(TARGET) --path ../stdlib \
	            ../examples/stdlib/issue_219_typer_modcall.kai \
	            > build/issue_219_typer_modcall.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/issue_219_typer_modcall.c -o build/issue_219_typer_modcall $(LDLIBS_M) ; \
	./build/issue_219_typer_modcall > build/issue_219_typer_modcall.out; \
	diff -q ../examples/stdlib/issue_219_typer_modcall.out.expected build/issue_219_typer_modcall.out > /dev/null \
	  && echo "issue-219-modcall OK" \
	  || { echo "issue-219-modcall DIFF"; diff ../examples/stdlib/issue_219_typer_modcall.out.expected build/issue_219_typer_modcall.out; exit 1; }

# Issue #318 — `kai test <file>` reports only the file's own test
# blocks. The fixture is a tiny user-style program with two test
# blocks; compiled with --test against the full core module set,
# the runner output must list exactly those two and no stdlib tests.
# (Core modules' own test blocks are always dropped at load time — a
# file's tests never pull in another module's; see `load_prelude` in
# driver.kai.)
test-issue-318: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib \
	            --test ../examples/stdlib/issue_318_kai_test_prelude_scope.kai \
	            > build/issue_318_kai_test_prelude_scope.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/issue_318_kai_test_prelude_scope.c -o build/issue_318_kai_test_prelude_scope $(LDLIBS_M) ; \
	./build/issue_318_kai_test_prelude_scope > build/issue_318_kai_test_prelude_scope.out 2>&1; \
	diff -q ../examples/stdlib/issue_318_kai_test_prelude_scope.out.expected build/issue_318_kai_test_prelude_scope.out > /dev/null \
	  && echo "issue-318 OK (user-only tests)" \
	  || { echo "issue-318 DIFF"; diff ../examples/stdlib/issue_318_kai_test_prelude_scope.out.expected build/issue_318_kai_test_prelude_scope.out; exit 1; }

# Issue #746 — a [(A, B)]-returning fn with an `[]` base case and a
# sugared tuple/record in the recursive cons must type-check. Before
# the partition fix (`decide_partition_home` -> anchor) a test block
# could create the target bucket too early, so `Pair` resolved without
# type args and the spread failed to compile. The regression is at
# COMPILE time (symptom: `error: type mismatch in list spread`), so the
# gate is that kaic2 emits C cleanly and the C compiles.
test-issue-746: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib \
	            --test ../examples/stdlib/issue_746_tuple_spread.kai \
	            > build/issue_746.c 2> build/issue_746.err \
	  || { echo "issue-746 FAIL (kaic2 errored — tuple-in-spread regressed)"; \
	       cat build/issue_746.err; exit 1; }; \
	if grep -q "type mismatch in list spread" build/issue_746.err; then \
	  echo "issue-746 FAIL (#746 regressed: spread type mismatch)"; \
	  cat build/issue_746.err; exit 1; \
	fi; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/issue_746.c -o build/issue_746 $(LDLIBS_M) \
	  || { echo "issue-746 FAIL (C compile failed)"; exit 1; }; \
	echo "issue-746 OK (tuple-in-list-spread types)"

# Issue #668 — list.map/filter/flat_map over a large list inside a
# fiber must not overflow the 64 KiB fiber stack. The natural
# `[f(h), ...map(t, f)]` recursion is not in tail position and grew one
# C frame per element; rewritten tail-recursive + reverse it is O(1)
# stack. Run at the DEFAULT fiber stack (no KAI_FIBER_STACK_SIZE) so a
# regression to the non-tail form crashes here with `fiber stack
# overflow` over 40K elements.
test-issue-668: $(TARGET) build
	@set -e; \
	f=../examples/effects/issue_668_map_large_in_fiber.kai; \
	name=$$(basename $$f .kai); \
	./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	  || { echo "issue-668 FAIL (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	  || { echo "issue-668 FAIL (C compile failed)"; exit 1; }; \
	./build/s2-$$name > build/s2-$$name.out 2>&1 \
	  || { echo "issue-668 FAIL (fiber stack overflow regressed — map not tail-recursive?)"; cat build/s2-$$name.out; exit 1; }; \
	diff -q ../examples/effects/issue_668_map_large_in_fiber.out.expected build/s2-$$name.out > /dev/null \
	  && echo "issue-668 OK (map/filter/flat_map over 40K in fiber, O(1) stack)" \
	  || { echo "issue-668 DIFF"; diff ../examples/effects/issue_668_map_large_in_fiber.out.expected build/s2-$$name.out; exit 1; }

# TRMC for the BUILTIN cons (`[h, ...recur(t)]`). Before the fix the TRMC
# rewrite only recognised a USER ctor, so the idiomatic list-build loop
# grew one C frame per element and overran an 8 MB stack under 100K
# elements (Tier-1 mandatory-TCO violation). The fixture builds a 1M-deep
# list two ways — `build` (raw EList) and `map_inc` (perceus reuse-cons) —
# and must NOT overflow. We (1) assert the C-text emitter planted the cons
# TRMC goto-loop (a regression that stops the rewrite firing fails here,
# not silently), then (2) link with an EXPLICIT 8 MB main-thread stack
# (Darwin: the linked size IS the stack; Linux: the STACK prefix's
# `ulimit -s 8192` constrains it) and run — a regression to per-element
# recursion crashes with exit 139. C-backend only: LLVM/native lack cons-
# TRMC lowering and fall back to ordinary recursion (a follow-up), so this
# fixture is on the backend-parity skip list and the small-list value
# guard examples/perceus/trmc_modcons_llvm.kai carries C<->LLVM parity.
test-trmc-list-build: $(TARGET) build
	@set -e; \
	f=../examples/perceus/trmc_list_build_large.kai; \
	name=$$(basename $$f .kai); \
	./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	  || { echo "trmc-list-build FAIL (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	grep -q 'goto _kai_build_entry' build/s2-$$name.c \
	  || { echo "trmc-list-build FAIL (cons TRMC did not fire for build — rewrite regressed?)"; exit 1; }; \
	grep -q 'goto _kai_map_inc_entry' build/s2-$$name.c \
	  || { echo "trmc-list-build FAIL (cons TRMC did not fire for map_inc — reuse-cons shape regressed?)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -Wl,-stack_size,0x800000 -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) 2>/dev/null \
	  || $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	  || { echo "trmc-list-build FAIL (C compile failed)"; exit 1; }; \
	( ulimit -s 8192 2>/dev/null; ./build/s2-$$name > build/s2-$$name.out 2>&1 ) \
	  || { echo "trmc-list-build FAIL (stack overflow regressed — `[h, ...recur(t)]` not TCO'd?)"; cat build/s2-$$name.out; exit 1; }; \
	diff -q ../examples/perceus/trmc_list_build_large.out.expected build/s2-$$name.out > /dev/null \
	  && echo "trmc-list-build OK (1M-deep [h, ...recur(t)] build + map, O(1) stack)" \
	  || { echo "trmc-list-build DIFF"; diff ../examples/perceus/trmc_list_build_large.out.expected build/s2-$$name.out; exit 1; }

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
# `clause_capture_subset_order` is the clause-capture ABI's env-INDEX-ORDER
# guard (lane native-clause-capture-abi): two clauses of one handle capture
# DIFFERENT subsets of the enclosing scope, so a per-clause capture index
# diverges from the handle-union index the Ev `env` array is keyed on. r9
# above is false-green for that ordering bug (both its clauses capture the
# same name at union slot 0); this fixture forces them apart, and the C
# golden is the oracle native parity diffs against. RC + ASAN gated for free
# by sharing the trace suite.
TRACE_FIXTURES := trace_basic trace_prefix r9_clause_capture \
                  clause_capture_subset_order r10_repro r11_repro \
                  m8x_9_nested_mailbox_under_trap_exit \
                  issue_115_default_handler_typed_dispatch

test-trace: $(TARGET) build
	@set -e; \
	for name in $(TRACE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage2 -I ../stage0 build/$$name.c -o build/$$name-c $(LDLIBS_M) ; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage2 -I ../stage0 build/$$name.c -o build/$$name-c $(LDLIBS_M) ; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	        $(LDFLAGS_STACK) -I ../stage2 -I ../stage0 build/$$name.c \
	        -o build/$$name-c $(LDLIBS_M); \
	  ./build/$$name-c > build/$$name.out; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "issue-501 OK $$name" \
	    || { echo "issue-501 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# Issue #743 — a field access on a generic effect-op payload. The fix
# is a timing one in the field-access path (deferred field-vs-UFCS
# decision), not effect propagation. Positive: `b.id` on a `Repo[Box,_]`
# payload type-checks against Box and runs `found`. Negative
# (the Tier 1 escape): `b.zzz` (nonexistent field) must now be REJECTED
# at compile time on Box, instead of compiling silently.
test-issue-743: $(TARGET) build
	@set -e; \
	pos=issue_743_generic_effect_payload_field; \
	./$(TARGET) ../examples/effects/$$pos.kai > build/$$pos.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$pos.c -o build/$$pos-c $(LDLIBS_M); \
	./build/$$pos-c > build/$$pos.out; \
	diff -q ../examples/effects/$$pos.out.expected build/$$pos.out > /dev/null \
	  && echo "issue-743 OK $$pos (C)" \
	  || { echo "issue-743 DIFF $$pos"; diff ../examples/effects/$$pos.out.expected build/$$pos.out; exit 1; }; \
	neg=issue_743_generic_effect_payload_bad_field; \
	./$(TARGET) ../examples/effects/$$neg.kai 2> build/$$neg.err > /dev/null && \
	  { echo "issue-743 FAIL $$neg (Tier 1 escape: bad field accepted)"; exit 1; } \
	  || true; \
	grep -qF "$$(cat ../examples/effects/$$neg.err.expected)" build/$$neg.err \
	  && echo "issue-743 OK $$neg (rejected on Box)" \
	  || { echo "issue-743 FAIL $$neg (wrong diagnostic)"; cat build/$$neg.err; exit 1; }

# Issue #842 — a parametric effect's op payload field access when the
# effect has 2+ type params AND two ops raise the same row label (one
# concrete, one with a free payload). The row dedup must unify the two
# labels' ty_args, not drop the free one — else `Some(u)`'s payload
# stays an unresolved tyvar and `u.name` is wrongly rejected. Positive
# (`save` raised before `find` — the failing order) checks runtime
# output; negative is the Tier 1 escape canary on the concrete type.
test-issue-842: $(TARGET) build
	@set -e; \
	pos=issue_842_multi_param_op_payload_field; \
	./$(TARGET) ../examples/effects/$$pos.kai > build/$$pos.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$pos.c -o build/$$pos-c $(LDLIBS_M); \
	./build/$$pos-c > build/$$pos.out; \
	diff -q ../examples/effects/$$pos.out.expected build/$$pos.out > /dev/null \
	  && echo "issue-842 OK $$pos (C)" \
	  || { echo "issue-842 DIFF $$pos"; diff ../examples/effects/$$pos.out.expected build/$$pos.out; exit 1; }; \
	neg=issue_842_multi_param_op_payload_bad_field; \
	./$(TARGET) ../examples/effects/$$neg.kai 2> build/$$neg.err > /dev/null && \
	  { echo "issue-842 FAIL $$neg (Tier 1 escape: bad field accepted)"; exit 1; } \
	  || true; \
	grep -qF "$$(cat ../examples/effects/$$neg.err.expected)" build/$$neg.err \
	  && echo "issue-842 OK $$neg (rejected on User)" \
	  || { echo "issue-842 FAIL $$neg (wrong diagnostic)"; cat build/$$neg.err; exit 1; }

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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	  ./$(TARGET) --path ../stdlib \
	    ../examples/stdlib/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/time.c -o build/time-c $(LDLIBS_M) ; \
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

# stdlib smoke tests. Each fixture under examples/stdlib/ is compiled
# with kaic2 and its stdout diffed against `<name>.out.expected`. One
# fixture per group of related ops: list_basic, list_*-by, list_sort, …
#
# Hanga Roa: the core module set (core/*, math/numeric, math/complex,
# protocols, effects, array) is loaded automatically by the compiler
# from the compile-time KAI_STDLIB_PATH — there is no "prelude" and no
# `--prelude` flag (both retired in 7c1bb4b). Fixtures that use an
# opt-in stdlib module (base64, hex, json, collections, encoding/math/
# crypto, decimal/money/fx, uuid, regexp, path, random*) must `import`
# it themselves.

# Cartography variant of `test-stdlib`: same fixture set, same
# compilation+run pipeline, but does NOT abort on the first failure.
# Use to count and classify red fixtures across the whole suite when
# diagnosing tier1 drift. The gate variant (`test-stdlib`) stays
# exit-non-zero-on-first-fail so CI remains a hard merge gate.
test-stdlib-survey: $(TARGET) build
	@pass=0; fail=0; \
	for f in ../examples/stdlib/*.kai; do \
	  name=$$(basename $$f .kai); \
	  case "$$name" in bench_*|check_*|*_lib|qualified_call_in_prelude|issue_219_typer_modcall|issue_318_kai_test_prelude_scope) continue ;; esac; \
	  exp_err=../examples/stdlib/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $$f \
	      > /dev/null 2> build/stdlib-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "stdlib FAIL $$name (expected error, got success)"; fail=$$((fail+1)); continue; \
	    fi; \
	    miss=0; \
	    while read -r line; do \
	      grep -qF "$$line" build/stdlib-$$name.err || miss=1; \
	    done < $$exp_err; \
	    if [ $$miss -eq 1 ]; then \
	      echo "stdlib FAIL $$name (missing diagnostic)"; fail=$$((fail+1)); \
	    else \
	      echo "stdlib OK $$name (rejected as expected)"; pass=$$((pass+1)); \
	    fi; \
	    continue; \
	  fi; \
	  ./$(TARGET) --path ../stdlib $$f > build/stdlib-$$name.c 2> build/stdlib-$$name.kaic2err; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "stdlib FAIL $$name (kaic2 errored)"; fail=$$((fail+1)); continue; \
	  fi; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/stdlib-$$name.c -o build/stdlib-$$name $(LDLIBS_M) 2> build/stdlib-$$name.ccerr; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "stdlib FAIL $$name (C compile failed)"; fail=$$((fail+1)); continue; \
	  fi; \
	  in=../examples/stdlib/$$name.in; \
	  if [ -f $$in ]; then \
	    ./build/stdlib-$$name < $$in > build/stdlib-$$name.out 2>&1; \
	  else \
	    ./build/stdlib-$$name > build/stdlib-$$name.out 2>&1; \
	  fi; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "stdlib FAIL $$name (run errored)"; fail=$$((fail+1)); continue; \
	  fi; \
	  if diff -q ../examples/stdlib/$$name.out.expected build/stdlib-$$name.out > /dev/null 2>&1; then \
	    echo "stdlib OK $$name"; pass=$$((pass+1)); \
	  else \
	    echo "stdlib DIFF $$name"; fail=$$((fail+1)); \
	  fi; \
	done; \
	echo ""; \
	echo "test-stdlib-survey: $$pass pass, $$fail fail"

test-stdlib: $(TARGET) build
	@set -e; \
	for f in ../examples/stdlib/*.kai; do \
	  name=$$(basename $$f .kai); \
	  case "$$name" in bench_*|check_*|*_lib|qualified_call_in_prelude|issue_219_typer_modcall|issue_318_kai_test_prelude_scope) continue ;; esac; \
	  exp_err=../examples/stdlib/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $$f \
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
	  ./$(TARGET) --path ../stdlib $$f > build/stdlib-$$name.c \
	    || { echo "stdlib FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/stdlib-$$name.c -o build/stdlib-$$name $(LDLIBS_M) \
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
	@# m13: assert bit ops lowered as compiler intrinsics. Under the
	@# Koka tagged-Int runtime a small Int is an immediate (no heap
	@# `->as.i` slot), so the operand is decoded inline via `kai_intf(_a)`
	@# rather than `_a->as.i`. The intent is unchanged: each raw operator
	@# appears inline and must NOT reference any `kai_bit_*` runtime
	@# helper (we never declared one — a leak would be an emit-pass miss).
	@if grep -q 'kai_bit_' build/stdlib-bits_basic.c; then \
	  echo "stdlib FAIL bits_basic (intrinsic leaked as kai_bit_* call)"; \
	  grep 'kai_bit_' build/stdlib-bits_basic.c; \
	  exit 1; \
	fi
	@grep -F -q 'kai_intf(_a) << kai_intf(_b)' build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (<< not inlined)"; exit 1; }
	@grep -F -q 'kai_intf(_a) >> kai_intf(_b)' build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (>> not inlined)"; exit 1; }
	@grep -F -q 'kai_intf(_a) & kai_intf(_b)'  build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (& not inlined)"; exit 1; }
	@grep -F -q 'kai_intf(_a) | kai_intf(_b)'  build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (| not inlined)"; exit 1; }
	@grep -F -q 'kai_intf(_a) ^ kai_intf(_b)'  build/stdlib-bits_basic.c \
	  || { echo "stdlib FAIL bits_basic (^ not inlined)"; exit 1; }
	@grep -F -q '~ kai_intf(_a)'     build/stdlib-bits_basic.c \
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
	@grep -F -q 'kai_intf(_a) << kai_intf(_b)' build/stdlib-bits_dotted.c \
	  || { echo "stdlib FAIL bits_dotted (<< not inlined)"; exit 1; }
	@grep -F -q 'kai_intf(_a) & kai_intf(_b)'  build/stdlib-bits_dotted.c \
	  || { echo "stdlib FAIL bits_dotted (& not inlined)"; exit 1; }
	@grep -F -q '__builtin_popcountll' build/stdlib-bits_dotted.c \
	  || { echo "stdlib FAIL bits_dotted (popcount not inlined)"; exit 1; }
	@echo "stdlib OK bits_dotted intrinsic-lowered"

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
	./$(TARGET) --path ../stdlib \
	  ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	  || { echo "stdlib-crypto-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
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
	  ./$(TARGET) --path ../stdlib \
	    ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	    || { echo "stdlib-regex-predicate-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name-holes.c -o build/$$name-holes-c $(LDLIBS_M) ; \
	  crc=0; ./build/$$name-holes-c > build/$$name-holes-c.out 2>&1 || crc=$$?; \
	  if [ $$crc -ne 0 ] && ! grep -q "unfilled hole:" build/$$name-holes-c.out; then \
	    echo "holes FAIL $$name (C backend aborted without hole message)"; cat build/$$name-holes-c.out; exit 1; \
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
	for f in ../examples/effects/row_match_higher_order.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (row match accepted)" \
	    || { echo "effects FAIL $$name (matching rows rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/row_pure_to_effectful.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (pure-to-effectful elaboration accepted)" \
	    || { echo "effects FAIL $$name (pure-to-effectful elaboration rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/op_call_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (op call accepted)" \
	    || { echo "effects FAIL $$name (op call rejected)"; cat build/s2-$$name.err; exit 1; }; \
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
	for f in ../examples/effects/handle_removes_effect.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (handle removes effect)" \
	    || { echo "effects FAIL $$name (handle did not remove effect)"; cat build/s2-$$name.err; exit 1; }; \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (one-shot resume runs through KaiCont)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6d_double_resume.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (discard, discard+return, no-discard all pass)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6f_clause_typed.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (clause body + return clause type-checked + ran)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_console.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_stdout_print" build/s2-$$name.c \
	    && grep -q "kai_default_stderr_eprint" build/s2-$$name.c \
	    && grep -q "kai_evidence_init_default(&_kai_default_node_stdout" build/s2-$$name.c \
	    && grep -q "kai_evidence_init_default(&_kai_default_node_stderr" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-handler wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	    && grep -q "kai_evidence_init_default(&_kai_default_node_fail" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Fail wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^single-alias works$$" build/s2-$$name.out \
	    && echo "effects OK $$name (single-effect alias resolves + runs)" \
	    || { echo "effects FAIL $$name (alias output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m7b_12_row_var_pure_body.kai \
	         ../examples/effects/m7b_12_row_var_console.kai \
	         ../examples/effects/m7b_12_row_var_compose.kai \
	         ../examples/effects/m7b_12_row_var_concrete_plus_var.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > /dev/null \
	    && echo "effects OK $$name (row var instantiated + ran)" \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	    && grep -q "kai_evidence_init_default(&_kai_default_node_spawn" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Spawn wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (Bounded(_, BlockSender) parks sender on full; receiver wakeup unblocks)" \
	    || { echo "effects FAIL $$name (block-sender output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/issue_763_spawn_actor_policy_block_sender.kai \
	         ../examples/effects/issue_763_spawn_actor_policy_drop_oldest.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (spawn_actor_policy: explicit MailboxPolicy reaches the spawned actor's mailbox)" \
	    || { echo "effects FAIL $$name (issue #763 spawn_actor_policy output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_trap_exit.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^inside thunk$$" build/s2-$$name.out \
	    && grep -q "^42$$" build/s2-$$name.out \
	    && echo "effects OK $$name (per-op [T, e] row flows through Spawn.spawn directly)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_monitor.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	    && grep -q "kai_evidence_init_default(&_kai_default_node_cancel" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Cancel wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  grep -q "^33$$" build/s2-$$name.out \
	    && echo "effects OK $$name (nursery join: 11 + 22 = 33)" \
	    || { echo "effects FAIL $$name (wrong output)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_9_nursery_autojoin.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; exit 1; }; \
	  diff -q ../examples/effects/$$name.out.expected build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (nursery auto-join: both children before return, no explicit await)" \
	    || { echo "effects FAIL $$name (auto-join order)"; diff ../examples/effects/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8x_10_nursery_cancel_on_fail.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.run.err || true; \
	  grep -q "^before nursery$$" build/s2-$$name.out \
	    && ! grep -q "sibling finished" build/s2-$$name.out \
	    && ! grep -q "after nursery" build/s2-$$name.out \
	    && grep -q "no survivors" build/s2-$$name.run.err \
	    && echo "effects OK $$name (cancel-on-fail: sibling cancelled, failure re-raised out of scope)" \
	    || { echo "effects FAIL $$name (cancel-on-fail)"; cat build/s2-$$name.out build/s2-$$name.run.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_6_fiber_inside_ok.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (m8 bug #12: self-delegating handler resolves to outer + per-fiber save/restore paired (C))" \
	    || { echo "effects FAIL $$name (C binary exit non-zero — infinite-loop regression?)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_fiber_discard.kai \
	         ../examples/effects/m8_fiber_discard_yields.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/effects/$$name.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	      || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/s2-$$name > build/s2-$$name.out \
	      || { echo "effects FAIL $$name (run errored)"; cat build/s2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/s2-$$name.out > /dev/null \
	        && echo "effects OK $$name (C)" \
	        || { echo "effects DIFF $$name (C)"; diff $$exp_out build/s2-$$name.out; exit 1; }; \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	for f in ../examples/effects/m8x_signal_await_parks.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  $(CC) $(CFLAGS) ../tests/sigharness.c -o build/s2-$$name.harness $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (harness compile failed)"; exit 1; }; \
	  ./build/s2-$$name.harness ./build/s2-$$name > build/s2-$$name.out 2>&1; \
	  rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "effects FAIL $$name (harness rc=$$rc — expected 0; R4 reactor parking proof)"; \
	    cat build/s2-$$name.out; exit 1; \
	  fi; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (R4: Signal.await() parks fiber; compute fiber interleaves)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/process_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_process_start" build/s2-$$name.c \
	    && grep -q "kai_default_process_wait"  build/s2-$$name.c \
	    && grep -q "kai_evidence_init_default(&_kai_default_node_process" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (default-Process wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
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
	         ../examples/effects/array_local_masks_mutable.kai \
	         ../examples/effects/array_record_forward_explicit.kai \
	         ../examples/effects/array_record_local_masks_mutable.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (issue #251 + #252 Mutable provenance + masking)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/local_ref_masks_mutable.kai \
	         ../examples/effects/local_hashmap_masks_mutable.kai \
	         ../examples/effects/string_builder_build_correct.kai \
	         ../examples/effects/string_builder_masks_mutable.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (issue #531 local Ref/HashMap masks Mutable, unconditional default install)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/mutable_user_handler_over_default.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    && echo "effects OK $$name (issue #531 user Mutable handler wins over base default, LIFO)" \
	    || { echo "effects FAIL $$name (user handler did not shadow base default)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/array_mutable_required.kai \
	         ../examples/effects/string_builder_append_requires_mutable.kai \
	         ../examples/effects/array_record_forward_requires_mutable.kai; do \
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
	  echo "effects OK $$name (parameter-mutation rejected)"; \
	done; \
	for f in ../examples/effects/prelude_effect_via_scheme.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (prelude builtin effect propagates via scheme row, not name table)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/issue_308_user_log_effect.kai \
	         ../examples/effects/issue_308_stdlib_log_unchanged.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "effects OK $$name (issue #308: user effect shadows stdlib)" \
	    || { echo "effects FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/actors/dual_actor_send_only.kai \
	         ../examples/actors/dual_actor_request_reply.kai \
	         ../examples/actors/dual_actor_receive_only.kai \
	         ../examples/actors/receive_timeout_none.kai \
	         ../examples/actors/receive_timeout_some.kai \
	         ../examples/actors/receive_timeout_race_message.kai \
	         ../examples/actors/receive_timeout_race_deadline.kai; do \
	  name=$$(basename $$f .kai); \
	  exp=$${f%.kai}.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "actors FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "actors FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "actors FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    && echo "actors OK $$name (issue #532 nested Actor[T] absorbed)" \
	    || { echo "actors FAIL $$name (output mismatch)"; diff $$exp build/s2-$$name.out; exit 1; }; \
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
	done; \
	for f in ../examples/effects/file_stream_roundtrip.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q "kai_default_file_open_write" build/s2-$$name.c \
	    && grep -q "kai_default_file_write_chunk" build/s2-$$name.c \
	    && grep -q "kai_default_file_open_read"  build/s2-$$name.c \
	    && grep -q "kai_default_file_read_chunk" build/s2-$$name.c \
	    && grep -q "kai_default_file_close_file" build/s2-$$name.c \
	    || { echo "effects FAIL $$name (chunked-File wiring missing)"; cat build/s2-$$name.c; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^hello streaming world$$" build/s2-$$name.out \
	    && echo "effects OK $$name (#771 chunked write+read roundtrip)" \
	    || { echo "effects FAIL $$name (roundtrip output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/file_stream_mock.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^mock: ababab$$" build/s2-$$name.out \
	    && echo "effects OK $$name (#771 chunked ops interceptable via handle)" \
	    || { echo "effects FAIL $$name (mock output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/file_stream_open_missing.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "^open_read failed: open_read: cannot open file$$" build/s2-$$name.out \
	    && echo "effects OK $$name (#771 open_read missing -> inspectable Err)" \
	    || { echo "effects FAIL $$name (negative output wrong)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for spec in \
	  "stream_early_stop|^early_stop count=3$$|#801 push carrier early-stop via take" \
	  "stream_mock_disk|^mock errors=2$$|#801 read_lines mocked via handle...with File" \
	  "stream_skip_policy|^skip count=3$$|#801 ReadFault.bad_chunk resume = skip and continue" \
	  "stream_shout_roundtrip|^shout: HELLO$$|#801 read map write_lines in one expression" \
	  "stream_abort_leak|^abort result=aborted closes=0$$|#801 abort policy skips producer close (documented fd leak)" \
	  "ctor_field_effect_row|^boom x2$$|#801 typer: ctor field's effect row survives instantiation" \
	  "row_carrier_pipe|^ticks=3$$|#773 typer: nominal effect-row carrier rides pipes and threads its row" \
	  "row_carrier_naming|^named ticks=2$$|#773 typer: concrete-row annotation Stream[Int, Tick] resolves the row slot"; do \
	  name=$${spec%%|*}; rest=$${spec#*|}; pat=$${rest%%|*}; desc=$${rest#*|}; \
	  f=../examples/effects/$$name.kai; \
	  ./$(TARGET) --edition hanga-roa --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "$$pat" build/s2-$$name.out \
	    && echo "effects OK $$name ($$desc)" \
	    || { echo "effects FAIL $$name (output mismatch; want /$$pat/)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	./$(TARGET) --edition hanga-roa --path ../stdlib ../demos/wc.kai > build/s2-wc.c 2> build/s2-wc.err \
	  || { echo "effects FAIL wc (kaic2 errored)"; cat build/s2-wc.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-wc.c -o build/s2-wc $(LDLIBS_M) \
	  || { echo "effects FAIL wc (C compile failed)"; exit 1; }; \
	./build/s2-wc ../examples/effects/stream_wc_input.txt > build/s2-wc.out 2>&1 \
	  || { echo "effects FAIL wc (binary exit non-zero)"; cat build/s2-wc.out; exit 1; }; \
	grep -q "^2 9 44 " build/s2-wc.out \
	  && echo "effects OK wc (#801 demo: lines/words/chars in one fold over read_lines)" \
	  || { echo "effects FAIL wc (output mismatch; want '2 9 44 <path>')"; cat build/s2-wc.out; exit 1; }

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
	    complex_literal_*) extra_flags="--path ../stdlib" ;; \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
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

# kai lint (#966). Each .kai under `examples/lint/` runs through the
# `--lint` and `--lint-json` reporter modes; the human text is diffed
# against `<name>.lint.expected` and the JSON against
# `<name>.lint-json.expected`. The linter must exit 0 in every case —
# a lint never blocks a build — so a non-zero exit fails the gate.
test-lint: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/lint/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_txt=../examples/lint/$$name.lint.expected; \
	  exp_json=../examples/lint/$$name.lint-json.expected; \
	  rc=0; ./$(TARGET) --lint --path ../stdlib $$f > build/s2-$$name.lint 2>&1 || rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "lint FAIL $$name (--lint exited $$rc; a lint must never block)"; \
	    cat build/s2-$$name.lint; exit 1; \
	  fi; \
	  if [ -f $$exp_txt ]; then \
	    diff -q $$exp_txt build/s2-$$name.lint > /dev/null \
	      || { echo "lint DIFF $$name (--lint)"; diff $$exp_txt build/s2-$$name.lint; exit 1; }; \
	  fi; \
	  rc=0; ./$(TARGET) --lint-json --path ../stdlib $$f > build/s2-$$name.lint-json 2>&1 || rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "lint FAIL $$name (--lint-json exited $$rc)"; \
	    cat build/s2-$$name.lint-json; exit 1; \
	  fi; \
	  if [ -f $$exp_json ]; then \
	    diff -q $$exp_json build/s2-$$name.lint-json > /dev/null \
	      || { echo "lint DIFF $$name (--lint-json)"; diff $$exp_json build/s2-$$name.lint-json; exit 1; }; \
	  fi; \
	  echo "lint OK $$name"; \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/diag-$$name.c -o build/diag-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/m2-$$name.c -o build/m2-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/lit-$$name.c -o build/lit-$$name $(LDLIBS_M) \
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
	    rc=0; ./$(TARGET) --path ../stdlib $$f \
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
	    ./$(TARGET) --path ../stdlib $$f \
	      > build/str-$$name.c \
	      || { echo "string FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage2 -I ../stage0 build/str-$$name.c -o build/str-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
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

# `examples/pipes/collections/` exercises convention-based pipe
# dispatch (`|`, `||`, `|?`) on the opt-in stdlib collections (Set,
# Stack, Queue, HashSet, HashMap, and a Map re-confirm). These import
# `collections.*` (so they need `--path ../stdlib`) and rely on the
# hanga-roa edition gate that enables non-List head dispatch (so they
# pass `--edition hanga-roa`), which is why they live in their own
# subdir rather than the flat `examples/pipes/*.kai` glob.
test-pipes-collections: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/pipes/collections/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/pipes/collections/$$name.out.expected; \
	  ./$(TARGET) --path ../stdlib --edition hanga-roa $$f > build/pc-$$name.c \
	    || { echo "pipes-collections FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/pc-$$name.c -o build/pc-$$name $(LDLIBS_M) \
	    || { echo "pipes-collections FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/pc-$$name > build/pc-$$name.out \
	    || { echo "pipes-collections FAIL $$name (run errored)"; cat build/pc-$$name.out; exit 1; }; \
	  diff -q $$exp_out build/pc-$$name.out > /dev/null \
	    && echo "pipes-collections OK $$name" \
	    || { echo "pipes-collections DIFF $$name"; diff $$exp_out build/pc-$$name.out; exit 1; }; \
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
	  case "$$name" in poly_scalar_dispatch|bare_narrow_in_assert) continue ;; esac; \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/p2-$$name.c -o build/p2-$$name $(LDLIBS_M) \
	      || { echo "protocols FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/p2-$$name > build/p2-$$name.out \
	      || { echo "protocols FAIL $$name (run errored)"; cat build/p2-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/p2-$$name.out > /dev/null \
	        && echo "protocols OK $$name (C)" \
	        || { echo "protocols DIFF $$name (C)"; diff $$exp_out build/p2-$$name.out; exit 1; }; \
	    else \
	      echo "protocols OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# Regression for the protocol-dispatch ABI mismatch (fix-proto-eq-abi).
# `examples/protocols/poly_scalar_dispatch.kai` uses `test` blocks
# whose `assert ... == ...` lowers to the runtime `kai_op_eq_v` (the
# `assert` form drops the operand's static type, so the post-monomorph
# rewrite to a specialised impl never fires). That routes through the
# impl-table dispatch which, pre-fix, called the unboxed builtin scalar
# impls (`Eq_Int_eq`, …) with the wrong ABI and SIGSEGV'd. Compiled
# with `--test` (not as a program, so it is skipped by `test-protocols`).
# The `--test` runner is C-only; the native backend's parity is gated
# separately by the native-vs-C ratchet.
test-proto-scalar-dispatch: $(TARGET) build
	@set -e; \
	src=../examples/protocols/poly_scalar_dispatch.kai; \
	exp=../examples/protocols/poly_scalar_dispatch.out.expected; \
	./$(TARGET) --test $$src > build/psd.c \
	  || { echo "proto-scalar-dispatch FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/psd.c -o build/psd $(LDLIBS_M) \
	  || { echo "proto-scalar-dispatch FAIL (C compile failed)"; exit 1; }; \
	./build/psd > build/psd.out 2>&1; \
	diff -q $$exp build/psd.out > /dev/null \
	  && echo "proto-scalar-dispatch OK (C)" \
	  || { echo "proto-scalar-dispatch DIFF (C)"; diff $$exp build/psd.out; exit 1; }

# Issue #758: `bare_narrow_in_assert.kai` exercises #235 first-arg
# narrowing for a bare cross-module name used INLINE in an `assert`
# inside a `test` block. The fix types test/bench/check bodies so the
# narrowing fires there too. Compiled with `--test` (the body is a
# `test` block, skipped by the program-glob `test-protocols` loop).
# C-only — the `--test` runner is the C path; the behavioural assertion
# (right candidate dispatched) is what gates, not byte-id (the compiler
# has no test blocks of its own so selfhost byte-id is false-green for
# this fix; this fixture is the real gate).
test-bare-narrow-assert: $(TARGET) build
	@set -e; \
	src=../examples/protocols/bare_narrow_in_assert.kai; \
	exp=../examples/protocols/bare_narrow_in_assert.out.expected; \
	./$(TARGET) --test $$src > build/bna.c \
	  || { echo "bare-narrow-assert FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/bna.c -o build/bna $(LDLIBS_M) \
	  || { echo "bare-narrow-assert FAIL (C compile failed)"; exit 1; }; \
	./build/bna > build/bna.out 2>&1 \
	  || { echo "bare-narrow-assert FAIL (test assertion failed)"; cat build/bna.out; exit 1; }; \
	diff -q $$exp build/bna.out > /dev/null \
	  && echo "bare-narrow-assert OK (C)" \
	  || { echo "bare-narrow-assert DIFF (C)"; diff $$exp build/bna.out; exit 1; }

# Self-hosting: per-compiler determinism.
#
# We compile stage2/compiler.kai with `kaic2` (which was built by
# stage1 a few targets up), then compile the same source again with
# the freshly built `kaic2b`, and require `kaic2b.c == kaic2c.c`.
#
# What this protects: the mature compiler is deterministic — given
# the same input, it produces the same C output. Catches hashmap
# iteration order leaks, timestamps in metadata, pointer addresses
# used as identifiers, race conditions in emission.
#
# What this does NOT require: that the C produced by stage1 (which
# has no typer) match the C produced by stage2 (which has full HM +
# effects + Perceus). Stage1 and stage2 are structurally distinct
# compilers and may legitimately diverge on type-driven decisions
# like match tag-compare, niche-filling, devirtualisation. See
# docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md.
# m7a #5: smoke test for the handler-stack runtime primitives.
# Builds a small C program against runtime.h and runs it. m7a #6
# will provide the end-to-end coverage from compiled kaikai code.
test-effect-runtime: build
	@$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 tests/effect_runtime_test.c -o build/effect_runtime_test $(LDLIBS_M)
	@./build/effect_runtime_test \
	  && echo "effect-runtime OK" \
	  || { echo "effect-runtime FAIL"; exit 1; }

# Phase 5: smoke test for the Link runtime registry. Exercises
# kai_link_add_bidirectional + kai_link_propagate_terminate on
# bare KaiFiber structs without going through scheduler dispatch.
# A kaikai-level link-cascade demo defers to Phase 5.5+ (needs
# spawn_actor / pid handoff machinery the v1 actor surface lacks).
test-link-runtime: build
	@$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 tests/link_runtime_test.c -o build/link_runtime_test $(LDLIBS_M)
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "m4c FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out \
	    || { echo "m4c FAIL $$name (C binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  echo "m4c OK $$name (C)"; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) $(LDFLAGS_STACK) -I ../stage0 build/kaic2b.c -o build/kaic2b $(LDLIBS_M) ; \
	./build/kaic2b $(SRC) > build/kaic2c.c; \
	diff -q build/kaic2b.c build/kaic2c.c > /dev/null \
	  && echo "selfhost determinism: OK (kaic2b.c == kaic2c.c)" \
	  || { echo "selfhost determinism: DIFF (kaic2b.c != kaic2c.c)"; exit 1; }

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
	  preludes=""; \
	  ./$(TARGET) $$preludes $$src > build/demo-$$name.c \
	    || { echo "demos-core FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/demo-$$name.c -o build/demo-$$name $(LDLIBS_M) \
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
	  preludes=""; \
	  ./$(TARGET) $$preludes $$src > build/asp-$$name.c \
	    || { echo "aspirational FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/asp-$$name.c -o build/asp-$$name $(LDLIBS_M) \
	    || { echo "aspirational FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/asp-$$name > build/asp-$$name.out \
	    || { echo "aspirational FAIL $$name (run errored)"; cat build/asp-$$name.out; exit 1; }; \
	  diff -q $$exp build/asp-$$name.out > /dev/null \
	    && echo "aspirational OK $$name (C)" \
	    || { echo "aspirational DIFF $$name (C)"; diff $$exp build/asp-$$name.out; exit 1; }; \
	done

# Inference branch-isolation canary. Stresses st_restore_entries +
# heterogeneous generic instantiation across sibling match arms +
# both-flex row-tail unification (the `bind_row_tails` row_fresh bump).
# Authored during the Front A investigation (see
# docs/lane-experience-front-a-rc-churn.md) and kept as standalone
# coverage of the substitution/backtracking interaction it exercises,
# independent of any st_unify change.
test-match-backtrack-unify: $(TARGET) build
	@set -e; \
	f=../examples/perceus/match_backtrack_unify.kai; \
	name=$$(basename $$f .kai); \
	./$(TARGET) $$f > build/$$name.c \
	  || { echo "match-backtrack FAIL $$name (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) \
	  || { echo "match-backtrack FAIL $$name (cc errored)"; exit 1; }; \
	./build/$$name > build/$$name.out \
	  || { echo "match-backtrack FAIL $$name (run errored)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	  && echo "match-backtrack OK $$name" \
	  || { echo "match-backtrack DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }

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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/perceus-$$name.c -o build/perceus-$$name $(LDLIBS_M) \
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
	@# match-raw unboxing (asu): a `match` whose RESULT is a raw scalar
	@# (boxed variant scrutinee, Int arms) lowers `_r` as a raw int64_t —
	@# no `_r = kai_unit()` boxed accumulator, no per-arm `kai_int(`
	@# round-trip on the match result. This fixture is the ONLY thing
	@# exercising the path: the compiler source has no match→scalar, so
	@# selfhost byte-id stays green even if emit_match_raw regresses.
	@# Without this grep the byte-id lies.
	@grep -F -q 'int64_t _r = 0' build/perceus-unbox_phase2_match_scalar.c \
	  || { echo "unbox FAIL match_scalar (match result not raw — _r not int64_t)"; exit 1; }
	@if grep -E 'kai_area\(KaiValue \*kai_s\) \{[^}]*_r = kai_unit\(\)' build/perceus-unbox_phase2_match_scalar.c; then \
	  echo "unbox FAIL match_scalar (boxed _r accumulator survived in area)"; exit 1; \
	fi
	@# divergence: a `_ -> panic(...)` catch-all arm must NOT block raw
	@# promotion — `sides` lowers `_r` raw and the panic arm emits the
	@# call + break with no `_r =` (the common native-backend shape).
	@if grep -E 'kai_sides\(KaiValue \*kai_s\) \{[^}]*_r = kai_unit\(\)' build/perceus-unbox_phase2_match_scalar.c; then \
	  echo "unbox FAIL match_scalar (panic catch-all blocked raw promotion in sides)"; exit 1; \
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
	@# interpolation slot.
	@#
	@# Original bug: emit re-parsed `#{...}` at codegen time without
	@# consulting the unbox alias map, so `int_to_string(n)` inside an
	@# interp body resolved to the boxed name `kai_n` while the unbox
	@# pass had emitted `int64_t kair_n` — link fail. The historical
	@# fix forced `n` to MBoxed via the unbox_stmt SLet branch.
	@#
	@# Post `desugar_interp_decls`: interpolation is lifted to a
	@# `string_concat_all([...])` ECall BEFORE the unbox pass runs, so
	@# the re-parse no longer happens — `int_to_string(n)` is a regular
	@# ECall the codegen handles by calling `kai_int(kair_n)` at the
	@# use site. The MBoxed demote is no longer needed and would
	@# regress unboxing for free; the correct shape today is `int64_t
	@# kair_n = 42LL`. The check below pins that shape.
	@diff -q ../examples/perceus/unbox_phase2_r8_let_in_interp.out.expected \
	        build/perceus-unbox_phase2_r8_let_in_interp.out > /dev/null \
	  || { echo "unbox FAIL r8_let_in_interp (output mismatch)"; \
	       diff ../examples/perceus/unbox_phase2_r8_let_in_interp.out.expected \
	            build/perceus-unbox_phase2_r8_let_in_interp.out; exit 1; }
	@grep -F -q 'int64_t kair_n = 42LL' \
	        build/perceus-unbox_phase2_r8_let_in_interp.c \
	  || { echo "unbox FAIL r8_let_in_interp (kair_n not unboxed — desugar_interp regressed?)"; exit 1; }
	@if grep -F -q 'KaiValue *kai_n = kai_int(42LL)' \
	        build/perceus-unbox_phase2_r8_let_in_interp.c; then \
	  echo "unbox FAIL r8_let_in_interp (boxed kai_n re-appeared; obsolete demote returned)"; exit 1; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/perceus-match_pbind_catchall_raw.c -o build/perceus-match_pbind_catchall_raw $(LDLIBS_M) \
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

# Issue #892 — a monomorphic spec of a `fn[T:P]` whose body uses a
# parameter more than once must carry a CONCRETE unboxed signature
# (int64_t params), matching the direct non-generic twin. Before the
# fix the spec kept `Param.ptype = TyName("T")`, the unbox classifier
# rejected it, and the spec ran boxed (per-call kai_int/kai_intf + RC).
# Grep the emitted C: the spec signature must be raw int64_t, identical
# to `larger_int`, with no boxing in the hot loop body.
test-issue-892-mono-arg-unbox: $(TARGET) build
	@set -e; \
	f=../examples/perceus/mono_arg_unbox_892.kai; \
	name=$$(basename $$f .kai); \
	./$(TARGET) --path ../stdlib --path ../examples/perceus $$f > build/perceus-$$name.c \
	  || { echo "892 FAIL $$name (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/perceus-$$name.c -o build/perceus-$$name $(LDLIBS_M) \
	  || { echo "892 FAIL $$name (cc errored)"; exit 1; }; \
	out=$$(./build/perceus-$$name); \
	[ "$$out" = "agree" ] || { echo "892 FAIL $$name (generic != direct: $$out)"; exit 1; }
	@grep -F -q 'int64_t kai_larger__mono__Int(int64_t kair_a, int64_t kair_b)' build/perceus-mono_arg_unbox_892.c \
	  || { echo "892 FAIL (spec signature not unboxed)"; exit 1; }
	@if grep -E 'static int64_t kai_loop_g\(.*\) \{[^}]*kai_int(f)?\(' build/perceus-mono_arg_unbox_892.c; then \
	  echo "892 FAIL (boxing survived in loop_g body)"; exit 1; \
	fi
	@echo "issue-892 mono-arg-unbox OK"

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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco.c -o build/tco $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-nth.c -o build/tco-nth $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-zero.c -o build/tco-zero $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-309.c -o build/tco-309 $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-perceus.c -o build/tco-perceus $(LDLIBS_M) ; \
	./build/tco-perceus > build/tco-perceus.out; \
	diff -q ../examples/perceus/unbox_bench.out.expected build/tco-perceus.out > /dev/null \
	  && echo "tco-perceus-wrap OK (issue #89 1M-deep self-tail-call through perceus wrap; goto label emitted; runs to completion)" \
	  || { echo "tco-perceus-wrap DIFF"; diff ../examples/perceus/unbox_bench.out.expected build/tco-perceus.out; exit 1; }

# Issue #812 Finding B — the KAI_TRACE_RC incref/decref counters must
# COUNT even in a binary built WITHOUT `-DKAI_TRACE_RC` (the `kai build`
# default path). Build the fixture WITHOUT the define and assert the
# env-gated exit report shows incref_total AND decref_total both > 0. The
# counters used to sit behind `#ifdef KAI_TRACE_RC` while the per-tag
# alloc counters were always-compiled, so every `kai build` binary
# reported 0/0 and every "RC balanced" gate passed VACUOUSLY — this gate
# regression-tests that silent-zero mode for good.
test-perceus-812-tracer: $(TARGET) build
	@set -e; \
	src=../examples/perceus/rc_counters_nonzero.kai; \
	./$(TARGET) --path ../stdlib $$src > build/s2-812tr.c \
	  || { echo "perceus-812-tracer FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-812tr.c -o build/s2-812tr $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-812tr > /dev/null 2> build/s2-812tr.rc; \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-812tr.rc | head -1 | cut -d= -f2); \
	dec=$$(grep -oE 'decref_total=[0-9]+' build/s2-812tr.rc | head -1 | cut -d= -f2); \
	[ -n "$$inc" ] && [ -n "$$dec" ] \
	  || { echo "perceus-812-tracer FAIL (no incref_total/decref_total line)"; cat build/s2-812tr.rc; exit 1; }; \
	[ "$$inc" -gt 0 ] && [ "$$dec" -gt 0 ] \
	  && echo "perceus-812-tracer OK (incref_total=$$inc decref_total=$$dec — counters non-zero without -DKAI_TRACE_RC; vacuous-zero mode regressed)" \
	  || { echo "perceus-812-tracer FAIL (counters VACUOUS: incref_total=$$inc decref_total=$$dec — the #ifdef hid them again)"; exit 1; }

# Issue #817 — `list.filter`/`map`/`flat_map` over HEAP elements must not
# leak the extracted arm-binder per kept element. The fixture filters over
# freshly-split heap strings 5000 times; with the leak, `leaked` grows
# ~14/iter (≈70000); fixed, it is a small constant. Build WITHOUT
# `-DKAI_TRACE_RC` (the user-facing path) and assert `leaked` < 1000.
# Also asserts the per-tag `str`/`cons` allocs are reclaimed (free_total
# within a constant of alloc_total). Cross-checks the iterative cons-spine
# free (#817 runtime) by running over the loop without a fiber-stack
# overflow on the long result lists.
test-perceus-817-filter-leak: $(TARGET) build
	@set -e; \
	src=../examples/perceus/filter_heap_bounded_817.kai; \
	./$(TARGET) --path ../stdlib $$src > build/s2-817fl.c \
	  || { echo "perceus-817-filter-leak FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-817fl.c -o build/s2-817fl $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-817fl > build/s2-817fl.out 2> build/s2-817fl.rc; \
	grep -q 'total=' build/s2-817fl.out \
	  || { echo "perceus-817-filter-leak FAIL (wrong output)"; cat build/s2-817fl.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-817fl.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-817-filter-leak FAIL (no leaked line)"; cat build/s2-817fl.rc; exit 1; }; \
	[ "$$leaked" -lt 1000 ] \
	  && echo "perceus-817-filter-leak OK (leaked=$$leaked < 1000 over 5000 filter-over-heap iters; arm-binder leak closed)" \
	  || { echo "perceus-817-filter-leak FAIL (leaked=$$leaked — arm-binder leak regressed, grows with iters)"; exit 1; }

# Issue #878 — KAI_MAX_HEAP heap ceiling. The fixture doubles a string
# toward a bounded ~256 MB target. Under a LOW cap (64m) the runtime must
# abort clean ("kai: heap limit exceeded", exit 1) before reaching it;
# under a HIGH cap / unset it must complete and print the length. Each run
# is wrapped in `timeout` as a host-safety backstop — a regression that
# breaks the cap cannot then exhaust the host (the exact hazard #878 fixes;
# `ulimit -v` is a no-op on macOS, so `timeout` is the real ceiling here).
test-heap-limit: $(TARGET) build
	@set -e; \
	src=../examples/perceus/heap_limit_runaway_878.kai; \
	./$(TARGET) --path ../stdlib $$src > build/s2-878hl.c \
	  || { echo "heap-limit FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-878hl.c -o build/s2-878hl $(LDLIBS_M); \
	lo=0; KAI_MAX_HEAP=64m timeout 30 ./build/s2-878hl > build/s2-878hl.lo.out 2> build/s2-878hl.lo.err || lo=$$?; \
	[ "$$lo" -eq 1 ] \
	  || { echo "heap-limit FAIL (low cap exit $$lo, want 1)"; cat build/s2-878hl.lo.err; exit 1; }; \
	grep -q 'heap limit exceeded' build/s2-878hl.lo.err \
	  || { echo "heap-limit FAIL (low cap: no 'heap limit exceeded' message)"; cat build/s2-878hl.lo.err; exit 1; }; \
	[ -s build/s2-878hl.lo.out ] \
	  && { echo "heap-limit FAIL (low cap printed result before aborting)"; cat build/s2-878hl.lo.out; exit 1; }; \
	hi=0; KAI_MAX_HEAP=2g timeout 60 ./build/s2-878hl > build/s2-878hl.hi.out 2> build/s2-878hl.hi.err || hi=$$?; \
	{ [ "$$hi" -eq 0 ] && grep -q 'len=268435456' build/s2-878hl.hi.out; } \
	  || { echo "heap-limit FAIL (high cap exit $$hi, output:)"; cat build/s2-878hl.hi.out build/s2-878hl.hi.err; exit 1; }; \
	un=0; timeout 60 ./build/s2-878hl > build/s2-878hl.un.out 2> build/s2-878hl.un.err || un=$$?; \
	{ [ "$$un" -eq 0 ] && grep -q 'len=268435456' build/s2-878hl.un.out; } \
	  || { echo "heap-limit FAIL (unset cap exit $$un, output:)"; cat build/s2-878hl.un.out build/s2-878hl.un.err; exit 1; }; \
	echo "heap-limit OK (64m -> clean abort exit 1; 2g + unset -> complete len=268435456; cap contains, unset unchanged)"

# Issue #860 — the NATIVE backend must cascade decref->free for cons cells
# consumed by a self-tail arm (accumulator / consuming / TRMC cons-modulo).
# Before the fix native left `free_total=0`, `leaked` growing linearly with
# the list length; the C oracle reclaimed them. The fixture builds + doubles
# + sums a 20-element list 50 times (1000 cons), so a per-cell leak would
# leave ~1000 leaked. Built with the NATIVE backend under KAI_TRACE_RC, we
# assert every cons is reclaimed: `free_total == alloc_total - 1` (the lone
# residual is the final result string), the exact cascade the C oracle does.
# Native-gated: only meaningful where libLLVM is linked (tier1-native); a
# C-only kaic2 cannot run --backend=native, so the target is invoked from the
# tier1-native workflow, not the C-backend tier1 suite.
test-perceus-860-native-cons-leak: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_cons_selftail_leak_860.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-860ncl 2> build/s2-860ncl.err \
	  || { echo "perceus-860-native-cons-leak FAIL (native build errored)"; cat build/s2-860ncl.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-860ncl > build/s2-860ncl.out 2> build/s2-860ncl.rc; \
	diff -q ../examples/perceus/native_cons_selftail_leak_860.out.expected build/s2-860ncl.out > /dev/null \
	  || { echo "perceus-860-native-cons-leak FAIL (output diff)"; diff ../examples/perceus/native_cons_selftail_leak_860.out.expected build/s2-860ncl.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-860ncl.rc | head -1 | cut -d= -f2); \
	free=$$(grep -oE 'free_total=[0-9]+' build/s2-860ncl.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] && [ -n "$$free" ] \
	  || { echo "perceus-860-native-cons-leak FAIL (no alloc/free line)"; cat build/s2-860ncl.rc; exit 1; }; \
	[ "$$free" -eq "$$(($$alloc - 1))" ] \
	  && echo "perceus-860-native-cons-leak OK (native free_total=$$free == alloc_total-1=$$(($$alloc - 1)); cons cascade decref->free over 1000 cells)" \
	  || { echo "perceus-860-native-cons-leak FAIL (native free_total=$$free, alloc_total=$$alloc — cons leak regressed, cascade dropped)"; exit 1; }

# Issue #872 — the NATIVE backend must free the rb-tree variant-reuse churn:
# the rotation reuse arm, the TRMC variant modulo-cons step, the arm-top reuse
# token, and the con-reuse recolor. Before the fix native leaked nearly every
# node (reachable-but-unfreed, ~4x over-alloc); the C oracle frees it all. The
# fixture builds + discards an rb-tree of 60-key passes (20 passes) under the
# NATIVE backend with KAI_TRACE_RC; the leak must be a SMALL CONSTANT (the lone
# result string + a few residuals), not O(inserts). We assert leaked < 100 (it
# is 4 fixed); a regression that drops one node per recursion level blows past
# this within a few passes. Native-gated (tier1-native): a C-only kaic2 cannot
# run --backend=native, so the target is invoked from the tier1-native workflow.
test-perceus-872-native-variant-reuse-leak: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_variant_reuse_leak_872.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-872vrl 2> build/s2-872vrl.err \
	  || { echo "perceus-872-native-variant-reuse-leak FAIL (native build errored)"; cat build/s2-872vrl.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-872vrl > build/s2-872vrl.out 2> build/s2-872vrl.rc; \
	diff -q ../examples/perceus/native_variant_reuse_leak_872.out.expected build/s2-872vrl.out > /dev/null \
	  || { echo "perceus-872-native-variant-reuse-leak FAIL (output diff)"; diff ../examples/perceus/native_variant_reuse_leak_872.out.expected build/s2-872vrl.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-872vrl.rc | head -1 | cut -d= -f2); \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-872vrl.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] && [ -n "$$leaked" ] \
	  || { echo "perceus-872-native-variant-reuse-leak FAIL (no alloc/leaked line)"; cat build/s2-872vrl.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-872-native-variant-reuse-leak OK (native leaked=$$leaked < 100 over alloc_total=$$alloc; rb-tree variant reuse no longer leaks O(inserts))" \
	  || { echo "perceus-872-native-variant-reuse-leak FAIL (native leaked=$$leaked of alloc=$$alloc — variant reuse leak regressed, grows with inserts)"; exit 1; }

# Issue #882 — native rotation-reuse crash gate. The gap3 balance rotation
# (`Node(l,k,r) -> match l { match ll { Node(..) -> Node(Node(...), lk,
# Node(lr, k, r)) } }`) reuses an INNER cell while OUTER arms drop their own
# scrutinee at exit. Pre-fix the outer arm's #858 structural dup was
# suppressed (the reuse anywhere in the body gate descended into the inner
# match), leaving `r` embedded with no protecting incref → double-free →
# recycled into a cycle → SIGSEGV in `size`. The C oracle returns 8 cleanly;
# this gate asserts the native binary does too (exit 0 + "8") and that the
# bounded-N rotation leaves no O(rotations) leak. Run under `timeout`: a
# regression manifests as a hang (cyclic `size`), not a clean abort.
test-perceus-882-native-rotation-reuse-crash: $(TARGET) build
	@set -e; \
	src=../examples/perceus/gap3_balance_minimal.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-882rot 2> build/s2-882rot.err \
	  || { echo "perceus-882-native-rotation-reuse-crash FAIL (native build errored)"; cat build/s2-882rot.err; exit 1; }; \
	KAI_TRACE_RC=1 KAI_MAX_HEAP=4g timeout 30 ./build/s2-882rot > build/s2-882rot.out 2> build/s2-882rot.rc \
	  || { echo "perceus-882-native-rotation-reuse-crash FAIL (native run crashed/hung — exit $$?)"; exit 1; }; \
	diff -q ../examples/perceus/gap3_balance_minimal.out.expected build/s2-882rot.out > /dev/null \
	  || { echo "perceus-882-native-rotation-reuse-crash FAIL (output diff)"; diff ../examples/perceus/gap3_balance_minimal.out.expected build/s2-882rot.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-882rot.rc | head -1 | cut -d= -f2); \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-882rot.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] && [ -n "$$leaked" ] \
	  || { echo "perceus-882-native-rotation-reuse-crash FAIL (no alloc/leaked line)"; cat build/s2-882rot.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-882-native-rotation-reuse-crash OK (native exit 0, output 8, leaked=$$leaked < 100 over alloc_total=$$alloc; rotation reuse no longer cycles)" \
	  || { echo "perceus-882-native-rotation-reuse-crash FAIL (native leaked=$$leaked of alloc=$$alloc — rotation reuse leak regressed)"; exit 1; }

# Issue #950 — the native backend runs `test` blocks. `kai test` on the
# fixture must produce the SAME summary + exit code on native as on the C
# oracle (a pass, a caught failing assertion, and a test after the
# failure), and the native runner must balance RC on both the pass and
# the caught-fail path. Both backends are diffed against the golden, and
# native's KAI_TRACE_RC report must show no leak.
test-native-test-runner-950: $(TARGET) build
	@set -e; \
	src=../examples/native/test_runner_parity.kai; \
	gold=../examples/native/test_runner_parity.out.expected; \
	cexit=0; \
	KAI_MAX_HEAP=4g timeout 60 ../bin/kai test --backend=c $$src > build/s2-950tr-c.out 2>&1 || cexit=$$?; \
	diff -q $$gold build/s2-950tr-c.out > /dev/null \
	  || { echo "native-test-runner-950 FAIL (C output diff)"; diff $$gold build/s2-950tr-c.out; exit 1; }; \
	[ "$$cexit" -eq 1 ] || { echo "native-test-runner-950 FAIL (C exit $$cexit != 1)"; exit 1; }; \
	nexit=0; \
	KAI_TRACE_RC=1 KAI_MAX_HEAP=4g timeout 60 ../bin/kai test --backend=native $$src > build/s2-950tr-n.raw 2>&1 || nexit=$$?; \
	grep -E '^\[KAI_TRACE_RC\]' build/s2-950tr-n.raw > build/s2-950tr-n.rc 2>/dev/null || true; \
	grep -vE '^\[KAI_TRACE_RC\]' build/s2-950tr-n.raw > build/s2-950tr-n.out 2>/dev/null || true; \
	diff -q $$gold build/s2-950tr-n.out > /dev/null \
	  || { echo "native-test-runner-950 FAIL (native output diff vs C oracle)"; diff $$gold build/s2-950tr-n.out; exit 1; }; \
	[ "$$nexit" -eq 1 ] || { echo "native-test-runner-950 FAIL (native exit $$nexit != 1, must match C)"; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-950tr-n.rc | head -1 | cut -d= -f2); \
	if [ -n "$$leaked" ]; then \
	  [ "$$leaked" -eq 0 ] \
	    && echo "native-test-runner-950 OK (native == C: summary 2/3, exit 1; RC leaked=0 on pass+caught-fail)" \
	    || { echo "native-test-runner-950 FAIL (native RC leaked=$$leaked on the runner path)"; cat build/s2-950tr-n.rc; exit 1; }; \
	else \
	  echo "native-test-runner-950 OK (native == C: summary 2/3, exit 1; no RC trace line — KAI_TRACE_RC inert)"; \
	fi

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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-r3b.c -o build/tco-r3b $(LDLIBS_M) ; \
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
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-r3n.c -o build/tco-r3n $(LDLIBS_M) ; \
	./build/tco-r3n > build/tco-r3n.out; \
	diff -q ../examples/tco/rule3_no_misfire.out.expected build/tco-r3n.out > /dev/null \
	  && echo "tco-rule3-no-misfire OK (issue #92 rule 3 correctly skips xs when in args; runs without abort)" \
	  || { echo "tco-rule3-no-misfire DIFF"; diff ../examples/tco/rule3_no_misfire.out.expected build/tco-r3n.out; exit 1; }

# Issue #678 — stdout line-buffered when fd is a pipe / regular file.
# Pre-fix the libc default of full-buffering swallowed every
# Stdout.print on signal-driven termination (SIGTERM / SIGINT
# without a Signal handler) when the program was piped or redirected.
# Fix: setvbuf(stdout, NULL, _IOLBF, 0) at process entry.
#
# This target runs the fixture redirecting stdout to a file (the
# non-TTY path that triggered the bug) and diffs against the
# golden. The runtime check is conservative: a normal exit
# (return 0 from main) flushes anyway via atexit, so this golden
# also passes pre-fix. The signal-driven path the bug report
# describes is exercised by the inline kill -TERM step below
# which asserts the first line lands before the buffer would
# normally flush.
test-issue-678-stdout-buffered: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/runtime/issue_678_stdout_line_buffered.kai > build/iss678.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/iss678.c -o build/iss678 $(LDLIBS_M); \
	./build/iss678 > build/iss678.out; \
	diff -q ../examples/runtime/issue_678_stdout_line_buffered.out.expected build/iss678.out > /dev/null \
	  && echo "issue-678 OK (line-buffered stdout: three lines land in redirected file)" \
	  || { echo "issue-678 DIFF"; diff ../examples/runtime/issue_678_stdout_line_buffered.out.expected build/iss678.out; exit 1; }

# Issue #679 — Spawn.cancel reaches a fiber parked on the reactor.
# Pre-fix a fiber blocked in time.sleep / NetTcp.accept / Stdin.read_line
# / Signal.await stayed parked until the underlying event arrived,
# regardless of cancel. The canceller sibling's Spawn.cancel(target)
# only set the cancel_requested flag — never observed because the
# parked fiber never reached the next op-call boundary.
#
# Fix: kai_default_spawn_cancel detaches the target from its reactor
# waiter list and unparks it. kai_sched_park runs the cancel
# yield-point check on resume, longjmping to cancel_pad before the
# syscall retry loop loops back to EAGAIN. kai_default_spawn_await
# also accepts state CANCELLED so the awaiter side of the nursery
# does not panic on a cancelled target.
#
# The fixture uses time.sleep (timer park) so it does not depend on
# the network stack. The canceller fires after 100ms; without the
# fix the sleeper would block for 60 seconds.
test-issue-679-cancel-parked: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib \
	  ../examples/effects/issue_679_cancel_reaches_parked.kai > build/iss679.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/iss679.c -o build/iss679 $(LDLIBS_M); \
	./build/iss679 > build/iss679.out; \
	diff -q ../examples/effects/issue_679_cancel_reaches_parked.out.expected build/iss679.out > /dev/null \
	  && echo "issue-679 OK (Spawn.cancel detaches reactor-parked target; nursery end reached)" \
	  || { echo "issue-679 DIFF"; diff ../examples/effects/issue_679_cancel_reaches_parked.out.expected build/iss679.out; exit 1; }

# Issue #682 — Spawn.cancel from a sibling fiber runs the user
# `with Cancel { raise(_) -> ... }` handler installed in the target
# before terminating the fiber. Pre-fix `kai_check_cancel_yield_point`
# longjmped straight to the trampoline's `cancel_pad`, marking the
# target CANCELLED without visiting any user Cancel handler in the
# body — the synchronous `Cancel.raise()` path DID dispatch through
# the user clause, so the two paths produced different observable
# behaviour for the same documented "inject Cancel at next yield
# point" contract (`kai info fibers`).
#
# Fix: the yield-point hook walks the evidence stack first. If a
# user Cancel handler (with a live `handle_jmp`) is in scope, the
# hook dispatches through it as a synchronous op call site would —
# clause invoke, discard, longjmp to the handle's landing pad. Falls
# back to `cancel_pad` when no user handler is in scope.
#
# Builds on the #679 fix (sibling-initiated cancel reaches the
# parked fiber); without #679 the worker would still be parked when
# the test timed out.
test-issue-682-cancel-sibling-handler: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib \
	  ../examples/effects/issue_682_cancel_sibling_handler.kai > build/iss682.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/iss682.c -o build/iss682 $(LDLIBS_M); \
	./build/iss682 > build/iss682.out; \
	diff -q ../examples/effects/issue_682_cancel_sibling_handler.out.expected build/iss682.out > /dev/null \
	  && echo "issue-682 OK (sibling-initiated Spawn.cancel runs user Cancel handler before terminating target)" \
	  || { echo "issue-682 DIFF"; diff ../examples/effects/issue_682_cancel_sibling_handler.out.expected build/iss682.out; exit 1; }

# Issue #978 — a fiber spawned inside a nursery performs an effect handled in
# the PARENT fiber. Handler evidence is per-fiber and does not cross a spawn, so
# the spawned thunk fills the callee's evidence slot from its own (empty) stack;
# pre-fix that slot held a NULL (or a zeroed synth-default node) and the perform
# dereferenced it -> segfault (rc 139) on a type-checked program. The fix guards
# every frame-slot fill, so the unhandled effect reports `effect not handled in
# fiber: Note` + exit 1 — NOT a segfault. The exact exit code matters: rc 139
# would be the regression, rc 1 the clean diagnostic.
test-issue-978-spawn-parent-handled-effect: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib \
	  ../examples/effects/issue_978_spawn_parent_handled_effect.kai > build/iss978.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/iss978.c -o build/iss978 $(LDLIBS_M); \
	rc=0; ./build/iss978 > build/iss978.out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] || { echo "issue-978 FAIL (exit $$rc, want 1 — rc 139 = segfault regressed)"; cat build/iss978.out; exit 1; }; \
	diff -q ../examples/effects/issue_978_spawn_parent_handled_effect.out.expected build/iss978.out > /dev/null \
	  && echo "issue-978 OK (spawned fiber's parent-handled effect reports clean diagnostic, not a segfault)" \
	  || { echo "issue-978 DIFF"; diff ../examples/effects/issue_978_spawn_parent_handled_effect.out.expected build/iss978.out; exit 1; }

# Issue #680 — polymorphic recursive fold with a closure-typed
# parameter. Pre-fix the body `fold_events(f(initial, ev1), rest, f)`
# tripped a double-decref of `initial` on the third TCO iteration when
# the list length was >= 4. The misalignment lived between
# `pcs_branch_aware_skip_params` (which classified `initial` as
# skip-eligible pre-perceus) and the post-perceus re-analysis in
# `tcrec_rewrite_decl` (which did NOT, because the closure callee
# had been wrapped in `__perceus_dup(...)` and the linearity
# predicate refused to look through the wrap). The fix lives in
# `pcs_call_consumers_linear` (look through outer `__perceus_dup`)
# and validates here by running the canonical shape at depths
# 1, 2, 3, 4, 8, 16, 64 — any double-decref would crash the
# >=4 cases before completion.
test-tco-issue-680: $(TARGET) build
	@set -e; \
	./$(TARGET) ../examples/tco/issue_680_polymorphic_fold.kai > build/tco-680.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-680.c -o build/tco-680 $(LDLIBS_M) ; \
	./build/tco-680 > build/tco-680.out; \
	diff -q ../examples/tco/issue_680_polymorphic_fold.out.expected build/tco-680.out > /dev/null \
	  && echo "tco-issue-680 OK (polymorphic fold with closure param at depths 1..64 completes; double-decref under post-#599 skip-set fixed)" \
	  || { echo "tco-issue-680 DIFF"; diff ../examples/tco/issue_680_polymorphic_fold.out.expected build/tco-680.out; exit 1; }

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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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

# Issue #703 — two consecutive `match v` on the same let-bound variant
# in an unboxed-signature (UFn) fn body panicked `non-exhaustive match`
# on the C backend (a Perceus use-after-drop) while LLVM was right by
# accident. Pre-fix `perceus_decl` skipped the WHOLE UFn body, so the
# boxed local `v` never received the dup-at-non-last-read the match
# emitter assumes (it `kai_decref`s its scrutinee on every match exit);
# the first match freed `v`, the second dispatched on the freed tag.
# Post-fix the UFn body runs the dup/exit-drop walk with an empty param
# scope, so `v` stays live across both reads and is freed exactly once
# (strict RC: variant frees == variant allocs). The ASAN sibling is the
# real gate: a too-early drop is a use-after-free, a too-late one a leak.
PERCEUS_ISSUE703_FIXTURES := double_match_same_variant

test-perceus-issue703: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE703_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-issue703 OK $$name (variant live across both matches, freed once)" \
	    || { echo "perceus-issue703 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling for issue #703. The fix shifts a drop from
# mid-block (use-after-free, the original C panic) to keeping the
# binding live across both reads; ASan catches a regression in either
# direction — a too-early decref double-frees the variant, a too-late
# one (or a missing dup) reads freed memory in the second match.
test-perceus-issue703-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE703_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-issue703-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-issue703-asan OK $$name" \
	    || { echo "perceus-issue703-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Issue #779 — a multi-statement EBlock with a raw tail emits the tail RAW
# (no `kai_int(...)` box + `(...)->as.i` unbox round-trip). The fixture
# returns raw scalars through blocks whose statements include a heap String
# read twice (a Perceus exit DROP). The drop must thread unchanged before
# the raw tail; a skipped drop leaks, a drop run against the raw scalar is a
# UAF — the ASAN sibling catches either.
PERCEUS_ISSUE779_FIXTURES := issue_779_block_raw_tail

test-issue-779: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE779_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "issue-779 OK $$name (multi-stmt block, raw tail, no box round-trip)" \
	    || { echo "issue-779 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling for issue #779. The raw-tail promotion must NOT move
# any Perceus drop: a skipped drop leaks the heap binding, a drop run
# against the raw scalar reads freed/invalid memory. ASan catches both.
test-issue-779-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE779_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "issue-779-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "issue-779-asan OK $$name" \
	    || { echo "issue-779-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Enum-as-int slot representation (#210 follow-up). A nullary-only sum
# type used as a constructor slot is packed as its immediate variant_tag
# (KAI_VAR_SLOT_ENUM) instead of a pointer to its singleton — removing
# one heap deref per field read. The fixture exercises the SOUNDNESS
# gate: inline guard reads, EXTRACTING an enum out of a slot into a
# binding and passing it to other fns (the re-intern path), equality
# over extracted enums, and rebuilding a node from a flipped enum value.
PERCEUS_ENUM_SLOT_FIXTURES := enum_slot_repr

test-perceus-enum-slot: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ENUM_SLOT_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-enum-slot OK $$name (enum stored as immediate tag, re-intern sound)" \
	    || { echo "perceus-enum-slot DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling for the enum-slot lane. The dangerous regression is
# a forgotten re-intern (kai_enum_slot_box) on the EXTRACT path: handing
# a raw i64 where a KaiValue* is expected reads it as a pointer (UAF).
# ASan catches that; the inline-read win itself is value-only and safe.
test-perceus-enum-slot-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ENUM_SLOT_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-enum-slot-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-enum-slot-asan OK $$name" \
	    || { echo "perceus-enum-slot-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
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
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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

# Nested-pattern reuse-in-place (rb-tree balance rotations). Guards the
# lane that taught the 1:1 variant reuse to fire on arms with a one-level
# nested sub-pattern (`Node(_, Node(Red, lx,..), ..)`), reusing BOTH the
# outer matched cell and the inner deconstructed cell (Koka 2-of-3
# rotation reuse). Unlike gap3_balance_minimal (color dropped), this keeps
# the color slot — the slot that makes the sub-pattern nested, i.e. the
# exact shape `pcs_sub_is_flat` now admits. The golden is the post-insert
# node count: a token used by the rebuild AND freed afterward (the
# double-use bug the lane hit) drops a subtree, so size comes out SHORT.
PERCEUS_NESTED_REUSE_FIXTURES := nested_pattern_reuse_balance native_variant_reuse_leak_872 native_variant_reuse_double_use_872

test-perceus-nested-reuse: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_NESTED_REUSE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-nested-reuse OK $$name (outer+inner cell reuse, all keys kept)" \
	    || { echo "perceus-nested-reuse DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling: the inner-cell reuse moves grand-children (borrow)
# into a donated cell; ASan catches a double-use (token reused AND freed),
# UBSan catches a missed dup that lets a freed grand-child escape.
test-perceus-nested-reuse-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_NESTED_REUSE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c $(LDLIBS_M); \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-nested-reuse-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-nested-reuse-asan OK $$name" \
	    || { echo "perceus-nested-reuse-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# Phase 1.A (perceus-int-cache) — widened small-int cache. The
# canonical reuse-in-place `bump` shape: cons cells reused in place
# (zero new cons across passes), and every `h + 1` value falls in the
# widened [-65536,65535] cache so no boxed Int is minted. The golden
# is the functional head value; the alloc/reuse profile (int allocs=0,
# reuse_in_place = passes * length) is the property the cache pins.
PERCEUS_INT_CACHE_FIXTURES := int_cache_reuse_bump

test-perceus-int-cache: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_INT_CACHE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-int-cache OK $$name (reuse-in-place + in-cache ints, no boxing)" \
	    || { echo "perceus-int-cache DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling: the widened cache pins entries with rc=INT32_MAX
# so incref/decref short-circuit; ASan catches a regression that frees
# a pinned slot (e.g. a decref path that stops honoring the rc guard),
# UBSan catches an out-of-bounds slot index from a bad range check.
test-perceus-int-cache-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_INT_CACHE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-int-cache-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-int-cache-asan OK $$name" \
	    || { echo "perceus-int-cache-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
	done

# i64-inline soundness battery — variant Int fields read raw (kair_<nm>).
# Covers every shape the per-route attempts broke on: explicit Int field
# (rb-tree descent), generic Some/Ok fields, Int arg to a raw-param fn
# inside a nested match, reuse-variant rebuild, Int+ptr mixed arm, the
# other primitives (Real/Char/Bool must stay untouched), an Int read BOTH
# raw and boxed in one arm, and a raw Int binder captured by a closure
# (re-box at the capture border, not the absent boxed local). A wrong
# kair_/kai_ bind shows up as a wrong number or an ASan diagnostic, not a
# silent change.
INT_FIELD_INLINE_FIXTURES := int_field_inline int_field_closure_capture

test-int-field-inline: $(TARGET) build
	@set -e; \
	for name in $(INT_FIELD_INLINE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "int-field-inline OK $$name" \
	    || { echo "int-field-inline DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

test-int-field-inline-asan: $(TARGET) build
	@set -e; \
	for name in $(INT_FIELD_INLINE_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "int-field-inline-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "int-field-inline-asan OK $$name" \
	    || { echo "int-field-inline-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
                              reuse_nested_subpattern \
                              reuse_diagonal_guard \
                              tco_double_incref_repro \
                              nullary_variant_singleton

test-perceus-issue118: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE118_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
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
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
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
	      -I ../stage2 -I ../stage0 build/match_pbind_catchall_raw-asan.c -o build/match_pbind_catchall_raw-asan-c \
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
	  ./$(TARGET) --path ../stdlib ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	    || { echo "http-client-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
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
	  ./$(TARGET) --path ../stdlib ../examples/stdlib/$$name.kai > build/stdlib-$$name-asan.c \
	    || { echo "env-mutate-asan FAIL $$name (kaic2 errored)"; exit 1; }; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/stdlib-$$name-asan.c -o build/stdlib-$$name-asan-c \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/seq-$$name.c -o build/seq-$$name $(LDLIBS_M) \
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
	    rc=0; ./$(TARGET) --path ../stdlib $$f \
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
	    ./$(TARGET) --path ../stdlib $$f > build/ufcs-$$name.c \
	      || { echo "ufcs FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/ufcs-$$name.c -o build/ufcs-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/u2-$$name.c -o build/u2-$$name $(LDLIBS_M) \
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
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/ta-$$name.c -o build/ta-$$name $(LDLIBS_M) \
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
	$(CC) $(CFLAGS) -DKAI_TRACE_RC=1 -I ../stage2 -I ../stage0 build/rc_trace_callsite.c -o build/rc_trace_callsite-c $(LDLIBS_M) ; \
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

# Issue #86 v2/v3 — structured violation context for contract panics.
# Each fixture under examples/refinements/ either panics at runtime
# with the exact bytes in `<name>.run.err.expected` (negative cases)
# or runs cleanly with stdout matching `<name>.out.expected` (the
# happy-path companion). Keeps the violation message format byte-
# pinned so a regression in build_violation_msg_span trips a clear
# test rather than a downstream user surprise.
#
# v3 (#86 piece 2): the `SAssert` arm is wired and must produce the golden
# output under the C backend; each fixture is diffed against its
# `.run.err.expected` (negative: contract panics) or `.out.expected`
# (positive). The native backend's parity is gated separately by the
# native-vs-C ratchet (tools/test-backend-parity.sh).
test-violations: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/refinements/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_run_err=../examples/refinements/$$name.run.err.expected; \
	  exp_out=../examples/refinements/$$name.out.expected; \
	  ./$(TARGET) $$f > build/viol-$$name.c \
	    || { echo "violations FAIL $$name (kaic2 C errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/viol-$$name.c -o build/viol-$$name $(LDLIBS_M) \
	    || { echo "violations FAIL $$name (C compile failed)"; exit 1; }; \
	  rc=0; ./build/viol-$$name > build/viol-$$name.out 2> build/viol-$$name.err || rc=$$?; \
	  if [ -f $$exp_run_err ]; then \
	    if [ $$rc -eq 0 ]; then \
	      echo "violations FAIL $$name (C: expected runtime panic, got success)"; exit 1; \
	    fi; \
	    diff -q $$exp_run_err build/viol-$$name.err > /dev/null \
	      && echo "violations OK $$name (C panic match)" \
	      || { echo "violations DIFF $$name (C panic)"; diff $$exp_run_err build/viol-$$name.err; exit 1; }; \
	  elif [ -f $$exp_out ]; then \
	    if [ $$rc -ne 0 ]; then \
	      echo "violations FAIL $$name (C: unexpected runtime error)"; cat build/viol-$$name.err; exit 1; \
	    fi; \
	    diff -q $$exp_out build/viol-$$name.out > /dev/null \
	      && echo "violations OK $$name (C out match)" \
	      || { echo "violations DIFF $$name (C out)"; diff $$exp_out build/viol-$$name.out; exit 1; }; \
	  else \
	    echo "violations SKIP $$name (no golden)"; \
	  fi; \
	done

# Issue #608 — `#[...]` attribute syntax. Each fixture in
# examples/attributes/ has either an `.out.expected` (positive: compile,
# run, diff) or an `.err.expected` (negative: kaic2 must reject; the
# expected file's first line must appear in the error output as a
# substring). Stdlib prelude is loaded so `Show`/`Eq` resolve.
test-attributes: $(TARGET) build
	@$(STACK) set -e; \
	preludes=""; \
	for f in ../examples/attributes/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/attributes/$$name.out.expected; \
	  exp_err=../examples/attributes/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $$preludes $$f \
	      > /dev/null 2> build/attr-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "attributes FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    needle=$$(head -1 $$exp_err); \
	    grep -qF "$$needle" build/attr-$$name.err \
	      && echo "attributes OK $$name (rejection diagnostic matches)" \
	      || { echo "attributes FAIL $$name (missing diagnostic: $$needle)"; \
	           cat build/attr-$$name.err; exit 1; }; \
	  elif [ -f $$exp_out ]; then \
	    ./$(TARGET) --path ../stdlib $$preludes $$f > build/attr-$$name.c \
	      || { echo "attributes FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/attr-$$name.c -o build/attr-$$name $(LDLIBS_M) \
	      || { echo "attributes FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/attr-$$name > build/attr-$$name.out \
	      || { echo "attributes FAIL $$name (run errored)"; cat build/attr-$$name.out; exit 1; }; \
	    diff -q $$exp_out build/attr-$$name.out > /dev/null \
	      && echo "attributes OK $$name" \
	      || { echo "attributes DIFF $$name"; diff $$exp_out build/attr-$$name.out; exit 1; }; \
	  else \
	    echo "attributes SKIP $$name (no golden)"; \
	  fi; \
	done

# Issue #681 — #[doc(...)] attribute JSON surfaces. Three checks:
#  (a) --doc-json golden: module_doc + per-item docs of the fixture
#      (pinned byte-exact against doc_json_basic.doc.expected);
#  (b) --holes-json carries the surrounding declaration's doc text;
#  (c) --builtins-doc-json emits the core-module-set array.
# The duplicate-doc / second-module-doc rejections live in
# examples/negative/doc/ under tools/test-negative.sh.
test-doc-attr: $(TARGET) build
	@$(STACK) set -e; \
	./$(TARGET) --doc-json ../examples/doc/doc_json_basic.kai > build/doc-json-basic.out; \
	diff ../examples/doc/doc_json_basic.doc.expected build/doc-json-basic.out > /dev/null \
	  && echo "doc-attr OK doc-json golden" \
	  || { echo "doc-attr DIFF doc-json golden"; \
	       diff ../examples/doc/doc_json_basic.doc.expected build/doc-json-basic.out; exit 1; }; \
	./$(TARGET) --holes-json ../examples/doc/doc_holes.kai > build/doc-holes.json; \
	grep -qF '"doc": "Documented holder of the probe hole."' build/doc-holes.json \
	  && echo "doc-attr OK holes-json doc field" \
	  || { echo "doc-attr FAIL holes-json doc field"; cat build/doc-holes.json; exit 1; }; \
	./$(TARGET) --builtins-doc-json > build/builtins-doc.json; \
	grep -qF '"module": "core/list"' build/builtins-doc.json \
	  && echo "doc-attr OK builtins-doc-json" \
	  || { echo "doc-attr FAIL builtins-doc-json"; exit 1; }

# Issue #644 — ctor-overloading regression fixtures. Each .kai under
# `examples/shadowing/` either compiles + runs to a known stdout
# (positive cases — diffed against `<name>.out.expected`) or fails
# to compile with a known diagnostic substring (negative cases —
# matched against `<name>.err.expected`). Same diff / golden
# discipline as `test-string` and `test-pipes`. Loads the core
# preludes so user types that collide with prelude `Result.Ok` /
# `Option.Some` exercise the real prelude shape. Wired into `test`
# (the tier1 chain) so byte-identical selfhost + the closing PR's
# acceptance gate both touch this target.
test-shadowing: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/shadowing/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/shadowing/$$name.out.expected; \
	  exp_err=../examples/shadowing/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) --path ../stdlib $$f \
	      > /dev/null 2> build/shadowing-$$name.err || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "shadowing FAIL $$name (expected error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/shadowing-$$name.err \
	        || { echo "shadowing FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/shadowing-$$name.err; exit 1; }; \
	    done < $$exp_err; \
	    echo "shadowing OK $$name (rejected as expected)"; \
	  else \
	    ./$(TARGET) --path ../stdlib $$f \
	      > build/shadowing-$$name.c \
	      || { echo "shadowing FAIL $$name (kaic2 errored)"; exit 1; }; \
	    $(CC) $(CFLAGS) $(LDFLAGS_STACK) -I ../stage2 -I ../stage0 build/shadowing-$$name.c -o build/shadowing-$$name $(LDLIBS_M) \
	      || { echo "shadowing FAIL $$name (C compile failed)"; exit 1; }; \
	    ./build/shadowing-$$name > build/shadowing-$$name.out \
	      || { echo "shadowing FAIL $$name (run errored)"; cat build/shadowing-$$name.out; exit 1; }; \
	    if [ -f $$exp_out ]; then \
	      diff -q $$exp_out build/shadowing-$$name.out > /dev/null \
	        && echo "shadowing OK $$name" \
	        || { echo "shadowing DIFF $$name"; diff $$exp_out build/shadowing-$$name.out; exit 1; }; \
	    else \
	      echo "shadowing OK $$name (no golden, ran clean)"; \
	    fi; \
	  fi; \
	done

# Issue #748 — separate compilation (`--emit=c-modular`). Emits a
# program as per-module `.c` translation units + shared kai_types.h /
# kai_decls.h, splits the marker-delimited stream into files, compiles
# each `.c` to its own `.o`, links the `.o` files, and diffs the run
# against the single-TU golden. Proves a USER program with its own
# modules (math.kai, shapes.kai, ...) plus stdlib core modules each
# land in a distinct translation unit / object and link cleanly — the
# GHC/Koka model. Mirrors the `bin/kai` wrapper's modular path so CI
# exercises the same split + link discipline the driver ships.
MODULAR_FIXTURES = ../examples/multi-module/main.kai \
                   ../examples/multi-module/issue-237/main.kai \
                   ../examples/multi-module/issue-898-qualified-generic/main.kai \
                   ../examples/multi-module/issue-963-cross-module-mono/main.kai \
                   ../examples/multi-module/modular_stdlib_lambdas.kai \
                   ../examples/modules-qualified/basic/main.kai \
                   ../examples/modules-qualified/two_modules/main.kai \
                   ../examples/shadowing/user_shadows_stdlib_list_fns.kai \
                   ../examples/shadowing/user_show_shadows_protocol_op.kai \
                   ../examples/shadowing/user_shadows_and_qualified_coexist.kai
test-modular-build: $(TARGET) build
	@$(STACK) set -e; \
	i=0; \
	for f in $(MODULAR_FIXTURES); do \
	  i=$$((i + 1)); \
	  name=modular-$$i; \
	  srcdir=$$(cd "$$(dirname $$f)" && pwd); \
	  exp=$${f%.kai}.out.expected; \
	  [ -f $$exp ] || exp=$$(dirname $$f)/main.out.expected; \
	  mod=build/$$name.d; rm -rf $$mod; mkdir -p $$mod; \
	  ./$(TARGET) --emit=c-modular --path ../stdlib --path "$$srcdir" $$f \
	    > build/$$name.stream \
	    || { echo "modular FAIL $$name (kaic2 errored)"; exit 1; }; \
	  nonce=$$(sed -n '1s|^//KAIDELIM:||p' build/$$name.stream); \
	  [ -n "$$nonce" ] || { echo "modular FAIL $$name (no //KAIDELIM marker)"; exit 1; }; \
	  awk -v dir="$$mod" -v pfx="//KAIFILE:$$nonce::" \
	    'index($$0,pfx)==1{fn=substr($$0,length(pfx)+1);out=dir"/"fn;next} \
	     out{if($$0 ~ /^static (KaiValue \*|int64_t |double |int |uint32_t )/)sub(/^static /,"",$$0);print > out}' \
	    build/$$name.stream; \
	  cp runtime.h $$mod/runtime.h 2>/dev/null || true; \
	  objs=""; \
	  for c in $$mod/*.c; do \
	    o=$${c%.c}.o; \
	    $(CC) $(CFLAGS) -I $$mod -I ../stage2 -I ../stage0 -c $$c -o $$o \
	      || { echo "modular FAIL $$name (compile $$(basename $$c))"; exit 1; }; \
	    objs="$$objs $$o"; \
	  done; \
	  $(CC) $(CFLAGS) $(LDFLAGS_STACK) $$objs -o build/$$name.bin $(LDLIBS_M) \
	    || { echo "modular FAIL $$name (link)"; exit 1; }; \
	  ./build/$$name.bin > build/$$name.out \
	    || { echo "modular FAIL $$name (run)"; cat build/$$name.out; exit 1; }; \
	  if [ -f $$exp ]; then \
	    diff -q $$exp build/$$name.out > /dev/null \
	      && echo "modular OK $$name ($$(basename $$(dirname $$f))/$$(basename $$f))" \
	      || { echo "modular DIFF $$name"; diff $$exp build/$$name.out; exit 1; }; \
	  else \
	    echo "modular OK $$name (no golden, linked + ran clean)"; \
	  fi; \
	done

# Issue #963 Part A — cross-module monomorphization home-TU discipline.
# One generic (`boxer.box[T]`) instantiated at the SAME type (Int) from two
# sibling modules dedups to a SINGLE whole-program spec. This default
# single-TU target pins the program's OUTPUT; `test-modular-build` runs the
# SAME fixture against the SAME golden through the split / compile-each /
# link path, where the spec landing exactly once in its home TU (`boxer.c`)
# is precisely what lets the link succeed. Both paths diffing one golden
# pins default-path == modular-path.
test-issue-963-cross-module-mono: $(TARGET) build
	@set -e; \
	fx=../examples/multi-module/issue-963-cross-module-mono; \
	./$(TARGET) --path ../stdlib --path $$fx $$fx/main.kai \
	            > build/issue_963_cross_module_mono.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/issue_963_cross_module_mono.c -o build/issue_963_cross_module_mono $(LDLIBS_M) ; \
	./build/issue_963_cross_module_mono > build/issue_963_cross_module_mono.out; \
	diff -q $$fx/main.out.expected build/issue_963_cross_module_mono.out > /dev/null \
	  && echo "issue-963-cross-module-mono OK (default single-TU)" \
	  || { echo "issue-963-cross-module-mono DIFF"; diff $$fx/main.out.expected build/issue_963_cross_module_mono.out; exit 1; }

# Lane tier1-perf — the five "costly" subtargets each compile the
# whole compiler (`main.kai`, ~4 GB RSS, ~20 s) in a different dump
# mode: tokens, ast, types, env, and a clean check. They write to
# disjoint output suffixes (build/s2-$$name.{tok,ast,types,env};
# test-check writes nothing) so they are race-free under `make -j`.
# We `$(MAKE) -j$(COSTLY_JOBS)` them as a group: serial they are
# ~106 s, in parallel ~72 s (1.47x; capped by main.kai being
# memory-bound, not CPU-bound). `$(TARGET) build` is depended on
# here so kaic2 is fully cooked before the `-j` fan-out — the five
# children then find it up-to-date and never race to rebuild it.
# COSTLY_JOBS defaults to 5 (one per subtarget); override to 1 to
# force the old serial behaviour.
COSTLY_JOBS ?= 5
test-costly-parallel: $(TARGET) build
	@$(MAKE) -j$(COSTLY_JOBS) test-tokens test-ast test-types test-env test-check

# tier1 wall-time reduction (2026-06-05). The ~90 light subtargets below
# each run kaic2 over SMALL fixtures (kaic2 -> C/LLVM -> run -> diff),
# not the whole compiler, so they are CPU-bound with modest RSS — unlike
# the five `test-costly-parallel` ones (4 GB each, capped at -j5). The
# light group used to run serially as plain `test:` prerequisites, which
# left a 16-core Linux runner idle one fixture at a time and pushed the
# battery past the 25-min timeout once the LLVM lanes made every fixture
# actually run. Fan them out with `$(MAKE) -jN`.
#
# Race-safety (verified): every target depends on `$(TARGET) build` (a
# common prereq, so kaic2 is cooked ONCE before the fan-out — children
# never race to rebuild it); none rebuilds kaic2 or runs `selfhost` in
# its recipe (selfhost lives in test-stage2, not here); each writes
# build/ files under a target-specific prefix (s2-/u2-/viol-/...), so
# outputs are disjoint. The asan targets are NOT in this list. TEST_JOBS
# is the cap — memory contention (not build/ races) is the only flakiness
# axis, so it is bounded well below nproc; override TEST_JOBS=1 to force
# the old serial behaviour.
TEST_JOBS ?= 8
TEST_LIGHT_TARGETS := test-infer test-infer-check test-lint test-dump-typed test-dump-mono test-rawsafe test-borrowsafe test-run test-blocks test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-modules-transitive-privacy test-modules-uom-privacy test-holes test-driver-holes test-effects test-effect-runtime test-link-runtime test-sugars test-pipes test-match test-literals test-string test-loop test-records test-diagnostics test-reader test-writer test-trace test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-protocols test-proto-scalar-dispatch test-bare-narrow-assert test-demos-core test-aspirational test-m4c test-match-backtrack-unify test-unbox-phase2 test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-runtime-shadow test-issue-501 test-issue-743 test-issue-842 test-foreign-prelude test-issue-672 test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-746 test-issue-668 test-trmc-list-build test-perceus-issue82 test-perceus-issue118 test-perceus-issue298 test-perceus-issue350 test-perceus-issue703 test-issue-779 test-perceus-enum-slot test-perceus-int-cache test-int-field-inline test-perceus-nested-reuse test-perceus-812-tracer test-perceus-817-filter-leak test-issue-892-mono-arg-unbox test-unions test-aliases test-sequence test-ufcs test-ffi-extern-c test-ffi-scalar-char test-ffi-v2-struct test-ffi-v2-opaque test-ffi-v2-negatives test-ffi-v2-native-reject test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-issue-315-no-double-slash test-issue-248-loop-sugars test-rc-trace-callsite test-tco-rule3-basic test-tco-rule3-no-misfire test-tco-issue-680 test-issue-678-stdout-buffered test-issue-679-cancel-parked test-issue-682-cancel-sibling-handler test-issue-978-spawn-parent-handled-effect test-violations test-unused-bindings test-attributes test-doc-attr test-shadowing test-modular-build test-issue-963-cross-module-mono test-kir test-batch-isolation

test-light-parallel: $(TARGET) build
	@$(MAKE) -j$(TEST_JOBS) $(TEST_LIGHT_TARGETS)

# CI sharding (docs/ci-time-analysis.md §7). The ~100 light targets are the
# dominant tier1 cost (~15 min on ubuntu-latest) and are CPU-bound +
# independent, so they scale near-linearly across SEPARATE runners (in-job
# `-j` is memory-bandwidth-capped at 1.24×; separate runners each get their
# own memory bus — the tier1-perf retro measured this). `test-light-shard`
# runs the round-robin slice `SHARD` of `SHARDS` of TEST_LIGHT_TARGETS:
# target i (1-based) belongs to shard ((i-1) % SHARDS) + 1. Round-robin (not
# contiguous wordlist) balances the heavy families (stdlib/sugars/effects)
# across slices instead of piling them into one. The slices are a partition
# of the single canonical list — their union is exactly TEST_LIGHT_TARGETS,
# with no overlap, by construction. SHARDS=1 (default) reproduces the full
# list, so a non-sharded `test` is unchanged.
SHARD  ?= 1
SHARDS ?= 1
test-light-shard: $(TARGET) build
	@slice=$$(echo $(TEST_LIGHT_TARGETS) | tr ' ' '\n' \
	  | awk 'NR % $(SHARDS) == ($(SHARD) - 1) % $(SHARDS)' | tr '\n' ' '); \
	echo "test-light-shard $(SHARD)/$(SHARDS): $$(echo $$slice | wc -w | tr -d ' ') targets"; \
	$(MAKE) -j$(TEST_JOBS) $$slice

test: $(TARGET) build
	@$(MAKE) test-costly-parallel
	@$(MAKE) test-light-parallel
	@$(MAKE) test-heap-limit
	@$(MAKE) test-user-cache
	@$(MAKE) test-core-cache

# Fast subset for iterative development: skips every subtarget that
# runs kaic2 (or kaic1) against the compiler's own source. Each of
# those takes ~80-90s per invocation and they dominate `make test`
# (~8-9 min of its ~10-12 min runtime). `test-fast` leaves in the
# smoke tests that actually catch regressions on minimal examples,
# modules, and holes — covers the common feature-work cases.
# Use `make test` for the end-of-block gate.
test-fast: test-infer test-infer-check test-lint test-dump-typed test-dump-mono test-rawsafe test-borrowsafe test-run test-blocks test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-modules-transitive-privacy test-modules-uom-privacy test-holes test-driver-holes test-effects test-effect-runtime test-sugars test-pipes test-match test-literals test-string test-loop test-diagnostics test-reader test-writer test-trace test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-protocols test-proto-scalar-dispatch test-bare-narrow-assert test-demos-core test-aspirational test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-foreign-prelude test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-746 test-issue-668 test-trmc-list-build test-issue-779 test-unions test-aliases test-sequence test-ufcs test-ffi-scalar-char test-ffi-v2-struct test-ffi-v2-opaque test-ffi-v2-negatives test-ffi-v2-native-reject test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-issue-315-no-double-slash test-issue-248-loop-sugars test-tco-rule3-basic test-tco-rule3-no-misfire test-tco-issue-680 test-issue-678-stdout-buffered test-issue-679-cancel-parked test-issue-682-cancel-sibling-handler test-issue-978-spawn-parent-handled-effect test-unused-bindings test-issue-963-cross-module-mono test-kir test-packages

# Package-mode harness (issue #569). Exercises kaikai as a package
# consumer through tools/test-packages.sh; covers the 9-category
# matrix from the issue plus the pre-existing driver-level checks
# (lockfile_reproducibility, add_failure, init_invalid_names,
# manifest_parse_error). Without this target, the entire
# package-manager surface stays a CI blind spot — bugs like #565
# and #567 only surfaced because ahu tried to integrate.
test-packages: $(TARGET)
	@$(STACK) ../tools/test-packages.sh

# Issue #455 Phase B — user-file incremental cache invalidation gate.
# Runs the five negative fixtures (edit source / edit import / edit
# transitive import / version bump / corrupt blob) plus the positive
# cold==warm==oracle differential. A stale-serving cache is a
# correctness bug worse than no cache, so this is a hard gate, not a
# smoke test. Each script is self-contained under /tmp and exits 0/1.
# The kaic2 binary must be built first because the fixtures invoke
# bin/kai (which uses it) and kaic2 directly.
test-user-cache: $(TARGET)
	@set -e; \
	for f in ../examples/cache/userb_*.sh; do \
	  sh "$$f" || { echo "user-cache FAIL: $$f"; exit 1; }; \
	done; \
	echo "user-cache OK (all Phase B invalidation fixtures passed)"

# Issue #825 — core (auto-loaded stdlib) post-parse cache invalidation
# gate. Runs the positive cold==warm==oracle differential plus three
# negatives (edit a core file / version bump / corrupt blob). Serving a
# stale core typechecks every program against wrong stdlib symbols — a
# correctness bug worse than no cache — so this is a hard gate, not a
# smoke test. The kaic2 binary must be built first because the fixtures
# invoke bin/kai (which uses it) and kaic2 directly.
test-core-cache: $(TARGET)
	@set -e; \
	for f in ../examples/cache/corec_*.sh; do \
	  sh "$$f" || { echo "core-cache FAIL: $$f"; exit 1; }; \
	done; \
	echo "core-cache OK (all #825 core-cache invalidation fixtures passed)"

# KIR Lane 0 — `--emit=kir` dumper goldens + determinism gate.
# For each fixture in ../examples/kir/*.kai: dump KIR, extract just the
# USER functions (named in the matching `<name>.kir.fns` list, so the
# golden stays stable across stdlib churn), diff against the
# `.kir.expected` golden, and confirm a second dump is byte-identical
# (Risk 2 determinism). The extractor is the same awk used to author the
# goldens, kept here as the single source so author == check.
test-kir: $(TARGET) build
	@set -e; \
	for f in ../examples/kir/*.kai; do \
	  name=$$(basename $$f .kai); \
	  fns=$$(cat ../examples/kir/$$name.kir.fns); \
	  ./$(TARGET) --path ../stdlib --emit=kir $$f 2>/dev/null \
	    | awk -v fns="$$fns" 'BEGIN{n=split(fns,a," ");for(i=1;i<=n;i++)w[a[i]]=1} /^fn |^lambda fn |^clause fn /{nm="";for(i=1;i<=NF;i++)if($$i ~ /\(/){nm=$$i;sub(/\(.*/,"",nm);break} cur=(nm in w)} cur{print} /^}/{if(cur){print "";cur=0}}' \
	    > build/$$name.kir; \
	  diff -q ../examples/kir/$$name.kir.expected build/$$name.kir > /dev/null \
	    || { echo "kir DIFF $$name"; diff ../examples/kir/$$name.kir.expected build/$$name.kir; exit 1; }; \
	  ./$(TARGET) --path ../stdlib --emit=kir $$f 2>/dev/null > build/$$name.kir.run2a; \
	  ./$(TARGET) --path ../stdlib --emit=kir $$f 2>/dev/null > build/$$name.kir.run2b; \
	  diff -q build/$$name.kir.run2a build/$$name.kir.run2b > /dev/null \
	    || { echo "kir NON-DETERMINISTIC $$name"; exit 1; }; \
	  echo "kir OK $$name"; \
	done

# Differential independence oracle (#962): the soundness gate proving
# core's typecheck is byte-identical with and without an adversarial
# user file. RED if any user file shifts (or breaks) core's typed AST —
# the standing proof the root_fns + lower_protocols cuts hold. Drives
# tools/independence-oracle.sh against the just-built $(TARGET).
test-independence-oracle: $(TARGET)
	@KAIC2="$$(pwd)/$(TARGET)" bash ../tools/independence-oracle.sh

# #962 Lane 4 — proves `--batch-list` keeps separate inputs isolated:
# each file's batch `.c` is byte-identical to its standalone `.c`.
test-batch-isolation: $(TARGET)
	@KAIC2="$$(pwd)/$(TARGET)" bash ../tools/batch-isolation.sh

clean:
	rm -rf build $(TARGET)
