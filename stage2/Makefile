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

# `timeout(1)` is GNU coreutils: present on Linux, absent from a stock macOS
# (Homebrew coreutils ships it as `gtimeout`). Hosts with neither use the
# shim, which bounds via perl or — with no implementation at all — warns and
# runs unbounded rather than aborting on `command not found`.
TIMEOUT := $(shell command -v timeout >/dev/null 2>&1 && echo timeout \
  || { command -v gtimeout >/dev/null 2>&1 && echo gtimeout; })
ifeq ($(TIMEOUT),)
TIMEOUT := $(abspath $(CURDIR)/../tools/lib/timeout.sh)
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
#   - `make KAI_LLVM=1 kaic2` FORCES native-capable: if llvm-config does
#     not resolve, make stops with an error naming the fix (used by the
#     release job, where a missing libLLVM MUST break, not silently ship
#     a C-only binary).
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
LLVM_CONFIG_RESOLVED := $(shell command -v $(LLVM_CONFIG) 2>/dev/null)
ifeq ($(origin KAI_LLVM),undefined)
KAI_LLVM := $(if $(LLVM_CONFIG_RESOLVED),1,0)
KAI_LLVM_AUTO := 1
endif
ifeq ($(KAI_LLVM),1)
ifeq ($(LLVM_CONFIG_RESOLVED),)
$(error KAI_LLVM=1 but '$(LLVM_CONFIG)' was not found. Put llvm-config on PATH or pass LLVM_CONFIG=/path/to/llvm-config (Homebrew keg-only: LLVM_CONFIG=$$(brew --prefix llvm@18)/bin/llvm-config))
endif
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
LLVM_SYSLIBS  := $(shell $(LLVM_CONFIG) --link-static --system-libs passes core analysis target nativecodegen bitwriter)
LLVM_LDFLAGS  := $(shell $(LLVM_CONFIG) --ldflags) $(shell $(LLVM_CONFIG) --link-static --libs passes core analysis target nativecodegen bitwriter) $(LLVM_SYSLIBS) $(LLVM_CXXLIB)
# Homebrew LLVM's --system-libs asks for -lzstd, but zstd is a separate
# formula under the brew prefix, which the macOS linker does not search
# by default. Gated on -lzstd actually appearing, so a vendored static
# libLLVM (built without zstd) never picks up Homebrew lib dirs.
ifeq ($(UNAME_S),Darwin)
ifneq ($(filter -lzstd,$(LLVM_SYSLIBS)),)
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
LLVM_LDFLAGS += -L$(BREW_PREFIX)/lib
endif
endif
endif
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
BUNDLE_SRCS := compiler/util.kai compiler/chars.kai compiler/diag.kai compiler/lex.kai compiler/ast.kai compiler/kinds.kai compiler/unit_walk.kai compiler/kind_resolve.kai compiler/layout_synth.kai compiler/home_tu.kai compiler/rows.kai compiler/doc_attr.kai compiler/native_prims.kai compiler/intervals.kai compiler/refine_help.kai compiler/refinements.kai compiler/parse.kai compiler/resolve.kai compiler/fmt_writer.kai compiler/fmt_expr.kai compiler/fmt_effect.kai compiler/fmt_decl.kai compiler/fmt.kai compiler/migrate_rules.kai compiler/migrate_types.kai compiler/migrate_walk.kai compiler/migrate_scan.kai compiler/migrate.kai compiler/cache.kai compiler/modules.kai compiler/user_cache.kai compiler/core_cache.kai compiler/emit_cache.kai compiler/loop_lower.kai compiler/desugar.kai compiler/cell_scan.kai compiler/cell_promote.kai compiler/refine_enforce.kai compiler/region.kai compiler/infer_index.kai compiler/pipe_fusion.kai compiler/lit_mint.kai compiler/vec_surface.kai compiler/infer.kai compiler/evidence_scan.kai compiler/evidence.kai compiler/evidence_inject.kai compiler/evidence_frame.kai compiler/fnreg.kai compiler/unbox_native_raw.kai compiler/unbox.kai compiler/rawsafe.kai compiler/ast_map.kai compiler/borrow_escape.kai compiler/borrow_infer.kai compiler/perceus_op_arg.kai compiler/perceus_plant_drop.kai compiler/perceus_tail_drop.kai compiler/perceus.kai compiler/purity.kai compiler/impl_index.kai compiler/layout_derive.kai compiler/layout_validate.kai compiler/protos.kai compiler/fwd_inline.kai compiler/monomorph.kai compiler/emit_shared.kai compiler/emit_tcrec_live.kai compiler/vec_raw.kai compiler/ffi_v2.kai compiler/kir.kai compiler/kir_uses.kai compiler/kir_varslot.kai compiler/kir_inline_clone.kai compiler/kir_inline_beta.kai compiler/kir_inline.kai compiler/kir_lower.kai compiler/kir_pat_alias.kai compiler/kir_lower_reuse.kai compiler/kir_lower_raw.kai compiler/kir_lower_bind.kai compiler/kir_lower_variant.kai compiler/kir_lower_match.kai compiler/kir_lower_lit.kai compiler/kir_lower_rec.kai compiler/kir_lower_walk.kai compiler/kir_lower_fns.kai compiler/kir_dump.kai compiler/emit_native_di.kai compiler/emit_native_fn.kai compiler/emit_native_slot.kai compiler/emit_native_ops.kai compiler/emit_native_abi.kai compiler/emit_native_struct.kai compiler/emit_native_ffi.kai compiler/emit_native_fx.kai compiler/emit_native_fx2.kai compiler/emit_native_trmc.kai compiler/emit_native_term.kai compiler/emit_native_def.kai compiler/emit_native_proto.kai compiler/emit_native_test.kai compiler/emit_native.kai compiler/emit_native_modkey.kai compiler/emit_native_core.kai compiler/emit_native_modular.kai compiler/emit_ffi_c.kai compiler/emit_reuse_donate.kai compiler/emit_slices.kai compiler/emit_c.kai compiler/modular_linkage.kai compiler/lint_idiom.kai compiler/lint_redundant.kai compiler/lint_combinator.kai compiler/lint_deadcode.kai compiler/lint_effects.kai compiler/lint.kai compiler/closure_spec_ast.kai compiler/closure_spec_emit.kai compiler/closure_spec.kai compiler/law_checks.kai compiler/driver.kai main.kai

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

# The only project header the generated C pulls in: it emits a bare
# `#include "runtime.h"`, which the `-I .` above binds to stage2/runtime.h,
# and that header includes no other project header. make cannot see through
# the generated file, so every rule compiling it must name this explicitly
# or a runtime-only edit leaves a stale binary in place. Deliberately NOT
# ../stage0/runtime.h: this translation unit never reaches it, and listing
# it would force the ~70 s -O2 rebuild on unrelated stage 0 edits.
RUNTIME_H := runtime.h

# Issue #42 closure (2026-05-01): the recursive lex_loop / parse_*
# inside kaic1 / kaic2 used to need a 32-64 MB stack bump to handle
# stage2/compiler.kai's ~32 k tokens. With both stage 0 and stage 1
# emitting goto-form self-tail-calls, kaic1 / kaic2 run with O(1)
# C-stack on the recursive paths and the bump is no longer needed.
# Left as an empty variable so the existing `$(STACK)` recipe
# prefixes still expand to a no-op and don't have to be edited.
STACK :=

.PHONY: test-kir-inline-trmc-1158 test-kir-while-slot-1158 test-kir-while-param-slot-1173 test-kir-var-escape-state-1173
.PHONY: test-issue-1331-op-arg-release-asan test-issue-1331-borrowed-op-arg-asan
.PHONY: test-mn-sigaltstack-asan
.PHONY: all clean core-cache-warm test test-fast test-costly-parallel test-tokens test-ast test-check test-run test-blocks test-holes test-lint test-purity test-native-purity-attrs test-driver-holes test-effects test-effect-runtime test-link-runtime test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-modules-transitive-privacy test-modules-uom-privacy test-sugars test-pipes test-pipes-collections test-match test-literals test-string test-loop test-records test-diagnostics test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-stdlib-survey test-stdlib-crypto-asan test-stdlib-regex-predicate-asan test-http-client-asan test-env-mutate-asan test-securerandom-asan test-match-pbind-catchall-asan test-protocols test-proto-scalar-dispatch test-bare-narrow-assert test-demos-core test-aspirational test-match-backtrack-unify test-unbox-phase2 test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-tco-param-shadow test-trace test-trace-asan test-issue-501 test-issue-743 test-issue-842 test-foreign-prelude test-issue-672 test-issue-1194-reassigned-record-field-in-interp test-issue-1263-abandon-across-resumed-frame test-issue-1263-abandon-across-resumed-frame-native test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-746 test-issue-668 test-trmc-list-build test-region-1123 test-region-1193-native test-region-1197-parity test-log-asan test-ffi-extern-c test-ffi-scalar-char test-ffi-cflags-shim test-ffi-cflags-shim-native test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-numeric-fixed-width test-numeric-bigint test-numeric-bigint-native test-numeric-decimal-rational test-numeric-decimal-rational-native test-numeric-div-by-zero-traps test-numeric-div-by-zero-traps-native test-numeric-literal-mint test-issue-315-no-double-slash test-issue-248-loop-sugars test-issue-1179-var-slot-loop test-issue-1214-nonliteral-init test-perceus-issue118 test-perceus-issue118-asan test-perceus-issue298 test-perceus-issue298-asan test-perceus-issue350 test-perceus-issue350-asan test-perceus-trmc-spread test-perceus-trmc-spread-asan test-perceus-issue703 test-perceus-issue703-asan test-issue-779 test-issue-779-asan test-perceus-enum-slot test-perceus-enum-slot-asan test-perceus-int-cache test-perceus-int-cache-asan test-int-field-inline test-int-field-inline-asan test-perceus-nested-reuse test-perceus-nested-reuse-asan test-perceus-812-tracer test-perceus-817-filter-leak test-perceus-1219-bootstrap-seed test-perceus-1117-map-pipe-record-leak test-perceus-1134-pipe-fusion test-perceus-1134-pipe-fusion-ordering test-perceus-1134-pipe-fusion-terminal test-perceus-1134-pipe-fusion-native test-perceus-1134-pipe-fusion-terminal-native test-perceus-1186-map-pipe-terminal-field test-perceus-1120-array-borrow test-perceus-1120-array-borrow-native test-perceus-1126-read-loop-drop test-perceus-1135-vec-value test-perceus-1135-vec-value-native test-perceus-1135-vec-raw test-perceus-1135-vec-raw-native test-perceus-1151-vec-push-growth test-perceus-1151-vec-push-growth-native test-perceus-1151-vec-push-growth-asan test-perceus-1153-modcall-linear test-perceus-1153-modcall-linear-native test-perceus-1153-modcall-linear-asan test-perceus-1150-vec-surface test-perceus-1150-vec-surface-native test-perceus-1150-vec-surface-asan test-perceus-1127-borrow-closure test-perceus-1127-borrow-hof test-perceus-1127-borrow-hof-native test-perceus-1127-borrow-tco test-perceus-1131-borrow-descent test-perceus-1131-modular-escape test-perceus-1295-borrow-slot-nested-arg test-perceus-1295-borrow-slot-nested-arg-asan test-perceus-1303-single-use-branch-leak test-perceus-1303-single-use-branch-leak-asan test-perceus-1302-tcrec-goto-drops test-perceus-1302-tcrec-goto-drops-asan test-perceus-1302-tcrec-goto-drops-native test-perceus-1315-borrowed-match-release test-perceus-1315-borrowed-match-release-asan test-perceus-1315-borrowed-match-release-native test-perceus-1355-closure-temp-release test-perceus-1355-closure-temp-release-asan test-perceus-1355-closure-temp-release-native test-perceus-1328-selftail-borrowed-local test-perceus-1328-selftail-borrowed-local-asan test-perceus-1328-selftail-borrowed-local-native test-perceus-1127-borrow-tco-native test-perceus-1130-borrow-pipe test-perceus-1130-borrow-pipe-native test-perceus-1130-hof-tail-drop test-perceus-1130-hof-tail-drop-native test-perceus-1126-read-loop-drop-native test-perceus-1127-borrow-closure test-perceus-1127-borrow-closure-native test-perceus-1127-borrow-hof test-perceus-1117-native-map-pipe-record-leak test-perceus-860-native-cons-leak test-perceus-1048-native-record-leak test-perceus-1112-native-discard-decref test-perceus-747-native-real-slot-uaf test-perceus-872-native-variant-reuse-leak test-perceus-1053-native-trmc-spine-reuse test-perceus-1053-nested-rotation-reuse test-perceus-1104-balance-token-donation test-perceus-1164-cross-call-donation test-perceus-1104-sibling-param-move test-perceus-1136-sized-variant test-perceus-1136-sized-variant-native test-perceus-1156-raw-widen test-perceus-1156-raw-widen-native test-perceus-1160-de-opaque test-perceus-1160-de-opaque-native test-perceus-882-native-rotation-reuse-crash test-perceus-995-native-shared-reuse-corruption test-perceus-995-native-dead-donor-reuse test-perceus-1069-projection-reuse test-perceus-1083-variant-spine-free test-native-1083-inline-gate test-native-1083-field-read test-native-static-slot-read test-native-1110-reuse-int-slot test-native-1229-arm-int-binder test-native-1229-writeside test-issue-892-mono-arg-unbox test-heap-limit test-unions test-aliases test-sequence test-ufcs test-rc-trace-callsite selfhost test-tco-rule3-basic test-tco-rule3-no-misfire test-tco-issue-680 test-issue-678-stdout-buffered test-issue-679-cancel-parked test-issue-682-cancel-sibling-handler test-issue-978-spawn-parent-handled-effect test-issue-1051-spawn-parent-handled-builtin-default test-issue-1067-fiber-trap-isolation test-issue-1331-op-arg-release test-issue-1093-divzero-trap test-issue-997-spawn-cell-fiber-local test-violations test-attributes test-doc-attr test-shadowing test-modular-build test-modular-identity test-modular-cache test-modular-hermetic test-native-core-obj test-native-dead-strip test-modular-selfhost kaic2-fast kaic2-fast-verify FORCE test-issue-963-cross-module-mono test-home-tu-partition test-native-modular test-packages test-kir test-user-cache test-core-cache test-independence-oracle test-batch-isolation test-native-selfhost-gate rc-detector test-build-modes test-perceus-1180-range-lazy test-perceus-1180-range-lazy-native test-perceus-1180-range-lazy-asan test-perceus-1180-range-sum test-perceus-1180-range-sum-native test-shape-fusion-1200 test-shape-laws-1200 test-mn-native-soundness test-mn-c-clang-soundness
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
$(TARGET): build/stage2.c $(RUNTIME_H)
ifeq ($(KAI_LLVM),1)
	@echo "building stage2: native backend ENABLED ($(LLVM_CONFIG_RESOLVED))"
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
	@# Terminal banner, deliberately the last line of the build: the P2
	@# opt-out notice above scrolls past mid-run, and a dev who misses it
	@# emits native code several times slower than CI and the release do on
	@# heap-bound work, without ever being told.
	@echo "building stage2: native P2 bitcode: $$(../tools/gen-runtime-bc.sh --status-line)"
endif

# --- Dev fast rebuild: whole-bundle at -O0 ----------------------------
# Same trust path as kaic2 (kaic1 regenerates stage2.c from the bundle),
# but cc compiles the 17 MB TU at -O0 instead of -O2. The cc step drops
# from ~70 s to ~5 s; the resulting binary self-compiles slower, so this
# is for the edit-check loop only, never for CI or the selfhost oracle.
CFLAGS_DEV := $(filter-out -O2 -O1,$(CFLAGS)) -O0
kaic2-dev: build/stage2.c $(RUNTIME_H)
	$(CC) $(CFLAGS_DEV) $(CPPFLAGS_CORE) $(LLVM_CPPFLAGS) $(LDFLAGS_STACK) -I ../stage0 $< -o $@ $(LDLIBS_M) $(LLVM_LDFLAGS)
	@echo "kaic2-dev OK (-O0; edit-loop binary, not for CI/selfhost)"

# --- Dev fast rebuild: modular self-compile ---------------------------
# An existing kaic2 recompiles the compiler from main.kai directly,
# resolving imports — no kaic1, no bundle. Rides bin/kai's c-modular
# path: per-module TUs compiled in parallel plus the .o content-hash
# cache, so an edit to one module recompiles one TU. C backend only.
# This is a REBUILD convenience: the bootstrap chain (`make kaic2`)
# remains the trust path for fresh machines, CI, and the selfhost
# oracle. The stdlib path is baked in exactly as the bootstrap build
# does, so the result resolves core imports with no --path flags.
KAIC2_FAST_STAGING := build/kaic2-fast.bin
KAIC2_FAST_CFLAGS  := $(CFLAGS) -DKAI_STDLIB_PATH=\"$(STDLIB_ABS)\"

$(KAIC2_FAST_STAGING): FORCE | build
	@set -e; \
	[ -x ./$(TARGET) ] || { echo "kaic2-fast: no existing ./$(TARGET) — bootstrap first with 'make kaic2'"; exit 1; }; \
	out=$$(pwd)/$@; rm -f $$out; \
	( cd .. && KAI_BACKEND=c KAI_MODULAR=1 KAI_MODULAR_STATS=1 \
	    CFLAGS="$(KAIC2_FAST_CFLAGS)" \
	    bin/kai build stage2/$(SRC) -o $$out )

# The staging binary is swapped in only after a sanity gate (--version
# plus a golden demo compiled with NO flags, exercising the baked
# stdlib path), so a broken build never clobbers the working kaic2.
kaic2-fast: $(KAIC2_FAST_STAGING)
	@set -e; \
	./$(KAIC2_FAST_STAGING) --version | grep -q "self-hosted" \
	  || { echo "kaic2-fast FAIL (--version) — keeping existing ./$(TARGET)"; exit 1; }; \
	./$(KAIC2_FAST_STAGING) ../examples/portfolio/portfolio.kai > build/kaic2-fast-smoke.c \
	  || { echo "kaic2-fast FAIL (smoke compile) — keeping existing ./$(TARGET)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) $(LDFLAGS_STACK) -I ../stage0 build/kaic2-fast-smoke.c -o build/kaic2-fast-smoke $(LDLIBS_M); \
	./build/kaic2-fast-smoke > build/kaic2-fast-smoke.out; \
	diff -q ../examples/portfolio/portfolio.out.expected build/kaic2-fast-smoke.out > /dev/null \
	  || { echo "kaic2-fast FAIL (smoke DIFF) — keeping existing ./$(TARGET)"; diff ../examples/portfolio/portfolio.out.expected build/kaic2-fast-smoke.out; exit 1; }; \
	mv $(KAIC2_FAST_STAGING) $(TARGET); \
	echo "kaic2-fast OK — ./$(TARGET) rebuilt via modular self-compile (C backend)"

# Equivalence gate for the fast path. Precondition: ./kaic2 was built
# from the CURRENT source (bootstrap or a previous kaic2-fast). The
# fast-built staging binary must emit byte-identical C to ./kaic2 for
# the compiler itself and for a sample program. Byte-identity of the
# BINARIES is out of reach (multi-.o parallel link vs one -O2 TU);
# identical emitted C from the same source is the functional gate.
# Does not touch ./kaic2.
kaic2-fast-verify: $(KAIC2_FAST_STAGING)
	@set -e; \
	./$(TARGET) $(SRC) > build/kaic2-fast-ref.c; \
	./$(KAIC2_FAST_STAGING) $(SRC) > build/kaic2-fast-new.c; \
	diff -q build/kaic2-fast-ref.c build/kaic2-fast-new.c > /dev/null \
	  && echo "kaic2-fast-verify: self-compile OK (emitted C byte-identical)" \
	  || { echo "kaic2-fast-verify: self-compile DIFF — is ./$(TARGET) built from the current source?"; exit 1; }; \
	./$(TARGET) ../examples/portfolio/portfolio.kai > build/kaic2-fast-ref-smoke.c; \
	./$(KAIC2_FAST_STAGING) ../examples/portfolio/portfolio.kai > build/kaic2-fast-new-smoke.c; \
	diff -q build/kaic2-fast-ref-smoke.c build/kaic2-fast-new-smoke.c > /dev/null \
	  && echo "kaic2-fast-verify OK (compiler + sample emitted C byte-identical)" \
	  || { echo "kaic2-fast-verify: sample DIFF"; exit 1; }

FORCE:

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
	  ./$(TARGET) $(TEST_CORE_CACHE) --infer $$f > build/s2-$$name.infer 2> build/s2-$$name.infer.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --dump-mono $$f > build/s2-$$name.mono 2> build/s2-$$name.mono.err; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --dump-typed $$f > build/s2-$$name.typed 2> build/s2-$$name.typed.err; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --dump-rawsafe ../examples/perceus/$$name.kai 2> build/$$name.rawsafe.err \
	    | grep -E "^($$pat) (load|store)" > build/$$name.rawsafe || true; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.rawsafe > /dev/null \
	    && echo "rawsafe OK $$name (use-analysis marks match golden)" \
	    || { echo "rawsafe DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.rawsafe; exit 1; }; \
	done

# Issue #1139 — confirmed-purity analysis (--dump-purity). The golden
# pins the root file's per-fn verdict (`<name> -> const|impure`); stdlib
# marks are filtered by the dumper, so a stdlib edit cannot perturb it.
# Verifies the load-bearing shapes: raw-scalar arithmetic (incl. divide
# and a recursive self-call) confirms `const`, an interprocedural call
# to a `const` callee stays `const`, and a body that allocates / holds a
# boxed param / performs an effect stays `impure`. Pure analysis; no
# runtime and no emitted text change on this path.
test-purity: $(TARGET) build
	@set -e; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --dump-purity ../examples/perceus/purity_scalar.kai 2> build/purity_scalar.err \
	  > build/purity_scalar.out || true; \
	diff -q ../examples/perceus/purity_scalar.out.expected build/purity_scalar.out > /dev/null \
	  && echo "purity OK purity_scalar (confirmation marks match golden)" \
	  || { echo "purity DIFF purity_scalar"; diff ../examples/perceus/purity_scalar.out.expected build/purity_scalar.out; exit 1; }

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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --dump-borrowsafe ../examples/perceus/$$name.kai 2> build/$$name.borrowsafe.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --check $$f > build/s2-$$name.check 2> build/s2-$$name.check.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/run-$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --test $$f > build/$$name-t.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/modules/main.kai > build/modules.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$src > build/mq-$$d.c \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$src > /dev/null 2> build/mq-neg-$$d.err || rc=$$?; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../examples/modules/$$d $$src > build/mc-$$d.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../examples/modules/$$d --path ../stdlib $$src > build/mde-$$d.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../examples/modules/$$d $$src > build/mtp-$$d.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../examples/modules/$$d $$src > build/muomp-$$d.c \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../examples/modules/$$d $$src > /dev/null 2> build/muomp-$$d.err || rc=$$?; \
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
	rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$MAIN > /dev/null 2> build/modules-path.miss.err || rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "modules-path FAIL (expected error without --path)"; exit 1; \
	fi; \
	grep -q "cannot open module" build/modules-path.miss.err \
	  || { echo "modules-path FAIL (wrong error message)"; cat build/modules-path.miss.err; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --path $$LIB $$MAIN > build/modules-path.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/loop/main.kai > build/loop.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/reader/main.kai > build/reader.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/reader.c -o build/reader-c $(LDLIBS_M) ; \
	./build/reader-c > build/reader.out; \
	diff -q ../examples/reader/main.out.expected build/reader.out > /dev/null \
	  && echo "reader OK" \
	  || { echo "reader DIFF"; diff ../examples/reader/main.out.expected build/reader.out; exit 1; }

test-writer: $(TARGET) build
	@set -e; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/writer/main.kai > build/writer.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/foreign_prelude.kai > build/foreign_prelude.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/cap_read_in_interp.kai > build/cap_read_in_interp.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/cap_read_in_interp.c -o build/cap_read_in_interp-c $(LDLIBS_M) ; \
	./build/cap_read_in_interp-c > build/cap_read_in_interp.out; \
	diff -q ../examples/effects/cap_read_in_interp.out.expected build/cap_read_in_interp.out > /dev/null \
	  && echo "issue-672 OK (@cap in interp resolves)" \
	  || { echo "issue-672 DIFF"; diff ../examples/effects/cap_read_in_interp.out.expected build/cap_read_in_interp.out; exit 1; }

# A reassigned record `var` whose field is read inside a `#{...}`
# interpolation. Guards two coupled paths: the op-label dedup that must
# thread a `State.get` payload from the earlier `State.set`, and the interp
# span scan that must not skip the second of two adjacent `#{...}#{...}`.
test-issue-1194-reassigned-record-field-in-interp: $(TARGET) build
	@set -e; \
	f=issue_1194_reassigned_record_field_in_interp; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$f.kai > build/$$f.c \
	  || { echo "issue-1194 FAIL (kaic2)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) \
	  || { echo "issue-1194 FAIL (cc)"; exit 1; }; \
	./build/$$f-c > build/$$f.out \
	  || { echo "issue-1194 FAIL (run)"; exit 1; }; \
	diff -q ../examples/effects/$$f.out.expected build/$$f.out > /dev/null \
	  && echo "issue-1194 OK (reassigned record field in interp resolves)" \
	  || { echo "issue-1194 DIFF"; diff ../examples/effects/$$f.out.expected build/$$f.out; exit 1; }

# A clause that discards `resume` abandons the handle body by longjmping
# to its handle, invalidating every evidence node the body pushed — the
# abandoning handle's own included. Popping a single entry unwinds
# correctly only when no other handler frame sits on top; with a resumed
# frame in between, the dead node stays linked as the evidence top and the
# next handle to reuse that stack storage becomes its own parent, cycling
# every later lookup. Both backends emitted the lone pop, so the
# native-vs-C parity sweep cannot see this shape — it needs a golden.
#
# The run is bounded through `kai_timeout`: the regression is a HANG, and
# this gate must fail the shard rather than stall it on a host with no
# GNU coreutils (the platform the bug was reported on).
test-issue-1263-abandon-across-resumed-frame: $(TARGET) build
	@set -e; \
	. ../tools/lib/timeout.sh; \
	f=issue_1263_abandon_across_resumed_frame; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$f.kai > build/s2-$$f.c \
	  || { echo "issue-1263 FAIL (kaic2)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$f.c -o build/s2-$$f-c $(LDLIBS_M) \
	  || { echo "issue-1263 FAIL (cc)"; exit 1; }; \
	kai_timeout 30 ./build/s2-$$f-c > build/s2-$$f-c.out \
	  || { echo "issue-1263 FAIL (C run — hang or nonzero exit)"; exit 1; }; \
	diff -q ../examples/effects/$$f.out.expected build/s2-$$f-c.out > /dev/null \
	  && echo "issue-1263 OK (C: abandon unwinds evidence past the resumed frame)" \
	  || { echo "issue-1263 DIFF (C)"; diff ../examples/effects/$$f.out.expected build/s2-$$f-c.out; exit 1; }

# Native half of the same gate — the backend the hang was reported on.
test-issue-1263-abandon-across-resumed-frame-native: $(TARGET) build
	@set -e; \
	. ../tools/lib/timeout.sh; \
	f=issue_1263_abandon_across_resumed_frame; \
	KAI_BACKEND=native ../bin/kai build ../examples/effects/$$f.kai -o build/s2-$$f-nat 2> build/s2-$$f-nat.err \
	  || { echo "issue-1263-native FAIL (native build errored)"; cat build/s2-$$f-nat.err; exit 1; }; \
	kai_timeout 30 ./build/s2-$$f-nat > build/s2-$$f-nat.out \
	  || { echo "issue-1263-native FAIL (run — hang or nonzero exit)"; exit 1; }; \
	diff -q ../examples/effects/$$f.out.expected build/s2-$$f-nat.out > /dev/null \
	  && echo "issue-1263-native OK (abandon unwinds evidence past the resumed frame)" \
	  || { echo "issue-1263-native DIFF"; diff ../examples/effects/$$f.out.expected build/s2-$$f-nat.out; exit 1; }

# m12.7.x FFI v1 regression — `[<extern_c>]` axiom lowers to a
# direct `extern "C"` call. Compiles + runs the fixture on the C
# backend so a regression on the FFI path (parser, lower_axiom_one,
# perceus skip, emit_fn_body detection) shows up under tier1. The
# fixture stays libc-only so it links without extra `cc` flags.
test-ffi-extern-c: $(TARGET) build
	@set -e; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/ffi_extern_c_basic.kai > build/ffi_extern_c_basic.c; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage2 -I ../stage0 build/ffi_extern_c_basic.c -o build/ffi_extern_c_basic-c $(LDLIBS_M) ; \
	./build/ffi_extern_c_basic-c > build/ffi_extern_c_basic.out; \
	diff -q ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic.out > /dev/null \
	  && echo "ffi-extern-c OK (C backend)" \
	  || { echo "ffi-extern-c DIFF (C backend)"; diff ../examples/effects/ffi_extern_c_basic.out.expected build/ffi_extern_c_basic.out; exit 1; }

# Numeric lane A (#514) — fixed-width Int32/UInt32/UInt64/Int128. The
# two pure fixtures (Int128 arithmetic past 2^63, every width + Show/Eq/
# Ord) run libc-only; the honest FFI fixture links the standalone
# ffi_fixed_shim.c (int32_t/uint32_t/uint64_t at the boundary). The
# negative fixture proves the widths are kind-distinct from Int (an
# Int32 value rejected against an Int annotation). C backend; the native
# parity is covered by the backend-parity harness.
test-numeric-fixed-width: $(TARGET) build
	@set -e; \
	for f in int128_overflow fixed_width_protocols; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) ../examples/numeric/$$f.kai > build/$$f.c; \
	  $(CC) $(CFLAGS) -Wno-absolute-value -I ../stage2 -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M); \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q ../examples/numeric/$$f.out.expected build/$$f.out > /dev/null \
	    && echo "numeric $$f OK (C backend)" \
	    || { echo "numeric $$f DIFF"; diff ../examples/numeric/$$f.out.expected build/$$f.out; exit 1; }; \
	done; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/numeric/ffi_fixed_honest.kai > build/ffi_fixed_honest.c; \
	$(CC) $(CFLAGS) -Wno-absolute-value -I ../stage2 -I ../stage0 build/ffi_fixed_honest.c ../examples/numeric/ffi_fixed_shim.c -o build/ffi_fixed_honest-c $(LDLIBS_M); \
	./build/ffi_fixed_honest-c > build/ffi_fixed_honest.out; \
	diff -q ../examples/numeric/ffi_fixed_honest.out.expected build/ffi_fixed_honest.out > /dev/null \
	  && echo "numeric ffi_fixed_honest OK (C backend)" \
	  || { echo "numeric ffi_fixed_honest DIFF"; diff ../examples/numeric/ffi_fixed_honest.out.expected build/ffi_fixed_honest.out; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/numeric/kind_mismatch.err.kai > build/kind_mismatch.out 2>&1 \
	  && { echo "kind_mismatch DID NOT FAIL — Int32 must not unify with Int"; exit 1; } \
	  || true; \
	grep -F "$$(head -1 ../examples/numeric/kind_mismatch.err.expected)" build/kind_mismatch.out > /dev/null \
	  && echo "numeric kind_mismatch OK (kind-distinct rejection)" \
	  || { echo "numeric kind_mismatch DIFF — expected type-mismatch diagnostic"; cat build/kind_mismatch.out; exit 1; }

# Pure-kaikai BigInt (numeric lane B) — C backend. Each fixture compiles
# to C, runs, and diffs against its golden. Verifies arbitrary-precision
# arithmetic beyond 2^64, factorials, the i64<->heap border, the `99n`
# literal suffix, and error paths (bad parse / divide-by-zero -> None).
# Native parity is gated separately in test-numeric-bigint-native.
test-numeric-bigint: $(TARGET) build
	@set -e; \
	for f in bigint_arith bigint_factorial bigint_border bigint_literal bigint_errors; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/$$f.kai > build/$$f.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M); \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q ../examples/numeric/$$f.out.expected build/$$f.out > /dev/null \
	    && echo "bigint $$f OK (C backend)" \
	    || { echo "bigint $$f DIFF"; diff ../examples/numeric/$$f.out.expected build/$$f.out; exit 1; }; \
	done; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/int_overflow.err.kai > build/int_overflow.out 2>&1 \
	  && { echo "int_overflow DID NOT FAIL — Int literal past i64 must diagnose"; exit 1; } \
	  || true; \
	grep -F "$$(head -1 ../examples/numeric/int_overflow.err.expected)" build/int_overflow.out > /dev/null \
	  && echo "bigint int_overflow OK (out-of-range Int rejection)" \
	  || { echo "bigint int_overflow DIFF — expected out-of-range diagnostic"; cat build/int_overflow.out; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/hex_literal.kai > build/hex_literal.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/hex_literal.c -o build/hex_literal-c $(LDLIBS_M); \
	./build/hex_literal-c > build/hex_literal.out; \
	diff -q ../examples/numeric/hex_literal.out.expected build/hex_literal.out > /dev/null \
	  && echo "hex_literal OK (C backend)" \
	  || { echo "hex_literal DIFF"; diff ../examples/numeric/hex_literal.out.expected build/hex_literal.out; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/hex_overflow.err.kai > build/hex_overflow.out 2>&1 \
	  && { echo "hex_overflow DID NOT FAIL — hex literal past 64 bits must diagnose"; exit 1; } \
	  || true; \
	grep -F "$$(head -1 ../examples/numeric/hex_overflow.err.expected)" build/hex_overflow.out > /dev/null \
	  && echo "hex_overflow OK (over-width hex rejection)" \
	  || { echo "hex_overflow DIFF — expected over-width diagnostic"; cat build/hex_overflow.out; exit 1; }

# Pure-kaikai BigInt — NATIVE backend parity. Same fixtures and goldens,
# built with the in-process libLLVM backend. Native-gated: a C-only
# kaic2 cannot run --backend=native, so this is invoked from the
# tier1-native workflow, not the C-backend tier1 suite.
test-numeric-bigint-native: $(TARGET) build
	@set -e; \
	for f in bigint_arith bigint_factorial bigint_border bigint_literal bigint_errors; do \
	  KAI_BACKEND=native ../bin/kai build ../examples/numeric/$$f.kai -o build/$$f-nat 2> build/$$f-nat.err \
	    || { echo "bigint $$f FAIL (native build errored)"; cat build/$$f-nat.err; exit 1; }; \
	  ./build/$$f-nat > build/$$f-nat.out; \
	  diff -q ../examples/numeric/$$f.out.expected build/$$f-nat.out > /dev/null \
	    && echo "bigint $$f OK (native backend)" \
	    || { echo "bigint $$f DIFF (native)"; diff ../examples/numeric/$$f.out.expected build/$$f-nat.out; exit 1; }; \
	done; \
	KAI_BACKEND=native ../bin/kai build ../examples/numeric/hex_literal.kai -o build/hex_literal-nat 2> build/hex_literal-nat.err \
	  || { echo "hex_literal FAIL (native build errored)"; cat build/hex_literal-nat.err; exit 1; }; \
	./build/hex_literal-nat > build/hex_literal-nat.out; \
	diff -q ../examples/numeric/hex_literal.out.expected build/hex_literal-nat.out > /dev/null \
	  && echo "hex_literal OK (native backend)" \
	  || { echo "hex_literal DIFF (native)"; diff ../examples/numeric/hex_literal.out.expected build/hex_literal-nat.out; exit 1; }

# DecimalBig + Rational (numeric lane D) — C backend. Each fixture
# compiles to C, runs, and diffs against its golden. Verifies a
# BigInt-carrier decimal past the ~38-digit Int128 ceiling and exact
# rationals with gcd normalisation. Native parity is in
# test-numeric-decimal-rational-native.
test-numeric-decimal-rational: $(TARGET) build
	@set -e; \
	for f in decimal_big_demo rational_demo big_numeric_operators complex_div_operator decimal_operators; do \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/$$f.kai > build/$$f.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M); \
	./build/$$f-c > build/$$f.out; \
	diff -q ../examples/numeric/$$f.out.expected build/$$f.out > /dev/null \
	&& echo "decimal-rational $$f OK (C backend)" \
	|| { echo "decimal-rational $$f DIFF"; diff ../examples/numeric/$$f.out.expected build/$$f.out; exit 1; }; \
	done

# DecimalBig + Rational — NATIVE backend parity. Same fixtures and
# goldens, built with the in-process libLLVM backend. Native-gated;
# invoked from the tier1-native workflow.
test-numeric-decimal-rational-native: $(TARGET) build
	@set -e; \
	for f in decimal_big_demo rational_demo big_numeric_operators complex_div_operator decimal_operators; do \
	KAI_BACKEND=native ../bin/kai build ../examples/numeric/$$f.kai -o build/$$f-nat 2> build/$$f-nat.err \
	|| { echo "decimal-rational $$f FAIL (native build errored)"; cat build/$$f-nat.err; exit 1; }; \
	./build/$$f-nat > build/$$f-nat.out; \
	diff -q ../examples/numeric/$$f.out.expected build/$$f-nat.out > /dev/null \
	&& echo "decimal-rational $$f OK (native backend)" \
	|| { echo "decimal-rational $$f DIFF (native)"; diff ../examples/numeric/$$f.out.expected build/$$f-nat.out; exit 1; }; \
	done

# `/` on the non-primitive numeric types traps on a zero divisor — the
# same failure class as an out-of-range index (a program bug, not an
# Option to unpack). Each fixture prints one line, then divides by a
# runtime zero: exit must be non-zero, stdout must match the golden
# (only the pre-trap line), and stderr must carry `trap: divide by
# zero`. The opt-in Option-returning `checked_div`/`div` is exercised by
# the positive fixtures.
test-numeric-div-by-zero-traps: $(TARGET) build
	@set -e; \
	for name in bigint_div_by_zero_traps rational_div_by_zero_traps \
	            decimal_big_div_by_zero_traps decimal_div_by_zero_traps; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	  rc=0; ./build/$$name > build/$$name.out 2> build/$$name.err || rc=$$?; \
	  [ $$rc -ne 0 ] || { echo "div-trap FAIL $$name (exit 0 — did not trap)"; cat build/$$name.out; exit 1; }; \
	  diff -q ../examples/numeric/$$name.out.expected build/$$name.out > /dev/null \
	    || { echo "div-trap DIFF $$name (stdout)"; diff ../examples/numeric/$$name.out.expected build/$$name.out; exit 1; }; \
	  grep -q "trap: divide by zero" build/$$name.err \
	    && echo "div-trap OK $$name (traps: divide by zero)" \
	    || { echo "div-trap FAIL $$name (want 'trap: divide by zero' on stderr)"; cat build/$$name.err; exit 1; }; \
	done

# Same fixtures, NATIVE backend parity. Native-gated; invoked from the
# tier1-native workflow.
test-numeric-div-by-zero-traps-native: $(TARGET) build
	@set -e; \
	for name in bigint_div_by_zero_traps rational_div_by_zero_traps \
	            decimal_big_div_by_zero_traps decimal_div_by_zero_traps; do \
	  KAI_BACKEND=native ../bin/kai build ../examples/numeric/$$name.kai -o build/$$name-nat 2> build/$$name-nat.buildlog \
	    || { echo "div-trap FAIL $$name (native build errored)"; cat build/$$name-nat.buildlog; exit 1; }; \
	  rc=0; ./build/$$name-nat > build/$$name-nat.out 2> build/$$name-nat.err || rc=$$?; \
	  [ $$rc -ne 0 ] || { echo "div-trap FAIL $$name (native, exit 0 — did not trap)"; cat build/$$name-nat.out; exit 1; }; \
	  diff -q ../examples/numeric/$$name.out.expected build/$$name-nat.out > /dev/null \
	    || { echo "div-trap DIFF $$name (native stdout)"; diff ../examples/numeric/$$name.out.expected build/$$name-nat.out; exit 1; }; \
	  grep -q "trap: divide by zero" build/$$name-nat.err \
	    && echo "div-trap OK $$name (native, traps: divide by zero)" \
	    || { echo "div-trap FAIL $$name (native, want 'trap: divide by zero' on stderr)"; cat build/$$name-nat.err; exit 1; }; \
	done

# Context-driven numeric-literal minting — C backend. Positive fixtures
# coin literals to the annotated carrier (exact Decimal/DecimalBig,
# BigInt, Byte/Decimal suffixes) and check the folded value; the
# principality fixture confirms a literal at a type variable still
# defaults to Int. Negative fixtures assert the fold-time errors
# (point-to-int, Byte/u8/Decimal overflow).
test-numeric-literal-mint: $(TARGET) build
	@set -e; \
	for f in literal_mint literal_suffix literal_principality; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/$$f.kai > build/$$f.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M); \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q ../examples/numeric/$$f.out.expected build/$$f.out > /dev/null \
	    && echo "literal-mint $$f OK (C backend)" \
	    || { echo "literal-mint $$f DIFF"; diff ../examples/numeric/$$f.out.expected build/$$f.out; exit 1; }; \
	done; \
	for f in mint_int_from_point mint_int_from_whole_point mint_byte_overflow mint_u8_suffix_overflow mint_decimal_overflow; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/numeric/$$f.err.kai > build/$$f.out 2>&1 \
	    && { echo "literal-mint $$f DID NOT FAIL"; exit 1; } \
	    || true; \
	  grep -F "$$(head -1 ../examples/numeric/$$f.err.expected)" build/$$f.out > /dev/null \
	    && echo "literal-mint $$f OK (rejection)" \
	    || { echo "literal-mint $$f DIFF — expected fold-time error"; cat build/$$f.out; exit 1; }; \
	done

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
	./$(TARGET) $(TEST_CORE_CACHE) --backend=c ../examples/ffi/v2_struct_by_value.kai > build/v2_struct.stream; \
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
	./$(TARGET) $(TEST_CORE_CACHE) --backend=c ../examples/ffi/v2_opaque_handle.kai > build/v2_opaque.c; \
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

# FFI v2 (#1030) — native backend marshals struct-by-value with the
# platform C-ABI classification done in the emitter (AAPCS64 / x86-64
# SysV). Both struct fixtures compile via `--emit=native` and link against
# a CLANG-compiled callee that names the real C struct types — the parity
# gate that proves the emitter's coercion matches the C compiler's actual
# ABI, not just its own C backend. Gated on KAI_LLVM (the native backend
# must be built). A target the classifier does not model keeps the honest
# `--backend=c` reject (emit_native_ffi's `ffi_reject_struct`).
ifeq ($(KAI_LLVM),1)
test-ffi-v2-native-struct: $(TARGET) build
	@set -e; \
	for pair in "v2_struct_by_value v2_struct_native_shim.c" "v2_struct_adv v2_struct_adv_shim.c"; do \
	  set -- $$pair; f=$$1; shim=$$2; \
	  cp ../examples/ffi/$$f.kai build/$$f.kai; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --emit=native --path ../stdlib --path ../examples/ffi build/$$f.kai > build/$$f-nat.err 2>&1 \
	    || { echo "ffi-v2-native-struct $$f EMIT FAIL"; cat build/$$f-nat.err; exit 1; }; \
	  $(CC) -c ../examples/ffi/$$shim -o build/$$f-shim.o; \
	  $(CC) $(CFLAGS) build/$$f.o build/$$f-shim.o ../stage0/runtime_llvm.c -I ../stage2 -I ../stage0 -o build/$$f-nat $(LDLIBS_M) -ldl; \
	  ./build/$$f-nat > build/$$f-nat.out; \
	  diff -q ../examples/ffi/$$f.out.expected build/$$f-nat.out > /dev/null \
	    && echo "ffi-v2-native-struct $$f OK (native == clang ABI)" \
	    || { echo "ffi-v2-native-struct $$f DIFF"; diff ../examples/ffi/$$f.out.expected build/$$f-nat.out; exit 1; }; \
	done
else
test-ffi-v2-native-struct:
	@echo "ffi-v2-native-struct SKIP (C-only build; native backend not compiled)"
endif

# A `.c` shim supplied via CFLAGS (the documented FFI-shim pattern) must ride
# the LINK line: as an extra input next to `-c … -o` clang rejects it
# ("cannot specify -o when generating multiple output files"). Exercises the
# bin/kai wrapper end-to-end, not raw kaic2 — the routing lives in the wrapper.
test-ffi-cflags-shim: $(TARGET) build
	@set -e; \
	CFLAGS="../examples/ffi/cflags_shim.c" ../bin/kai build --backend=c ../examples/ffi/cflags_shim_link.kai -o build/cflags-shim-c 2> build/cflags-shim-c.err \
	  || { echo "ffi-cflags-shim BUILD FAIL (C backend)"; cat build/cflags-shim-c.err; exit 1; }; \
	./build/cflags-shim-c > build/cflags-shim-c.out; \
	diff -q ../examples/ffi/cflags_shim_link.out.expected build/cflags-shim-c.out > /dev/null \
	  && echo "ffi-cflags-shim OK (C backend)" \
	  || { echo "ffi-cflags-shim DIFF (C backend)"; diff ../examples/ffi/cflags_shim_link.out.expected build/cflags-shim-c.out; exit 1; }

ifeq ($(KAI_LLVM),1)
test-ffi-cflags-shim-native: $(TARGET) build
	@set -e; \
	CFLAGS="../examples/ffi/cflags_shim.c" KAI_BACKEND=native ../bin/kai build ../examples/ffi/cflags_shim_link.kai -o build/cflags-shim-nat 2> build/cflags-shim-nat.err \
	  || { echo "ffi-cflags-shim BUILD FAIL (native)"; cat build/cflags-shim-nat.err; exit 1; }; \
	./build/cflags-shim-nat > build/cflags-shim-nat.out; \
	diff -q ../examples/ffi/cflags_shim_link.out.expected build/cflags-shim-nat.out > /dev/null \
	  && echo "ffi-cflags-shim OK (native)" \
	  || { echo "ffi-cflags-shim DIFF (native)"; diff ../examples/ffi/cflags_shim_link.out.expected build/cflags-shim-nat.out; exit 1; }
else
test-ffi-cflags-shim-native:
	@echo "ffi-cflags-shim-native SKIP (C-only build; native backend not compiled)"
endif

# Native separate compilation (#989/#963): the whole-program KIR is partitioned
# into one object per module + the root, each content-addressed, then linked
# with the runtime owner. Three assertions from one fixture (mono cross-module,
# effects, cross-module fiber): (1) the modular binary is byte-identical to the
# whole-program native build — the partition is a projection, not a re-compile;
# (2) a body-only edit re-emits exactly ONE object, the rest are cache hits —
# the content-addressed incremental; (3) a producer's signature change re-emits
# its consumer — the anti-ABI-skew guarantee (a changed callee shape rewrites
# the caller's key). Gated on KAI_LLVM (the native backend must be built).
ifeq ($(KAI_LLVM),1)
# Issue #1234 — M:N native soundness. The work-stealing TLS-hoist miscompile is
# NATIVE-only (the C backend + TSAN gates are sound and never saw it, which is
# how it shipped): clang -O2 caches the thread pointer across swapcontext, so a
# work-stolen fiber reads the creator thread's scheduler state. This gate builds
# the cross-thread stress fixture with --backend=native and loops it under
# KAI_THREADS=4 and 8 — a sound runtime crashes 0 times every run. Self-skips on
# a C-only kaic2. Rides tier1-native (needs the libLLVM native backend).
MN_SOUNDNESS_ITERS ?= 40

# Repeats for the trampoline-tail identity fixture inside test-effects. The
# window is narrow (~1 run in 4 unfixed) and each run costs a few ms, so the
# count buys a decisive signal for free.
MN_TAIL_STEAL_ITERS ?= 40

test-mn-native-soundness: $(TARGET)
	@../tools/run-mn-native-soundness.sh $(MN_SOUNDNESS_ITERS)

# C-backend soundness under clang (issue #1238). The native gate above runs
# --backend=native only; this one builds the same stress fixture on the C
# backend with CC=clang — both `bin/kai` link shapes (single-TU + KAI_MODULAR)
# plus a raw single-TU -O2 build with no scheduler owner object, the shape
# these recipes themselves use. Self-skips on a clang-less host. Loops
# KAI_THREADS=4/8.
test-mn-c-clang-soundness: $(TARGET)
	@../tools/run-mn-c-clang-soundness.sh $(MN_SOUNDNESS_ITERS)

test-native-modular: $(TARGET) build
	@set -e; \
	fx=../examples/multi-module/issue-989-native-modular; \
	work=build/nmod; rm -rf $$work; mkdir -p $$work; cp $$fx/*.kai $$work/; \
	cache=build/nmod-cache; rm -rf $$cache; mkdir -p $$cache; \
	emit() { ./$(TARGET) --emit=native-modular --path ../stdlib --path $$work $$work/main.kai 2>build/nmod.err; }; \
	link() { $(CC) $(CFLAGS) $(LDFLAGS_STACK) $$1 ../stage0/runtime_llvm.c -I ../stage2 -I ../stage0 -o $$2 $(LDLIBS_M) -ldl; }; \
	./$(TARGET) --emit=native --path ../stdlib --path $$work $$work/main.kai >/dev/null 2>build/nmod-whole.err \
	  || { echo "native-modular: whole-program EMIT FAIL"; cat build/nmod-whole.err; exit 1; }; \
	link "$$work/main.o" build/nmod-whole; ./build/nmod-whole > build/nmod-whole.out; \
	emit | sort > build/nmod-objs1.txt \
	  || { echo "native-modular: EMIT FAIL"; cat build/nmod.err; exit 1; }; \
	[ -s build/nmod-objs1.txt ] || { echo "native-modular: no objects"; cat build/nmod.err; exit 1; }; \
	link "$$(tr '\n' ' ' < build/nmod-objs1.txt)" build/nmod-prog; ./build/nmod-prog > build/nmod.out; \
	diff -q $$fx/main.out.expected build/nmod.out > /dev/null \
	  || { echo "native-modular: modular OUT != expected"; diff $$fx/main.out.expected build/nmod.out; exit 1; }; \
	diff -q build/nmod-whole.out build/nmod.out > /dev/null \
	  && echo "native-modular OK: modular == whole-program (byte-identical)" \
	  || { echo "native-modular: modular != whole-program"; diff build/nmod-whole.out build/nmod.out; exit 1; }; \
	n1=$$(wc -l < build/nmod-objs1.txt); \
	printf 'pub fn total(xs: [Int]) : Int = fold_add(xs, 0)\nfn fold_add(xs: [Int], acc: Int) : Int = match xs {\n  [] -> acc\n  [x, ...rest] -> fold_add(rest, x + acc)\n}\npub fn pick[T](a: T, b: T) : T = a\n' > $$work/mathx.kai; \
	emit | sort > build/nmod-objs2.txt; \
	changed=$$(comm -13 build/nmod-objs1.txt build/nmod-objs2.txt | wc -l | tr -d ' '); \
	hits=$$(comm -12 build/nmod-objs1.txt build/nmod-objs2.txt | wc -l | tr -d ' '); \
	[ "$$changed" = "1" ] \
	  && echo "native-modular OK: body edit re-emitted 1 object, $$hits cache hits (of $$n1)" \
	  || { echo "native-modular: body edit re-emitted $$changed objects (want 1)"; exit 1; }; \
	printf 'pub fn total(xs: [Int]) : Int = fold_add(xs, 0)\nfn fold_add(xs: [Int], acc: Int) : Int = match xs {\n  [] -> acc\n  [x, ...rest] -> fold_add(rest, acc + x)\n}\npub fn pick[T](a: T, b: T, c: T) : T = a\n' > $$work/mathx.kai; \
	sed 's/mathx.pick(9, 1)/mathx.pick(9, 1, 0)/; s/mathx.pick(2, 8)/mathx.pick(2, 8, 0)/' $$fx/main.kai > $$work/main.kai; \
	emit | sort > build/nmod-objs3.txt; \
	sigchg=$$(comm -13 build/nmod-objs2.txt build/nmod-objs3.txt | wc -l | tr -d ' '); \
	[ "$$sigchg" -ge "2" ] \
	  && echo "native-modular OK: pick signature change re-emitted $$sigchg objects (consumer + producer, anti-ABI-skew)" \
	  || { echo "native-modular: signature change re-emitted only $$sigchg objects (want >= 2: producer + consumer)"; exit 1; }
else
test-native-modular:
	@echo "native-modular SKIP (C-only build; native backend not compiled)"
endif

# Native core-object cache (#1323): a whole-program native build with the core
# cache available splits into a user object + a content-addressed core object
# under the cache dir. Assertions: (1) the cold build reports a miss, prints
# two object paths, and populates exactly one ncore-*.o; (2) the split binary's
# behaviour is identical to the whole-program one (KAI_NATIVE_CORE_OBJ=0);
# (3) a warm build reports a hit, prints the SAME core object path, and mints
# no second cache entry — the core was linked, not re-emitted.
ifeq ($(KAI_LLVM),1)
test-native-core-obj: $(TARGET) build
	@set -e; \
	work=build/ncoreobj; rm -rf $$work; mkdir -p $$work; \
	cache=build/ncoreobj-cache; rm -rf $$cache; mkdir -p $$cache; \
	printf 'fn main() : Unit / Console = println("core-obj hello")\n' > $$work/hello.kai; \
	link() { $(CC) $(CFLAGS) $(LDFLAGS_STACK) $$1 ../stage0/runtime_llvm.c -I ../stage2 -I ../stage0 -o $$2 $(LDLIBS_M) -ldl; }; \
	KAI_NATIVE_CORE_OBJ=0 ./$(TARGET) --emit=native --core-cache-dir $$cache --toolchain-id nco-test --path ../stdlib --path $$work $$work/hello.kai > build/ncoreobj-whole.txt 2> build/ncoreobj-whole.err \
	  || { echo "native-core-obj: whole-program EMIT FAIL"; cat build/ncoreobj-whole.err; exit 1; }; \
	link "$$work/hello.o" build/ncoreobj-whole; ./build/ncoreobj-whole > build/ncoreobj-whole.out; \
	rm -f $$work/hello.o; \
	./$(TARGET) --emit=native --core-cache-dir $$cache --toolchain-id nco-test --core-cache-stats --path ../stdlib --path $$work $$work/hello.kai > build/ncoreobj-objs1.txt 2> build/ncoreobj1.err \
	  || { echo "native-core-obj: cold split EMIT FAIL"; cat build/ncoreobj1.err; exit 1; }; \
	grep -q "native-core-obj: miss" build/ncoreobj1.err \
	  || { echo "native-core-obj: cold build did not report a miss"; cat build/ncoreobj1.err; exit 1; }; \
	[ "$$(wc -l < build/ncoreobj-objs1.txt | tr -d ' ')" = "2" ] \
	  || { echo "native-core-obj: expected 2 object paths, got:"; cat build/ncoreobj-objs1.txt; exit 1; }; \
	[ "$$(ls $$cache/ncore-*.o | wc -l | tr -d ' ')" = "1" ] \
	  || { echo "native-core-obj: expected exactly one cached core object"; ls $$cache; exit 1; }; \
	link "$$(tr '\n' ' ' < build/ncoreobj-objs1.txt)" build/ncoreobj-split; ./build/ncoreobj-split > build/ncoreobj-split.out; \
	diff -q build/ncoreobj-whole.out build/ncoreobj-split.out > /dev/null \
	  && echo "native-core-obj OK: split output == whole-program output" \
	  || { echo "native-core-obj: split != whole-program"; diff build/ncoreobj-whole.out build/ncoreobj-split.out; exit 1; }; \
	rm -f $$work/hello.o; \
	./$(TARGET) --emit=native --core-cache-dir $$cache --toolchain-id nco-test --core-cache-stats --path ../stdlib --path $$work $$work/hello.kai > build/ncoreobj-objs2.txt 2> build/ncoreobj2.err \
	  || { echo "native-core-obj: warm split EMIT FAIL"; cat build/ncoreobj2.err; exit 1; }; \
	grep -q "native-core-obj: hit" build/ncoreobj2.err \
	  || { echo "native-core-obj: warm build did not report a hit"; cat build/ncoreobj2.err; exit 1; }; \
	diff -q build/ncoreobj-objs1.txt build/ncoreobj-objs2.txt > /dev/null \
	  || { echo "native-core-obj: warm build changed the object list"; diff build/ncoreobj-objs1.txt build/ncoreobj-objs2.txt; exit 1; }; \
	[ "$$(ls $$cache/ncore-*.o | wc -l | tr -d ' ')" = "1" ] \
	  && echo "native-core-obj OK: warm build reused the one cached core object" \
	  || { echo "native-core-obj: warm build minted a second core object"; ls $$cache; exit 1; }
else
test-native-core-obj:
	@echo "native-core-obj SKIP (C-only build; native backend not compiled)"
endif

# Link-time dead-strip on the native link. The fixture reaches every target
# only through the runtime impl table (Hash/Eq, Show, Default, boxed
# adapters) or closure/handler thunks — the reachability a strip must not
# break. Assertions: (1) the default (stripped) build's output is identical
# to KAI_NATIVE_DEAD_STRIP=0; (2) the stripped binary is smaller — the strip
# actually fired. End-to-end through bin/kai: the flags live in the wrapper.
ifeq ($(KAI_LLVM),1)
test-native-dead-strip: $(TARGET) build
	@set -e; \
	KAI_BACKEND=native ../bin/kai build ../examples/stdlib/dead_strip_dispatch.kai -o build/dead-strip-on 2> build/dead-strip-on.err \
	  || { echo "native-dead-strip BUILD FAIL (default)"; cat build/dead-strip-on.err; exit 1; }; \
	KAI_NATIVE_DEAD_STRIP=0 KAI_BACKEND=native ../bin/kai build ../examples/stdlib/dead_strip_dispatch.kai -o build/dead-strip-off 2> build/dead-strip-off.err \
	  || { echo "native-dead-strip BUILD FAIL (KAI_NATIVE_DEAD_STRIP=0)"; cat build/dead-strip-off.err; exit 1; }; \
	./build/dead-strip-on > build/dead-strip-on.out; \
	./build/dead-strip-off > build/dead-strip-off.out; \
	diff -q build/dead-strip-on.out build/dead-strip-off.out > /dev/null \
	  || { echo "native-dead-strip DIFF — stripped output diverges"; diff build/dead-strip-off.out build/dead-strip-on.out; exit 1; }; \
	on=$$(wc -c < build/dead-strip-on | tr -d ' '); off=$$(wc -c < build/dead-strip-off | tr -d ' '); \
	[ "$$on" -lt "$$off" ] \
	  && echo "native-dead-strip OK: identical output, $$off -> $$on bytes" \
	  || { echo "native-dead-strip: no size reduction ($$off -> $$on) — strip did not fire"; exit 1; }
else
test-native-dead-strip:
	@echo "native-dead-strip SKIP (C-only build; native backend not compiled)"
endif

# FFI v2 (#417) — negatives the surface still rejects: a struct field
# with no exact C width, and a function-typed (callback-from-C) extern
# param. Each must fail to compile with the matching diagnostic.
test-ffi-v2-negatives: $(TARGET) build
	@set -e; \
	for f in v2_struct_field_non_c v2_callback_param; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --check ../examples/negative/ffi/$$f.kai > build/$$f.out 2>&1 \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/aspirational/issue_315_no_double_slash.err.kai \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$src > build/$$f.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) ; \
	  ./build/$$f-c > build/$$f.out; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    && echo "$$f OK (C backend)" \
	    || { echo "$$f DIFF (C backend)"; diff $$exp build/$$f.out; exit 1; }; \
	done; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/ffi/extern_c_old_syntax.err.kai > build/extern_c_old_syntax.out 2>&1 \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../examples/effects/ffi_pub_axiom_cross_module \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/$$f.c \
	    || { echo "$$f FAIL (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) \
	    || { echo "$$f FAIL (cc)"; exit 1; }; \
	  ./build/$$f-c > build/$$f.out \
	    || { echo "$$f FAIL (run)"; exit 1; }; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    && echo "$$f OK" \
	    || { echo "$$f DIFF"; diff $$exp build/$$f.out; exit 1; }; \
	done

# Issue #1179 gap 1 — the C backend erases the State handler for a
# local `var` whose only capture is a while/until/repeat body: the
# combinator call lowers to a loop intrinsic, the var to a stack slot.
# Positive fixture must emit ZERO desugar-minted State clauses and run
# to the golden; the negative (`each` closure) must KEEP its clauses.
test-issue-1179-var-slot-loop: $(TARGET) build
	@set -e; \
	for f in issue_1179_var_slot_loops issue_1179_each_closure_state issue_1179_cell_register; do \
	  src=../examples/sugars/loop/$$f.kai; \
	  exp=../examples/sugars/loop/$$f.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/$$f.c \
	    || { echo "$$f FAIL (kaic2)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) \
	    || { echo "$$f FAIL (cc)"; exit 1; }; \
	  ./build/$$f-c > build/$$f.out \
	    || { echo "$$f FAIL (run)"; exit 1; }; \
	  diff -q $$exp build/$$f.out > /dev/null \
	    || { echo "$$f DIFF"; diff $$exp build/$$f.out; exit 1; }; \
	done; \
	for fn in sum_to count_down reps joins nested; do \
	  grep -q "kai_$${fn}__clause" build/issue_1179_var_slot_loops.c \
	    && { echo "issue_1179 FAIL ($$fn still carries a State clause)"; exit 1; } || true; \
	done; \
	grep -q "kai_each_sum__clause" build/issue_1179_each_closure_state.c \
	  || { echo "issue_1179 FAIL (each_sum lost its State clauses)"; exit 1; }; \
	for cell in acc_int acc_real flag post; do \
	  grep -q "kair_$${cell}__slot" build/issue_1179_cell_register.c \
	    || { echo "issue_1179 FAIL ($$cell not promoted to a raw cell)"; exit 1; }; \
	  grep -q "kai_$${cell}__slot = kai_core_array_make" build/issue_1179_cell_register.c \
	    && { echo "issue_1179 FAIL ($$cell still an array slot)"; exit 1; } || true; \
	done; \
	grep -q "kai_s_txt__slot = kai_core_array_make" build/issue_1179_cell_register.c \
	  || { echo "issue_1179 FAIL (String var lost its array slot)"; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir ../examples/sugars/loop/issue_1179_cell_register.kai > build/issue_1179_cell_register.kir \
	  || { echo "issue_1179 FAIL (kir dump)"; exit 1; }; \
	grep -q "acc_int__slot: i64" build/issue_1179_cell_register.kir \
	  || { echo "issue_1179 FAIL (acc_int not a raw i64 register in KIR)"; exit 1; }; \
	echo "issue-1179-var-slot-loop OK (loops slot + inline, scalar cells raw, each keeps State)"

# Issue #1214 — a non-literal PURE scalar var init (`var m := n`,
# `var m := n*2+1`) promotes to a raw register just like a literal init,
# closing the ~16x while+var gap. An impure init (a call) or a non-scalar
# init (String) keeps the array slot. Structural: KIR shows the promoted
# cells as raw `i64`; the emitted C shows the non-promoted ones still
# `array_make`. Runs C (default) with a native+C parity diff.
test-issue-1214-nonliteral-init: $(TARGET) build
	@set -e; \
	f=issue_1214_nonliteral_init; \
	src=../examples/sugars/loop/$$f.kai; \
	exp=../examples/sugars/loop/$$f.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/$$f.c \
	  || { echo "$$f FAIL (kaic2)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$f.c -o build/$$f-c $(LDLIBS_M) \
	  || { echo "$$f FAIL (cc)"; exit 1; }; \
	./build/$$f-c > build/$$f.out \
	  || { echo "$$f FAIL (run)"; exit 1; }; \
	diff -q $$exp build/$$f.out > /dev/null \
	  || { echo "$$f DIFF"; diff $$exp build/$$f.out; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir $$src > build/$$f.kir \
	  || { echo "$$f FAIL (kir dump)"; exit 1; }; \
	awk '/^fn collatz/,/^}/' build/$$f.kir | grep -q "m__slot: i64" \
	  || { echo "$$f FAIL (collatz m := n not promoted to raw i64)"; exit 1; }; \
	awk '/^fn arith/,/^}/' build/$$f.kir | grep -q "m__slot: i64" \
	  || { echo "$$f FAIL (arith m := n*2+1 not promoted to raw i64)"; exit 1; }; \
	awk '/^fn callinit/,/^}/' build/$$f.kir | grep -q "array_make" \
	  || { echo "$$f FAIL (callinit m := twice(n) wrongly promoted — call init must keep the slot)"; exit 1; }; \
	grep -q "kai_acc__slot = kai_core_array_make" build/$$f.c \
	  || { echo "$$f FAIL (String var lost its array slot)"; exit 1; }; \
	echo "issue-1214-nonliteral-init OK (pure non-literal scalar inits promoted; call + String inits keep the slot)"

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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$src > build/$$f.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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

# issue #1123 — kind Region: a Tree<r> built in an arena across fn
# boundaries (region-polymorphic helpers), folded to a scalar with the
# arena freed wholesale (no escape); plus the escaping-tree case whose
# value is deep-copied out. The first guards that Node routes to
# kai_arena_variant (the cross-fn by-type routing); the second guards the
# deep-copy-out slot-mask fix (a raw Int slot must not be chased as a
# pointer). Both run on the C backend and diff their output.
test-region-1123: $(TARGET) build
	@set -e; \
	for name in region_tree_1123 region_tree_escape_1123; do \
	  f=../examples/perceus/$$name.kai; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "region-1123 FAIL ($$name: kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  grep -q 'kai_arena_variant(' build/s2-$$name.c \
	    || { echo "region-1123 FAIL ($$name: Node did not route to the arena — cross-fn routing regressed?)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) 2>/dev/null \
	    || { echo "region-1123 FAIL ($$name: C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "region-1123 FAIL ($$name: run crashed — arena UAF?)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	    || { echo "region-1123 DIFF ($$name)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	done; \
	echo "region-1123 OK (Tree<r> arena routing + wholesale-free + escape deep-copy-out)"

# Native-backend arena routing. The C gate above greps the emitted C for
# `kai_arena_variant(`; nothing checked the NATIVE binary, so a feature that
# routed on C but heap-allocated on native (the default) passed silently. This
# gate emits the native object and asserts it references the arena runtime
# symbols (`kaix_arena_variant*` for the ctors, `kaix_arena_push` for the
# region bracket) — the exact check whose absence let that gap ship. Both
# region fixtures also run to a byte-identical result on the native backend.
test-region-1193-native: $(TARGET) build
	@set -e; \
	for name in region_tree_1123 region_tree_escape_1123; do \
	  f=../examples/perceus/$$name.kai; \
	  ./$(TARGET) --emit=native --path ../stdlib $$f > /dev/null 2> build/s2-1193-$$name.err \
	    || { echo "region-1193-native FAIL ($$name: native emit errored)"; cat build/s2-1193-$$name.err; exit 1; }; \
	  obj=../examples/perceus/$$name.o; \
	  nm $$obj 2>/dev/null | grep -q 'kaix_arena_variant' \
	    || { echo "region-1193-native FAIL ($$name: Node did not route to the arena on native — heap-RC regressed?)"; rm -f $$obj; exit 1; }; \
	  nm $$obj 2>/dev/null | grep -q 'kaix_arena_push' \
	    || { echo "region-1193-native FAIL ($$name: region block did not bracket the arena on native)"; rm -f $$obj; exit 1; }; \
	  rm -f $$obj; \
	  KAI_BACKEND=native ../bin/kai build $$f -o build/s2-1193-$$name-nat 2> build/s2-1193-$$name-nat.err \
	    || { echo "region-1193-native FAIL ($$name: native build errored)"; cat build/s2-1193-$$name-nat.err; exit 1; }; \
	  ./build/s2-1193-$$name-nat > build/s2-1193-$$name-nat.out 2>&1 \
	    || { echo "region-1193-native FAIL ($$name: run crashed — arena UAF?)"; cat build/s2-1193-$$name-nat.out; exit 1; }; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-1193-$$name-nat.out > /dev/null \
	    || { echo "region-1193-native DIFF ($$name)"; diff ../examples/perceus/$$name.out.expected build/s2-1193-$$name-nat.out; exit 1; }; \
	done; \
	echo "region-1193-native OK (native arena routing + region bracket + byte-identical result)"

# Backend RC-trace parity for region code. The gap that let the reuse/arena
# divergence ship: no gate compared the PER-REGION RC trace between backends.
# `region_tree_1123` (guard insert) and `region_spine_reuse_1197` (right-spine
# insert) are two nearly-identical inserts whose reuse-vs-arena decision used to
# split by control-flow shape AND by backend, landing on opposite incref/arena
# counts. This gate builds each on BOTH backends and asserts:
#   1. incref_total == 0     — the region's zero-per-node-RC promise holds;
#   2. arena_alloc(C) == arena_alloc(native) AND incref(C) == incref(native)
#      — the two backends agree on the RC/arena model, not just the result.
# A ctor under a live region NEVER takes a reuse token — it bump-allocates fresh
# so the arena frees it wholesale. The native leg needs LLVM, so this rides
# tier1-native, not the LLVM-free light suite.
test-region-1197-parity: $(TARGET) build
	@set -e; \
	for name in region_tree_1123 region_spine_reuse_1197; do \
	  f=../examples/perceus/$$name.kai; \
	  KAI_BACKEND=c ../bin/kai build $$f -o build/s2-1197-$$name-c 2> build/s2-1197-$$name-c.err \
	    || { echo "region-1197-parity FAIL ($$name: C build errored)"; cat build/s2-1197-$$name-c.err; exit 1; }; \
	  KAI_BACKEND=native ../bin/kai build $$f -o build/s2-1197-$$name-nat 2> build/s2-1197-$$name-nat.err \
	    || { echo "region-1197-parity FAIL ($$name: native build errored)"; cat build/s2-1197-$$name-nat.err; exit 1; }; \
	  ic=$$(KAI_TRACE_RC=1 ./build/s2-1197-$$name-c 2>&1 | grep -oE 'incref_total=[0-9]+' | head -1 | cut -d= -f2); \
	  in=$$(KAI_TRACE_RC=1 ./build/s2-1197-$$name-nat 2>&1 | grep -oE 'incref_total=[0-9]+' | head -1 | cut -d= -f2); \
	  ac=$$(KAI_TRACE_RC=1 ./build/s2-1197-$$name-c 2>&1 | grep -oE 'arena_alloc=[0-9]+' | head -1 | cut -d= -f2); \
	  an=$$(KAI_TRACE_RC=1 ./build/s2-1197-$$name-nat 2>&1 | grep -oE 'arena_alloc=[0-9]+' | head -1 | cut -d= -f2); \
	  [ "$$ic" = "0" ] || { echo "region-1197-parity FAIL ($$name: C incref_total=$$ic, expected 0 — per-node RC under region)"; exit 1; }; \
	  [ "$$in" = "0" ] || { echo "region-1197-parity FAIL ($$name: native incref_total=$$in, expected 0 — per-node RC under region)"; exit 1; }; \
	  [ "$$ic" = "$$in" ] || { echo "region-1197-parity FAIL ($$name: incref diverges C=$$ic native=$$in)"; exit 1; }; \
	  [ "$$ac" = "$$an" ] || { echo "region-1197-parity FAIL ($$name: arena_alloc diverges C=$$ac native=$$an)"; exit 1; }; \
	done; \
	echo "region-1197-parity OK (zero-per-node-RC + C/native RC-trace parity on guard + spine inserts)"

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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$name.kai > build/$$name.c; \
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

# The runtime must adopt, not displace, an alternate signal stack that a
# sanitizer already installed — a displaced owner munmaps our malloc'd block
# at thread exit and ASAN treats the EINVAL as fatal. Only ASAN can observe
# this, and only above one thread: the diagnostic count tracks the number of
# scheduler threads plus the reactor. N=1 is kept in the sweep as the negative
# control, since a fix that merely silenced the multi-thread path would still
# pass it.
MN_SIGALTSTACK_THREADS := 1 2 8 16

test-mn-sigaltstack-asan: $(TARGET) build
	@set -e; \
	name=mn_sigaltstack_teardown; \
	./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c $(LDLIBS_M); \
	for n in $(MN_SIGALTSTACK_THREADS); do \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  KAI_THREADS=$$n ./build/$$name-asan-c \
	    > build/$$name-asan.out 2> build/$$name-asan.err \
	    || { echo "mn-sigaltstack-asan FAIL $$name (KAI_THREADS=$$n: non-zero exit)"; \
	         cat build/$$name-asan.err; exit 1; }; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:|Failed to munmap' build/$$name-asan.err; then \
	    echo "mn-sigaltstack-asan FAIL $$name (KAI_THREADS=$$n) — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "mn-sigaltstack-asan OK $$name (KAI_THREADS=$$n)" \
	    || { echo "mn-sigaltstack-asan DIFF $$name (KAI_THREADS=$$n)"; \
	         diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$name.kai > build/$$name.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$pos.kai > build/$$pos.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$pos.c -o build/$$pos-c $(LDLIBS_M); \
	./build/$$pos-c > build/$$pos.out; \
	diff -q ../examples/effects/$$pos.out.expected build/$$pos.out > /dev/null \
	  && echo "issue-743 OK $$pos (C)" \
	  || { echo "issue-743 DIFF $$pos"; diff ../examples/effects/$$pos.out.expected build/$$pos.out; exit 1; }; \
	neg=issue_743_generic_effect_payload_bad_field; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$neg.kai 2> build/$$neg.err > /dev/null && \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$pos.kai > build/$$pos.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$pos.c -o build/$$pos-c $(LDLIBS_M); \
	./build/$$pos-c > build/$$pos.out; \
	diff -q ../examples/effects/$$pos.out.expected build/$$pos.out > /dev/null \
	  && echo "issue-842 OK $$pos (C)" \
	  || { echo "issue-842 DIFF $$pos"; diff ../examples/effects/$$pos.out.expected build/$$pos.out; exit 1; }; \
	neg=issue_842_multi_param_op_payload_bad_field; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/$$neg.kai 2> build/$$neg.err > /dev/null && \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/time/main.kai > build/time.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --dump-intervals $$f > build/iv-$$name.out 2>&1 \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --dump-call-elision $$f > build/ce-$$name.out 2>&1 \
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
	./$(TARGET) $(TEST_CORE_CACHE) --check $$f > build/ce-too-loose.out 2> build/ce-too-loose.err; \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/typer-$$name.c 2> build/typer-$$name.err || rc=$$?; \
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
# crypto, decimal, uuid, regexp, path, random*) must `import`
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/stdlib-$$name.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --holes $$f > build/s2-$$name.holes; \
	  if ! grep -q "type hole" build/s2-$$name.holes; then \
	    echo "holes FAIL $$name (no report)"; cat build/s2-$$name.holes; exit 1; \
	  fi; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --holes-json $$f > build/s2-$$name.holes.json; \
	  python3 ../scripts/validate_holes_json.py build/s2-$$name.holes.json \
	    || { echo "holes FAIL $$name (invalid JSON / schema)"; cat build/s2-$$name.holes.json; exit 1; }; \
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --strict-holes $$f > /dev/null 2> build/s2-$$name.strict.err || rc=$$?; \
	  if [ $$rc -eq 0 ]; then \
	    echo "holes FAIL $$name (--strict-holes did not fail)"; cat build/s2-$$name.strict.err; exit 1; \
	  fi; \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/$$name-holes.c; \
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
	grep -q "in scope (local):" build/driver-hole-pretty.out \
	  || { echo "driver-holes FAIL hole_pretty (local scope header missing)"; cat build/driver-hole-pretty.out; exit 1; }; \
	if grep -q "Spawn.spawn" build/driver-hole-pretty.out; then \
	  echo "driver-holes FAIL hole_pretty (--holes leaked the full scope)"; exit 1; \
	fi; \
	grep -q "more bindings in scope (--holes-scope lists them)" build/driver-hole-pretty.out \
	  || { echo "driver-holes FAIL hole_pretty (elided-count line missing)"; cat build/driver-hole-pretty.out; exit 1; }; \
	$$KAI build --holes-scope $$pretty > build/driver-hole-scope.out; \
	grep -q "Spawn.spawn" build/driver-hole-scope.out \
	  || { echo "driver-holes FAIL hole_pretty (--holes-scope missing stdlib bindings)"; exit 1; }; \
	echo "driver-holes OK hole_scope (--holes local vs --holes-scope full)"; \
	python3 -c "import json,sys; d=json.load(open(sys.argv[1])); r=d[0]; assert r['scope_elided'] > 0, r['scope_elided']; assert len(r['in_scope']) < 20, len(r['in_scope'])" \
	  build/driver-hole-json.out \
	  || { echo "driver-holes FAIL hole_json (default not bounded)"; exit 1; }; \
	$$KAI build --holes-json-scope $$json > build/driver-hole-json-scope.out; \
	python3 ../scripts/validate_holes_json.py build/driver-hole-json-scope.out \
	  || { echo "driver-holes FAIL hole_json_scope (schema)"; cat build/driver-hole-json-scope.out; exit 1; }; \
	python3 -c "import json,sys; d=json.load(open(sys.argv[1])); r=d[0]; assert r['scope_elided'] == 0, r['scope_elided']; assert len(r['in_scope']) > 100, len(r['in_scope'])" \
	  build/driver-hole-json-scope.out \
	  || { echo "driver-holes FAIL hole_json_scope (full scope missing)"; exit 1; }; \
	echo "driver-holes OK hole_json_scope (--holes-json bounded vs -scope full)"; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --ast $$f > build/s2-$$name.ast \
	    && grep -q "effect \"Io\"" build/s2-$$name.ast \
	    && grep -q "effect \"Fail\"" build/s2-$$name.ast \
	    && grep -q "op \"print\"" build/s2-$$name.ast \
	    && echo "effects OK $$name" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.ast; exit 1; }; \
	done; \
	for f in ../examples/effects/parse_row_syntax.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) --ast $$f > build/s2-$$name.ast \
	    && grep -q "row Io" build/s2-$$name.ast \
	    && grep -q "row Io + Fail" build/s2-$$name.ast \
	    && echo "effects OK $$name" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.ast; exit 1; }; \
	done; \
	for f in ../examples/effects/row_match_higher_order.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (row match accepted)" \
	    || { echo "effects FAIL $$name (matching rows rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/row_pure_to_effectful.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (pure-to-effectful elaboration accepted)" \
	    || { echo "effects FAIL $$name (pure-to-effectful elaboration rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/op_call_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (op call accepted)" \
	    || { echo "effects FAIL $$name (op call rejected)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_basic.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) --ast $$f > build/s2-$$name.ast 2> build/s2-$$name.err \
	    && grep -q "handle Io" build/s2-$$name.ast \
	    && echo "effects OK $$name (handle parsed)" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.err build/s2-$$name.ast; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_alias.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (alias resolved)" \
	    || { echo "effects FAIL $$name"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/handle_removes_effect.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    && echo "effects OK $$name (handle removes effect)" \
	    || { echo "effects FAIL $$name (handle did not remove effect)"; cat build/s2-$$name.err; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_6a_struct_emit.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name \
	    && echo "effects OK $$name (clause body + return clause type-checked + ran)" \
	    || { echo "effects FAIL $$name (binary asserts failed)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m7a_7_default_console.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --dump-brands --path ../stdlib $$f > build/s2-$$name.brands 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored on --dump-brands)"; \
	         cat build/s2-$$name.err; exit 1; }; \
	  diff -u $$exp build/s2-$$name.brands \
	    && echo "effects OK $$name (--dump-brands golden, issue #71 option (b))" \
	    || { echo "effects FAIL $$name (--dump-brands diverged from golden)"; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_7_actor_self_send.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero — R4 fiber-discard regression?)"; cat build/s2-$$name.out; exit 1; }; \
	  diff -u $$exp_out build/s2-$$name.out \
	    && echo "effects OK $$name (R4: fiber-discard does not deadlock)" \
	    || { echo "effects FAIL $$name (output diff)"; exit 1; }; \
	done; \
	for f in ../examples/effects/mn_trampoline_tail_steal.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/effects/$$name.out.expected; \
	  ./$(TARGET) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  bad=0; \
	  for r in $$(seq 1 $(MN_TAIL_STEAL_ITERS)); do \
	    KAI_THREADS=8 ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	      || { bad=$$((bad+1)); continue; }; \
	    cmp -s $$exp_out build/s2-$$name.out || bad=$$((bad+1)); \
	  done; \
	  test "$$bad" = 0 \
	    && echo "effects OK $$name (trampoline tail keeps fiber identity across a steal: $(MN_TAIL_STEAL_ITERS)/$(MN_TAIL_STEAL_ITERS) clean at KAI_THREADS=8)" \
	    || { echo "effects FAIL $$name ($$bad of $(MN_TAIL_STEAL_ITERS) bad at KAI_THREADS=8)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	for f in ../examples/effects/m8_fiber_stack_overflow.kai; do \
	  name=$$(basename $$f .kai); \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  for n in 1 4 8 16; do \
	    rc=0; KAI_THREADS=$$n ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.stderr || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "effects FAIL $$name (KAI_THREADS=$$n: expected non-zero exit from guard-page SIGSEGV, got 0)"; \
	      cat build/s2-$$name.out; exit 1; \
	    fi; \
	    grep -q "fiber stack overflow" build/s2-$$name.stderr \
	      && echo "effects OK $$name (KAI_THREADS=$$n: guard page caught overflow with diagnostic, exit=$$rc)" \
	      || { echo "effects FAIL $$name (KAI_THREADS=$$n: no overflow diagnostic on stderr; exit=$$rc)"; \
	           echo '--- stderr:'; cat build/s2-$$name.stderr; \
	           echo '--- stdout:'; cat build/s2-$$name.out; exit 1; }; \
	  done; \
	done; \
	for f in ../examples/effects/m12_8_y_phase4_*.kai; do \
	  [ -e $$f ] || continue; \
	  name=$$(basename $$f .kai); \
	  exp_out=../examples/effects/$$name.out.expected; \
	  exp_err=../examples/effects/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --edition hanga-roa --path ../stdlib $$f > build/s2-$$name.c 2> build/s2-$$name.err \
	    || { echo "effects FAIL $$name (kaic2 errored)"; cat build/s2-$$name.err; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M) \
	    || { echo "effects FAIL $$name (C compile failed)"; exit 1; }; \
	  ./build/s2-$$name > build/s2-$$name.out 2>&1 \
	    || { echo "effects FAIL $$name (binary exit non-zero)"; cat build/s2-$$name.out; exit 1; }; \
	  grep -q "$$pat" build/s2-$$name.out \
	    && echo "effects OK $$name ($$desc)" \
	    || { echo "effects FAIL $$name (output mismatch; want /$$pat/)"; cat build/s2-$$name.out; exit 1; }; \
	done; \
	./$(TARGET) $(TEST_CORE_CACHE) --edition hanga-roa --path ../stdlib ../demos/wc.kai > build/s2-wc.c 2> build/s2-wc.err \
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
	    coll_literal_*) extra_flags="--path ../stdlib" ;; \
	    shape_kind_*) extra_flags="--path ../stdlib" ;; \
	    *) extra_flags="" ;; \
	  esac; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$extra_flags $$f > /dev/null 2> build/s2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$extra_flags $$f > build/$$name.c \
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
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --lint --path ../stdlib $$f > build/s2-$$name.lint 2>&1 || rc=$$?; \
	  if [ $$rc -ne 0 ]; then \
	    echo "lint FAIL $$name (--lint exited $$rc; a lint must never block)"; \
	    cat build/s2-$$name.lint; exit 1; \
	  fi; \
	  if [ -f $$exp_txt ]; then \
	    diff -q $$exp_txt build/s2-$$name.lint > /dev/null \
	      || { echo "lint DIFF $$name (--lint)"; diff $$exp_txt build/s2-$$name.lint; exit 1; }; \
	  fi; \
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --lint-json --path ../stdlib $$f > build/s2-$$name.lint-json 2>&1 || rc=$$?; \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/r-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/diag-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/diag-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/m2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/m2-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/lit-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/lit-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/p2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/p2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/p2-$$name.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) --test $$src > build/psd.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) --test $$src > build/bna.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/s2-$$name.c 2> build/s2-$$name.err \
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
	@grep -qE "= kai_process_with_log(__cspec__F[A-Za-z0-9_]+)?__mono__Int__Int\(" build/s2-m4c_handler_in_body.c \
	  && grep -qE "= kai_process_with_log(__cspec__F[A-Za-z0-9_]+)?__mono__String__Int\(" build/s2-m4c_handler_in_body.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$preludes $$src > build/demo-$$name.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$preludes $$src > build/asp-$$name.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) $$f > build/$$name.c \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/perceus-$$name.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/perceus/match_pbind_catchall_raw.kai > build/perceus-match_pbind_catchall_raw.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --path ../examples/perceus $$f > build/perceus-$$name.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/main.kai > build/tco.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/list_nth_shape.kai > build/tco-nth.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/perceus/r12_zero_arg_recursive.kai > build/tco-zero.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/issue_309_countdown.kai > build/tco-309.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/perceus/unbox_bench.kai > build/tco-perceus.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-812tr.c \
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

# Issue #1219 — the C bootstrap must not eager-seed the builtin nullary ctors
# (None, Sig*) as immortal `variant` singletons; the native path already
# excludes them. A program constructing none of them must trace ZERO `variant`
# allocations, keeping the KAI_TRACE_RC `leaked` baseline str-only and
# backend-comparable. Pre-fix the emitter seeded all 11 reserved builtins, 6 of
# them nullary, so every C binary reported `tag variant allocs=6` at bootstrap.
test-perceus-1219-bootstrap-seed: $(TARGET) build
	@set -e; \
	src=../examples/perceus/bootstrap_no_builtin_nullary_seed_1219.kai; \
	exp=../examples/perceus/bootstrap_no_builtin_nullary_seed_1219.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1219boot.c \
	  || { echo "perceus-1219-bootstrap-seed FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1219boot.c -o build/s2-1219boot $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1219boot > build/s2-1219boot.out 2> build/s2-1219boot.rc; \
	diff -q $$exp build/s2-1219boot.out > /dev/null \
	  || { echo "perceus-1219-bootstrap-seed DIFF"; diff $$exp build/s2-1219boot.out; exit 1; }; \
	if grep -qE 'tag variant' build/s2-1219boot.rc; then \
	  echo "perceus-1219-bootstrap-seed FAIL (bootstrap seeded builtin nullary variants):"; \
	  grep -E 'tag variant|leaked=' build/s2-1219boot.rc | sed 's/^/      /'; exit 1; \
	fi; \
	echo "perceus-1219-bootstrap-seed OK (no variant bootstrap allocs; builtin nullary seed excluded on C, native-parity)"

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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-817fl.c \
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

# Issue #1117 — a map pipe (`|`) whose closure PROJECTS a field of a record
# element leaks the record box, one per element. `map_loop` hands `h` to `f`
# as an owned arg, but a closure body only DUPs its param reads, so the
# param's birth ref is never consumed. The fix drops each closure param at
# the end of the lifted thunk. The fixture maps a field-projecting closure
# over 1000 heap records, 20 times; with the leak `leaked` grows ~1000/iter
# (≈20000), fixed it is a small constant. Built WITHOUT `-DKAI_TRACE_RC`
# (user-facing path) and asserts `leaked` < 1000.
test-perceus-1117-map-pipe-record-leak: $(TARGET) build
	@set -e; \
	src=../examples/perceus/map_pipe_record_leak_1117.kai; \
	exp=../examples/perceus/map_pipe_record_leak_1117.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1117mprl.c \
	  || { echo "perceus-1117-map-pipe-record-leak FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1117mprl.c -o build/s2-1117mprl $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1117mprl > build/s2-1117mprl.out 2> build/s2-1117mprl.rc; \
	diff -q $$exp build/s2-1117mprl.out > /dev/null \
	  || { echo "perceus-1117-map-pipe-record-leak FAIL (output diff)"; diff $$exp build/s2-1117mprl.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1117mprl.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1117-map-pipe-record-leak FAIL (no leaked line)"; cat build/s2-1117mprl.rc; exit 1; }; \
	[ "$$leaked" -lt 1000 ] \
	  && echo "perceus-1117-map-pipe-record-leak OK (leaked=$$leaked < 1000 over 20 map-project-over-1000-records iters; closure-param leak closed)" \
	  || { echo "perceus-1117-map-pipe-record-leak FAIL (leaked=$$leaked — closure-param leak regressed, grows with iters)"; exit 1; }

# Issue #1134 — map-pipe deforestation, purity gate. Three adjacent pure
# `map` stages fuse into one traversal (functor law), so the run builds
# one result spine over a 100-element source. The gate asserts cons
# allocations stay under the single-map budget (< 400; three unfused maps
# would be ~600) AND the output is exact. A regression that stops fusing
# pushes cons allocs back toward ~600 and trips the threshold.
test-perceus-1134-pipe-fusion: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_purity_1134.kai; \
	exp=../examples/perceus/pipe_fusion_purity_1134.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1134pf.c \
	  || { echo "perceus-1134-pipe-fusion FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1134pf.c -o build/s2-1134pf $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1134pf > build/s2-1134pf.out 2> build/s2-1134pf.rc; \
	diff -q $$exp build/s2-1134pf.out > /dev/null \
	  || { echo "perceus-1134-pipe-fusion FAIL (output diff)"; diff $$exp build/s2-1134pf.out; exit 1; }; \
	cons=$$(grep -oE 'tag cons[[:space:]]+allocs=[0-9]+' build/s2-1134pf.rc | grep -oE '[0-9]+$$'); \
	[ -n "$$cons" ] \
	  || { echo "perceus-1134-pipe-fusion FAIL (no cons alloc line)"; cat build/s2-1134pf.rc; exit 1; }; \
	[ "$$cons" -lt 400 ] \
	  && echo "perceus-1134-pipe-fusion OK (cons allocs=$$cons < 400; three pure maps fused into one)" \
	  || { echo "perceus-1134-pipe-fusion FAIL (cons allocs=$$cons — fusion regressed, intermediates rebuilt)"; exit 1; }

# Issue #1134 — effect-ordering soundness. Both stages perform `Stdout`,
# so their rows are impure and the fusion gate must decline: the golden
# pins the non-interleaved `d d d i i i` order. A soundness regression
# that fused effectful stages would interleave (`d i d i d i`) and fail
# the diff.
test-perceus-1134-pipe-fusion-ordering: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_ordering_1134.kai; \
	exp=../examples/perceus/pipe_fusion_ordering_1134.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1134pfo.c \
	  || { echo "perceus-1134-pipe-fusion-ordering FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1134pfo.c -o build/s2-1134pfo $(LDLIBS_M); \
	./build/s2-1134pfo > build/s2-1134pfo.out; \
	diff -q $$exp build/s2-1134pfo.out > /dev/null \
	  && echo "perceus-1134-pipe-fusion-ordering OK (effectful stages declined fusion; order preserved)" \
	  || { echo "perceus-1134-pipe-fusion-ordering FAIL (order diff — effectful fusion fired)"; diff $$exp build/s2-1134pfo.out; exit 1; }

# Issue #1134 — terminal-fold + range-head deforestation. A pure
# `[1..1M] | stage |> foldl(init, combine)` fuses the range head, the map
# stage, and the terminal fold into one counting loop that materialises
# nothing. The gate asserts alloc_total stays in the bookkeeping range
# (< 100; materialising the range alone would be ~1M) AND the output is
# exact. A regression that stops fusing rematerialises the range and
# trips the threshold.
test-perceus-1134-pipe-fusion-terminal: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_terminal_1134.kai; \
	exp=../examples/perceus/pipe_fusion_terminal_1134.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1134pft.c \
	  || { echo "perceus-1134-pipe-fusion-terminal FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1134pft.c -o build/s2-1134pft $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1134pft > build/s2-1134pft.out 2> build/s2-1134pft.rc; \
	diff -q $$exp build/s2-1134pft.out > /dev/null \
	  || { echo "perceus-1134-pipe-fusion-terminal FAIL (output diff)"; diff $$exp build/s2-1134pft.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1134pft.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] \
	  || { echo "perceus-1134-pipe-fusion-terminal FAIL (no alloc_total line)"; cat build/s2-1134pft.rc; exit 1; }; \
	[ "$$tot" -lt 100 ] \
	  && echo "perceus-1134-pipe-fusion-terminal OK (alloc_total=$$tot < 100; range + map + fold fused, nothing materialised)" \
	  || { echo "perceus-1134-pipe-fusion-terminal FAIL (alloc_total=$$tot — range rematerialised, fusion regressed)"; exit 1; }

# Issue #1143 — inline-lambda stages fuse like named ones. A lambda left
# inside the composed closure's body would allocate a fresh closure per
# element (~1M at this N); the hoisted rewrite keeps the per-element
# path allocation-free. Gate: exact output + alloc_total < 100.
test-perceus-1143-pipe-fusion-lambda: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_lambda_1143.kai; \
	exp=../examples/perceus/pipe_fusion_lambda_1143.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1143pfl.c \
	  || { echo "perceus-1143-pipe-fusion-lambda FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1143pfl.c -o build/s2-1143pfl $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1143pfl > build/s2-1143pfl.out 2> build/s2-1143pfl.rc; \
	diff -q $$exp build/s2-1143pfl.out > /dev/null \
	  || { echo "perceus-1143-pipe-fusion-lambda FAIL (output diff)"; diff $$exp build/s2-1143pfl.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1143pfl.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] \
	  || { echo "perceus-1143-pipe-fusion-lambda FAIL (no alloc_total line)"; cat build/s2-1143pfl.rc; exit 1; }; \
	[ "$$tot" -lt 100 ] \
	  && echo "perceus-1143-pipe-fusion-lambda OK (alloc_total=$$tot < 100; lambda stages fused, nothing per-element)" \
	  || { echo "perceus-1143-pipe-fusion-lambda FAIL (alloc_total=$$tot — per-element allocation returned)"; exit 1; }

# Issue #1143 — sum/product/length recognized as fusion terminals over
# range / step-range / list sources, including a non-Int Numeric ring;
# an effectful stage still declines. Gate: exact output (pins the
# decline's per-element effects too) + alloc_total < 100.
test-perceus-1143-pipe-fusion-ring: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_ring_1143.kai; \
	exp=../examples/perceus/pipe_fusion_ring_1143.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1143pfr.c \
	  || { echo "perceus-1143-pipe-fusion-ring FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1143pfr.c -o build/s2-1143pfr $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1143pfr > build/s2-1143pfr.out 2> build/s2-1143pfr.rc; \
	diff -q $$exp build/s2-1143pfr.out > /dev/null \
	  || { echo "perceus-1143-pipe-fusion-ring FAIL (output diff)"; diff $$exp build/s2-1143pfr.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1143pfr.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] \
	  || { echo "perceus-1143-pipe-fusion-ring FAIL (no alloc_total line)"; cat build/s2-1143pfr.rc; exit 1; }; \
	[ "$$tot" -lt 100 ] \
	  && echo "perceus-1143-pipe-fusion-ring OK (alloc_total=$$tot < 100; ring terminals fused, nothing materialised)" \
	  || { echo "perceus-1143-pipe-fusion-ring FAIL (alloc_total=$$tot — a recognized terminal rematerialised)"; exit 1; }

# Issue #1186 — a map-pipe `| (r) => r.field` piped into a fusible
# terminal (`sum`) must fuse AND type-check. The purity gate synthesised
# each stage in isolation; a record-field stage with an unannotated param
# deferred the field against a receiver tyvar the probe never pinned, so
# the orphan drained to "type annotation needed". No per-PR suite
# compiled demos/vs/ so only the serial parity harness caught it. Gate:
# compiles + exact output + the emitted C still calls the fused `map_sum`
# (a regression that disabled fusion to dodge the type bug fails here).
test-perceus-1186-map-pipe-terminal-field: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_terminal_field_1186.kai; \
	exp=../examples/perceus/pipe_fusion_terminal_field_1186.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1186mptf.c \
	  || { echo "perceus-1186-map-pipe-terminal-field FAIL (kaic2 errored — annotation-needed regression)"; exit 1; }; \
	grep -q map_sum build/s2-1186mptf.c \
	  || { echo "perceus-1186-map-pipe-terminal-field FAIL (no map_sum — fusion regressed)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1186mptf.c -o build/s2-1186mptf $(LDLIBS_M); \
	./build/s2-1186mptf > build/s2-1186mptf.out; \
	diff -q $$exp build/s2-1186mptf.out > /dev/null \
	  && echo "perceus-1186-map-pipe-terminal-field OK (field-stage map-pipe fused into map_sum and typed)" \
	  || { echo "perceus-1186-map-pipe-terminal-field FAIL (output diff)"; diff $$exp build/s2-1186mptf.out; exit 1; }

# Issue #1134 (native) — the fused chain must run byte-identically on the
# native backend. Native-gated (tier1-native). Same purity source; asserts
# output parity + the fused cons budget carries to native codegen.
test-perceus-1134-pipe-fusion-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_purity_1134.kai; \
	exp=../examples/perceus/pipe_fusion_purity_1134.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1134pfn 2> build/s2-1134pfn.err \
	  || { echo "perceus-1134-pipe-fusion-native FAIL (native build errored)"; cat build/s2-1134pfn.err; exit 1; }; \
	./build/s2-1134pfn > build/s2-1134pfn.out; \
	diff -q $$exp build/s2-1134pfn.out > /dev/null \
	  && echo "perceus-1134-pipe-fusion-native OK (fused chain byte-identical on native)" \
	  || { echo "perceus-1134-pipe-fusion-native FAIL (native output diff)"; diff $$exp build/s2-1134pfn.out; exit 1; }

# Issue #1205 (native) — closure specialisation. The idiomatic `vec.foldl`
# over a pure lambda must (a) produce the exact sum and (b) emit a
# specialised loop with ZERO per-element `call_ind` — the lambda body is
# inlined into the tcrec loop. Native-gated (tier1-native).
test-closure-spec-1205: $(TARGET) build
	@set -e; \
	src=../examples/perceus/closure_spec_1205.kai; \
	exp=../examples/perceus/closure_spec_1205.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-cs1205 2> build/s2-cs1205.err \
	  || { echo "closure-spec-1205 FAIL (native build errored)"; cat build/s2-cs1205.err; exit 1; }; \
	./build/s2-cs1205 > build/s2-cs1205.out; \
	diff -q $$exp build/s2-cs1205.out > /dev/null \
	  || { echo "closure-spec-1205 FAIL (output diff)"; diff $$exp build/s2-cs1205.out; exit 1; }; \
	./$(TARGET) --emit=kir --path ../stdlib --edition hanga-roa $$src > build/s2-cs1205.kir 2>/dev/null; \
	awk '/^fn foldl_loop__cspec.*mono/,/^}/' build/s2-cs1205.kir > build/s2-cs1205-loop.kir; \
	test -s build/s2-cs1205-loop.kir \
	  || { echo "closure-spec-1205 FAIL (no specialised loop emitted — closure not specialised)"; exit 1; }; \
	if grep -F -q 'call_ind' build/s2-cs1205-loop.kir; then \
	  echo "closure-spec-1205 FAIL (specialised loop still has a per-element indirect call)"; \
	  grep -F 'call_ind' build/s2-cs1205-loop.kir; exit 1; \
	fi; \
	echo "closure-spec-1205 OK (sum matches; specialised loop has zero per-element call_ind, lambda inlined into tcrec)"

# Issue #1208 (native) — grounding a return-only tparam from the call
# site. `let v: Vec[Point] = [a..b] | stage` collects into a Vec whose
# element type is return-only; mono now grounds it from the annotated
# return type, so the specialised collect loop unboxes (`i64` indices,
# record ctor inlined) and carries zero per-element indirect calls.
# Native-gated (tier1-native).
test-collect-ground-1208: $(TARGET) build
	@set -e; \
	src=../examples/perceus/collect_ground_1208.kai; \
	exp=../examples/perceus/collect_ground_1208.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-cg1208 2> build/s2-cg1208.err \
	  || { echo "collect-ground-1208 FAIL (native build errored)"; cat build/s2-cg1208.err; exit 1; }; \
	./build/s2-cg1208 > build/s2-cg1208.out; \
	diff -q $$exp build/s2-cg1208.out > /dev/null \
	  || { echo "collect-ground-1208 FAIL (output diff)"; diff $$exp build/s2-cg1208.out; exit 1; }; \
	./$(TARGET) --emit=kir --path ../stdlib --edition hanga-roa $$src > build/s2-cg1208.kir 2>/dev/null; \
	awk '/^fn rmcv_loop__cspec.*mono__Point/,/^}/' build/s2-cg1208.kir > build/s2-cg1208-loop.kir; \
	test -s build/s2-cg1208-loop.kir \
	  || { echo "collect-ground-1208 FAIL (no grounded collect loop emitted — return-only tparam not grounded)"; exit 1; }; \
	grep -E -q '^fn rmcv_loop__cspec.*mono__Point\(i: i64' build/s2-cg1208-loop.kir \
	  || { echo "collect-ground-1208 FAIL (collect loop index not unboxed to i64)"; head -1 build/s2-cg1208-loop.kir; exit 1; }; \
	if grep -F -q 'call_ind' build/s2-cg1208-loop.kir; then \
	  echo "collect-ground-1208 FAIL (grounded collect loop still has a per-element indirect call)"; \
	  grep -F 'call_ind' build/s2-cg1208-loop.kir; exit 1; \
	fi; \
	echo "collect-ground-1208 OK (sum matches; return-only element grounded from call site, collect loop unboxed with zero per-element call_ind)"

# Issue #1208 (native) — bind-once in the beta-reducer. A specialised
# stage that reads a BOXED param more than once (`p => R { a: p.a, b: p.b }`)
# must bind it to one let, else naive substitution duplicates the value and
# perceus drops it once per read (second read = freed memory). Every field
# of every element must round-trip; the emitted spec must carry the `__csb`
# bind-once binder (proof the guard fired). Native-gated (tier1-native).
test-collect-multiread-1208: $(TARGET) build
	@set -e; \
	src=../examples/perceus/collect_multiread_1208.kai; \
	exp=../examples/perceus/collect_multiread_1208.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-mr1208 2> build/s2-mr1208.err \
	  || { echo "collect-multiread-1208 FAIL (native build errored)"; cat build/s2-mr1208.err; exit 1; }; \
	./build/s2-mr1208 > build/s2-mr1208.out; \
	diff -q $$exp build/s2-mr1208.out > /dev/null \
	  || { echo "collect-multiread-1208 FAIL (output diff — a boxed field was freed before its second read)"; diff $$exp build/s2-mr1208.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src 2>/dev/null | grep -F -q '__csb_' \
	  || { echo "collect-multiread-1208 FAIL (no bind-once binder emitted — the multi-read guard did not fire)"; exit 1; }; \
	echo "collect-multiread-1208 OK (every boxed field round-trips; bind-once binder present)"

# Issue #1134 (native) — the range-headed terminal fold must fold a 1M-wide
# range in O(1) stack on native (the counting loop is tail-recursive) and
# produce the exact result. Native-gated (tier1-native).
test-perceus-1134-pipe-fusion-terminal-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_terminal_1134.kai; \
	exp=../examples/perceus/pipe_fusion_terminal_1134.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1134pftn 2> build/s2-1134pftn.err \
	  || { echo "perceus-1134-pipe-fusion-terminal-native FAIL (native build errored)"; cat build/s2-1134pftn.err; exit 1; }; \
	./build/s2-1134pftn > build/s2-1134pftn.out; \
	diff -q $$exp build/s2-1134pftn.out > /dev/null \
	  && echo "perceus-1134-pipe-fusion-terminal-native OK (range terminal-fold byte-identical on native, O(1) stack)" \
	  || { echo "perceus-1134-pipe-fusion-terminal-native FAIL (native output diff)"; diff $$exp build/s2-1134pftn.out; exit 1; }

# Issue #1143 (native) — lambda-stage fusion parity: exact output + the
# per-element path stays allocation-free on native too. Native-gated
# (tier1-native).
test-perceus-1143-pipe-fusion-lambda-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_lambda_1143.kai; \
	exp=../examples/perceus/pipe_fusion_lambda_1143.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1143pfln 2> build/s2-1143pfln.err \
	  || { echo "perceus-1143-pipe-fusion-lambda-native FAIL (native build errored)"; cat build/s2-1143pfln.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1143pfln > build/s2-1143pfln.out 2> build/s2-1143pfln.rc; \
	diff -q $$exp build/s2-1143pfln.out > /dev/null \
	  || { echo "perceus-1143-pipe-fusion-lambda-native FAIL (output diff)"; diff $$exp build/s2-1143pfln.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1143pfln.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] && [ "$$tot" -lt 100 ] \
	  && echo "perceus-1143-pipe-fusion-lambda-native OK (alloc_total=$$tot < 100 on native)" \
	  || { echo "perceus-1143-pipe-fusion-lambda-native FAIL (alloc_total=$$tot)"; exit 1; }

# Issue #1143 (native) — ring/length terminal fusion parity on native.
# Native-gated (tier1-native).
test-perceus-1143-pipe-fusion-ring-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/pipe_fusion_ring_1143.kai; \
	exp=../examples/perceus/pipe_fusion_ring_1143.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1143pfrn 2> build/s2-1143pfrn.err \
	  || { echo "perceus-1143-pipe-fusion-ring-native FAIL (native build errored)"; cat build/s2-1143pfrn.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1143pfrn > build/s2-1143pfrn.out 2> build/s2-1143pfrn.rc; \
	diff -q $$exp build/s2-1143pfrn.out > /dev/null \
	  || { echo "perceus-1143-pipe-fusion-ring-native FAIL (output diff)"; diff $$exp build/s2-1143pfrn.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1143pfrn.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] && [ "$$tot" -lt 100 ] \
	  && echo "perceus-1143-pipe-fusion-ring-native OK (alloc_total=$$tot < 100 on native)" \
	  || { echo "perceus-1143-pipe-fusion-ring-native FAIL (alloc_total=$$tot)"; exit 1; }

# Issue #1117 (native) — same closure-param leak on the native backend: the
# lifted `KFnLambda` never dropped its owned params. The fix emits a `KDrop`
# per param before `KRet`. Native-gated (tier1-native); asserts output parity
# + `leaked` < 1000. The map pipe here projects a field, so no nested-closure
# capture is involved (the capture-lifecycle residual is a separate concern).
test-perceus-1117-native-map-pipe-record-leak: $(TARGET) build
	@set -e; \
	src=../examples/perceus/map_pipe_record_leak_1117.kai; \
	exp=../examples/perceus/map_pipe_record_leak_1117.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1117mprln 2> build/s2-1117mprln.err \
	  || { echo "perceus-1117-native-map-pipe-record-leak FAIL (native build errored)"; cat build/s2-1117mprln.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1117mprln > build/s2-1117mprln.out 2> build/s2-1117mprln.rc; \
	diff -q $$exp build/s2-1117mprln.out > /dev/null \
	  || { echo "perceus-1117-native-map-pipe-record-leak FAIL (output diff)"; diff $$exp build/s2-1117mprln.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1117mprln.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1117-native-map-pipe-record-leak FAIL (no leaked line)"; cat build/s2-1117mprln.rc; exit 1; }; \
	[ "$$leaked" -lt 1000 ] \
	  && echo "perceus-1117-native-map-pipe-record-leak OK (native leaked=$$leaked < 1000; closure-param KDrop landed)" \
	  || { echo "perceus-1117-native-map-pipe-record-leak FAIL (native leaked=$$leaked — closure-param leak regressed)"; exit 1; }

# Issue #1120 — borrow annotations on read-only Array prims collapse the
# per-access container refcounting. The Array[Int] sum over N=100 used to
# cost 2 rc ops per access (200 incref) — one container incref + decref
# each. Borrowing the container drops those to 0, leaving only the element
# incref (100, the value escaping the array). The gate asserts the total
# rc traffic fell below the pre-borrow container-inclusive floor (< 150,
# vs 200 before) AND the output is exact. Runs the C oracle (this target)
# and the native backend (the -native variant) so both inherit the same
# borrow KIR. A regression that re-emits the container dup pushes incref
# back to ~200 and trips the threshold.
test-perceus-1120-array-borrow: $(TARGET) build
	@set -e; \
	src=../examples/perceus/array_borrow_read_1120.kai; \
	exp=../examples/perceus/array_borrow_read_1120.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1120ab.c \
	  || { echo "perceus-1120-array-borrow FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1120ab.c -o build/s2-1120ab $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1120ab > build/s2-1120ab.out 2> build/s2-1120ab.rc; \
	diff -q $$exp build/s2-1120ab.out > /dev/null \
	  || { echo "perceus-1120-array-borrow FAIL (output diff)"; diff $$exp build/s2-1120ab.out; exit 1; }; \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-1120ab.rc | head -1 | cut -d= -f2); \
	[ -n "$$inc" ] \
	  || { echo "perceus-1120-array-borrow FAIL (no incref_total line)"; cat build/s2-1120ab.rc; exit 1; }; \
	[ "$$inc" -lt 150 ] \
	  && echo "perceus-1120-array-borrow OK (incref_total=$$inc < 150 over 100 reads; container per-access RC elided, only the element escapes)" \
	  || { echo "perceus-1120-array-borrow FAIL (incref_total=$$inc — container dup regressed, expected ~200 pre-borrow)"; exit 1; }

test-perceus-1120-array-borrow-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/array_borrow_read_1120.kai; \
	exp=../examples/perceus/array_borrow_read_1120.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1120abn 2> build/s2-1120abn.err \
	  || { echo "perceus-1120-array-borrow-native FAIL (native build errored)"; cat build/s2-1120abn.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1120abn > build/s2-1120abn.out 2> build/s2-1120abn.rc; \
	diff -q $$exp build/s2-1120abn.out > /dev/null \
	  || { echo "perceus-1120-array-borrow-native FAIL (output diff)"; diff $$exp build/s2-1120abn.out; exit 1; }; \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-1120abn.rc | head -1 | cut -d= -f2); \
	[ -n "$$inc" ] \
	  || { echo "perceus-1120-array-borrow-native FAIL (no incref_total line)"; cat build/s2-1120abn.rc; exit 1; }; \
	[ "$$inc" -lt 150 ] \
	  && echo "perceus-1120-array-borrow-native OK (native incref_total=$$inc < 150; container borrow inherited from the shared KIR)" \
	  || { echo "perceus-1120-array-borrow-native FAIL (native incref_total=$$inc — container dup regressed)"; exit 1; }

# Issue #1135 stage 1 — Vec[T] pure value vector: uniqueness in-place /
# copy-on-write discipline plus unboxed element storage. Four gates:
#   1. unique linear chain (100 sets + 50 pushes): output exact AND the
#      trace shows vec_inplace=150 vec_cow=0 — zero copies, the writes
#      all landed in the one buffer;
#   2. shared write: output proves the ORIGINAL is intact after a write
#      through a second binding, and the trace shows vec_cow=1 —
#      exactly one copy (an in-place write here silently corrupts a
#      shared value; a second copy is a lost-uniqueness regression);
#   3. storage kinds: raw Int/Real, inline all-scalar record, boxed
#      fallback (String / Option), and the PENDING empty-vec path all
#      round-trip through set/push/get;
#   4. purity: every helper in the effects fixture carries a PURE row —
#      compiling at all asserts no Vec op demands an effect.
PERCEUS_1135_FIXTURES := vec_unique_inplace_1135 vec_shared_cow_1135 vec_unboxed_storage_1135

test-perceus-1135-vec-value: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1135_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-$$name.c \
	    || { echo "perceus-1135-vec FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1135-vec FAIL ($$name output diff)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	grep -q 'vec_inplace=150 vec_cow=0' build/s2-vec_unique_inplace_1135.rc \
	  || { echo "perceus-1135-vec FAIL (unique chain: expected vec_inplace=150 vec_cow=0)"; \
	       grep vec_ build/s2-vec_unique_inplace_1135.rc || true; exit 1; }; \
	grep -q 'vec_cow=1' build/s2-vec_shared_cow_1135.rc \
	  || { echo "perceus-1135-vec FAIL (shared write: expected vec_cow=1)"; \
	       grep vec_ build/s2-vec_shared_cow_1135.rc || true; exit 1; }; \
	src=../examples/effects/vec_pure_row_1135.kai; \
	exp=../examples/effects/vec_pure_row_1135.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-vec_pure_row.c \
	  || { echo "perceus-1135-vec FAIL (pure-row fixture rejected — a Vec op raised an effect demand)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-vec_pure_row.c -o build/s2-vec_pure_row $(LDLIBS_M); \
	./build/s2-vec_pure_row > build/s2-vec_pure_row.out; \
	diff -q $$exp build/s2-vec_pure_row.out > /dev/null \
	  || { echo "perceus-1135-vec FAIL (pure-row output diff)"; diff $$exp build/s2-vec_pure_row.out; exit 1; }; \
	echo "perceus-1135-vec OK (in-place=150/cow=0 unique, cow=1 shared, 3 storage kinds round-trip, rows stay pure)"

test-perceus-1135-vec-value-native: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1135_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ../bin/kai build $$src -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1135-vec-native FAIL ($$name native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc; \
	  diff -q $$exp build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1135-vec-native FAIL ($$name output diff)"; diff $$exp build/s2-$$name-n.out; exit 1; }; \
	done; \
	grep -q 'vec_inplace=150 vec_cow=0' build/s2-vec_unique_inplace_1135-n.rc \
	  || { echo "perceus-1135-vec-native FAIL (unique chain: expected vec_inplace=150 vec_cow=0)"; \
	       grep vec_ build/s2-vec_unique_inplace_1135-n.rc || true; exit 1; }; \
	grep -q 'vec_cow=1' build/s2-vec_shared_cow_1135-n.rc \
	  || { echo "perceus-1135-vec-native FAIL (shared write: expected vec_cow=1)"; \
	       grep vec_ build/s2-vec_shared_cow_1135-n.rc || true; exit 1; }; \
	src=../examples/effects/vec_pure_row_1135.kai; \
	exp=../examples/effects/vec_pure_row_1135.out.expected; \
	../bin/kai build $$src -o build/s2-vec_pure_row-n 2> build/s2-vec_pure_row-n.err \
	  || { echo "perceus-1135-vec-native FAIL (pure-row native build errored)"; cat build/s2-vec_pure_row-n.err; exit 1; }; \
	./build/s2-vec_pure_row-n > build/s2-vec_pure_row-n.out; \
	diff -q $$exp build/s2-vec_pure_row-n.out > /dev/null \
	  || { echo "perceus-1135-vec-native FAIL (pure-row output diff)"; diff $$exp build/s2-vec_pure_row-n.out; exit 1; }; \
	echo "perceus-1135-vec-native OK (counters + outputs match the C-direct gate)"

# Issue #1135 stage 2 — Vec[T] raw element paths. Three gates:
#   1. raw fusion on the inline-record carrier: fused push/set/get-field
#      round-trip (output exact), PENDING classified through the raw
#      push, and the trace shows vec_inplace=101 vec_cow=2 — the raw
#      writes still ride the unique-or-copy decision (a shared raw set
#      COPIES; in-place there would corrupt the second binding);
#   2. the shared-write outputs prove the original vector is intact
#      after a raw set through a second binding;
#   3. the stdlib-module map shape: a module-qualified self-call must
#      still tcrec-rewrite to the goto (over-release + stack growth
#      without it) — gated by output diff incl. a 200k-deep map.
PERCEUS_1135_RAW_FIXTURES := vec_rec_raw_fusion_1135 vec_stdlib_map_qualified_tco_1135

test-perceus-1135-vec-raw: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1135_RAW_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-$$name.c \
	    || { echo "perceus-1135-vec-raw FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1135-vec-raw FAIL ($$name output diff)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	grep -q 'vec_inplace=101 vec_cow=2' build/s2-vec_rec_raw_fusion_1135.rc \
	  || { echo "perceus-1135-vec-raw FAIL (raw fusion: expected vec_inplace=101 vec_cow=2)"; \
	       grep vec_ build/s2-vec_rec_raw_fusion_1135.rc || true; exit 1; }; \
	echo "perceus-1135-vec-raw OK (raw moves ride unique-or-copy; qualified self-call keeps the goto)"

test-perceus-1135-vec-raw-native: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1135_RAW_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ../bin/kai build $$src -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1135-vec-raw-native FAIL ($$name native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc; \
	  diff -q $$exp build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1135-vec-raw-native FAIL ($$name output diff)"; diff $$exp build/s2-$$name-n.out; exit 1; }; \
	done; \
	grep -q 'vec_inplace=101 vec_cow=2' build/s2-vec_rec_raw_fusion_1135-n.rc \
	  || { echo "perceus-1135-vec-raw-native FAIL (raw fusion: expected vec_inplace=101 vec_cow=2)"; \
	       grep vec_ build/s2-vec_rec_raw_fusion_1135-n.rc || true; exit 1; }; \
	echo "perceus-1135-vec-raw-native OK (counters + outputs match the C-direct gate)"

# Issue #1151 — recursive vec_push growth from empty. A loop threading
# the vec through the stdlib module call appends on the SHARED (CoW)
# path every iteration; the clone capacity must grow only when the vec
# is full — unconditional doubling overflowed the size computation at
# ~100 pushes ("out of memory" with kilobytes of live data). Fixture 1
# is the issue repro (Int + record, N=100); fixture 2 stresses both
# append regimes (module CoW + builtin raw) to 100k with probe sums
# over the grown range.
PERCEUS_1151_GROWTH_FIXTURES := vec_push_growth_1151 vec_push_growth_stress_1151

test-perceus-1151-vec-push-growth: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1151_GROWTH_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-$$name.c \
	    || { echo "perceus-1151-vec-push-growth FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  ./build/s2-$$name > build/s2-$$name.out; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1151-vec-push-growth FAIL ($$name output diff)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	echo "perceus-1151-vec-push-growth OK (recursive push growth linear on both append regimes)"

test-perceus-1151-vec-push-growth-native: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1151_GROWTH_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ../bin/kai build $$src -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1151-vec-push-growth-native FAIL ($$name native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  ./build/s2-$$name-n > build/s2-$$name-n.out; \
	  diff -q $$exp build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1151-vec-push-growth-native FAIL ($$name output diff)"; diff $$exp build/s2-$$name-n.out; exit 1; }; \
	done; \
	echo "perceus-1151-vec-push-growth-native OK (outputs match the C-direct gate)"

test-perceus-1151-vec-push-growth-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1151_GROWTH_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	    echo "perceus-1151-vec-push-growth-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/s2-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	    || { echo "perceus-1151-vec-push-growth-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	done; \
	echo "perceus-1151-vec-push-growth-asan OK"

# Issue #1153 — module-call consumer linearity + forwarder inlining.
# Both fixtures assert exact counters: every vec write in the loops
# lands in place (vec_cow=0), on the qualified non-forwarder call
# (modcall_linear_1153) and on the idiomatic stdlib surface
# (vec_module_surface_1153, whose emitted C must also reach the raw
# record-push fuse the forwarder inline exposes).
PERCEUS_1153_FIXTURES := modcall_linear_1153 vec_module_surface_1153

test-perceus-1153-modcall-linear: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1153_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-$$name.c \
	    || { echo "perceus-1153-modcall-linear FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1153-modcall-linear FAIL ($$name output diff)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	  grep -q 'vec_inplace=2000 vec_cow=0' build/s2-$$name.rc \
	    || { echo "perceus-1153-modcall-linear FAIL ($$name: expected vec_inplace=2000 vec_cow=0)"; \
	         grep vec_ build/s2-$$name.rc || true; exit 1; }; \
	done; \
	grep -q 'kai_vec_push_rec_raw' build/s2-vec_module_surface_1153.c \
	  || { echo "perceus-1153-modcall-linear FAIL (module surface missed the raw push fuse)"; exit 1; }; \
	echo "perceus-1153-modcall-linear OK (module-call loops move the vec: writes in place on both spellings)"

test-perceus-1153-modcall-linear-native: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1153_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ../bin/kai build $$src -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1153-modcall-linear-native FAIL ($$name native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc; \
	  diff -q $$exp build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1153-modcall-linear-native FAIL ($$name output diff)"; diff $$exp build/s2-$$name-n.out; exit 1; }; \
	  grep -q 'vec_inplace=2000 vec_cow=0' build/s2-$$name-n.rc \
	    || { echo "perceus-1153-modcall-linear-native FAIL ($$name: expected vec_inplace=2000 vec_cow=0)"; \
	         grep vec_ build/s2-$$name-n.rc || true; exit 1; }; \
	done; \
	echo "perceus-1153-modcall-linear-native OK (counters match the C-direct gate)"

test-perceus-1153-modcall-linear-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1153_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	    echo "perceus-1153-modcall-linear-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/s2-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	    || { echo "perceus-1153-modcall-linear-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	done; \
	echo "perceus-1153-modcall-linear-asan OK"

# Issue #1150 — Vec surface stage 3: seamless slices, literal minting,
# fused pipeline collect. Gates:
#   1. descent: `[h, ...t]` binds an O(1) view and a unique view
#      re-slices IN PLACE — vec_cow=0 and alloc_total < 50 over a
#      1000-deep walk (a per-level allocation regression lands >1000);
#   2. pin/sharing: exact vec_inplace=2 vec_cow=2 — a live slice pins
#      the buffer, writes to the pinned source and through the slice
#      copy, an unshared write stays in place;
#   3. minting: annotation + argument position build pre-sized vecs,
#      the bare literal stays a cons list, Byte elements retag;
#   4. collect: a pure Vec-typed range chain collects into ONE buffer —
#      alloc_total < 100 at 100k elements (unfused would cons ~100k) —
#      and list-source / impure chains fall back through vec_from_list.
PERCEUS_1150_FIXTURES := vec_slice_descent_1150 vec_slice_pin_1150 vec_mint_1150 vec_collect_1150

test-perceus-1150-vec-surface: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1150_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-$$name.c \
	    || { echo "perceus-1150-vec-surface FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1150-vec-surface FAIL ($$name output diff)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	done; \
	grep -q 'vec_cow=0' build/s2-vec_slice_descent_1150.rc \
	  || { echo "perceus-1150-vec-surface FAIL (descent: expected vec_cow=0)"; \
	       grep vec_ build/s2-vec_slice_descent_1150.rc || true; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-vec_slice_descent_1150.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] && [ "$$tot" -lt 50 ] \
	  || { echo "perceus-1150-vec-surface FAIL (descent alloc_total=$$tot — per-level allocation regressed)"; exit 1; }; \
	grep -q 'vec_inplace=2 vec_cow=2' build/s2-vec_slice_pin_1150.rc \
	  || { echo "perceus-1150-vec-surface FAIL (pin: expected vec_inplace=2 vec_cow=2)"; \
	       grep vec_ build/s2-vec_slice_pin_1150.rc || true; exit 1; }; \
	ctot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-vec_collect_1150.rc | head -1 | cut -d= -f2); \
	[ -n "$$ctot" ] && [ "$$ctot" -lt 100 ] \
	  || { echo "perceus-1150-vec-surface FAIL (collect alloc_total=$$ctot — chain rematerialised)"; exit 1; }; \
	echo "perceus-1150-vec-surface OK (descent alloc=$$tot cow=0, pin exact, mint, collect alloc=$$ctot)"

test-perceus-1150-vec-surface-native: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1150_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ../bin/kai build $$src -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1150-vec-surface-native FAIL ($$name native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc; \
	  diff -q $$exp build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1150-vec-surface-native FAIL ($$name output diff)"; diff $$exp build/s2-$$name-n.out; exit 1; }; \
	done; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-vec_slice_descent_1150-n.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] && [ "$$tot" -lt 50 ] \
	  || { echo "perceus-1150-vec-surface-native FAIL (descent alloc_total=$$tot)"; exit 1; }; \
	echo "perceus-1150-vec-surface-native OK (outputs match; descent alloc=$$tot)"

test-perceus-1150-vec-surface-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1150_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	    echo "perceus-1150-vec-surface-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/s2-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	    || { echo "perceus-1150-vec-surface-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	done; \
	echo "perceus-1150-vec-surface-asan OK"

# Issue #1126 — the tail-recursive read loop must drop its re-threaded
# Array container. Post-#1120 the borrow read stopped consuming the
# container but the dup fallback still fired on the re-thread, so the goto
# re-bound the array to itself without dropping the original: one leaked
# container ref per iteration. With Array[Record] the un-freed array
# pinned all N boxed records, so `leaked` scaled with N (~1004 for N=1000).
# The fix suppresses that dup while keeping the base arm's exit drop, so
# the array — and its records — are reclaimed and `leaked` is bounded
# (constant, independent of N). Gates the record + Int fixtures on BOTH
# backends: output exact, `leaked` well under the O(N) regression floor.
PERCEUS_1126_FIXTURES := array_read_loop_drop_1126 array_read_loop_drop_int_1126

test-perceus-1126-read-loop-drop: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1126_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-$$name.c \
	    || { echo "perceus-1126-read-loop-drop FAIL ($$name: kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	  diff -q $$exp build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1126-read-loop-drop FAIL ($$name: output diff)"; diff $$exp build/s2-$$name.out; exit 1; }; \
	  leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name.rc | head -1 | cut -d= -f2); \
	  [ -n "$$leaked" ] \
	    || { echo "perceus-1126-read-loop-drop FAIL ($$name: no leaked line)"; cat build/s2-$$name.rc; exit 1; }; \
	  [ "$$leaked" -lt 100 ] \
	    || { echo "perceus-1126-read-loop-drop FAIL ($$name: leaked=$$leaked — container drop regressed, grows with N)"; exit 1; }; \
	  echo "perceus-1126-read-loop-drop OK ($$name: C leaked=$$leaked < 100; array + records reclaimed)"; \
	done

test-perceus-1126-read-loop-drop-native: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_1126_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; exp=../examples/perceus/$$name.out.expected; \
	  KAI_BACKEND=native ../bin/kai build $$src -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1126-read-loop-drop-native FAIL ($$name: native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc; \
	  diff -q $$exp build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1126-read-loop-drop-native FAIL ($$name: output diff)"; diff $$exp build/s2-$$name-n.out; exit 1; }; \
	  leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name-n.rc | head -1 | cut -d= -f2); \
	  [ -n "$$leaked" ] \
	    || { echo "perceus-1126-read-loop-drop-native FAIL ($$name: no leaked line)"; cat build/s2-$$name-n.rc; exit 1; }; \
	  [ "$$leaked" -lt 100 ] \
	    || { echo "perceus-1126-read-loop-drop-native FAIL ($$name: native leaked=$$leaked — container drop regressed)"; exit 1; }; \
	  echo "perceus-1126-read-loop-drop-native OK ($$name: native leaked=$$leaked < 100; array + records reclaimed)"; \
	done

# Issue #1127 — borrow generalized to user parameters. The closure probe
# (`go(n, acc, f)`) threads a closure through a tail-recursive loop; under
# the owned convention it paid 2 rc ops per iteration (caller incref +
# callee decref of `f`). The relaxed inference borrows `f` (no annotation —
# `go` is non-`pub`). A CLOSURE param is not borrowed in codegen — the
# runtime `kai_apply` consumes it and there is no borrowing call variant —
# so this gate pins SOUNDNESS: the `^` serializes in the ABI but the closure
# stays owned, neither leaked nor double-freed. `leaked` stays bounded
# (constant, not O(iterations)). The RC collapse is gated on a borrowing
# call variant (follow-up). The stdlib HOF gate mirrors this.
test-perceus-1127-borrow-closure: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_closure_1127.kai; \
	exp=../examples/perceus/borrow_closure_1127.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1127bc.c \
	  || { echo "perceus-1127-borrow-closure FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1127bc.c -o build/s2-1127bc $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1127bc > build/s2-1127bc.out 2> build/s2-1127bc.rc; \
	diff -q $$exp build/s2-1127bc.out > /dev/null \
	  || { echo "perceus-1127-borrow-closure FAIL (output diff)"; diff $$exp build/s2-1127bc.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1127bc.rc | head -1 | cut -d= -f2); \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-1127bc.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] && [ "$$leaked" -lt 100 ] \
	  || { echo "perceus-1127-borrow-closure FAIL (leaked=$$leaked — closure orphaned / double-freed)"; exit 1; }; \
	[ -n "$$inc" ] && [ "$$inc" -lt 100 ] \
	  && echo "perceus-1127-borrow-closure OK (#1130: incref_total=$$inc < 100 over 1000 iters — closure per-iteration RC collapsed to ~0; leaked=$$leaked bounded)" \
	  || { echo "perceus-1127-borrow-closure FAIL (incref_total=$$inc — closure ^ NOT borrowed, per-iter dup regressed; expected ~2000 pre-#1130)"; exit 1; }

test-perceus-1127-borrow-closure-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_closure_1127.kai; \
	exp=../examples/perceus/borrow_closure_1127.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1127bcn 2> build/s2-1127bcn.err \
	  || { echo "perceus-1127-borrow-closure-native FAIL (native build errored)"; cat build/s2-1127bcn.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1127bcn > build/s2-1127bcn.out 2> build/s2-1127bcn.rc; \
	diff -q $$exp build/s2-1127bcn.out > /dev/null \
	  || { echo "perceus-1127-borrow-closure-native FAIL (output diff)"; diff $$exp build/s2-1127bcn.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1127bcn.rc | head -1 | cut -d= -f2); \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-1127bcn.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] && [ "$$leaked" -lt 100 ] \
	  || { echo "perceus-1127-borrow-closure-native FAIL (native leaked=$$leaked — closure orphaned)"; exit 1; }; \
	[ -n "$$inc" ] && [ "$$inc" -lt 100 ] \
	  && echo "perceus-1127-borrow-closure-native OK (#1130 native: incref_total=$$inc < 100; closure borrow inherited from shared KIR, leaked=$$leaked bounded)" \
	  || { echo "perceus-1127-borrow-closure-native FAIL (native incref_total=$$inc — closure per-iter dup regressed)"; exit 1; }

test-perceus-1127-borrow-hof: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_stdlib_hof_1127.kai; \
	exp=../examples/perceus/borrow_stdlib_hof_1127.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1127hof.c \
	  || { echo "perceus-1127-borrow-hof FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1127hof.c -o build/s2-1127hof $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1127hof > build/s2-1127hof.out 2> build/s2-1127hof.rc; \
	diff -q $$exp build/s2-1127hof.out > /dev/null \
	  || { echo "perceus-1127-borrow-hof FAIL (output diff)"; diff $$exp build/s2-1127hof.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1127hof.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] && [ "$$leaked" -lt 100 ] \
	  && echo "perceus-1127-borrow-hof OK (#1130: stdlib map/filter/foldl ^ closure ACTIVE — foldl borrow-through, map/filter dup-on-consume at the owned loop; leaked=$$leaked bounded, no UAF)" \
	  || { echo "perceus-1127-borrow-hof FAIL (leaked=$$leaked — closure ^ orphaned the closure or double-freed)"; exit 1; }

# Native half of the HOF gate (runs on the native suite — the tier1 shard's
# kaic2 is C-only). Split out per the #1126 precedent.
test-perceus-1127-borrow-hof-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_stdlib_hof_1127.kai; \
	exp=../examples/perceus/borrow_stdlib_hof_1127.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1127hofn 2> build/s2-1127hofn.err \
	  || { echo "perceus-1127-borrow-hof-native FAIL (native build errored)"; cat build/s2-1127hofn.err; exit 1; }; \
	./build/s2-1127hofn > build/s2-1127hofn.out; \
	diff -q $$exp build/s2-1127hofn.out > /dev/null \
	  && echo "perceus-1127-borrow-hof-native OK" \
	  || { echo "perceus-1127-borrow-hof-native FAIL (output diff)"; diff $$exp build/s2-1127hofn.out; exit 1; }

# Issue #1127 — TCO preserved for a borrowed self-recursive descent. A
# borrow-through tail call plants no drop, so a 1M-deep left-descent runs
# without growing the stack. A regression that broke tailness would SIGSEGV
# (stack overflow) here.
test-perceus-1127-borrow-tco: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_tco_descent_1127.kai; \
	exp=../examples/perceus/borrow_tco_descent_1127.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1127tco.c \
	  || { echo "perceus-1127-borrow-tco FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1127tco.c -o build/s2-1127tco $(LDLIBS_M); \
	./build/s2-1127tco > build/s2-1127tco.out \
	  || { echo "perceus-1127-borrow-tco FAIL (stack overflow — tailness lost)"; exit 1; }; \
	diff -q $$exp build/s2-1127tco.out > /dev/null \
	  && echo "perceus-1127-borrow-tco OK (1M-deep borrowed descent, no stack growth)" \
	  || { echo "perceus-1127-borrow-tco FAIL (output diff)"; diff $$exp build/s2-1127tco.out; exit 1; }

# Native half of the TCO gate (native suite — the tier1 shard's kaic2 is
# C-only). Split out per the #1126 precedent.
test-perceus-1127-borrow-tco-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_tco_descent_1127.kai; \
	exp=../examples/perceus/borrow_tco_descent_1127.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1127tcon 2> build/s2-1127tcon.err \
	  || { echo "perceus-1127-borrow-tco-native FAIL (native build errored)"; cat build/s2-1127tcon.err; exit 1; }; \
	./build/s2-1127tcon > build/s2-1127tcon.out \
	  || { echo "perceus-1127-borrow-tco-native FAIL (stack overflow)"; exit 1; }; \
	diff -q $$exp build/s2-1127tcon.out > /dev/null \
	  && echo "perceus-1127-borrow-tco-native OK" \
	  || { echo "perceus-1127-borrow-tco-native FAIL (output diff)"; diff $$exp build/s2-1127tcon.out; exit 1; }

# Issue #1130 — a `^`-borrowed closure through the apply-pipe (`a |> f`)
# AND a later direct call. A regression that let the pipe consume the
# skip-set closure crashes "non-callable value" on the second read.
test-perceus-1130-borrow-pipe: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_closure_pipe_1130.kai; \
	exp=../examples/perceus/borrow_closure_pipe_1130.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1130bp.c \
	  || { echo "perceus-1130-borrow-pipe FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1130bp.c -o build/s2-1130bp $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1130bp > build/s2-1130bp.out 2> build/s2-1130bp.rc; \
	diff -q $$exp build/s2-1130bp.out > /dev/null \
	  || { echo "perceus-1130-borrow-pipe FAIL (output diff — pipe-borrow UAF?)"; diff $$exp build/s2-1130bp.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1130bp.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] && [ "$$leaked" -lt 100 ] \
	  && echo "perceus-1130-borrow-pipe OK (apply-pipe borrow sound over two reads; leaked=$$leaked bounded)" \
	  || { echo "perceus-1130-borrow-pipe FAIL (leaked=$$leaked)"; exit 1; }

# Native half (tier1-native suite; the tier1 shard's kaic2 is C-only).
test-perceus-1130-borrow-pipe-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_closure_pipe_1130.kai; \
	exp=../examples/perceus/borrow_closure_pipe_1130.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1130bpn 2> build/s2-1130bpn.err \
	  || { echo "perceus-1130-borrow-pipe-native FAIL (native build errored)"; cat build/s2-1130bpn.err; exit 1; }; \
	./build/s2-1130bpn > build/s2-1130bpn.out \
	  || { echo "perceus-1130-borrow-pipe-native FAIL (crash — pipe-borrow UAF?)"; exit 1; }; \
	diff -q $$exp build/s2-1130bpn.out > /dev/null \
	  && echo "perceus-1130-borrow-pipe-native OK" \
	  || { echo "perceus-1130-borrow-pipe-native FAIL (output diff)"; diff $$exp build/s2-1130bpn.out; exit 1; }

# Issue #1130 — a freshly materialised closure passed to a borrowed HOF
# slot in the caller's TAIL position, inside a 5000-iteration loop. The
# caller owns the closure but the HOF borrows it, so the drop-after-call
# must free it each iteration WITHOUT breaking the caller's tailness. A
# regression leaks one closure per iteration (leaked grows O(N)).
test-perceus-1130-hof-tail-drop: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_hof_tail_drop_1130.kai; \
	exp=../examples/perceus/borrow_hof_tail_drop_1130.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1130htd.c \
	  || { echo "perceus-1130-hof-tail-drop FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1130htd.c -o build/s2-1130htd $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1130htd > build/s2-1130htd.out 2> build/s2-1130htd.rc; \
	diff -q $$exp build/s2-1130htd.out > /dev/null \
	  || { echo "perceus-1130-hof-tail-drop FAIL (output diff)"; diff $$exp build/s2-1130htd.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1130htd.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] && [ "$$leaked" -lt 100 ] \
	  && echo "perceus-1130-hof-tail-drop OK (5000 tail-position closures dropped, leaked=$$leaked bounded; TCO intact)" \
	  || { echo "perceus-1130-hof-tail-drop FAIL (leaked=$$leaked — closure leaked per iter, drop-after-call regressed)"; exit 1; }

# Native half (tier1-native suite).
test-perceus-1130-hof-tail-drop-native: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_hof_tail_drop_1130.kai; \
	exp=../examples/perceus/borrow_hof_tail_drop_1130.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1130htdn 2> build/s2-1130htdn.err \
	  || { echo "perceus-1130-hof-tail-drop-native FAIL (native build errored)"; cat build/s2-1130htdn.err; exit 1; }; \
	./build/s2-1130htdn > build/s2-1130htdn.out \
	  || { echo "perceus-1130-hof-tail-drop-native FAIL (crash / stack overflow)"; exit 1; }; \
	diff -q $$exp build/s2-1130htdn.out > /dev/null \
	  && echo "perceus-1130-hof-tail-drop-native OK" \
	  || { echo "perceus-1130-hof-tail-drop-native FAIL (output diff)"; diff $$exp build/s2-1130htdn.out; exit 1; }
# --- issue #1131 targets (appended on rebase over #1132) ---
test-perceus-1131-borrow-descent: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_descent_1127.kai; \
	exp=../examples/perceus/borrow_descent_1127.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1131desc.c \
	  || { echo "perceus-1131-borrow-descent FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1131desc.c -o build/s2-1131desc $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1131desc > build/s2-1131desc.out 2> build/s2-1131desc.rc; \
	diff -q $$exp build/s2-1131desc.out > /dev/null \
	  || { echo "perceus-1131-borrow-descent FAIL (output diff)"; diff $$exp build/s2-1131desc.out; exit 1; }; \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-1131desc.rc | head -1 | cut -d= -f2); \
	[ -n "$$inc" ] && [ "$$inc" -lt 22000 ] \
	  && echo "perceus-1131-borrow-descent OK (inferred borrow elides per-level dup; incref_total=$$inc < 22000 — below the owned baseline, NO annotation)" \
	  || { echo "perceus-1131-borrow-descent FAIL (incref_total=$$inc >= 22000 — borrow not inferred, regressed to owned)"; exit 1; }

# Issue #1131 — the sep-comp regression gate. A `#[derive(Show)]` record with a
# unit-typed field runs the compiler's inspect-then-escape derive path; the
# relaxed borrow once trusted a dup-on-bind the RC pass never had, so the
# MODULAR-built compiler (donation OFF) UAF'd here with `non-exhaustive match`
# — a shape no single-TU gate could see. This gate builds the compiler under
# `--emit=c-modular` and asserts it compiles the fixture byte-identically to
# the single-TU compiler (a UAF would panic or diverge). Needs KAI_BACKEND=c
# on a native-default kaic2 (the wrapper else routes main.kai to single-TU
# native and the helper link lacks the libLLVM shim).
test-perceus-1131-modular-escape: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_modular_escape_1131.kai; \
	out=$$(pwd)/build/s2-1131modesc.bin; rm -f $$out; \
	( cd .. && KAI_BACKEND=c KAI_MODULAR=1 KAI_MODULAR_CACHE_DIR=$(CMODULAR_SELFHOST_CACHE) KAI_MODULAR_STATS=1 bin/kai build stage2/main.kai -o $$out ) \
	  2> build/s2-1131modesc.log \
	  || { echo "perceus-1131-modular-escape FAIL (modular compiler build/link)"; cat build/s2-1131modesc.log; exit 1; }; \
	KAIKAI_STDLIB_PATH=$(STDLIB_ABS) $$out $$src > build/s2-1131modesc.c 2> build/s2-1131modesc.err \
	  || { echo "perceus-1131-modular-escape FAIL (modular compiler UAF'd compiling the fixture)"; cat build/s2-1131modesc.err; exit 1; }; \
	KAIKAI_STDLIB_PATH=$(STDLIB_ABS) ./$(TARGET) $(TEST_CORE_CACHE) $$src > build/s2-1131modesc-ref.c; \
	diff -q build/s2-1131modesc-ref.c build/s2-1131modesc.c > /dev/null \
	  && echo "perceus-1131-modular-escape OK (modular compiler compiles the escape shape byte-identically to single-TU)" \
	  || { echo "perceus-1131-modular-escape FAIL (modular emit differs from single-TU)"; exit 1; }

# Issue #1295 — borrow-aware max-path discount over-applied to a COMPOUND
# arg in a borrowed callee slot. The fixture's filter loop reads its binder
# nested inside a borrowed slot (`opt_same(rec_mo(r), mo)`) and stores it on
# the kept path; the discount once made every read raw, so the inner owned
# call consumed the binder's only ref and the input list's elements hit rc 0
# while still referenced — the gen2-compiler heap-use-after-free. The gate
# re-reads the input after two filter passes and diffs the golden.
test-perceus-1295-borrow-slot-nested-arg: $(TARGET) build
	@set -e; \
	src=../examples/perceus/borrow_slot_nested_arg_1295.kai; \
	exp=../examples/perceus/borrow_slot_nested_arg_1295.out.expected; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1295bsna.c \
	  || { echo "perceus-1295-borrow-slot-nested-arg FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1295bsna.c -o build/s2-1295bsna $(LDLIBS_M); \
	./build/s2-1295bsna > build/s2-1295bsna.out \
	  || { echo "perceus-1295-borrow-slot-nested-arg FAIL (crash)"; exit 1; }; \
	diff -q $$exp build/s2-1295bsna.out > /dev/null \
	  && echo "perceus-1295-borrow-slot-nested-arg OK" \
	  || { echo "perceus-1295-borrow-slot-nested-arg FAIL (output diff — nested-arg borrow discount regressed)"; diff $$exp build/s2-1295bsna.out; exit 1; }

# ASAN companion for the #1295 gate: same fixture under
# -fsanitize=address,undefined, failing on any sanitizer diagnostic OR
# an output diff (the over-free surfaces as either, depending on pool
# recycling).
test-perceus-1295-borrow-slot-nested-arg-asan: $(TARGET) build
	@set -e; \
	name=borrow_slot_nested_arg_1295; \
	./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	  echo "perceus-1295-borrow-slot-nested-arg-asan FAIL — sanitizer diagnostic:"; \
	  cat build/s2-$$name-asan.err; exit 1; \
	fi; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	  || { echo "perceus-1295-borrow-slot-nested-arg-asan DIFF"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	echo "perceus-1295-borrow-slot-nested-arg-asan OK"

# Issue #1303 — a single-use owned param whose only read sits on one branch
# must be released on every path that never reaches the read. The fixture's
# `tag_of` consumes its owned list only in the final else-arm; the 5000-iter
# driver always takes an early-return arm. Pre-fix each such call leaked the
# list spine (~3 cons/iter, leaked≈15000); fixed, `leaked` is the small
# static-string constant, independent of the loop count.
test-perceus-1303-single-use-branch-leak: $(TARGET) build
	@set -e; \
	name=single_use_param_branch_leak_1303; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-1303subl.c \
	  || { echo "perceus-1303-single-use-branch-leak FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1303subl.c -o build/s2-1303subl $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1303subl > build/s2-1303subl.out 2> build/s2-1303subl.rc \
	  || { echo "perceus-1303-single-use-branch-leak FAIL (crash)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1303subl.out > /dev/null \
	  || { echo "perceus-1303-single-use-branch-leak FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1303subl.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1303subl.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1303-single-use-branch-leak FAIL (no leaked line)"; cat build/s2-1303subl.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-1303-single-use-branch-leak OK (leaked=$$leaked < 100 over 5000 skip-path calls; branchy single-use param released)" \
	  || { echo "perceus-1303-single-use-branch-leak FAIL (leaked=$$leaked — single-use param leaks on paths that skip its read)"; exit 1; }

# ASAN companion for the #1303 gate: same fixture under
# -fsanitize=address,undefined, failing on any sanitizer diagnostic OR an
# output diff. Guards the other direction of the fix: the forced exit drop
# must not double-free the param on the path that DOES consume it.
test-perceus-1303-single-use-branch-leak-asan: $(TARGET) build
	@set -e; \
	name=single_use_param_branch_leak_1303; \
	./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	  echo "perceus-1303-single-use-branch-leak-asan FAIL — sanitizer diagnostic:"; \
	  cat build/s2-$$name-asan.err; exit 1; \
	fi; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	  || { echo "perceus-1303-single-use-branch-leak-asan DIFF"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	echo "perceus-1303-single-use-branch-leak-asan OK"

# Issue #1302 — the tcrec goto-tail path must release the enclosing
# owned match scrutinees and the arm's not-yet-consumed bind-increfs
# the skipped epilogues would have dropped. The fixture walks a 10k
# deconstructing list whose guarded arm tail-recurses; before the fix
# every iteration leaked the cons cell + the arm binder set (leaked
# grows linearly with list length, ~33k at 10k nodes); after, the RC
# report stays flat (a handful of constant startup/report allocations).
test-perceus-1302-tcrec-goto-drops: $(TARGET) build
	@set -e; \
	name=tcrec_goto_owned_binder_1302; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-1302tgd.c \
	  || { echo "perceus-1302-tcrec-goto-drops FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1302tgd.c -o build/s2-1302tgd $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1302tgd > build/s2-1302tgd.out 2> build/s2-1302tgd.rc \
	  || { echo "perceus-1302-tcrec-goto-drops FAIL (crash)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1302tgd.out > /dev/null \
	  || { echo "perceus-1302-tcrec-goto-drops FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1302tgd.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1302tgd.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1302-tcrec-goto-drops FAIL (no leaked line)"; cat build/s2-1302tgd.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-1302-tcrec-goto-drops OK (leaked=$$leaked < 100 over 10k goto-tail iters)" \
	  || { echo "perceus-1302-tcrec-goto-drops FAIL (leaked=$$leaked — goto-tail releases regressed, grows with iters)"; exit 1; }

# ASAN companion for the #1302 gate: the goto-site releases must never
# over-free (a wrongly dropped consumed binder is a UAF, worse than the
# leak). Same fixture under -fsanitize=address,undefined.
test-perceus-1302-tcrec-goto-drops-asan: $(TARGET) build
	@set -e; \
	name=tcrec_goto_owned_binder_1302; \
	./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	  echo "perceus-1302-tcrec-goto-drops-asan FAIL — sanitizer diagnostic:"; \
	  cat build/s2-$$name-asan.err; exit 1; \
	fi; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	  || { echo "perceus-1302-tcrec-goto-drops-asan DIFF"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	echo "perceus-1302-tcrec-goto-drops-asan OK"

# Native half of the #1302 gate (#1314): the KIR lowering threads the same
# goto-site release ledger, so the native goto-tail path must hold the
# leak flat too. Split out per the #1126/#1127 precedent (the tier1
# shard's kaic2 is C-only).
test-perceus-1302-tcrec-goto-drops-native: $(TARGET) build
	@set -e; \
	name=tcrec_goto_owned_binder_1302; \
	KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-1302tgdn 2> build/s2-1302tgdn.err \
	  || { echo "perceus-1302-tcrec-goto-drops-native FAIL (native build errored)"; cat build/s2-1302tgdn.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1302tgdn > build/s2-1302tgdn.out 2> build/s2-1302tgdn.rc \
	  || { echo "perceus-1302-tcrec-goto-drops-native FAIL (crash)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1302tgdn.out > /dev/null \
	  || { echo "perceus-1302-tcrec-goto-drops-native FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1302tgdn.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-1302tgdn.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1302-tcrec-goto-drops-native FAIL (no leaked line)"; cat build/s2-1302tgdn.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-1302-tcrec-goto-drops-native OK (native leaked=$$leaked < 100 over 10k goto-tail iters)" \
	  || { echo "perceus-1302-tcrec-goto-drops-native FAIL (native leaked=$$leaked — goto-tail releases regressed, grows with iters)"; exit 1; }

# Issue #1315 — a match on a BORROWED scrutinee must not under-release:
# the owner's drop of a local whose LAST use sits in a borrowed slot,
# and the unused rest-binder's non-bind. Two payload twins (String /
# [Int]) so a payload-specific fix cannot false-close the gate; each
# deconstructs a 1000-node list 1000 times borrowed and must keep the
# KAI_TRACE_RC `leaked` count flat (startup constant only).
test-perceus-1315-borrowed-match-release: $(TARGET) build
	@set -e; \
	for name in borrowed_match_str_payload_1315 borrowed_match_list_payload_1315; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name.c \
	    || { echo "perceus-1315-borrowed-match-release FAIL ($$name: kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc \
	    || { echo "perceus-1315-borrowed-match-release FAIL ($$name: crash)"; exit 1; }; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1315-borrowed-match-release FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	  leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name.rc | head -1 | cut -d= -f2); \
	  [ -n "$$leaked" ] \
	    || { echo "perceus-1315-borrowed-match-release FAIL ($$name: no leaked line)"; cat build/s2-$$name.rc; exit 1; }; \
	  [ "$$leaked" -lt 100 ] \
	    && echo "perceus-1315-borrowed-match-release OK ($$name leaked=$$leaked < 100 over 1000 borrowed reads)" \
	    || { echo "perceus-1315-borrowed-match-release FAIL ($$name leaked=$$leaked — borrowed-scrutinee release regressed)"; exit 1; }; \
	done

# ASAN companion for the #1315 gate: the owner-side drop after a
# borrowed call must never over-free (a wrongly dropped borrow-through
# param is a UAF). Both payload twins under -fsanitize=address,undefined.
test-perceus-1315-borrowed-match-release-asan: $(TARGET) build
	@set -e; \
	for name in borrowed_match_str_payload_1315 borrowed_match_list_payload_1315; do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	    echo "perceus-1315-borrowed-match-release-asan FAIL ($$name) — sanitizer diagnostic:"; \
	    cat build/s2-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	    || { echo "perceus-1315-borrowed-match-release-asan DIFF ($$name)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	  echo "perceus-1315-borrowed-match-release-asan OK ($$name)"; \
	done

# Native half of the #1315 gate (#1329): the KIR lowering must elide the
# `_`-prefixed rest binder exactly like the C oracle (no bind, no
# structural dup), so the borrowed deconstruction stays leak-flat on the
# native backend too. Split out per the #1302 precedent (the tier1
# shard's kaic2 is C-only).
test-perceus-1315-borrowed-match-release-native: $(TARGET) build
	@set -e; \
	for name in borrowed_match_str_payload_1315 borrowed_match_list_payload_1315; do \
	  KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc \
	    || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: crash)"; exit 1; }; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-n.out; exit 1; }; \
	  leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name-n.rc | head -1 | cut -d= -f2); \
	  [ -n "$$leaked" ] \
	    || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: no leaked line)"; cat build/s2-$$name-n.rc; exit 1; }; \
	  [ "$$leaked" -lt 100 ] \
	    && echo "perceus-1315-borrowed-match-release-native OK ($$name native leaked=$$leaked < 100 over 1000 borrowed reads)" \
	    || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name native leaked=$$leaked — rest-binder elision regressed)"; exit 1; }; \
	done; \
	name=reuse_unused_rest_binder_1329; \
	KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	  || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: native build errored — reuse arm lost its rest bind)"; cat build/s2-$$name-n.err; exit 1; }; \
	./build/s2-$$name-n > build/s2-$$name-n.out \
	  || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: crash)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-n.out > /dev/null \
	  || { echo "perceus-1315-borrowed-match-release-native FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-n.out; exit 1; }; \
	echo "perceus-1315-borrowed-match-release-native OK ($$name reuse arm keeps its rest bind)"

# Issue #1355 — a capturing lambda is not closure-specialised, so its
# `kai_closure` temp reaches the HOF's borrowed slot as an unbound owned
# rvalue; without a drop-after-call one closure cell leaks per executed
# call site (1000 waves leaked 1005 cells pre-fix). The fixture also
# carries the non-capturing twin, which specialises away and must stay
# leak-free — an over-eager drop there is a double-free the ASAN
# companion catches.
test-perceus-1355-closure-temp-release: $(TARGET) build
	@set -e; \
	name=closure_temp_borrowed_hof_1355; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name.c \
	  || { echo "perceus-1355-closure-temp-release FAIL ($$name: kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc \
	  || { echo "perceus-1355-closure-temp-release FAIL ($$name: crash)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	  || { echo "perceus-1355-closure-temp-release FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1355-closure-temp-release FAIL ($$name: no leaked line)"; cat build/s2-$$name.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-1355-closure-temp-release OK ($$name leaked=$$leaked < 100 over 1000 closure temps)" \
	  || { echo "perceus-1355-closure-temp-release FAIL ($$name leaked=$$leaked — closure temp in borrowed HOF slot leaks again)"; exit 1; }

# ASAN companion for the #1355 gate: the drop this fix adds must fire only
# on the un-consumed closure temp — a drop against a slot the callee
# consumes is a double-free.
test-perceus-1355-closure-temp-release-asan: $(TARGET) build
	@set -e; \
	name=closure_temp_borrowed_hof_1355; \
	./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	  echo "perceus-1355-closure-temp-release-asan FAIL ($$name) — sanitizer diagnostic:"; \
	  cat build/s2-$$name-asan.err; exit 1; \
	fi; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	  && echo "perceus-1355-closure-temp-release-asan OK ($$name)" \
	  || { echo "perceus-1355-closure-temp-release-asan DIFF ($$name)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }

# Native half of the #1355 gate: the mark's post-call KDrop must release
# the closure temp on the KIR lowering too, keeping both backends leak-flat.
test-perceus-1355-closure-temp-release-native: $(TARGET) build
	@set -e; \
	name=closure_temp_borrowed_hof_1355; \
	KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	  || { echo "perceus-1355-closure-temp-release-native FAIL ($$name: native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc \
	  || { echo "perceus-1355-closure-temp-release-native FAIL ($$name: crash)"; exit 1; }; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-n.out > /dev/null \
	  || { echo "perceus-1355-closure-temp-release-native FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-n.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name-n.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "perceus-1355-closure-temp-release-native FAIL ($$name: no leaked line)"; cat build/s2-$$name-n.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "perceus-1355-closure-temp-release-native OK ($$name native leaked=$$leaked < 100 over 1000 closure temps)" \
	  || { echo "perceus-1355-closure-temp-release-native FAIL ($$name native leaked=$$leaked — closure temp release regressed on native)"; exit 1; }
# Issue #1328 — a multi-use local read only through borrowed slots must
# release its birth ref at end of scope, including when the block tail is
# the self-tail-call (drop planted per path, before the goto, TCO intact).
# selftail/tail_read are the leak gates (leaked flat over the iteration
# count); consuming_multiuse is the exactly-once gate (borrow + consume
# mixed, plus an escaping return) whose real teeth are the ASAN twin.
test-perceus-1328-selftail-borrowed-local: $(TARGET) build
	@set -e; \
	for name in selftail_borrowed_local_1328 tail_read_borrowed_local_1328 selftail_consuming_multiuse_1328 selftail_arm_binder_move_1328; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name.c \
	    || { echo "perceus-1328-selftail-borrowed-local FAIL ($$name: kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	  KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc \
	    || { echo "perceus-1328-selftail-borrowed-local FAIL ($$name: crash)"; exit 1; }; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	    || { echo "perceus-1328-selftail-borrowed-local FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	  leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name.rc | head -1 | cut -d= -f2); \
	  [ -n "$$leaked" ] \
	    || { echo "perceus-1328-selftail-borrowed-local FAIL ($$name: no leaked line)"; cat build/s2-$$name.rc; exit 1; }; \
	  [ "$$leaked" -lt 100 ] \
	    && echo "perceus-1328-selftail-borrowed-local OK ($$name leaked=$$leaked < 100)" \
	    || { echo "perceus-1328-selftail-borrowed-local FAIL ($$name leaked=$$leaked — borrowed multi-use local release regressed)"; exit 1; }; \
	done

# ASAN companion for the #1328 gate: the end-of-scope drop must never
# over-release — a consumed dup plus the exit drop is exactly two refs,
# and an escaping value keeps the ref its tail read pays for.
test-perceus-1328-selftail-borrowed-local-asan: $(TARGET) build
	@set -e; \
	for name in selftail_borrowed_local_1328 tail_read_borrowed_local_1328 selftail_consuming_multiuse_1328 selftail_arm_binder_move_1328; do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	    echo "perceus-1328-selftail-borrowed-local-asan FAIL ($$name) — sanitizer diagnostic:"; \
	    cat build/s2-$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	    || { echo "perceus-1328-selftail-borrowed-local-asan DIFF ($$name)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out; exit 1; }; \
	  echo "perceus-1328-selftail-borrowed-local-asan OK ($$name)"; \
	done

# Native half of the #1328 gate: the planted per-path drop is a plain
# `__perceus_drop` in the AST, so the KIR lowering must release the same
# refs and keep the same output — leak flat on the native backend too.
test-perceus-1328-selftail-borrowed-local-native: $(TARGET) build
	@set -e; \
	for name in selftail_borrowed_local_1328 tail_read_borrowed_local_1328 selftail_consuming_multiuse_1328 selftail_arm_binder_move_1328; do \
	  KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-$$name-n 2> build/s2-$$name-n.err \
	    || { echo "perceus-1328-selftail-borrowed-local-native FAIL ($$name: native build errored)"; cat build/s2-$$name-n.err; exit 1; }; \
	  KAI_TRACE_RC=1 ./build/s2-$$name-n > build/s2-$$name-n.out 2> build/s2-$$name-n.rc \
	    || { echo "perceus-1328-selftail-borrowed-local-native FAIL ($$name: crash)"; exit 1; }; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-n.out > /dev/null \
	    || { echo "perceus-1328-selftail-borrowed-local-native FAIL ($$name: output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-n.out; exit 1; }; \
	  leaked=$$(grep -oE 'leaked=[0-9]+' build/s2-$$name-n.rc | head -1 | cut -d= -f2); \
	  [ -n "$$leaked" ] \
	    || { echo "perceus-1328-selftail-borrowed-local-native FAIL ($$name: no leaked line)"; cat build/s2-$$name-n.rc; exit 1; }; \
	  [ "$$leaked" -lt 100 ] \
	    && echo "perceus-1328-selftail-borrowed-local-native OK ($$name native leaked=$$leaked < 100)" \
	    || { echo "perceus-1328-selftail-borrowed-local-native FAIL ($$name native leaked=$$leaked — borrowed multi-use local release regressed)"; exit 1; }; \
	done

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
	lo=0; KAI_MAX_HEAP=64m $(TIMEOUT) 30 ./build/s2-878hl > build/s2-878hl.lo.out 2> build/s2-878hl.lo.err || lo=$$?; \
	[ "$$lo" -eq 1 ] \
	  || { echo "heap-limit FAIL (low cap exit $$lo, want 1)"; cat build/s2-878hl.lo.err; exit 1; }; \
	grep -q 'heap limit exceeded' build/s2-878hl.lo.err \
	  || { echo "heap-limit FAIL (low cap: no 'heap limit exceeded' message)"; cat build/s2-878hl.lo.err; exit 1; }; \
	[ -s build/s2-878hl.lo.out ] \
	  && { echo "heap-limit FAIL (low cap printed result before aborting)"; cat build/s2-878hl.lo.out; exit 1; }; \
	hi=0; KAI_MAX_HEAP=2g $(TIMEOUT) 60 ./build/s2-878hl > build/s2-878hl.hi.out 2> build/s2-878hl.hi.err || hi=$$?; \
	{ [ "$$hi" -eq 0 ] && grep -q 'len=268435456' build/s2-878hl.hi.out; } \
	  || { echo "heap-limit FAIL (high cap exit $$hi, output:)"; cat build/s2-878hl.hi.out build/s2-878hl.hi.err; exit 1; }; \
	un=0; $(TIMEOUT) 60 ./build/s2-878hl > build/s2-878hl.un.out 2> build/s2-878hl.un.err || un=$$?; \
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

# Issue #747 / #1054 — a variant REAL slot bound in a match arm and consumed by
# the arm body is a native use-after-free: the native reads the Real slot as a
# boxed borrow (`KProj`/`kaix_variant_arg`, mask 0 -> `.ptr`, no incref), while
# `pat_alias_binders` excluded Real slots, so no structural dup protected the
# borrow. A consuming builtin (`real_to_string` decref's its arg) freed the
# shared box, then the arm-exit scrutinee drop re-decref'd the freed slot
# (SIGSEGV 139 in the self-compile). The fix descends Real slots in
# `pat_alias_binders`. The gate builds NATIVE under ASAN + no-cell-pool so the
# double free trips the redzone, then checks output parity; without the dup the
# binary aborts on the UAF. Native-gated (tier1-native).
test-perceus-747-native-real-slot-uaf: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_variant_real_slot_uaf_747.kai; \
	exp=../examples/perceus/native_variant_real_slot_uaf_747.out.expected; \
	mv ../stage0/runtime_llvm.bc ../stage0/runtime_llvm.bc.hidden747 2>/dev/null || true; \
	CFLAGS="-std=c99 -O1 -g -fsanitize=address -fno-omit-frame-pointer -DKAI_NO_CELL_POOL -Wno-unused-function -Wno-unused-variable" \
	  KAI_BACKEND=native ../bin/kai build $$src -o build/s2-747rsu 2> build/s2-747rsu.err; \
	bs=$$?; \
	mv ../stage0/runtime_llvm.bc.hidden747 ../stage0/runtime_llvm.bc 2>/dev/null || true; \
	[ "$$bs" -eq 0 ] \
	  || { echo "perceus-747-native-real-slot-uaf FAIL (native ASAN build errored)"; cat build/s2-747rsu.err; exit 1; }; \
	ASAN_OPTIONS=abort_on_error=0:halt_on_error=1:detect_leaks=0 \
	  ./build/s2-747rsu > build/s2-747rsu.out 2> build/s2-747rsu.asan; \
	rc=$$?; \
	if grep -qE 'AddressSanitizer|heap-use-after-free|runtime error:' build/s2-747rsu.asan; then \
	  echo "perceus-747-native-real-slot-uaf FAIL (ASAN diagnostic — Real slot borrow freed live):"; \
	  grep -m3 -E 'AddressSanitizer|heap-use-after-free|runtime error:' build/s2-747rsu.asan; exit 1; \
	fi; \
	[ "$$rc" -eq 0 ] \
	  || { echo "perceus-747-native-real-slot-uaf FAIL (native ASAN run crashed, exit $$rc)"; tail -20 build/s2-747rsu.asan; exit 1; }; \
	diff -q $$exp build/s2-747rsu.out > /dev/null \
	  && echo "perceus-747-native-real-slot-uaf OK (native ASAN clean + output parity; Real slot dup protects the borrow)" \
	  || { echo "perceus-747-native-real-slot-uaf FAIL (output diff)"; diff $$exp build/s2-747rsu.out; exit 1; }
# Issue #1048 — the NATIVE backend must decref->free a heap RECORD when it
# leaves scope. Before the fix native left `free_total` near zero (every record
# leaked, `leaked` grew linearly with the loop count) while the C oracle
# reclaimed them; the fix is in the KIR lowering (dead-let block-exit drop +
# EField base drop, both mirrors of the C oracle). The fixture builds + drops
# 4000 records (bare, field-read, and list-of-records shapes) under the NATIVE
# backend with KAI_TRACE_RC; the leak must be a SMALL CONSTANT, not O(loops).
# We assert free_total >= alloc_total - 10 (it is alloc-3); a regression that
# drops the record decref makes free_total ~2 while alloc_total ~6000.
# Native-gated (tier1-native): a C-only kaic2 cannot run --backend=native.
test-perceus-1048-native-record-leak: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_record_scope_leak_1048.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1048nrl 2> build/s2-1048nrl.err \
	  || { echo "perceus-1048-native-record-leak FAIL (native build errored)"; cat build/s2-1048nrl.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1048nrl > build/s2-1048nrl.out 2> build/s2-1048nrl.rc; \
	diff -q ../examples/perceus/native_record_scope_leak_1048.out.expected build/s2-1048nrl.out > /dev/null \
	  || { echo "perceus-1048-native-record-leak FAIL (output diff)"; diff ../examples/perceus/native_record_scope_leak_1048.out.expected build/s2-1048nrl.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1048nrl.rc | head -1 | cut -d= -f2); \
	free=$$(grep -oE 'free_total=[0-9]+' build/s2-1048nrl.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] && [ -n "$$free" ] \
	  || { echo "perceus-1048-native-record-leak FAIL (no alloc/free line)"; cat build/s2-1048nrl.rc; exit 1; }; \
	[ "$$free" -ge "$$(($$alloc - 10))" ] \
	  && echo "perceus-1048-native-record-leak OK (native free_total=$$free >= alloc_total-10=$$(($$alloc - 10)); record decref->free over 4000 cells)" \
	  || { echo "perceus-1048-native-record-leak FAIL (native free_total=$$free, alloc_total=$$alloc — record leak regressed, decref dropped)"; exit 1; }

# Issue #1112 — the NATIVE backend must decref->free an OWNED boxed value
# DISCARDED in statement position (a bare ctor result, or the owned array ref
# `array_set` returns). Before the fix the native `KDo` arm dropped the LLVM
# Handle without a runtime decref, so the value leaked; the discarded
# `array_set` ref pinned the array, and its element slots were never walked
# (the issue's 7 GB RSS `Array[Record]` leak). The C oracle's `SExprStmt`
# lowers `{ KaiValue *_ = <e>; kai_decref(_); }` and reclaims them. The fixture
# discards 1000 ctors + builds/drops 100 arrays-of-records under KAI_TRACE_RC;
# `free_total` must track `alloc_total` (near-total reclaim). A regression that
# drops the discard decref leaves `free_total` near zero and `leaked` linear.
# Native-gated (tier1-native): a C-only kaic2 cannot run --backend=native.
test-perceus-1112-native-discard-decref: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_discard_decref_1112.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1112ndd 2> build/s2-1112ndd.err \
	  || { echo "perceus-1112-native-discard-decref FAIL (native build errored)"; cat build/s2-1112ndd.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1112ndd > build/s2-1112ndd.out 2> build/s2-1112ndd.rc; \
	diff -q ../examples/perceus/native_discard_decref_1112.out.expected build/s2-1112ndd.out > /dev/null \
	  || { echo "perceus-1112-native-discard-decref FAIL (output diff)"; diff ../examples/perceus/native_discard_decref_1112.out.expected build/s2-1112ndd.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1112ndd.rc | head -1 | cut -d= -f2); \
	free=$$(grep -oE 'free_total=[0-9]+' build/s2-1112ndd.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] && [ -n "$$free" ] \
	  || { echo "perceus-1112-native-discard-decref FAIL (no alloc/free line)"; cat build/s2-1112ndd.rc; exit 1; }; \
	[ "$$free" -ge "$$(($$alloc - 10))" ] \
	  && echo "perceus-1112-native-discard-decref OK (native free_total=$$free >= alloc_total-10=$$(($$alloc - 10)); discarded owned values decref->free)" \
	  || { echo "perceus-1112-native-discard-decref FAIL (native free_total=$$free, alloc_total=$$alloc — discard decref dropped, leak regressed)"; exit 1; }

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

# The NATIVE TRMC variant step must rebuild INTO the arm-top reuse token
# (the unique scrutinee's shell), not free it and fresh-alloc. The fixture
# rebuilds a 1000-node variant spine 1000 times; with the reuse the alloc
# total is O(N) (~1k), without it every level allocates (~1M+). Assert
# alloc_total < 100000 (100x headroom over the reused figure, 10x below the
# regressed one) plus golden output. Native-gated (tier1-native), like #872.
test-perceus-1053-native-trmc-spine-reuse: $(TARGET) build
	@set -e; \
	src=../examples/perceus/trmc_variant_spine_reuse_1053.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1053sr 2> build/s2-1053sr.err \
	  || { echo "perceus-1053-native-trmc-spine-reuse FAIL (native build errored)"; cat build/s2-1053sr.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1053sr > build/s2-1053sr.out 2> build/s2-1053sr.rc; \
	diff -q ../examples/perceus/trmc_variant_spine_reuse_1053.out.expected build/s2-1053sr.out > /dev/null \
	  || { echo "perceus-1053-native-trmc-spine-reuse FAIL (output diff)"; diff ../examples/perceus/trmc_variant_spine_reuse_1053.out.expected build/s2-1053sr.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1053sr.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] \
	  || { echo "perceus-1053-native-trmc-spine-reuse FAIL (no alloc_total line)"; cat build/s2-1053sr.rc; exit 1; }; \
	[ "$$alloc" -lt 100000 ] \
	  && echo "perceus-1053-native-trmc-spine-reuse OK (native alloc_total=$$alloc < 100000 over 1M spine rebuilds; TRMC step reuses the token shell)" \
	  || { echo "perceus-1053-native-trmc-spine-reuse FAIL (native alloc_total=$$alloc — the TRMC variant step fresh-allocs again, token reuse regressed)"; exit 1; }

# Nested (non-bijective) rotation reuse gate. Sequential inserts force a
# rotation on nearly every insert; the dual-branch lowering donates BOTH
# unique shells (outer overwrite + inner token), so alloc_total stays near
# one alloc per insert — a fresh-allocating rotation path triples it.
# Golden output + alloc ceiling, native backend.
test-perceus-1053-nested-rotation-reuse: $(TARGET) build
	@set -e; \
	src=../examples/perceus/nested_rotation_reuse_1053.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1053nr 2> build/s2-1053nr.err \
	  || { echo "perceus-1053-nested-rotation-reuse FAIL (native build errored)"; cat build/s2-1053nr.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1053nr > build/s2-1053nr.out 2> build/s2-1053nr.rc; \
	diff -q ../examples/perceus/nested_rotation_reuse_1053.out.expected build/s2-1053nr.out > /dev/null \
	  || { echo "perceus-1053-nested-rotation-reuse FAIL (output diff)"; diff ../examples/perceus/nested_rotation_reuse_1053.out.expected build/s2-1053nr.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1053nr.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] \
	  || { echo "perceus-1053-nested-rotation-reuse FAIL (no alloc_total line)"; cat build/s2-1053nr.rc; exit 1; }; \
	[ "$$alloc" -lt 1000000 ] \
	  && echo "perceus-1053-nested-rotation-reuse OK (native alloc_total=$$alloc < 1000000 over 100k sequential inserts; balance rebuilds donate the outer + inner shells)" \
	  || { echo "perceus-1053-nested-rotation-reuse FAIL (native alloc_total=$$alloc — balance rebuilds fresh-alloc again, nested reuse regressed)"; exit 1; }

# Issue #1104 Lane 1 — interprocedural reuse-token donation across the
# balance-call boundary. The `insert_loop` Black arm captures the parent as
# an arm-top token, then tails into `balance_left/right` out-of-line. Without
# donation the token cannot cross the call — the arm epilogue frees it (~4.24
# per insert) and the rotation fresh-allocs its third cell (~5.24 alloc per
# insert). With the hidden `_donor` param the caller MOVES the token in and
# the rotation consumes it: ~1 alloc per insert (Koka parity), zero freed
# tokens. Gate: alloc_total < 150000 over 100k sequential inserts (1.5/insert
# ceiling; baseline ≈524K) AND no reuse_freed line (every token consumed).
# C backend — the donation lives in emit_c.kai (the shared front-end lever);
# the native codegen path is a separate lane (Lane N) and still fresh-allocs.
test-perceus-1104-balance-token-donation: $(TARGET) build
	@set -e; \
	src=../examples/perceus/balance_token_donation_1104.kai; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1104btd.c \
	  || { echo "perceus-1104-balance-token-donation FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1104btd.c -o build/s2-1104btd $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1104btd > build/s2-1104btd.out 2> build/s2-1104btd.rc; \
	diff -q ../examples/perceus/balance_token_donation_1104.out.expected build/s2-1104btd.out > /dev/null \
	  || { echo "perceus-1104-balance-token-donation FAIL (output diff)"; diff ../examples/perceus/balance_token_donation_1104.out.expected build/s2-1104btd.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1104btd.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] \
	  || { echo "perceus-1104-balance-token-donation FAIL (no alloc_total line)"; cat build/s2-1104btd.rc; exit 1; }; \
	freed=$$(grep -oE 'reuse_freed=[0-9]+' build/s2-1104btd.rc | head -1 | cut -d= -f2); \
	[ "$$alloc" -lt 150000 ] \
	  || { echo "perceus-1104-balance-token-donation FAIL (alloc_total=$$alloc >= 150000 — token not donated across balance, rotation fresh-allocs again)"; exit 1; }; \
	[ -z "$$freed" ] \
	  && echo "perceus-1104-balance-token-donation OK (alloc_total=$$alloc < 150000, no reuse_freed — arm token donated across balance)" \
	  || { echo "perceus-1104-balance-token-donation FAIL (reuse_freed=$$freed — arm token freed instead of donated)"; exit 1; }

# Issue #1164 — the SAME cross-call donation shape on the NATIVE backend.
# The C target above proves emit_c donates the arm token across the
# balance call; this one proves the KIR/native path does too (the KFn
# donor ABI). Same fixture, same gates: alloc_total < 150000 over 100k
# sequential inserts (native baseline without donation ≈ 815K) AND no
# reuse_freed line (every captured token is consumed, in-arm or donated).
test-perceus-1164-cross-call-donation: $(TARGET) build
	@set -e; \
	src=../examples/perceus/balance_token_donation_1104.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1164ccd 2> build/s2-1164ccd.err \
	  || { echo "perceus-1164-cross-call-donation FAIL (native build errored)"; cat build/s2-1164ccd.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1164ccd > build/s2-1164ccd.out 2> build/s2-1164ccd.rc; \
	diff -q ../examples/perceus/balance_token_donation_1104.out.expected build/s2-1164ccd.out > /dev/null \
	  || { echo "perceus-1164-cross-call-donation FAIL (output diff)"; diff ../examples/perceus/balance_token_donation_1104.out.expected build/s2-1164ccd.out; exit 1; }; \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1164ccd.rc | head -1 | cut -d= -f2); \
	[ -n "$$alloc" ] \
	  || { echo "perceus-1164-cross-call-donation FAIL (no alloc_total line)"; cat build/s2-1164ccd.rc; exit 1; }; \
	freed=$$(grep -oE 'reuse_freed=[0-9]+' build/s2-1164ccd.rc | head -1 | cut -d= -f2); \
	[ "$$alloc" -lt 150000 ] \
	  || { echo "perceus-1164-cross-call-donation FAIL (native alloc_total=$$alloc >= 150000 — token not donated across balance, rotation fresh-allocs)"; exit 1; }; \
	[ -z "$$freed" ] \
	  && echo "perceus-1164-cross-call-donation OK (native alloc_total=$$alloc < 150000, no reuse_freed — arm token donated across balance)" \
	  || { echo "perceus-1164-cross-call-donation FAIL (native reuse_freed=$$freed — arm token freed instead of donated)"; exit 1; }

# Issue #1104 Lane 2 — owned sibling-param move + reuse-in-place RC elision
# (Koka Parc `ownedInScope` + incref/exit-decref fusion). `balance_left`'s
# owned `r` (and `balance_right`'s `l`) was dup'd in every RBNode arm plus
# dropped at fn exit; the reuse-in-place unique path incref'd `_scr` to
# survive a blanket exit decref. This lane moves the sibling param at its
# single use (dropping it only in the RBLeaf arm) and lets the rewritten
# cell move out with `_scr = NULL`. Net: incref_total + decref_total per
# insert 22.4 -> 4.0 (baseline ~2.24M over 100k). Gate: sum < 600000
# (6/insert ceiling; a regression toward the dup+exit-drop shape blows past
# it) AND output byte-identical AND alloc_total at Lane 1 parity (< 150000).
test-perceus-1104-sibling-param-move: $(TARGET) build
	@set -e; \
	src=../examples/perceus/balance_sibling_param_move_1104.kai; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1104spm.c \
	  || { echo "perceus-1104-sibling-param-move FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1104spm.c -o build/s2-1104spm $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-1104spm > build/s2-1104spm.out 2> build/s2-1104spm.rc; \
	diff -q ../examples/perceus/balance_sibling_param_move_1104.out.expected build/s2-1104spm.out > /dev/null \
	  || { echo "perceus-1104-sibling-param-move FAIL (output diff)"; diff ../examples/perceus/balance_sibling_param_move_1104.out.expected build/s2-1104spm.out; exit 1; }; \
	inc=$$(grep -oE 'incref_total=[0-9]+' build/s2-1104spm.rc | head -1 | cut -d= -f2); \
	dec=$$(grep -oE 'decref_total=[0-9]+' build/s2-1104spm.rc | head -1 | cut -d= -f2); \
	alloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1104spm.rc | head -1 | cut -d= -f2); \
	[ -n "$$inc" ] && [ -n "$$dec" ] && [ -n "$$alloc" ] \
	  || { echo "perceus-1104-sibling-param-move FAIL (no incref/decref/alloc line)"; cat build/s2-1104spm.rc; exit 1; }; \
	sum=$$((inc + dec)); \
	[ "$$alloc" -lt 150000 ] \
	  || { echo "perceus-1104-sibling-param-move FAIL (alloc_total=$$alloc >= 150000 — Lane 1 reuse regressed)"; exit 1; }; \
	[ "$$sum" -lt 600000 ] \
	  && echo "perceus-1104-sibling-param-move OK (incref+decref=$$sum < 600000 — sibling-param move + reuse RC elision landed; alloc_total=$$alloc)" \
	  || { echo "perceus-1104-sibling-param-move FAIL (incref+decref=$$sum >= 600000 — dup+exit-drop or reuse incref regressed)"; exit 1; }

# Issue #1136 — sized-exact variant representation gates. Three fixtures:
# static-kind slot tests (enum / literal / nested-ctor discrimination),
# the raw ctor-slot rebuild (write-side raw permit, 200k-deep TRMC spine),
# and the polymorphic frontier (sized variants through generic map / eq /
# interpolation). Golden outputs on the C oracle; the -native twin builds
# the same three with the native backend (tier1-native).
test-perceus-1136-sized-variant: $(TARGET) build
	@set -e; \
	for name in variant_static_match_1136 variant_raw_rebuild_1136 variant_poly_frontier_1136; do \
	  src=../examples/perceus/$$name.kai; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1136-$$name.c \
	    || { echo "perceus-1136-sized-variant FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1136-$$name.c -o build/s2-1136-$$name $(LDLIBS_M); \
	  ./build/s2-1136-$$name > build/s2-1136-$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-1136-$$name.out > /dev/null \
	    || { echo "perceus-1136-sized-variant FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1136-$$name.out; exit 1; }; \
	done; \
	echo "perceus-1136-sized-variant OK (static-kind tests + raw rebuild + poly frontier, C oracle)"

test-perceus-1136-sized-variant-native: $(TARGET) build
	@set -e; \
	for name in variant_static_match_1136 variant_raw_rebuild_1136 variant_poly_frontier_1136; do \
	  src=../examples/perceus/$$name.kai; \
	  KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1136-$$name-nat 2> build/s2-1136-$$name-nat.err \
	    || { echo "perceus-1136-sized-variant-native FAIL ($$name native build errored)"; cat build/s2-1136-$$name-nat.err; exit 1; }; \
	  ./build/s2-1136-$$name-nat > build/s2-1136-$$name-nat.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-1136-$$name-nat.out > /dev/null \
	    || { echo "perceus-1136-sized-variant-native FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1136-$$name-nat.out; exit 1; }; \
	done; \
	echo "perceus-1136-sized-variant-native OK (static-kind tests + raw rebuild + poly frontier, native)"

# Issue #1180 — lazy ranges. Correctness gate: a range VALUE through
# every consumer shape (match fallback incl. multi-element patterns,
# reverse/append/length/map/filter/eq/show, steps, descending, empty)
# prints exactly what the eager cons build printed, and a zero step
# traps. Perf gate (range-sum): `[1..1M] |> sum()/reduce/length` at
# alloc_total < 100 — one materialised range blows it by 10^4.
test-perceus-1180-range-lazy: $(TARGET) build
	@set -e; \
	name=range_lazy_1180; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name.c \
	  || { echo "perceus-1180-range-lazy FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	  || { echo "perceus-1180-range-lazy FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/range_step_zero_1180.kai > build/s2-1180-step0.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1180-step0.c -o build/s2-1180-step0 $(LDLIBS_M); \
	if ./build/s2-1180-step0 > /dev/null 2> build/s2-1180-step0.err; then \
	  echo "perceus-1180-range-lazy FAIL (zero step did not trap)"; exit 1; \
	fi; \
	grep -q "zero step in range" build/s2-1180-step0.err \
	  || { echo "perceus-1180-range-lazy FAIL (zero-step trap message missing)"; cat build/s2-1180-step0.err; exit 1; }; \
	echo "perceus-1180-range-lazy OK (range value correct through every consumer; zero step traps)"

test-perceus-1180-range-lazy-native: $(TARGET) build
	@set -e; \
	name=range_lazy_1180; \
	KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-$$name-nat 2> build/s2-$$name-nat.err \
	  || { echo "perceus-1180-range-lazy-native FAIL (native build errored)"; cat build/s2-$$name-nat.err; exit 1; }; \
	./build/s2-$$name-nat > build/s2-$$name-nat.out; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-nat.out > /dev/null \
	  && echo "perceus-1180-range-lazy-native OK (range value correct through every consumer, native)" \
	  || { echo "perceus-1180-range-lazy-native FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-nat.out; exit 1; }

test-perceus-1180-range-lazy-asan: $(TARGET) build
	@set -e; \
	name=range_lazy_1180; \
	./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/s2-$$name-asan.c -o build/s2-$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/s2-$$name-asan > build/s2-$$name-asan.out 2> build/s2-$$name-asan.err; \
	if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/s2-$$name-asan.err; then \
	  echo "perceus-1180-range-lazy-asan FAIL — sanitizer diagnostic:"; \
	  cat build/s2-$$name-asan.err; exit 1; \
	fi; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-asan.out > /dev/null \
	  && echo "perceus-1180-range-lazy-asan OK (norm/fallback paths sanitizer-clean)" \
	  || { echo "perceus-1180-range-lazy-asan FAIL (output diff)"; exit 1; }

test-perceus-1180-range-sum: $(TARGET) build
	@set -e; \
	name=range_lazy_sum_1180; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name.c \
	  || { echo "perceus-1180-range-sum FAIL (kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	  || { echo "perceus-1180-range-sum FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-$$name.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] \
	  || { echo "perceus-1180-range-sum FAIL (no alloc_total line)"; cat build/s2-$$name.rc; exit 1; }; \
	[ "$$tot" -lt 100 ] \
	  && echo "perceus-1180-range-sum OK (alloc_total=$$tot < 100; sum fused, reduce lazy, nothing materialised)" \
	  || { echo "perceus-1180-range-sum FAIL (alloc_total=$$tot — range materialised)"; exit 1; }

test-perceus-1180-range-sum-native: $(TARGET) build
	@set -e; \
	name=range_lazy_sum_1180; \
	KAI_BACKEND=native ../bin/kai build ../examples/perceus/$$name.kai -o build/s2-$$name-nat 2> build/s2-$$name-nat.err \
	  || { echo "perceus-1180-range-sum-native FAIL (native build errored)"; cat build/s2-$$name-nat.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-$$name-nat > build/s2-$$name-nat.out 2> build/s2-$$name-nat.rc; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name-nat.out > /dev/null \
	  || { echo "perceus-1180-range-sum-native FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name-nat.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-$$name-nat.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] && [ "$$tot" -lt 100 ] \
	  && echo "perceus-1180-range-sum-native OK (alloc_total=$$tot < 100 on native)" \
	  || { echo "perceus-1180-range-sum-native FAIL (alloc_total=$$tot)"; exit 1; }

# Issue #1200 — generalised pipe fusion for a `Shape` protocol container.
# A user arity-1 `Chain` implementing `Sequence` fuses its `map`-into-`foldl`
# pipeline: the emitted C must call the impl's `foldl` but NEVER its `map`
# (the mapped chain is never materialised), and the alloc count must match
# the source-build cost only.
test-shape-fusion-1200: $(TARGET) build
	@set -e; \
	name=shape_fusion_1200; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/s2-$$name.c \
	  || { echo "shape-fusion-1200 FAIL (kaic2 errored)"; exit 1; }; \
	grep -q 'kai___pimpl_Sequence_Chain_foldl' build/s2-$$name.c \
	  || { echo "shape-fusion-1200 FAIL (impl foldl not called — pipe did not dispatch)"; exit 1; }; \
	if grep 'kai_build(100LL' build/s2-$$name.c | grep -q 'kai___pimpl_Sequence_Chain_map('; then \
	  echo "shape-fusion-1200 FAIL (mapped chain materialised — not fused)"; exit 1; fi; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$name.c -o build/s2-$$name $(LDLIBS_M); \
	KAI_TRACE_RC=1 ./build/s2-$$name > build/s2-$$name.out 2> build/s2-$$name.rc; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-$$name.out > /dev/null \
	  || { echo "shape-fusion-1200 FAIL (output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-$$name.out; exit 1; }; \
	tot=$$(grep -oE 'alloc_total=[0-9]+' build/s2-$$name.rc | head -1 | cut -d= -f2); \
	[ -n "$$tot" ] && [ "$$tot" -lt 130 ] \
	  && echo "shape-fusion-1200 OK (foldl fused, map never called; alloc_total=$$tot ~= source-build only)" \
	  || { echo "shape-fusion-1200 FAIL (alloc_total=$$tot — intermediate materialised)"; exit 1; }

# Issue #1200 — autogenerated Functorial law-checks. A lawful impl
# passes every synthesised identity/fusion check; a non-functorial impl
# is caught by a counterexample; the same impl marked axiom generates
# zero checks (the escape hatch). Runs the --prop-check harness the
# kai check runner installs.
test-shape-laws-1200: $(TARGET) build
	@set -e; \
	run() { \
	  src=../examples/sugars/$$1.kai; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --prop-check --path ../stdlib $$src > build/s2-$$1.c \
	    || { echo "shape-laws-1200 FAIL ($$1 kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-$$1.c -o build/s2-$$1 $(LDLIBS_M); \
	  ./build/s2-$$1 > build/s2-$$1.out 2>&1 || true; \
	}; \
	run shape_kind_laws; \
	grep -q '6/6 checks passed' build/s2-shape_kind_laws.out \
	  || { echo "shape-laws-1200 FAIL (lawful impl: expected 6/6 passed)"; cat build/s2-shape_kind_laws.out; exit 1; }; \
	run shape_kind_laws_violation; \
	grep -q 'counterexample' build/s2-shape_kind_laws_violation.out \
	  || { echo "shape-laws-1200 FAIL (violation: expected a counterexample)"; cat build/s2-shape_kind_laws_violation.out; exit 1; }; \
	run shape_kind_laws_axiom; \
	grep -q '0/0 checks passed' build/s2-shape_kind_laws_axiom.out \
	  || { echo "shape-laws-1200 FAIL (axiom: expected 0/0 escape hatch)"; cat build/s2-shape_kind_laws_axiom.out; exit 1; }; \
	echo "shape-laws-1200 OK (lawful 6/6, violation caught, axiom exempts)"

# Issue #1156 — widened-register escape gate. Two sibling arms bind the
# same name on kind-1 Int slots with opposite rawness verdicts (raw
# compare+store vs boxed call use); the shared register widens to SBoxed
# and the raw arm's KLet store must box at the border or the boxed reads
# deref a raw word (the native self-compile SIGSEGV shape).
test-perceus-1156-raw-widen: $(TARGET) build
	@set -e; \
	name=variant_raw_widen_1156; \
	src=../examples/perceus/$$name.kai; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$src > build/s2-1156-$$name.c \
	  || { echo "perceus-1156-raw-widen FAIL ($$name kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1156-$$name.c -o build/s2-1156-$$name $(LDLIBS_M); \
	./build/s2-1156-$$name > build/s2-1156-$$name.out; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1156-$$name.out > /dev/null \
	  || { echo "perceus-1156-raw-widen FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1156-$$name.out; exit 1; }; \
	echo "perceus-1156-raw-widen OK (widened-register raw/boxed arm collision, C oracle)"

test-perceus-1156-raw-widen-native: $(TARGET) build
	@set -e; \
	name=variant_raw_widen_1156; \
	src=../examples/perceus/$$name.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1156-$$name-nat 2> build/s2-1156-$$name-nat.err \
	  || { echo "perceus-1156-raw-widen-native FAIL ($$name native build errored)"; cat build/s2-1156-$$name-nat.err; exit 1; }; \
	./build/s2-1156-$$name-nat > build/s2-1156-$$name-nat.out; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1156-$$name-nat.out > /dev/null \
	  || { echo "perceus-1156-raw-widen-native FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1156-$$name-nat.out; exit 1; }; \
	echo "perceus-1156-raw-widen-native OK (widened-register raw/boxed arm collision, native)"

# Issue #1157 — full variant_slot_mask parity gate. One ctor carries all
# four slot kinds (pointer / enum / Real / Int); the fixture exercises
# construction, the static enum/Real/Int slot tests, binder reads, the
# unique and shared reuse rebuilds and a 200k-deep TRMC spine, and its
# folds read every slot back — a word laid at the wrong kind is a wrong
# sum or a crash, on either backend.
test-perceus-1157-full-mask: $(TARGET) build
	@set -e; \
	name=variant_full_mask_1157; \
	src=../examples/perceus/$$name.kai; \
	./$(TARGET) --path ../stdlib $$src > build/s2-1157-$$name.c \
	  || { echo "perceus-1157-full-mask FAIL ($$name kaic2 errored)"; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1157-$$name.c -o build/s2-1157-$$name $(LDLIBS_M); \
	./build/s2-1157-$$name > build/s2-1157-$$name.out; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1157-$$name.out > /dev/null \
	  || { echo "perceus-1157-full-mask FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1157-$$name.out; exit 1; }; \
	echo "perceus-1157-full-mask OK (full slot-mask layout, C oracle)"

test-perceus-1157-full-mask-native: $(TARGET) build
	@set -e; \
	name=variant_full_mask_1157; \
	src=../examples/perceus/$$name.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1157-$$name-nat 2> build/s2-1157-$$name-nat.err \
	  || { echo "perceus-1157-full-mask-native FAIL ($$name native build errored)"; cat build/s2-1157-$$name-nat.err; exit 1; }; \
	./build/s2-1157-$$name-nat > build/s2-1157-$$name-nat.out; \
	diff -q ../examples/perceus/$$name.out.expected build/s2-1157-$$name-nat.out > /dev/null \
	  || { echo "perceus-1157-full-mask-native FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1157-$$name-nat.out; exit 1; }; \
	echo "perceus-1157-full-mask-native OK (full slot-mask layout, native)"

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
# issue #1160 — de-opaque regression family, one fixture per stage: the
# inline tag read (stage 1), the static-kind boxed slot read incl. the
# heap-Int mint (stage 2), the tagged-compare fast path with its generic
# slow paths (stage 3) and the raw call-arg binder verdict with sibling-arm
# widening (stage 4 increment). Golden output, both backends byte-equal.
DE_OPAQUE_1160_FIXTURES := variant_tag_inline_1160 variant_projkind_read_1160 tagged_compare_1160 binder_raw_call_arg_1160

test-perceus-1160-de-opaque: $(TARGET) build
	@set -e; \
	for name in $(DE_OPAQUE_1160_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; \
	  ./$(TARGET) --path ../stdlib $$src > build/s2-1160-$$name.c \
	    || { echo "perceus-1160-de-opaque FAIL ($$name kaic2 errored)"; exit 1; }; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1160-$$name.c -o build/s2-1160-$$name $(LDLIBS_M); \
	  ./build/s2-1160-$$name > build/s2-1160-$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-1160-$$name.out > /dev/null \
	    || { echo "perceus-1160-de-opaque FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1160-$$name.out; exit 1; }; \
	  echo "perceus-1160-de-opaque OK ($$name, C oracle)"; \
	done

test-perceus-1160-de-opaque-native: $(TARGET) build
	@set -e; \
	for name in $(DE_OPAQUE_1160_FIXTURES); do \
	  src=../examples/perceus/$$name.kai; \
	  KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1160-$$name-nat 2> build/s2-1160-$$name-nat.err \
	    || { echo "perceus-1160-de-opaque-native FAIL ($$name native build errored)"; cat build/s2-1160-$$name-nat.err; exit 1; }; \
	  ./build/s2-1160-$$name-nat > build/s2-1160-$$name-nat.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/s2-1160-$$name-nat.out > /dev/null \
	    || { echo "perceus-1160-de-opaque-native FAIL ($$name output diff)"; diff ../examples/perceus/$$name.out.expected build/s2-1160-$$name-nat.out; exit 1; }; \
	  echo "perceus-1160-de-opaque-native OK ($$name, native)"; \
	done

# Issue #1158 — the KIR inliner must not corrupt the TRMC spine of a
# caller it pastes into: the leaf helper inlines ahead of the modulo-cons
# step, and the 1M build still runs O(1)-stack with the right checksum.
test-kir-inline-trmc-1158: $(TARGET) build
	@set -e; \
	src=../examples/perceus/kir_inline_trmc_tail_1158.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1158trmc 2> build/s2-1158trmc.err \
	  || { echo "kir-inline-trmc-1158 FAIL (native build errored)"; cat build/s2-1158trmc.err; exit 1; }; \
	$(TIMEOUT) 60 ./build/s2-1158trmc > build/s2-1158trmc.out \
	  || { echo "kir-inline-trmc-1158 FAIL (run crashed/hung — TRMC spine broke under the paste)"; exit 1; }; \
	diff -q ../examples/perceus/kir_inline_trmc_tail_1158.out.expected build/s2-1158trmc.out > /dev/null \
	  || { echo "kir-inline-trmc-1158 FAIL (output diff)"; diff ../examples/perceus/kir_inline_trmc_tail_1158.out.expected build/s2-1158trmc.out; exit 1; }; \
	kir=$$(./$(TARGET) --path ../stdlib --emit=kir $$src 2>/dev/null | awk '/^fn build/,/^}/'); \
	echo "$$kir" | grep -q 'trmc-step' \
	  || { echo "kir-inline-trmc-1158 FAIL (build lost its trmc-step terminator)"; exit 1; }; \
	echo "$$kir" | grep -q 'classify' \
	  && { echo "kir-inline-trmc-1158 FAIL (classify call survived — leaf inline regressed)"; exit 1; } \
	  || echo "kir-inline-trmc-1158 OK (helper inlined, trmc-step intact, 1M build O(1)-stack, checksum matches)"

# Issue #1158 — the while/until fusion chain: combinator + lambdas paste
# into main, the var cells stop escaping, and the canonical State
# handlers rewrite to registers. The KIR must show slots, not dispatch.
test-kir-while-slot-1158: $(TARGET) build
	@set -e; \
	src=../examples/perceus/kir_while_slot_1158.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1158wslot 2> build/s2-1158wslot.err \
	  || { echo "kir-while-slot-1158 FAIL (native build errored)"; cat build/s2-1158wslot.err; exit 1; }; \
	./build/s2-1158wslot > build/s2-1158wslot.out \
	  || { echo "kir-while-slot-1158 FAIL (run errored)"; exit 1; }; \
	diff -q ../examples/perceus/kir_while_slot_1158.out.expected build/s2-1158wslot.out > /dev/null \
	  || { echo "kir-while-slot-1158 FAIL (output diff)"; diff ../examples/perceus/kir_while_slot_1158.out.expected build/s2-1158wslot.out; exit 1; }; \
	kir=$$(./$(TARGET) --path ../stdlib --emit=kir $$src 2>/dev/null | awk '/^fn main/,/^}/'); \
	echo "$$kir" | grep -qE 'install State|perform State' \
	  && { echo "kir-while-slot-1158 FAIL (State dispatch survived in main)"; exit 1; } \
	  || true; \
	echo "$$kir" | grep -qE '__slot: (i64|f64|i1)|__vcell_' \
	  || { echo "kir-while-slot-1158 FAIL (no var-cell register in main — neither a raw cell nor a vcell)"; exit 1; }; \
	echo "kir-while-slot-1158 OK (loop inline, var cells are registers, no State dispatch in main)"

# Issue #1173 — a while whose predicate captures a NON-alias register
# (the loop bound is a fn param). The dead-closure sweep must clear
# the fused predicate closure so the var cells still rewrite to slots.
test-kir-while-param-slot-1173: $(TARGET) build
	@set -e; \
	src=../examples/perceus/kir_while_param_slot_1173.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1173wpslot 2> build/s2-1173wpslot.err \
	  || { echo "kir-while-param-slot-1173 FAIL (native build errored)"; cat build/s2-1173wpslot.err; exit 1; }; \
	./build/s2-1173wpslot > build/s2-1173wpslot.out \
	  || { echo "kir-while-param-slot-1173 FAIL (run errored)"; exit 1; }; \
	diff -q ../examples/perceus/kir_while_param_slot_1173.out.expected build/s2-1173wpslot.out > /dev/null \
	  || { echo "kir-while-param-slot-1173 FAIL (output diff)"; diff ../examples/perceus/kir_while_param_slot_1173.out.expected build/s2-1173wpslot.out; exit 1; }; \
	kir=$$(./$(TARGET) --path ../stdlib --emit=kir $$src 2>/dev/null | awk '/^fn lcg/,/^}/'); \
	echo "$$kir" | grep -qE 'install State|perform State' \
	  && { echo "kir-while-param-slot-1173 FAIL (State dispatch survived in lcg)"; exit 1; } \
	  || true; \
	echo "$$kir" | grep -qE '__slot: (i64|f64|i1)|__vcell_' \
	  || { echo "kir-while-param-slot-1173 FAIL (no var-cell register in lcg — neither a raw cell nor a vcell)"; exit 1; }; \
	echo "kir-while-param-slot-1173 OK (dead predicate closure swept, var cells are registers, no State dispatch in lcg)"

# Issue #1173 negative — a var whose closure SURVIVES the inliner (it
# is stored in a list and called through a pattern binding) must keep
# its dispatched State handler; a slot there would be stale memory.
test-kir-var-escape-state-1173: $(TARGET) build
	@set -e; \
	src=../examples/perceus/var_escapes_stays_state_1173.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1173vesc 2> build/s2-1173vesc.err \
	  || { echo "kir-var-escape-state-1173 FAIL (native build errored)"; cat build/s2-1173vesc.err; exit 1; }; \
	./build/s2-1173vesc > build/s2-1173vesc.out \
	  || { echo "kir-var-escape-state-1173 FAIL (run errored)"; exit 1; }; \
	diff -q ../examples/perceus/var_escapes_stays_state_1173.out.expected build/s2-1173vesc.out > /dev/null \
	  || { echo "kir-var-escape-state-1173 FAIL (output diff)"; diff ../examples/perceus/var_escapes_stays_state_1173.out.expected build/s2-1173vesc.out; exit 1; }; \
	kir=$$(./$(TARGET) --path ../stdlib --emit=kir $$src 2>/dev/null | awk '/^fn main/,/^}/'); \
	echo "$$kir" | grep -q 'install State' \
	  || { echo "kir-var-escape-state-1173 FAIL (State install gone from main — an escaping var was demoted to a slot)"; exit 1; }; \
	echo "kir-var-escape-state-1173 OK (surviving closure keeps the cell a dispatched State handler)"

test-perceus-882-native-rotation-reuse-crash: $(TARGET) build
	@set -e; \
	src=../examples/perceus/gap3_balance_minimal.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-882rot 2> build/s2-882rot.err \
	  || { echo "perceus-882-native-rotation-reuse-crash FAIL (native build errored)"; cat build/s2-882rot.err; exit 1; }; \
	KAI_TRACE_RC=1 KAI_MAX_HEAP=4g $(TIMEOUT) 30 ./build/s2-882rot > build/s2-882rot.out 2> build/s2-882rot.rc \
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

# Issue #995 — the native backend reused a recursive-ADT node in place while
# the original was still live, mutating it (orig=5 instead of 3). The C oracle
# was correct. The parity ratchet catches native!=C divergence, but it would
# miss a regression that corrupts BOTH backends identically; this gate anchors
# the ABSOLUTE correct output (orig=3) on native against the golden. Both the
# binary tree and the linear list shapes are exercised in one fixture.
test-perceus-995-native-shared-reuse-corruption: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_shared_reuse_corruption_995.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-995corr 2> build/s2-995corr.err \
	  || { echo "perceus-995-native-shared-reuse-corruption FAIL (native build errored)"; cat build/s2-995corr.err; exit 1; }; \
	./build/s2-995corr > build/s2-995corr.out 2> build/s2-995corr.rc \
	  || { echo "perceus-995-native-shared-reuse-corruption FAIL (native run errored — exit $$?)"; exit 1; }; \
	diff -q ../examples/perceus/native_shared_reuse_corruption_995.out.expected build/s2-995corr.out > /dev/null \
	  && echo "perceus-995-native-shared-reuse-corruption OK (native orig=3 — original not mutated by the rewrite)" \
	  || { echo "perceus-995-native-shared-reuse-corruption FAIL (output diff — original corrupted)"; diff ../examples/perceus/native_shared_reuse_corruption_995.out.expected build/s2-995corr.out; exit 1; }

# Issue #995 companion — the fix must not disable valid reuse-in-place. With a
# DEAD donor (no live alias) the rewrite must still recycle the nodes: assert
# the native binary reports a non-zero `reuse_in_place` under KAI_TRACE_RC. A
# regression that fresh-allocated everywhere (the over-conservative fix) shows
# reuse_in_place=0 and fails here.
test-perceus-995-native-dead-donor-reuse: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_dead_donor_reuse_995.kai; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-995dead 2> build/s2-995dead.err \
	  || { echo "perceus-995-native-dead-donor-reuse FAIL (native build errored)"; cat build/s2-995dead.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-995dead > build/s2-995dead.out 2> build/s2-995dead.rc \
	  || { echo "perceus-995-native-dead-donor-reuse FAIL (native run errored — exit $$?)"; exit 1; }; \
	diff -q ../examples/perceus/native_dead_donor_reuse_995.out.expected build/s2-995dead.out > /dev/null \
	  || { echo "perceus-995-native-dead-donor-reuse FAIL (output diff)"; diff ../examples/perceus/native_dead_donor_reuse_995.out.expected build/s2-995dead.out; exit 1; }; \
	reuse=$$(grep -oE 'reuse_in_place=[0-9]+' build/s2-995dead.rc | head -1 | cut -d= -f2); \
	[ -n "$$reuse" ] \
	  || { echo "perceus-995-native-dead-donor-reuse FAIL (no reuse_in_place line)"; cat build/s2-995dead.rc; exit 1; }; \
	[ "$$reuse" -gt 0 ] \
	  && echo "perceus-995-native-dead-donor-reuse OK (native reuse_in_place=$$reuse > 0; dead-donor reuse-in-place preserved)" \
	  || { echo "perceus-995-native-dead-donor-reuse FAIL (reuse_in_place=0 — valid reuse disabled by the fix)"; exit 1; }

# Reuse-in-place must not donate a PROJECTION scrutinee (`match e.kind`):
# the parent's claim on the cell is invisible to the rc==1 guard, so a
# donation hands the cell two owners (double-free under any legitimate
# parent drop; a leak without it). Runs the fixture on both backends
# against the golden (the original tree must come back un-mutated), then
# asserts the RECOGNITION ratchet on the emitted C: the projection walk
# gets fresh allocators, no reuse call.
test-perceus-1069-projection-reuse: $(TARGET) build
	@set -e; \
	src=../examples/perceus/reuse_projection_scrutinee_1069.kai; \
	exp=../examples/perceus/reuse_projection_scrutinee_1069.out.expected; \
	../bin/kai build --backend=c $$src -o build/s2-1069proj-c 2> build/s2-1069proj.err \
	  || { echo "perceus-1069-projection-reuse FAIL (C build errored)"; cat build/s2-1069proj.err; exit 1; }; \
	./build/s2-1069proj-c > build/s2-1069proj-c.out \
	  || { echo "perceus-1069-projection-reuse FAIL (C run errored)"; exit 1; }; \
	diff -q $$exp build/s2-1069proj-c.out > /dev/null \
	  || { echo "perceus-1069-projection-reuse FAIL (C output diff — original corrupted)"; diff $$exp build/s2-1069proj-c.out; exit 1; }; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1069proj-n 2> build/s2-1069proj-n.err \
	  || { echo "perceus-1069-projection-reuse FAIL (native build errored)"; cat build/s2-1069proj-n.err; exit 1; }; \
	./build/s2-1069proj-n > build/s2-1069proj-n.out \
	  || { echo "perceus-1069-projection-reuse FAIL (native run errored — exit $$?)"; exit 1; }; \
	diff -q $$exp build/s2-1069proj-n.out > /dev/null \
	  || { echo "perceus-1069-projection-reuse FAIL (native output diff — original corrupted)"; diff $$exp build/s2-1069proj-n.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src > build/s2-1069proj.c; \
	body=$$(awk '/\*kai_grow_kind\(KaiValue \*kai_e\) \{/{f=1} f{print} f&&/^}/{exit}' build/s2-1069proj.c); \
	echo "$$body" | grep -q "kai_variant_u" \
	  || { echo "perceus-1069-projection-reuse FAIL (grow_kind not found in emitted C)"; exit 1; }; \
	if echo "$$body" | grep -qE "kai_variant_reuse_at|kai_reuse_or_alloc_variant"; then \
	  echo "perceus-1069-projection-reuse FAIL (projection scrutinee recognised as reuse donor)"; exit 1; \
	fi; \
	echo "perceus-1069-projection-reuse OK (both backends golden; projection walk gets fresh allocators)"

# Variant spine-free teardown gate. A dead unique variant tree is
# reclaimed by the iterative walk, which consumes unique children
# without a decref: the fixture's ~151k-node teardown leaves
# decref_total ~151k (measured; 302k with the per-slot cascade). The
# ceiling separates the two by a wide margin on both backends. The
# golden also proves a shared subtree survives its parent's teardown
# (a walk that wrongly consumed a shared child corrupts `size(s)`).
test-perceus-1083-variant-spine-free: $(TARGET) build
	@set -e; \
	src=../examples/perceus/variant_spine_free_1083.kai; \
	exp=../examples/perceus/variant_spine_free_1083.out.expected; \
	../bin/kai build --backend=c $$src -o build/s2-1083spine-c 2> build/s2-1083spine.err \
	  || { echo "perceus-1083-variant-spine-free FAIL (C build errored)"; cat build/s2-1083spine.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1083spine-c > build/s2-1083spine-c.out 2> build/s2-1083spine-c.rc \
	  || { echo "perceus-1083-variant-spine-free FAIL (C run errored — exit $$?)"; exit 1; }; \
	diff -q $$exp build/s2-1083spine-c.out > /dev/null \
	  || { echo "perceus-1083-variant-spine-free FAIL (C output diff)"; diff $$exp build/s2-1083spine-c.out; exit 1; }; \
	cdec=$$(grep -oE 'decref_total=[0-9]+' build/s2-1083spine-c.rc | head -1 | cut -d= -f2); \
	[ -n "$$cdec" ] \
	  || { echo "perceus-1083-variant-spine-free FAIL (no decref_total line, C)"; cat build/s2-1083spine-c.rc; exit 1; }; \
	[ "$$cdec" -lt 220000 ] \
	  || { echo "perceus-1083-variant-spine-free FAIL (C decref_total=$$cdec >= 220000 — teardown pays the per-slot cascade again)"; exit 1; }; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1083spine-n 2> build/s2-1083spine-n.err \
	  || { echo "perceus-1083-variant-spine-free FAIL (native build errored)"; cat build/s2-1083spine-n.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1083spine-n > build/s2-1083spine-n.out 2> build/s2-1083spine-n.rc \
	  || { echo "perceus-1083-variant-spine-free FAIL (native run errored — exit $$?)"; exit 1; }; \
	diff -q $$exp build/s2-1083spine-n.out > /dev/null \
	  || { echo "perceus-1083-variant-spine-free FAIL (native output diff)"; diff $$exp build/s2-1083spine-n.out; exit 1; }; \
	ndec=$$(grep -oE 'decref_total=[0-9]+' build/s2-1083spine-n.rc | head -1 | cut -d= -f2); \
	[ -n "$$ndec" ] \
	  || { echo "perceus-1083-variant-spine-free FAIL (no decref_total line, native)"; cat build/s2-1083spine-n.rc; exit 1; }; \
	[ "$$ndec" -lt 220000 ] \
	  || { echo "perceus-1083-variant-spine-free FAIL (native decref_total=$$ndec >= 220000 — teardown pays the per-slot cascade again)"; exit 1; }; \
	echo "perceus-1083-variant-spine-free OK (both backends golden; C decref_total=$$cdec, native decref_total=$$ndec < 220000 over ~151k-node teardown)"

# Issue #1083 — native runtime-op inlining gate. Asserts the emitter stamps
# host target-features so the opt pipeline folds the `kaix_*` runtime ops into
# the rb-tree hot loop. Without the stamp the residual `bl kaix_*` call count
# jumps from ~168 back past 1000 (areInlineCompatible blocks every inline).
test-native-1083-inline-gate: $(TARGET)
	@CC="$(CC)" LLVM_CONFIG="$(LLVM_CONFIG)" ../tools/native-perf/inline-gate.sh

# Issue #1083 — native pointer-slot borrow soundness. A `match` arm that binds
# a variant's POINTER children (kind-0, read as a direct `.ptr` borrow via
# `KProjBorrow`/`kaix_variant_arg_borrow`, skipping the mask branch) on every
# level of the rb-tree navigation shape. The driver builds and discards 2000
# trees, so the native RC must reclaim each. Native-gated (a C-only kaic2
# cannot run --backend=native). Asserts (1) native output parity with the
# golden, (2) native output == C output, and (3) native alloc_total does NOT
# exceed the C oracle's under KAI_TRACE_RC — the direct borrow keeps the same
# RC balance the mask read had (both are #747 borrows, no incref).
test-native-1083-field-read: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_field_read.kai; \
	exp=../examples/perceus/native_field_read.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-1083nfr 2> build/s2-1083nfr.err \
	  || { echo "native-1083-field-read FAIL (native build errored)"; cat build/s2-1083nfr.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-1083nfr > build/s2-1083nfr.out 2> build/s2-1083nfr.rc; \
	diff -q $$exp build/s2-1083nfr.out > /dev/null \
	  || { echo "native-1083-field-read FAIL (native output diff)"; diff $$exp build/s2-1083nfr.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src > build/s2-1083nfr-c.c 2> build/s2-1083nfr-c.err \
	  || { echo "native-1083-field-read FAIL (C build errored)"; cat build/s2-1083nfr-c.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-1083nfr-c.c -o build/s2-1083nfr-c $(LDLIBS_M) ; \
	KAI_TRACE_RC=1 ./build/s2-1083nfr-c > build/s2-1083nfr-c.out 2> build/s2-1083nfr-c.rc; \
	diff -q build/s2-1083nfr-c.out build/s2-1083nfr.out > /dev/null \
	  || { echo "native-1083-field-read FAIL (native vs C output diff)"; diff build/s2-1083nfr-c.out build/s2-1083nfr.out; exit 1; }; \
	nalloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1083nfr.rc | head -1 | cut -d= -f2); \
	calloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-1083nfr-c.rc | head -1 | cut -d= -f2); \
	[ -n "$$nalloc" ] && [ -n "$$calloc" ] \
	  || { echo "native-1083-field-read FAIL (no alloc line)"; cat build/s2-1083nfr.rc build/s2-1083nfr-c.rc; exit 1; }; \
	[ "$$nalloc" -le "$$(($$calloc + 10))" ] \
	  && echo "native-1083-field-read OK (native==C output; native alloc_total=$$nalloc <= C alloc_total+10=$$(($$calloc + 10)); pointer borrow keeps RC balance)" \
	  || { echo "native-1083-field-read FAIL (native alloc_total=$$nalloc > C=$$calloc — pointer borrow disturbed RC)"; exit 1; }

# Eje 1 — the native backend reads variant slots INLINE (`GEP(&v->as)[i]` +
# typed load) instead of the `kaix_variant_arg*` shim: an Int slot (kind-1,
# `.i64`), a Real slot (kind-2, `.r`), and pointer slots (kind-0, `.ptr`), all
# in hot match-arms of a binary tree. Native output must be BYTE-IDENTICAL to
# the C oracle and RC must stay balanced (the inline read adds no alloc vs C).
# Native-gated: a C-only kaic2 cannot run --backend=native.
test-native-static-slot-read: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_static_slot_read.kai; \
	exp=../examples/perceus/native_static_slot_read.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-nssr 2> build/s2-nssr.err \
	  || { echo "native-static-slot-read FAIL (native build errored)"; cat build/s2-nssr.err; exit 1; }; \
	KAI_TRACE_RC=1 ./build/s2-nssr > build/s2-nssr.out 2> build/s2-nssr.rc; \
	diff -q $$exp build/s2-nssr.out > /dev/null \
	  || { echo "native-static-slot-read FAIL (native output diff)"; diff $$exp build/s2-nssr.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src > build/s2-nssr-c.c 2> build/s2-nssr-c.err \
	  || { echo "native-static-slot-read FAIL (C build errored)"; cat build/s2-nssr-c.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-nssr-c.c -o build/s2-nssr-c $(LDLIBS_M) ; \
	KAI_TRACE_RC=1 ./build/s2-nssr-c > build/s2-nssr-c.out 2> build/s2-nssr-c.rc; \
	diff -q build/s2-nssr-c.out build/s2-nssr.out > /dev/null \
	  || { echo "native-static-slot-read FAIL (native vs C output diff)"; diff build/s2-nssr-c.out build/s2-nssr.out; exit 1; }; \
	nalloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-nssr.rc | head -1 | cut -d= -f2); \
	calloc=$$(grep -oE 'alloc_total=[0-9]+' build/s2-nssr-c.rc | head -1 | cut -d= -f2); \
	[ -n "$$nalloc" ] && [ -n "$$calloc" ] \
	  || { echo "native-static-slot-read FAIL (no alloc line)"; cat build/s2-nssr.rc build/s2-nssr-c.rc; exit 1; }; \
	[ "$$nalloc" -le "$$(($$calloc + 10))" ] \
	  && echo "native-static-slot-read OK (native==C output; native alloc_total=$$nalloc <= C+10; inline slot read keeps RC balance)" \
	  || { echo "native-static-slot-read FAIL (native alloc_total=$$nalloc > C=$$calloc — inline read disturbed RC)"; exit 1; }

# Issue #1139 -- the purity attribute closure. A confirmed-pure scalar fn
# carries `memory(none)` in the native IR (and NEVER `willreturn`); an
# allocating fn carries only `nounwind`. Binary acceptance: `opt -O2`
# CSEs two identical pure calls to one. Native-gated: a C-only kaic2
# cannot dump the native IR. Requires `opt` (LLVM 18).
# KAI_NATIVE_MODULAR=0: the purity attributes are a property of the emitter,
# identical on both native paths, but this gate INSPECTS the dumped IR for the
# user fns `@heavy`/`@noisy` — which the native-modular default splits across
# partition modules (the dumped module then carries them only as `declare`).
# Pin the whole-program path so the dump is the single module that DEFINES them.
test-native-purity-attrs: $(TARGET) build
	@set -e; \
	src=../examples/perceus/purity_licm.kai; \
	exp=../examples/perceus/purity_licm.out.expected; \
	KAI_NATIVE_DUMP_IR=build/s2-purity.ll KAI_NATIVE_MODULAR=0 KAI_BACKEND=native ../bin/kai build $$src -o build/s2-purity 2> build/s2-purity.err \
	  || { echo "native-purity FAIL (native build errored)"; cat build/s2-purity.err; exit 1; }; \
	./build/s2-purity > build/s2-purity.out 2>&1; \
	diff -q $$exp build/s2-purity.out > /dev/null \
	  || { echo "native-purity FAIL (native output diff)"; diff $$exp build/s2-purity.out; exit 1; }; \
	hgrp=$$(grep -oE 'define i64 @heavy\([^)]*\) #[0-9]+' build/s2-purity.ll | grep -oE '#[0-9]+'); \
	grep -Eq "^attributes $$hgrp = \{ nounwind memory\(none\) \}" build/s2-purity.ll \
	  || { echo "native-purity FAIL (heavy $$hgrp not nounwind memory(none))"; grep "^attributes" build/s2-purity.ll; exit 1; }; \
	ngrp=$$(grep -oE 'define ptr @noisy\([^)]*\) #[0-9]+' build/s2-purity.ll | grep -oE '#[0-9]+'); \
	grep -Eq "^attributes $$ngrp = \{ nounwind \}" build/s2-purity.ll \
	  || { echo "native-purity FAIL (noisy $$ngrp should be nounwind only)"; grep "^attributes" build/s2-purity.ll; exit 1; }; \
	if grep -q 'willreturn' build/s2-purity.ll; then echo "native-purity FAIL (willreturn stamped)"; exit 1; fi; \
	printf 'declare i64 @h(i64) #0\ndefine i64 @t(i64 %%a) {\n  %%x = call i64 @h(i64 %%a)\n  %%y = call i64 @h(i64 %%a)\n  %%r = add i64 %%x, %%y\n  ret i64 %%r\n}\nattributes #0 = { nounwind memory(none) }\n' > build/s2-purity-cse.ll; \
	ncalls=$$(opt -O2 -S build/s2-purity-cse.ll 2>/dev/null | grep -c 'call i64 @h'); \
	[ "$$ncalls" = "1" ] \
	  && echo "native-purity OK (heavy=memory(none), noisy=nounwind, no willreturn, duplicate pure call CSE'd to $$ncalls)" \
	  || { echo "native-purity FAIL (pure call NOT CSE'd: $$ncalls calls)"; exit 1; }

# Issue #1110 — a kind-1 Int arm binder read raw AND rebuilt into a reused
# variant Int slot must produce output byte-identical to the C oracle. Before
# the fix the native reuse-in-place path corrupted the value (444 -> 368) or
# handed a raw i64 to a tagged dispatch (self-compile SIGSEGV). The single
# backend-aware rawness verdict keeps the binder's mode and KSlot in lockstep,
# so native and C agree bit-for-bit.
test-native-1110-reuse-int-slot: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_reuse_int_slot_rebuild_1110.kai; \
	exp=../examples/perceus/native_reuse_int_slot_rebuild_1110.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-r1110 2> build/s2-r1110.err \
	  || { echo "native-1110-reuse FAIL (native build errored)"; cat build/s2-r1110.err; exit 1; }; \
	./build/s2-r1110 > build/s2-r1110.out; \
	diff -q $$exp build/s2-r1110.out > /dev/null \
	  || { echo "native-1110-reuse FAIL (native output diff)"; diff $$exp build/s2-r1110.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src > build/s2-r1110-c.c 2> build/s2-r1110-c.err \
	  || { echo "native-1110-reuse FAIL (C build errored)"; cat build/s2-r1110-c.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-r1110-c.c -o build/s2-r1110-c $(LDLIBS_M) ; \
	./build/s2-r1110-c > build/s2-r1110-c.out; \
	diff -q build/s2-r1110-c.out build/s2-r1110.out > /dev/null \
	  && echo "native-1110-reuse OK (native==C output; reuse-in-place Int slot rebuild sound)" \
	  || { echo "native-1110-reuse FAIL (native vs C output diff)"; diff build/s2-r1110-c.out build/s2-r1110.out; exit 1; }

# A kind-1 Int match-arm sub-binder seeded raw in a hot Ty walker must carry
# ONE rawness verdict across unbox/perceus/KIR-lowering. Before the fix the
# KSlot re-derived rawness by scanning the perceus-mutated body; a dropped
# `MUnboxed` read desynced KSlot from the register. The position-keyed side
# table keeps the unbox seed's verdict and the KSlot in lockstep, so native and
# C agree bit-for-bit (the bind_slot_kslot-rescan class, refs #1229).
test-native-1229-arm-int-binder: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_arm_int_binder_walker_1229.kai; \
	exp=../examples/perceus/native_arm_int_binder_walker_1229.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-r1229 2> build/s2-r1229.err \
	  || { echo "native-1229-arm-int-binder FAIL (native build errored)"; cat build/s2-r1229.err; exit 1; }; \
	./build/s2-r1229 > build/s2-r1229.out; \
	diff -q $$exp build/s2-r1229.out > /dev/null \
	  || { echo "native-1229-arm-int-binder FAIL (native output diff)"; diff $$exp build/s2-r1229.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src > build/s2-r1229-c.c 2> build/s2-r1229-c.err \
	  || { echo "native-1229-arm-int-binder FAIL (C build errored)"; cat build/s2-r1229-c.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-r1229-c.c -o build/s2-r1229-c $(LDLIBS_M) ; \
	./build/s2-r1229-c > build/s2-r1229-c.out; \
	diff -q build/s2-r1229-c.out build/s2-r1229.out > /dev/null \
	  && echo "native-1229-arm-int-binder OK (native==C output; raw arm-binder verdict single-sourced)" \
	  || { echo "native-1229-arm-int-binder FAIL (native vs C output diff)"; diff build/s2-r1229-c.out build/s2-r1229.out; exit 1; }

# The WRITE-side of #1229. A hot Ty walker rebuilds nodes reusing the unique
# scrutinee; one arm binds a kind-1 Int sub-binder inside a NESTED match, a
# sibling arm rebuilds via a reuse fork with no nested match. The nested match's
# `<join>$sd` exit-drop register used to leak into the sibling arms — their
# reuse fork read the stale `mscr`, so the exit drop freed the wrong register
# (a raw i64 from the nested arm dropped as a pointer). Restoring `mscr` per
# group keeps each arm's exit drop on its own scrutinee; native == C bit-for-bit.
test-native-1229-writeside: $(TARGET) build
	@set -e; \
	src=../examples/perceus/native_reuse_mscr_stale_1229.kai; \
	exp=../examples/perceus/native_reuse_mscr_stale_1229.out.expected; \
	KAI_BACKEND=native ../bin/kai build $$src -o build/s2-r1229w 2> build/s2-r1229w.err \
	  || { echo "native-1229-writeside FAIL (native build errored)"; cat build/s2-r1229w.err; exit 1; }; \
	./build/s2-r1229w > build/s2-r1229w.out; \
	diff -q $$exp build/s2-r1229w.out > /dev/null \
	  || { echo "native-1229-writeside FAIL (native output diff)"; diff $$exp build/s2-r1229w.out; exit 1; }; \
	./$(TARGET) --path ../stdlib $$src > build/s2-r1229w-c.c 2> build/s2-r1229w-c.err \
	  || { echo "native-1229-writeside FAIL (C build errored)"; cat build/s2-r1229w-c.err; exit 1; }; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/s2-r1229w-c.c -o build/s2-r1229w-c $(LDLIBS_M) ; \
	./build/s2-r1229w-c > build/s2-r1229w-c.out; \
	diff -q build/s2-r1229w-c.out build/s2-r1229w.out > /dev/null \
	  && echo "native-1229-writeside OK (native==C output; per-group mscr restore, reuse-fork exit drop sound)" \
	  || { echo "native-1229-writeside FAIL (native vs C output diff)"; diff build/s2-r1229w-c.out build/s2-r1229w.out; exit 1; }

# Issue #500 — native build profiles. Drives examples/build-modes/check.sh:
# --debug emits DWARF whose line table names the .kai source and a panic
# prints a kaikai-source stack trace; --release strips and is smaller than
# the default. The script SKIPs (exit 0) on a cc-only kaic2 (DWARF is
# native-only), so this is safe in the C-only tier1 and meaningful in the
# native (KAI_LLVM=1) build.
test-build-modes: $(TARGET) build
	@../examples/build-modes/check.sh ../bin/kai build/build-modes-work

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
	KAI_MAX_HEAP=4g $(TIMEOUT) 60 ../bin/kai test --backend=c $$src > build/s2-950tr-c.out 2>&1 || cexit=$$?; \
	diff -q $$gold build/s2-950tr-c.out > /dev/null \
	  || { echo "native-test-runner-950 FAIL (C output diff)"; diff $$gold build/s2-950tr-c.out; exit 1; }; \
	[ "$$cexit" -eq 1 ] || { echo "native-test-runner-950 FAIL (C exit $$cexit != 1)"; exit 1; }; \
	nexit=0; \
	KAI_TRACE_RC=1 KAI_MAX_HEAP=4g $(TIMEOUT) 60 ../bin/kai test --backend=native $$src > build/s2-950tr-n.raw 2>&1 || nexit=$$?; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/rule3_basic.kai > build/tco-r3b.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/rule3_no_misfire.kai > build/tco-r3n.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/runtime/issue_678_stdout_line_buffered.kai > build/iss678.c; \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
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
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib \
	  ../examples/effects/issue_978_spawn_parent_handled_effect.kai > build/iss978.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/iss978.c -o build/iss978 $(LDLIBS_M); \
	rc=0; ./build/iss978 > build/iss978.out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] || { echo "issue-978 FAIL (exit $$rc, want 1 — rc 139 = segfault regressed)"; cat build/iss978.out; exit 1; }; \
	diff -q ../examples/effects/issue_978_spawn_parent_handled_effect.out.expected build/iss978.out > /dev/null \
	  && echo "issue-978 OK (spawned fiber's parent-handled effect reports clean diagnostic, not a segfault)" \
	  || { echo "issue-978 DIFF"; diff ../examples/effects/issue_978_spawn_parent_handled_effect.out.expected build/iss978.out; exit 1; }

# Issue #1051 — a fiber spawned inside a nursery performs a BUILTIN effect with
# a stdlib default block (`Log`, `Env`) whose handler is installed in the PARENT
# fiber. Evidence is per-fiber (#820): the child starts with an empty stack, the
# parent handler does not cross the spawn, so the child resolves the effect to
# its default. Pre-fix the parent handler absorbed the effect from main's row,
# the default node was never initialised, and the child's perform dereferenced a
# zero-init node -> segfault (rc 139). Now the default runs in the child (rc 0),
# byte-identical to the no-parent-handler control. The Log op-field must land on
# `info` (not `warn`): the handler's op-thunks are reordered to the effect's
# default-block order, so a perform crossing handler->default indexes the same
# slot. Native op-field parity is enforced by the serial backend-parity sweep.
test-issue-1051-spawn-parent-handled-builtin-default: $(TARGET) build
	@set -e; \
	name=issue_1051_spawn_parent_handled_builtin_default; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	rc=0; ./build/$$name > build/$$name.out 2> build/$$name.err || rc=$$?; \
	[ $$rc -eq 0 ] || { echo "issue-1051 FAIL $$name (exit $$rc, want 0 — rc 139 = segfault regressed)"; cat build/$$name.out build/$$name.err; exit 1; }; \
	diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	  || { echo "issue-1051 DIFF $$name (stdout)"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\] INFO  hi from spawned fiber$$' build/$$name.err \
	  || { echo "issue-1051 FAIL $$name (default INFO line missing/misrouted — op-field regressed)"; cat build/$$name.err; exit 1; }; \
	echo "issue-1051 OK $$name (Log default runs in spawned fiber, op-field lands on info)"; \
	name=issue_1051_spawn_parent_handled_env_default; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	rc=0; ./build/$$name > build/$$name.out 2>&1 || rc=$$?; \
	[ $$rc -eq 0 ] || { echo "issue-1051 FAIL $$name (exit $$rc, want 0)"; cat build/$$name.out; exit 1; }; \
	diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	  && echo "issue-1051 OK $$name (Env default runs in spawned fiber, parent handler does not cross)" \
	  || { echo "issue-1051 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }

# Issue #1067 — a runtime trap (index out of range) inside a fiber unwinds to
# the fiber pad like Cancel, so a trap-exit supervisor contains the fault and
# the process survives — instead of exit(1) killing the whole process. Two
# fixtures: (1) contained — a trapping child delivers a distinguishable
# "Trapped: ..." reason to the supervisor's mailbox (not the "Crashed" a Cancel
# would give), and the program runs to completion (rc 0). (2) top-level — a
# trap with NO pad installed still terminates the process with a message and a
# non-zero exit, the pre-existing behaviour the pad-routing must not silence.
# Issue #1331 — an effect-op argument reaches its handler as an owned
# reference and a block-local whose only read sits in the tail must still be
# released. Both were orphaned on this shape, and each one alone pins a fiber
# wrapper at rc >= 1 for the life of the process, so its 64 KiB stack mmap is
# never reclaimed. Gated on the RC ledger (the output alone cannot see a leak)
# at KAI_THREADS=1, because the ledger reports one thread's counters (#1338).
# Reference point: the same fixture leaked 3103 cells before the fix.
test-issue-1331-op-arg-release: $(TARGET) build
	@set -e; \
	name=issue_1331_op_arg_release; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	KAI_THREADS=1 KAI_TRACE_RC=1 ./build/$$name > build/$$name.out 2> build/$$name.rc \
	  || { echo "issue-1331 FAIL $$name (crash)"; cat build/$$name.rc; exit 1; }; \
	diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	  || { echo "issue-1331 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	leaked=$$(grep -oE 'leaked=[0-9]+' build/$$name.rc | head -1 | cut -d= -f2); \
	[ -n "$$leaked" ] \
	  || { echo "issue-1331 FAIL $$name (no leaked line)"; cat build/$$name.rc; exit 1; }; \
	[ "$$leaked" -lt 100 ] \
	  && echo "issue-1331 OK $$name (leaked=$$leaked < 100 over 1000 fibers; op arg + tail-read local released)" \
	  || { echo "issue-1331 FAIL $$name (leaked=$$leaked — op arg, tail-read local, or closure temp orphaned)"; exit 1; }

# Issue #1331 — the other direction: a borrowed binder handed to an op must be
# duped, or the handler side releases a reference the caller still owns. Gated
# under ASAN because the symptom is a use-after-free on the NEXT read, which a
# plain build reads back as whatever the allocator left behind.
test-issue-1331-borrowed-op-arg-asan: $(TARGET) build
	@set -e; \
	name=issue_1331_borrowed_op_arg; \
	./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/$$name-asan > build/$$name-asan.out 2> build/$$name-asan.err; \
	grep -qE "runtime error|AddressSanitizer" build/$$name-asan.err \
	  && { echo "issue-1331-borrowed-op-arg FAIL $$name"; cat build/$$name-asan.err; exit 1; } \
	  || true; \
	diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	  && echo "issue-1331-borrowed-op-arg OK $$name (no sanitizer diagnostic, output matches)" \
	  || { echo "issue-1331-borrowed-op-arg DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }

# ASAN companion: the releases this fix adds must not free anything twice.
test-issue-1331-op-arg-release-asan: $(TARGET) build
	@set -e; \
	name=issue_1331_op_arg_release; \
	./$(TARGET) --path ../stdlib ../examples/effects/$$name.kai > build/$$name-asan.c; \
	$(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	      -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	      -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan; \
	ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	  ./build/$$name-asan > build/$$name-asan.out 2> build/$$name-asan.err; \
	grep -qE "runtime error|AddressSanitizer" build/$$name-asan.err \
	  && { echo "issue-1331-asan FAIL $$name"; cat build/$$name-asan.err; exit 1; } \
	  || true; \
	diff -q ../examples/effects/$$name.out.expected build/$$name-asan.out > /dev/null \
	  && echo "issue-1331-asan OK $$name (no sanitizer diagnostic, output matches)" \
	  || { echo "issue-1331-asan DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name-asan.out; exit 1; }

test-issue-1067-fiber-trap-isolation: $(TARGET) build
	@set -e; \
	name=issue_1067_trap_in_fiber_contained; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	rc=0; ./build/$$name > build/$$name.out 2>&1 || rc=$$?; \
	[ $$rc -eq 0 ] || { echo "issue-1067 FAIL $$name (exit $$rc, want 0 — trap escaped the fiber)"; cat build/$$name.out; exit 1; }; \
	diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	  && echo "issue-1067 OK $$name (fiber trap contained, supervisor got Trapped reason, process survived)" \
	  || { echo "issue-1067 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	name=issue_1067_trap_top_level_exits; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	rc=0; ./build/$$name > build/$$name.out 2> build/$$name.err || rc=$$?; \
	[ $$rc -ne 0 ] || { echo "issue-1067 FAIL $$name (exit 0 — top-level trap did not terminate)"; cat build/$$name.out; exit 1; }; \
	diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	  || { echo "issue-1067 DIFF $$name (stdout)"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	grep -q "trap: index 10 out of range" build/$$name.err \
	  && echo "issue-1067 OK $$name (top-level trap terminates with message + non-zero exit)" \
	  || { echo "issue-1067 FAIL $$name (trap message missing on stderr)"; cat build/$$name.err; exit 1; }

# Issue #1093 — raw integer `/0`, `%0` and INT64_MIN/-1 were silent UB
# returning divergent garbage across backends. They now trap cleanly (same
# mechanism as an out-of-range index): non-zero exit + a message on stderr,
# identical on the native and C backends. Three negative fixtures (each traps
# with its own message) plus one positive control proving normal integer
# div/mod are unchanged and Real `1.0 / 0.0` still yields inf (not a trap).
test-issue-1093-divzero-trap: $(TARGET) build
	@set -e; \
	for pair in "issue_1093_div_by_zero_traps:divide by zero" \
	            "issue_1093_mod_by_zero_traps:mod by zero" \
	            "issue_1093_div_overflow_traps:divide overflow"; do \
	  name=$${pair%%:*}; msg=$${pair#*:}; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	  rc=0; ./build/$$name > build/$$name.out 2> build/$$name.err || rc=$$?; \
	  [ $$rc -ne 0 ] || { echo "issue-1093 FAIL $$name (exit 0 — did not trap)"; cat build/$$name.out; exit 1; }; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	    || { echo "issue-1093 DIFF $$name (stdout)"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	  grep -q "trap: $$msg" build/$$name.err \
	    && echo "issue-1093 OK $$name (traps: $$msg)" \
	    || { echo "issue-1093 FAIL $$name (want 'trap: $$msg' on stderr)"; cat build/$$name.err; exit 1; }; \
	done; \
	name=issue_1093_div_normal_ok; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	rc=0; ./build/$$name > build/$$name.out 2>&1 || rc=$$?; \
	[ $$rc -eq 0 ] || { echo "issue-1093 FAIL $$name (exit $$rc — normal div/mod or Real div regressed)"; cat build/$$name.out; exit 1; }; \
	diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	  && echo "issue-1093 OK $$name (normal div/mod intact, Real 1.0/0.0 = inf)" \
	  || { echo "issue-1093 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }

# Issue #997 — a spawned fiber that reads/writes an enclosing `var` (a State
# cell) type-checked but segfaulted (rc 139, both backends): the cell's evidence
# is fiber-local (#820) and the by-id lookup on the child fiber returned NULL,
# whose handler deref crashed. The escape check now rejects the common shapes at
# compile time; a runtime guard turns the residue the static check cannot see
# (a cell read reached through a rebound closure alias) into a clean diagnostic
# instead of a segfault. Three reject fixtures, one runtime-floor fixture, two
# positive controls (a fiber's own inner cell; a spawn that never touches it).
test-issue-997-spawn-cell-fiber-local: $(TARGET) build
	@set -e; \
	for name in issue_997_spawn_cell_read_rejected issue_997_spawn_cell_write_rejected \
	            issue_997_spawn_cell_transitive_rejected; do \
	  rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c 2> build/$$name.err || rc=$$?; \
	  [ $$rc -ne 0 ] || { echo "issue-997 FAIL $$name (compiled, want compile-time reject)"; exit 1; }; \
	  grep -F "$$(cat ../examples/effects/$$name.err.expected)" build/$$name.err > /dev/null \
	    && echo "issue-997 OK $$name (cell-across-spawn rejected at compile time)" \
	    || { echo "issue-997 MISS $$name (rejected, wrong diagnostic)"; cat build/$$name.err; exit 1; }; \
	done; \
	./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/issue_997_spawn_cell_runtime_floor.kai > build/iss997rf.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/iss997rf.c -o build/iss997rf $(LDLIBS_M); \
	rc=0; ./build/iss997rf > build/iss997rf.out 2>&1 || rc=$$?; \
	[ $$rc -eq 1 ] || { echo "issue-997 FAIL runtime_floor (exit $$rc, want 1 — rc 139 = segfault regressed)"; cat build/iss997rf.out; exit 1; }; \
	diff -q ../examples/effects/issue_997_spawn_cell_runtime_floor.out.expected build/iss997rf.out > /dev/null \
	  && echo "issue-997 OK runtime_floor (unreachable cell reports clean diagnostic, not a segfault)" \
	  || { echo "issue-997 DIFF runtime_floor"; diff ../examples/effects/issue_997_spawn_cell_runtime_floor.out.expected build/iss997rf.out; exit 1; }; \
	for name in issue_997_spawn_inner_cell_ok issue_997_spawn_no_cell_ok issue_997_spawn_shadowed_cell_ok; do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/effects/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	  ./build/$$name > build/$$name.out 2>&1 \
	    || { echo "issue-997 FAIL $$name (valid program crashed)"; cat build/$$name.out; exit 1; }; \
	  diff -q ../examples/effects/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "issue-997 OK $$name (valid fiber-local cell use unaffected)" \
	    || { echo "issue-997 DIFF $$name"; diff ../examples/effects/$$name.out.expected build/$$name.out; exit 1; }; \
	done

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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/issue_680_polymorphic_fold.kai > build/tco-680.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-680.c -o build/tco-680 $(LDLIBS_M) ; \
	./build/tco-680 > build/tco-680.out; \
	diff -q ../examples/tco/issue_680_polymorphic_fold.out.expected build/tco-680.out > /dev/null \
	  && echo "tco-issue-680 OK (polymorphic fold with closure param at depths 1..64 completes; double-decref under post-#599 skip-set fixed)" \
	  || { echo "tco-issue-680 DIFF"; diff ../examples/tco/issue_680_polymorphic_fold.out.expected build/tco-680.out; exit 1; }

# Issue #1031 — self-tail goto-loop rebind vs same-named binder shadow.
# An arm pattern (or block let) binding a name equal to a fn param
# shadows the C param local at the sentinel site; a by-name rebind wrote
# the shadow and froze the param at its first-call value (Array writes
# all landed on slot 0). The fixture covers pattern shadow with/without
# an effectful store, let shadow, a raw unboxed scalar param, and the
# TRMC modulo-cons loop — all rebinding through the `_kai_tcrp<N>`
# param pointers.
test-tco-param-shadow: $(TARGET) build
	@set -e; \
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/tco/param_shadow_rebind.kai > build/tco-param-shadow.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/tco-param-shadow.c -o build/tco-param-shadow $(LDLIBS_M) ; \
	./build/tco-param-shadow > build/tco-param-shadow.out; \
	diff -q ../examples/tco/param_shadow_rebind.out.expected build/tco-param-shadow.out > /dev/null \
	  && echo "tco-param-shadow OK (issue #1031 shadowed params rebind through _kai_tcrp pointers; scalars advance)" \
	  || { echo "tco-param-shadow DIFF"; diff ../examples/tco/param_shadow_rebind.out.expected build/tco-param-shadow.out; exit 1; }

# Issue #82 — Perceus leak-audit fixture. Exercises four RC
# discipline gaps that survived the m5.x flip: match-scrutinee
# decref at exit, kai_op_truthy on fresh `if` conds, kai_field
# borrowing reads in record-pattern test paths, and the
# kai_core_mailbox_* helpers consuming their KaiValue * params.
# A regression in any of these surfaces under the asan sibling as
# a UAF (decref of a singly-held value the runtime later double-
# frees) or as a stdout diff against the golden.
PERCEUS_ISSUE82_FIXTURES := issue_82_leak_audit

test-perceus-issue82: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_ISSUE82_FIXTURES); do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M) ; \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-issue82 OK $$name (4 leak sources closed)" \
	    || { echo "perceus-issue82 DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

# ASAN+UBSan sibling for the issue #82 leak-audit. The fixes
# touch emit-side decref discipline (stages 1/2 plus LLVM if) and
# the kai_core_mailbox_* helpers in stage0/runtime.h; ASan
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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

# TRMC dropmask / perceus wrap-skip alignment: `[h, ...self(rest, b)]`
# with `b` read once per arm transfers `b` raw; the goto-block dropmask
# must not plant a drop for it (over-decref frees the shared arg while
# live). The plain target checks functional output; the ASAN sibling
# (cell pool disabled so a stale cons hits free'd memory) catches the
# use-after-free directly.
PERCEUS_TRMC_SPREAD_FIXTURES := trmc_spread_shared_arg

test-perceus-trmc-spread: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_TRMC_SPREAD_FIXTURES); do \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
	  $(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/$$name.c -o build/$$name $(LDLIBS_M); \
	  ./build/$$name > build/$$name.out; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name.out > /dev/null \
	    && echo "perceus-trmc-spread OK $$name (raw pass-through arg not over-dropped)" \
	    || { echo "perceus-trmc-spread DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name.out; exit 1; }; \
	done

test-perceus-trmc-spread-asan: $(TARGET) build
	@set -e; \
	for name in $(PERCEUS_TRMC_SPREAD_FIXTURES); do \
	  ./$(TARGET) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name-asan.c; \
	  $(CC) -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	        -DKAI_NO_CELL_POOL \
	        -Wno-unused-function -Wno-unused-variable $(LDFLAGS_STACK) \
	        -I ../stage2 -I ../stage0 build/$$name-asan.c -o build/$$name-asan-c; \
	  ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	  UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	    ./build/$$name-asan-c > build/$$name-asan.out 2> build/$$name-asan.err; \
	  if grep -qE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' build/$$name-asan.err; then \
	    echo "perceus-trmc-spread-asan FAIL $$name — sanitizer diagnostic:"; \
	    cat build/$$name-asan.err; exit 1; \
	  fi; \
	  diff -q ../examples/perceus/$$name.out.expected build/$$name-asan.out > /dev/null \
	    && echo "perceus-trmc-spread-asan OK $$name" \
	    || { echo "perceus-trmc-spread-asan DIFF $$name"; diff ../examples/perceus/$$name.out.expected build/$$name-asan.out; exit 1; }; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib ../examples/perceus/$$name.kai > build/$$name.c; \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/seq-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/seq-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f > build/ufcs-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/u2-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/u2-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/ta-$$name.err || rc=$$?; \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/ta-$$name.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) ../examples/effects/rc_trace_callsite.kai > build/rc_trace_callsite.c; \
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

# Structured violation context for contract and refinement panics.
# Each fixture under examples/refinements/ takes one of three shapes:
#   <name>.err.expected     — kaic2 must reject at compile time; every
#                             line must appear in stderr (substring).
#   <name>.run.err.expected — compiles, then panics at runtime with
#                             exactly these bytes on stderr.
#   <name>.out.expected     — compiles and runs cleanly with this
#                             stdout (the happy-path companion).
# Keeps the violation message format pinned so a regression in the
# panic builders trips a clear test rather than a downstream user
# surprise. The native backend's parity is gated separately by the
# native-vs-C ratchet (tools/test-backend-parity.sh).
test-violations: $(TARGET) build
	@$(STACK) set -e; \
	for f in ../examples/refinements/*.kai; do \
	  name=$$(basename $$f .kai); \
	  exp_run_err=../examples/refinements/$$name.run.err.expected; \
	  exp_out=../examples/refinements/$$name.out.expected; \
	  exp_err=../examples/refinements/$$name.err.expected; \
	  if [ -f $$exp_err ]; then \
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) $$f > /dev/null 2> build/viol-$$name.cerr || rc=$$?; \
	    if [ $$rc -eq 0 ]; then \
	      echo "violations FAIL $$name (expected compile error, got success)"; exit 1; \
	    fi; \
	    while read -r line; do \
	      grep -qF "$$line" build/viol-$$name.cerr \
	        || { echo "violations FAIL $$name (missing diagnostic: $$line)"; \
	             cat build/viol-$$name.cerr; exit 1; }; \
	    done < $$exp_err; \
	    echo "violations OK $$name (rejected at compile time)"; \
	    continue; \
	  fi; \
	  ./$(TARGET) $(TEST_CORE_CACHE) $$f > build/viol-$$name.c \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$preludes $$f \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$preludes $$f > build/attr-$$name.c \
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
	./$(TARGET) $(TEST_CORE_CACHE) --doc-json ../examples/doc/doc_json_basic.kai > build/doc-json-basic.out; \
	diff ../examples/doc/doc_json_basic.doc.expected build/doc-json-basic.out > /dev/null \
	  && echo "doc-attr OK doc-json golden" \
	  || { echo "doc-attr DIFF doc-json golden"; \
	       diff ../examples/doc/doc_json_basic.doc.expected build/doc-json-basic.out; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --holes-json ../examples/doc/doc_holes.kai > build/doc-holes.json; \
	grep -qF '"doc": "Documented holder of the probe hole."' build/doc-holes.json \
	  && echo "doc-attr OK holes-json doc field" \
	  || { echo "doc-attr FAIL holes-json doc field"; cat build/doc-holes.json; exit 1; }; \
	./$(TARGET) $(TEST_CORE_CACHE) --builtins-doc-json > build/builtins-doc.json; \
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
	    rc=0; ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f \
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
	    ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib $$f \
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
# program as per-module `.c` translation units + per-TU header slices
# (kai_types_<stem>.h / kai_decls_<stem>.h), splits the marker-delimited
# stream into files, compiles
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
                   ../examples/multi-module/issue-990-handle-linkage/main.kai \
                   ../examples/multi-module/issue-1033-cross-module-fiber/main.kai \
                   ../examples/multi-module/issue-1033-cross-module-arena/main.kai \
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
	    'index($$0,pfx)==1{out=dir"/"substr($$0,length(pfx)+1);next} out{print > out}' \
	    build/$$name.stream; \
	  cp runtime.h $$mod/runtime.h 2>/dev/null || true; \
	  objs=""; \
	  for c in $$mod/*.c; do \
	    o=$${c%.c}.o; \
	    defs=-DKAI_SEPARATE_COMPILATION=1; \
	    [ "$$(basename $$c)" = "__root__.c" ] && defs="$$defs -DKAI_RUNTIME_OWNER=1"; \
	    $(CC) $(CFLAGS) $$defs -I $$mod -I ../stage2 -I ../stage0 -c $$c -o $$o \
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

# Pointer-identity gate for the shared runtime state (issue #1033). stdout
# diffs cannot see identity: two disjoint per-TU copies of a singleton or the
# intern table still print the same bytes, and structural `==` is unaffected.
# A successful link does not prove uniqueness either — an internal-linkage
# (`static`) symbol links green while materialising one copy PER TU. So the
# regression is only visible after link: `nm` on the final executable must
# show exactly ONE address per shared symbol. Two addresses means the guard
# regressed to per-TU state. Runs on the cross-module fiber binary from
# test-modular-build, which links the widest set of runtime modules.
MODULAR_SHARED_SYMS = kai_singleton_unit kai_singleton_nil kai_str_intern_table \
                      kai_char_cache kai_active_fiber kai_ready_head kai_arena_sp \
                      kai_heap_committed kai_next_handler_id
test-modular-identity: test-modular-build
	@set -e; \
	bin=build/modular-6.bin; \
	[ -x $$bin ] || { echo "modular-identity FAIL (no fiber binary; run test-modular-build)"; exit 1; }; \
	fail=0; \
	for s in $(MODULAR_SHARED_SYMS); do \
	  n=$$(nm $$bin 2>/dev/null | grep -E " _?$$s\$$" | awk '{print $$1}' | sort -u | wc -l | tr -d ' '); \
	  if [ "$$n" = "1" ]; then \
	    echo "modular-identity OK $$s (1 address)"; \
	  else \
	    echo "modular-identity FAIL $$s ($$n addresses — per-TU state leaked)"; fail=1; \
	  fi; \
	done; \
	[ $$fail = 0 ] || exit 1; \
	echo "modular-identity OK (shared runtime state is one copy per process)"

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

# `home_tu` per-case partition: one fixture whose c-modular split lands
# each symbol in the TU `home_tu` names. Asserts every decl-to-TU case
# from one stream — a root pub fn in main.c, a root private fn + main +
# lambdas in __root__.c, an imported pub fn and its cross-module spec in
# widget.c — then runs the whole-program build to pin the output. A shift
# in any case (the empty-TU regression an over-broad `home_tu` causes)
# fails an assertion; the run pins the partition link-correct.
test-home-tu-partition: $(TARGET) build
	@set -e; \
	fx=../examples/multi-module/home-tu-partition; \
	KAI_MODULAR=1 KAI_MODULAR_NO_CACHE=1 KAIKAI_STDLIB_PATH=$(STDLIB_ABS) \
	  ./$(TARGET) --emit=c-modular --path ../stdlib --path $$fx $$fx/main.kai \
	  2>/dev/null > build/home_tu_partition.stream; \
	tu_of() { awk -v sym="$$1" '/^\/\/KAIFILE:.*\.c$$/{t=$$0;sub(/.*::/,"",t)} $$0 ~ sym && /\{$$/ {print t; exit}' build/home_tu_partition.stream; }; \
	assert_tu() { got=$$(tu_of "$$2"); [ "$$got" = "$$3" ] \
	  || { echo "home-tu FAIL: $$1 ($$2) in '$$got', want $$3"; exit 1; }; \
	  echo "home-tu OK: $$1 -> $$3"; }; \
	assert_tu "root pub fn"        "kai_main__badge"             "main.c"; \
	assert_tu "root private fn"    "kai_helper"                  "__root__.c"; \
	assert_tu "fn main"            "kai_main.void. .$$"          "__root__.c"; \
	assert_tu "root lambda"        "_kai_lam_main_19_14"         "__root__.c"; \
	assert_tu "imported pub fn"    "kai_widget__label"           "widget.c"; \
	assert_tu "cross-module spec"  "kai_widget__wrap__mono__Int" "widget.c"; \
	./$(TARGET) --path ../stdlib --path $$fx $$fx/main.kai > build/home_tu_partition.c; \
	$(CC) $(CFLAGS) $(CPPFLAGS_CORE) -I ../stage0 build/home_tu_partition.c -o build/home_tu_partition $(LDLIBS_M); \
	./build/home_tu_partition > build/home_tu_partition.out; \
	diff -q $$fx/main.out.expected build/home_tu_partition.out > /dev/null \
	  && echo "test-home-tu-partition OK (6 cases + run)" \
	  || { echo "home-tu DIFF"; diff $$fx/main.out.expected build/home_tu_partition.out; exit 1; }

# Backend incremental lever — parallel cc + .o content-hash cache.
# Drives bin/kai's KAI_MODULAR path (the parallel + cache wrapper, not the
# Makefile's own split loop above) and pins the three cache states from
# the KAI_MODULAR_STATS summary: cold (empty cache -> every TU compiled),
# warm (no change -> every TU a hit, zero recompiled — the cache win), and
# a body-only edit (same signature -> shared headers unchanged -> one
# module recompiled, the rest reused). Each state's binary is diffed
# against the single-TU golden, so a cache-hit .o is pinned link-correct.
# Hermetic: build-local cache dir, fixture copied so the edit never
# dirties the tree, KAI_MODULAR_JOBS capped so the light fan-out doesn't
# oversubscribe.
test-modular-cache: $(TARGET) build
	@set -e; \
	KAI=../bin/kai; \
	cache=build/modular-cache.d; rm -rf $$cache; \
	src=build/modular-cache.src; rm -rf $$src; mkdir -p $$src/geo; \
	cp ../examples/multi-module/main.kai ../examples/multi-module/math.kai $$src/; \
	cp ../examples/multi-module/geo/shapes.kai $$src/geo/; \
	exp=../examples/multi-module/main.out.expected; \
	build_run() { \
	  KAI_BACKEND=c KAI_MODULAR=1 KAI_MODULAR_STATS=1 KAI_MODULAR_JOBS=4 \
	  KAI_MODULAR_CACHE_DIR=$$cache \
	    $$KAI build $$src/main.kai -o build/modular-cache.bin 2> build/modular-cache.$$1.log \
	    || { echo "modular-cache FAIL $$1 (build)"; cat build/modular-cache.$$1.log; exit 1; }; \
	  ./build/modular-cache.bin > build/modular-cache.$$1.out \
	    || { echo "modular-cache FAIL $$1 (run)"; exit 1; }; \
	  diff -q $$exp build/modular-cache.$$1.out > /dev/null \
	    || { echo "modular-cache DIFF $$1"; diff $$exp build/modular-cache.$$1.out; exit 1; }; \
	}; \
	num() { sed -n "s/.*$$2=\([0-9][0-9]*\).*/\1/p" build/modular-cache.$$1.log | head -1; }; \
	tus() { sed -n "s/.*c-modular: \([0-9][0-9]*\) TUs.*/\1/p" build/modular-cache.$$1.log | head -1; }; \
	build_run cold; \
	n=$$(tus cold); cc0=$$(num cold compiled); \
	[ "$$cc0" -gt 0 ] \
	  || { echo "modular-cache FAIL cold (want compiled>0 from empty cache, got compiled=$$cc0 of $$n)"; exit 1; }; \
	echo "modular-cache OK cold ($$cc0/$$n TUs compiled from empty cache)"; \
	build_run warm; \
	c=$$(num warm compiled); \
	[ "$$c" = "0" ] \
	  || { echo "modular-cache FAIL warm (want compiled=0 on no-change rebuild, got compiled=$$c)"; exit 1; }; \
	echo "modular-cache OK warm (0 recompiled, $$n reused)"; \
	printf 'pub fn square(n: Int) : Int {\n  let r = n * n\n  r\n}\n' > $$src/math.kai; \
	build_run incr; \
	c=$$(num incr compiled); \
	[ "$$c" -ge 1 ] && [ "$$c" -lt "$$cc0" ] \
	  || { echo "modular-cache FAIL incr (want 1<=compiled<$$cc0, got compiled=$$c)"; exit 1; }; \
	echo "modular-cache OK incr (one-module edit -> $$c recompiled vs $$cc0 cold)"; \
	cp ../examples/multi-module/math.kai $$src/; \
	printf '\npub type CacheProbe = CPA | CPB\n' >> $$src/math.kai; \
	build_run typeadd; \
	c=$$(num typeadd compiled); \
	[ "$$c" -ge 1 ] && [ "$$c" -le 3 ] && [ "$$c" -lt "$$cc0" ] \
	  || { echo "modular-cache FAIL typeadd (want 1<=compiled<=3, got compiled=$$c of $$cc0 — a type-add must not re-key unreferencing TUs)"; exit 1; }; \
	echo "modular-cache OK typeadd (new sum type -> $$c recompiled vs $$cc0 cold)"; \
	cp ../examples/multi-module/math.kai $$src/; \
	printf '\npub fn cache_probe(xs: [Int]) : [Int] = map(xs, (x) => x + 1)\n' >> $$src/math.kai; \
	build_run fnadd; \
	c=$$(num fnadd compiled); \
	[ "$$c" -ge 1 ] && [ "$$c" -le 4 ] && [ "$$c" -lt "$$cc0" ] \
	  || { echo "modular-cache FAIL fnadd (want 1<=compiled<=4, got compiled=$$c of $$cc0 — a fn-add must not re-key uncalling TUs)"; exit 1; }; \
	echo "modular-cache OK fnadd (new fn -> $$c recompiled vs $$cc0 cold)"; \
	echo "modular-cache OK"

# Per-module emit hermeticity — the property the .o cache reuse rests on:
# an edit to one module must leave every sibling module's emitted .c AND
# every sibling's header slices byte-identical (variant tags are
# name-keyed, lambda symbols are (enc_fn, line, col)-keyed, spec order
# is name-sorted — none of them global counters; header slices carry
# only referenced declarations). Three edit shapes are pinned: a
# body-only edit (only that module's TU changes), a new sum type (only
# that module's TU + __root__.c's registration tables change — the new
# type's structs are referenced nowhere, so no slice moves), and a new
# fn with a lambda (that module's TU + __root__.c + their decls slices).
test-modular-hermetic: $(TARGET) build
	@set -e; \
	src=build/modular-hermetic.src; rm -rf $$src; mkdir -p $$src/geo; \
	cp ../examples/multi-module/main.kai ../examples/multi-module/math.kai $$src/; \
	cp ../examples/multi-module/geo/shapes.kai $$src/geo/; \
	d=build/modular-hermetic.d; rm -rf $$d; mkdir -p $$d; \
	emit_split() { \
	  out=$$d/$$1; rm -rf $$out; mkdir -p $$out; \
	  ./$(TARGET) --emit=c-modular --path ../stdlib --path $$src --edition $$(cat ../EDITION) $$src/main.kai 2> /dev/null > $$d/$$1.stream \
	    || { echo "modular-hermetic FAIL $$1 (emit)"; exit 1; }; \
	  nonce=$$(sed -n '1s/^\/\/KAIDELIM://p' $$d/$$1.stream); \
	  awk -v dir="$$out" -v pfx="//KAIFILE:$$nonce::" \
	    'index($$0, pfx) == 1 { o = dir "/" substr($$0, length(pfx) + 1); next } o { print > o }' \
	    $$d/$$1.stream; \
	}; \
	assert_only() { \
	  a=$$d/base; b=$$d/$$1; shift; \
	  for f in $$a/*; do \
	    n=$$(basename $$f); allow=0; \
	    for ok in "$$@"; do [ "$$n" = "$$ok" ] && allow=1; done; \
	    if cmp -s $$f $$b/$$n; then \
	      : ; \
	    else \
	      [ "$$allow" = "1" ] || { echo "modular-hermetic FAIL: $$n changed (expected only: $$*)"; exit 1; }; \
	    fi; \
	  done; \
	}; \
	emit_split base; \
	printf 'pub fn square(n: Int) : Int {\n  let r = n * n\n  r\n}\n' > $$src/math.kai; \
	emit_split body; \
	assert_only body math.c; \
	echo "modular-hermetic OK body-edit (only math.c changed)"; \
	cp ../examples/multi-module/math.kai $$src/; \
	printf '\npub type HermProbe = HPA | HPB\n' >> $$src/math.kai; \
	emit_split sum; \
	assert_only sum math.c __root__.c; \
	echo "modular-hermetic OK sum-add (only math.c/__root__.c changed; no header slice moved)"; \
	cp ../examples/multi-module/math.kai $$src/; \
	printf '\npub fn herm_lam(xs: [Int]) : [Int] = map(xs, (x) => x + 1)\n' >> $$src/math.kai; \
	emit_split lam; \
	assert_only lam math.c __root__.c kai_decls_math.h kai_decls___root__.h; \
	echo "modular-hermetic OK lambda-add (only math.c/__root__.c + their decls slices changed)"; \
	echo "modular-hermetic OK"

# Shared c-modular .o cache dir for the two full-compiler self-hosts below
# (test-modular-selfhost, test-perceus-1131-modular-escape). Build-local
# (gitignored), so it survives across both gates within a run and, when a
# harness persists it (CI's actions/cache), across runs. The cache keys each
# .o by a content hash folding the compiler, its flags, the target arch, the
# TU's header slices, and the emitted .c — a stale hit is impossible, so the cache
# only elides recompiling byte-identical emitted C. The link (which exercises
# the shared-pool guard) and the byte-identity checks below always run.
CMODULAR_SELFHOST_CACHE := $(CURDIR)/build/cmodular-selfhost-cache

# The WHOLE self-hosted compiler links under --emit=c-modular. runtime.h's
# RC free-list pools (~159 MB) are file-local `static` in the single-TU
# build; the ~86-TU modular split would replicate them into every TU and
# overflow the small-code-model link reach. The shared-pool guard
# (KAI_SEPARATE_COMPILATION / KAI_RUNTIME_OWNER) emits one copy, so the
# split links. Drives bin/kai's KAI_MODULAR path on the compiler itself,
# smokes `--version`, and then makes the modular-built binary COMPILE a
# program, byte-compared against $(TARGET)'s output — linking and
# answering --version is not evidence the binary works (per-TU runtime
# state can corrupt the first real compile while leaving --version
# intact). The .o cache never masks a regression: byte-identity is checked
# against the single-TU compiler on every run; the content hash guarantees a
# hit reproduces the exact object a fresh compile would.
test-modular-selfhost: $(TARGET) build
	@set -e; \
	out=$$(pwd)/build/modular-selfhost.bin; \
	rm -f $$out; \
	echo "== test-modular-selfhost: full compiler links under --emit=c-modular =="; \
	( cd .. && KAI_MODULAR=1 KAI_MODULAR_CACHE_DIR=$(CMODULAR_SELFHOST_CACHE) KAI_MODULAR_STATS=1 bin/kai build stage2/main.kai -o $$out ) \
	  2> build/modular-selfhost.log \
	  || { echo "modular-selfhost FAIL (build/link)"; cat build/modular-selfhost.log; exit 1; }; \
	[ -x $$out ] || { echo "modular-selfhost FAIL (no binary produced)"; exit 1; }; \
	$$out --version > build/modular-selfhost.ver 2>&1 \
	  || { echo "modular-selfhost FAIL (--version run)"; cat build/modular-selfhost.ver; exit 1; }; \
	grep -q "self-hosted" build/modular-selfhost.ver \
	  || { echo "modular-selfhost FAIL (--version output)"; cat build/modular-selfhost.ver; exit 1; }; \
	KAIKAI_STDLIB_PATH=$(STDLIB_ABS) $$out ../examples/portfolio/portfolio.kai > build/modular-selfhost-smoke.c \
	  || { echo "modular-selfhost FAIL (modular binary crashed compiling)"; exit 1; }; \
	KAIKAI_STDLIB_PATH=$(STDLIB_ABS) ./$(TARGET) ../examples/portfolio/portfolio.kai > build/modular-selfhost-smoke-ref.c; \
	diff -q build/modular-selfhost-smoke-ref.c build/modular-selfhost-smoke.c > /dev/null \
	  || { echo "modular-selfhost FAIL (emitted C differs from single-TU kaic2)"; exit 1; }; \
	echo "modular-selfhost OK (linked modularly + --version + compiles byte-identically)"

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

# Shared core-parse cache for the fixture loops (KAI_CORE_CACHE=0 disables,
# mirroring bin/kai). The dir is scoped by the compiler binary's mtime-size —
# the same invalidation bin/kai uses — so a rebuilt kaic2 never reads another
# build's blobs. Only --core-cache-dir is passed (no --toolchain-id), so the
# emitted-C and native core caches stay off here: the fixture pool shares only
# the post-parse core blobs. Blob writes are not atomic, so the parallel
# fan-outs below depend on `core-cache-warm` to populate the cache serially
# first; their children then only read. A truncated blob fails the KAB2
# decode and falls back to a fresh parse — a miss, never wrong decls.
TEST_CORE_CACHE_TID = $(shell stat -f '%m-%z' $(TARGET) 2>/dev/null || stat -c '%Y-%s' $(TARGET) 2>/dev/null)
TEST_CORE_CACHE_DIR = build/core-cache/$(TEST_CORE_CACHE_TID)
ifeq ($(KAI_CORE_CACHE),0)
TEST_CORE_CACHE :=
else
TEST_CORE_CACHE = $(if $(TEST_CORE_CACHE_TID),--core-cache-dir $(TEST_CORE_CACHE_DIR),)
endif

core-cache-warm: $(TARGET) build
	@if [ -n "$(TEST_CORE_CACHE)" ]; then \
	  mkdir -p $(TEST_CORE_CACHE_DIR); \
	  ./$(TARGET) $(TEST_CORE_CACHE) ../demos/fizzbuzz/main.kai > build/core-cache-warm.c 2> build/core-cache-warm.err \
	    || { echo "core-cache-warm FAIL"; cat build/core-cache-warm.err; exit 1; }; \
	  echo "core-cache-warm OK ($(TEST_CORE_CACHE_DIR))"; \
	else echo "core-cache-warm off (KAI_CORE_CACHE=0)"; fi

TEST_LIGHT_TARGETS := test-infer test-infer-check test-lint test-dump-typed test-dump-mono test-rawsafe test-borrowsafe test-purity test-run test-blocks test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-modules-transitive-privacy test-modules-uom-privacy test-holes test-driver-holes test-effects test-effect-runtime test-link-runtime test-sugars test-pipes test-match test-literals test-string test-loop test-records test-diagnostics test-reader test-writer test-trace test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-protocols test-proto-scalar-dispatch test-bare-narrow-assert test-demos-core test-aspirational test-m4c test-match-backtrack-unify test-unbox-phase2 test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-tco-param-shadow test-runtime-shadow test-issue-501 test-issue-743 test-issue-842 test-foreign-prelude test-issue-672 test-issue-1194-reassigned-record-field-in-interp test-issue-1263-abandon-across-resumed-frame test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-746 test-issue-668 test-trmc-list-build test-region-1123 test-perceus-issue82 test-perceus-issue118 test-perceus-issue298 test-perceus-issue350 test-perceus-trmc-spread test-perceus-issue703 test-issue-779 test-perceus-enum-slot test-perceus-int-cache test-int-field-inline test-perceus-nested-reuse test-perceus-812-tracer test-perceus-817-filter-leak test-perceus-1219-bootstrap-seed test-perceus-1117-map-pipe-record-leak test-perceus-1134-pipe-fusion test-perceus-1134-pipe-fusion-ordering test-perceus-1134-pipe-fusion-terminal test-perceus-1143-pipe-fusion-lambda test-perceus-1143-pipe-fusion-ring test-perceus-1186-map-pipe-terminal-field test-perceus-1120-array-borrow test-perceus-1126-read-loop-drop test-perceus-1135-vec-value test-perceus-1135-vec-raw test-perceus-1151-vec-push-growth test-perceus-1153-modcall-linear test-perceus-1150-vec-surface test-perceus-1127-borrow-closure test-perceus-1127-borrow-hof test-perceus-1127-borrow-tco test-perceus-1131-borrow-descent test-perceus-1131-modular-escape test-perceus-1295-borrow-slot-nested-arg test-perceus-1303-single-use-branch-leak test-perceus-1302-tcrec-goto-drops test-perceus-1315-borrowed-match-release test-perceus-1355-closure-temp-release test-perceus-1328-selftail-borrowed-local test-perceus-1130-borrow-pipe test-perceus-1130-hof-tail-drop test-perceus-1104-balance-token-donation test-perceus-1104-sibling-param-move test-perceus-1136-sized-variant test-perceus-1156-raw-widen test-issue-892-mono-arg-unbox test-unions test-aliases test-sequence test-ufcs test-ffi-extern-c test-ffi-scalar-char test-ffi-v2-struct test-ffi-v2-opaque test-ffi-v2-negatives test-ffi-v2-native-struct test-ffi-cflags-shim test-ffi-cflags-shim-native test-native-modular test-native-core-obj test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-numeric-fixed-width test-numeric-bigint test-numeric-decimal-rational test-numeric-div-by-zero-traps test-numeric-literal-mint test-issue-315-no-double-slash test-issue-248-loop-sugars test-issue-1179-var-slot-loop test-issue-1214-nonliteral-init test-rc-trace-callsite test-tco-rule3-basic test-tco-rule3-no-misfire test-tco-issue-680 test-issue-678-stdout-buffered test-issue-679-cancel-parked test-issue-682-cancel-sibling-handler test-issue-978-spawn-parent-handled-effect test-issue-1051-spawn-parent-handled-builtin-default test-issue-1067-fiber-trap-isolation test-issue-1331-op-arg-release test-issue-1093-divzero-trap test-issue-997-spawn-cell-fiber-local test-violations test-unused-bindings test-attributes test-doc-attr test-shadowing test-modular-build test-modular-identity test-modular-cache test-modular-hermetic test-issue-963-cross-module-mono test-home-tu-partition test-kir test-batch-isolation test-perceus-1180-range-lazy test-perceus-1180-range-sum test-shape-fusion-1200 test-shape-laws-1200

test-light-parallel: $(TARGET) build core-cache-warm
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
test-light-shard: $(TARGET) build core-cache-warm
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
test-fast: test-infer test-infer-check test-lint test-dump-typed test-dump-mono test-rawsafe test-borrowsafe test-purity test-run test-blocks test-modules test-modules-path test-modules-qualified test-modules-qualified-neg test-modules-collision test-modules-derive-export test-modules-transitive-privacy test-modules-uom-privacy test-holes test-driver-holes test-effects test-effect-runtime test-sugars test-pipes test-match test-literals test-string test-loop test-diagnostics test-reader test-writer test-trace test-time test-intervals test-call-elision test-call-elision-neg test-stdlib test-protocols test-proto-scalar-dispatch test-bare-narrow-assert test-demos-core test-aspirational test-tco test-tco-regression test-tco-zero-arg test-tco-309 test-tco-perceus-wrap test-tco-param-shadow test-foreign-prelude test-prelude-qualified-call test-issue-219-modcall test-issue-318 test-issue-746 test-issue-668 test-trmc-list-build test-region-1123 test-issue-1194-reassigned-record-field-in-interp test-issue-779 test-unions test-aliases test-sequence test-ufcs test-ffi-scalar-char test-ffi-v2-struct test-ffi-v2-opaque test-ffi-v2-negatives test-ffi-v2-native-struct test-ffi-cflags-shim test-ffi-cflags-shim-native test-native-modular test-native-core-obj test-ffi-pub-axiom-cross-module test-issue-260-261-extern-c-fn test-numeric-fixed-width test-numeric-bigint test-numeric-decimal-rational test-numeric-div-by-zero-traps test-numeric-literal-mint test-issue-315-no-double-slash test-issue-248-loop-sugars test-issue-1179-var-slot-loop test-issue-1214-nonliteral-init test-tco-rule3-basic test-tco-rule3-no-misfire test-tco-issue-680 test-issue-678-stdout-buffered test-issue-679-cancel-parked test-issue-682-cancel-sibling-handler test-issue-978-spawn-parent-handled-effect test-issue-1051-spawn-parent-handled-builtin-default test-issue-1067-fiber-trap-isolation test-issue-1331-op-arg-release test-issue-1093-divzero-trap test-issue-997-spawn-cell-fiber-local test-unused-bindings test-issue-963-cross-module-mono test-home-tu-partition test-kir test-packages

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
	for f in ../examples/cache/corec_*.sh ../examples/cache/emitc_*.sh; do \
	  sh "$$f" || { echo "core-cache FAIL: $$f"; exit 1; }; \
	done; \
	echo "core-cache OK (post-parse + emit-cache fixtures passed)"

# KIR Lane 0 — KIR dumper goldens + determinism gate. Goldens pin the RAW
# lowering (`--emit=kir-noinline`), decoupled from inline thresholds; the
# determinism gate covers both dump modes.
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
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir-noinline $$f 2>/dev/null \
	    | awk -v fns="$$fns" 'BEGIN{n=split(fns,a," ");for(i=1;i<=n;i++)w[a[i]]=1} /^fn |^lambda fn |^clause fn /{nm="";for(i=1;i<=NF;i++)if($$i ~ /\(/){nm=$$i;sub(/\(.*/,"",nm);break} cur=(nm in w)} cur{print} /^}/{if(cur){print "";cur=0}}' \
	    > build/$$name.kir; \
	  diff -q ../examples/kir/$$name.kir.expected build/$$name.kir > /dev/null \
	    || { echo "kir DIFF $$name"; diff ../examples/kir/$$name.kir.expected build/$$name.kir; exit 1; }; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir $$f 2>/dev/null > build/$$name.kir.run2a; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir $$f 2>/dev/null > build/$$name.kir.run2b; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir-noinline $$f 2>/dev/null > build/$$name.kir.run2c; \
	  ./$(TARGET) $(TEST_CORE_CACHE) --path ../stdlib --emit=kir-noinline $$f 2>/dev/null > build/$$name.kir.run2d; \
	  diff -q build/$$name.kir.run2a build/$$name.kir.run2b > /dev/null \
	    || { echo "kir NON-DETERMINISTIC $$name"; exit 1; }; \
	  diff -q build/$$name.kir.run2c build/$$name.kir.run2d > /dev/null \
	    || { echo "kir NON-DETERMINISTIC (noinline) $$name"; exit 1; }; \
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

# Native self-host gate: compile the compiler (stage2/main.kai) with the
# native backend and count the `unbound register` subset-gap aborts against
# tools/native-selfhost-baseline.txt. FAILS only on regression (count rises);
# the known gap is a documented expected-fail that PASSES. The script relinks
# kaic2 with KAI_LLVM=1 itself and SKIPs (exit 0) where libLLVM is absent —
# native-only, does not touch the C bootstrap. No $(TARGET) prereq: the script
# owns the native relink (mirrors test-native-parity.sh).
test-native-selfhost-gate:
	@CC="$(CC)" LLVM_CONFIG="$(LLVM_CONFIG)" bash ../tools/test-native-selfhost-gate.sh

# RC self-host detector: runs a directed RC corpus under ASAN + no-cell-pool
# and a strict per-tag ledger on BOTH backends, asserting the absolute
# invariant that no cell is freed more than it was allocated. Catches the
# double-free / use-after-free family that the cell pool masks and the C
# oracle shares. The native half needs a KAI_LLVM=1 kaic2 and instruments
# the runtime via the legacy link path; the script SKIPs native where
# libLLVM is absent. Heavy (build x2 x corpus) — cron / RC-path-gated, not a
# per-PR gate. Relinks kaic2 with KAI_LLVM=1 first when libLLVM is present.
rc-detector:
	@if command -v "$(LLVM_CONFIG)" >/dev/null 2>&1; then \
	  rm -f $(TARGET); \
	  LLVM_CONFIG="$(LLVM_CONFIG)" $(MAKE) KAI_LLVM=1 kaic2 >/dev/null 2>&1 \
	    || { echo "rc-detector FAIL — kaic2 (KAI_LLVM=1) build failed"; exit 1; }; \
	fi
	@CC="$(CC)" LLVM_CONFIG="$(LLVM_CONFIG)" bash ../tools/rc-selfhost-detector.sh

clean:
	rm -rf build $(TARGET)
