.PHONY: bench-mn-throughput all kaic0 kaic1 kaic2 kaic2-fast kaic2-fast-verify kaic-boot-verify kaic-boot-verify-one kaic-boot-verify-cross test test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record test-fmt test-fmt-package test-fmt-width test-fmt-ledger test-fmt-selfhost test-fmt-help-scope test-fmt-property test-namespace-matrix test-namespace-matrix-status test-km-ledger test-namespace-classes test-corrective-ratchet test-km-new-files test-migrate test-bench test-check test-typecheck test-check-parity test-library-mode test-lsp test-diagnostics-collected test-native-diag-path test-watch-survives-error test-negative test-stage1-rejections test-stage1-imports test-stage1-homonyms test-stage1-shadow-capture test-stage2-graph test-symtab test-resolve-sym test-symid-survives test-symid-pipeline test-decl-symid-survives test-evar-above-erase test-respelling-confined test-rboxed-prim-scope test-kai-namespace test-native-namespace test-module-name-ident test-private-type-shadow-audit test-runtime-global-audit test-wiring-audit test-perceus-position-audit test-perceus-read-audit test-tls-hoist-gate test-stdlib-modules test-independence-oracle test-packages test-editions test-binserialize-budget test-issue-779-asan demos-verify demos-no-regression selfhost test-arena test-heap-limit test-modular-selfhost test-perceus-1131-modular-escape test-mn-tsan test-mn-determinism test-mn-corpus test-mn-reactor-bench test-mn-idle-cpu test-upgrade-resolver test-release-platforms test-kaic-boot test-native-probe test-cli-flags test-kai-cli test-eval-order-gcc clean warm-core tier0 test-header-deps test-bin-kai-prereq test-llvm-force-guard test-parity-preserve-native tier1 tier1-shard-1 tier1-shard-2 tier1-shard-3 tier1-shard-4 tier1-shard-5 tier1-shard-6 tier1-shard-7 tier1-shard-8 test-light-partition test-core-cache-tid test-doc tier1-asan tier1-asan-a tier1-asan-b tier1-backend-parity daily daily-tail tier1-unsharded coverage-probe rc-budget stress-fixtures test-posix-shell rc-leak-gate test-partition-linearity test-binserialize-linearity test-rc-budget bin/kai test-walker-catchall-audit test-core-full

# A bare kaic2 with no `--edition` runs the OLDEST edition (tongariki),
# so a recipe driving the binary directly would test the previous
# surface while `bin/kai` tests the current one. $(KAIC2) is the binary
# already carrying the flag.
KAI_EDITION  := $(shell cat EDITION)
EDITION_FLAG := --edition $(KAI_EDITION)
KAIC2        := ./stage2/kaic2 $(EDITION_FLAG)

all: kaic1 kaic2 bin/kai

kaic0:
	$(MAKE) -C stage0 kaic0

kaic1: kaic0
	$(MAKE) -C stage1 kaic1

# The kai binary is built with kaic2 so no gate pays for it inside its own timeout.
# Only the kaic1 boot needs the stage0 -> stage1 chain (see KAIC_BOOT in stage2/Makefile).
kaic2: $(if $(filter kaic1,$(KAIC_BOOT)),kaic1)
	$(MAKE) -C stage2 kaic2
	$(MAKE) -s -C tools/kai kai

# Dev fast rebuild — an EXISTING kaic2 recompiles itself modularly
# (no kaic1, no bundle). Deliberately not wired into the bootstrap
# chain: `make kaic2` stays the trust path. See docs/build-system.md.
kaic2-fast:
	$(MAKE) -C stage2 kaic2-fast

kaic2-fast-verify:
	$(MAKE) -C stage2 kaic2-fast-verify

# A release-booted and a seed-booted kaic2 emit the same C. See docs/build-system.md.
kaic-boot-verify:
	$(MAKE) -C stage2 kaic-boot-verify

kaic-boot-verify-one:
	$(MAKE) -C stage2 kaic-boot-verify-one BOOT=$(BOOT)

kaic-boot-verify-cross:
	$(MAKE) -C stage2 kaic-boot-verify-cross

# The kai binary; tools/kai/Makefile decides staleness.
bin/kai:
	$(MAKE) -s -C tools/kai kai

test: test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record

test-stage0:
	$(MAKE) -C stage0 test

test-stage1:
	$(MAKE) -C stage1 test

test-stage2:
	$(MAKE) -C stage2 test

# Build + run + test every phase 4 demo via bin/kai.
test-demos: kaic2
	@set -e; \
	for f in examples/phase4/*.kai; do \
	  name=$$(basename $$f .kai); \
	  ./bin/kai run  $$f > /tmp/kaikai-$$name.out; \
	  ./bin/kai test $$f > /tmp/kaikai-$$name-t.out 2>&1 || true; \
	  echo "demo OK $$name"; \
	done

# issue #233 — `bin/kai run` auto-discovers sibling modules and
# nested directories under the entry file.
#
# Two invocation modes per fixture:
#   abs: from a foreign cwd (`/`) with an absolute source path. Exercises
#        the original PR #236 contract — `--path "$(dirname "$src")"`
#        evaluated to an absolute string.
#   rel: cd into the fixture dir and run `kai run main.kai`. Exercises
#        the user-facing flow from issue #237 — `dirname` returning `.`
#        and the cd+pwd absolutization in `bin/kai`.
test-multi-module: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	for case in multi-module multi-module/issue-237 multi-module/issue-897-shadow multi-module/issue-898-qualified-generic multi-module/issue-962-root-fns-isolation multi-module/pipe_custom_type multi-module/pipe_two_types_same_pkg multi-module/bound_impl_other_module multi-module/orphan_protocol_arg_local; do \
	  src_dir="$$root/examples/$$case"; \
	  exp="$$src_dir/main.out.expected"; \
	  out_abs=$$(mktemp); \
	  (cd / && "$$kai" run "$$src_dir/main.kai") > "$$out_abs" \
	    || { echo "$$case FAIL (abs kai run)"; rm -f "$$out_abs"; exit 1; }; \
	  diff -q "$$exp" "$$out_abs" > /dev/null \
	    || { echo "$$case DIFF (abs)"; diff "$$exp" "$$out_abs"; rm -f "$$out_abs"; exit 1; }; \
	  rm -f "$$out_abs"; \
	  out_rel=$$(mktemp); \
	  (cd "$$src_dir" && "$$kai" run main.kai) > "$$out_rel" \
	    || { echo "$$case FAIL (rel kai run)"; rm -f "$$out_rel"; exit 1; }; \
	  diff -q "$$exp" "$$out_rel" > /dev/null \
	    || { echo "$$case DIFF (rel)"; diff "$$exp" "$$out_rel"; rm -f "$$out_rel"; exit 1; }; \
	  rm -f "$$out_rel"; \
	  echo "$$case OK (abs+rel)"; \
	done

# Issue #279 regression — `kai run` must pass the resolved stdlib root as
# `--path`, not `$$ROOT/stdlib`, which does not exist in an installed
# layout; `import loop` (any non-prelude stdlib import) breaks otherwise.
# Driven through `bin/kai run`, not kaic2, so kai's argv is exercised.
test-import-stdlib: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	src="$$root/examples/imports/import_loop_basic.kai"; \
	exp="$$root/examples/imports/import_loop_basic.out.expected"; \
	out=$$(mktemp); \
	"$$kai" run "$$src" > "$$out" \
	  || { echo "import_loop_basic FAIL (kai run)"; rm -f "$$out"; exit 1; }; \
	diff -q "$$exp" "$$out" > /dev/null \
	  || { echo "import_loop_basic DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	rm -f "$$out"; \
	echo "import_loop_basic OK"

# Issue #425 regression: importing a prelude-loaded module
# (`encoding.toml`, `encoding.json`, `core.list`) must be a no-op
# instead of producing duplicate `kai_<mod>__*` symbols in the C
# output. Drives the fix end-to-end through `bin/kai run` so the
# prelude wiring + the resolver dedup both participate.
test-import-prelude-dedup: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	for case in double_import_prelude_toml double_import_prelude_json double_import_prelude_list; do \
	  src="$$root/examples/imports/$$case.kai"; \
	  exp="$$root/examples/imports/$$case.out.expected"; \
	  out=$$(mktemp); \
	  "$$kai" run "$$src" > "$$out" \
	    || { echo "$$case FAIL (kai run)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "$$case DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "$$case OK"; \
	done

# Issue #456 regression — qualified record literal `mod.Type { ... }`
# parses + resolves end-to-end. Each case lives in its own directory
# (main + 1..2 sibling modules) so the resolver walks the same
# multi-file discovery path the issue's reproducer uses.
test-import-qualified-record: kaic2
	@set -e; \
	root=$$(pwd); \
	kai="$$root/bin/kai"; \
	for case in qualified_record_basic qualified_record_ambiguous qualified_record_with_spread; do \
	  src_dir="$$root/examples/imports/$$case"; \
	  exp="$$src_dir/main.out.expected"; \
	  out=$$(mktemp); \
	  "$$kai" run "$$src_dir/main.kai" > "$$out" \
	    || { echo "$$case FAIL (kai run)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "$$case DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "$$case OK"; \
	done

# Core demo gate. Delegates to stage2's `test-demos-core` which builds
# each demo under examples/portfolio/ and examples/usd_to_eur/ with the
# C backend, redirects stdin from the `.in` fixture when present, and
# diffs against `.out.expected`.
demos-verify: kaic2
	$(MAKE) -C stage2 test-demos-core

# m12.8.x post-Core REOPEN — Eric level 3 gate. Runs the full demo
# probe set under demos/ and fails when the OK + PASS count drops
# below the baseline pinned in `demos/baseline.txt`. Used at every
# milestone close to prevent silent regressions in features the
# Core demo gate (`demos-verify`) does not exercise.
demos-no-regression: kaic2
	$(MAKE) -C demos no-regression

# Perceus RC leak ledger: every examples/perceus fixture's allocation count
# pinned in tools/baselines/rc-leak/. The corpus's other harnesses diff
# stdout, which a leak survives untouched.
rc-leak-gate: kaic2
	$(MAKE) -C stage2 rc-leak-gate

test-rc-budget: kaic2
	tools/rc-budget-gate.sh

# The home-module partition must stay O(decls). A per-decl append that
# copies its bucket spine is quadratic in decls-per-module: invisible on
# the compiler's own sources and dominant on one large generated file.
test-partition-linearity: kaic2
	@./tools/partition-linearity-gate.sh

# A derived BinSerialize encode must grow linearly with the payload.
test-binserialize-linearity: kaic2
	@./tools/binserialize-linearity-gate.sh

# Self-hosting: per-compiler determinism for stage 1 and stage 2.
# Each stage compiles its own source twice; output must match
# byte-for-byte. Stage 1 and stage 2 are NOT required to agree on
# emission shape — see
# docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md.
selfhost:
	$(MAKE) -C stage1 selfhost
	$(MAKE) -C stage2 selfhost

clean:
	$(MAKE) -C stage0 clean
	$(MAKE) -C stage1 clean
	$(MAKE) -C stage2 clean
	$(MAKE) -C tools/kai clean

# ---- testing tiers (see docs/testing-tiers.md) ----------------------
#
# Pre-warm the shared core cache (post-parse blobs + emitted-C core
# TUs) by building a throwaway program once per available backend, so
# the first real build pays no core compile. Lazy warm on first build
# is the fallback; this target is for install / CI-init steps.
warm-core: kaic2
	@tmp=$$(mktemp -d); \
	printf 'fn main() {\n  print("warm")\n}\n' > $$tmp/warm.kai; \
	KAI_MODULAR=1 ./bin/kai build --backend=c $$tmp/warm.kai -o $$tmp/warm-c >/dev/null 2>&1 || true; \
	./bin/kai build $$tmp/warm.kai -o $$tmp/warm-n >/dev/null 2>&1 || true; \
	rm -rf $$tmp; \
	echo "core cache warmed (post-parse blobs + c-modular emit entries + native core object)"

# Tier 0: pre-commit gate. ~30-60s. Every agent / human runs this
# before every commit. If it fails, no commit happens.
tier0: selfhost test-kai-namespace test-native-namespace test-module-name-ident demos-no-regression test-arena test-heap-limit test-evidence-frame test-runtime-global-audit test-wiring-audit test-perceus-position-audit test-perceus-read-audit test-tls-hoist-gate test-timeout-shim test-p2-status test-header-deps test-llvm-force-guard test-parity-preserve-native test-stage1-rejections test-rboxed-prim-scope test-namespace-matrix test-namespace-classes test-corrective-ratchet test-km-new-files test-posix-shell test-selfhost-gate-no-rebuild test-stage1-imports test-stage1-homonyms test-stage1-shadow-capture test-stage2-graph test-symtab test-resolve-sym test-symid-survives test-symid-pipeline test-decl-symid-survives test-evar-above-erase test-respelling-confined test-compile-alloc test-nest-alloc test-core-full test-light-partition test-core-cache-tid test-rc-budget test-native-probe test-bin-kai-prereq test-walker-catchall-audit
	@echo "tier0 OK — selfhost deterministic (kaic2b.c == kaic2c.c), emitted kai_* confined to the runtime namespace on both backends, a non-identifier basename still mints valid C symbols, demos baseline holds, arena gate passes, heap ceiling contains, evidence-frame gate holds, runtime globals classified, no thread-local escapes into an inlinable hot-bitcode function, timeout shim honours its exit-code contract, P2 status distinguishes its three states, header prerequisites declared, forced KAI_LLVM=1 without llvm-config stops loud, the parity gate cannot silently downgrade a native tree, kaic1 rejects its negative fixtures, kaic1 resolves imports across a multi-module package, two modules declaring one function name mint two C symbols, a symbol's owner is a module symbol's id, unbox, perceus and KPerform read the resolved ids, every stage2 module is reachable from main.kai, RC-string prims keep their shared-let binders in Perceus scope, #!/bin/sh scripts parse under dash, the native self-host gate consumes the published kaic2 instead of rebuilding it, the compiler allocates within its ceiling compiling a fixed program, the whole core types and emits, a KAI_LLVM=1 kaic2 that cannot emit native is not left installed, every target that runs bin/kai depends on what builds it, no new hand-rolled ExprKind walker closed by a catch-all"

# tier0 minus the selfhost. The selfhost is two whole compiler generations
# and dominates tier0's wall clock; the remaining gates are seconds. CI runs
# the two as separate jobs so a broken gate reports in a couple of minutes
# instead of waiting behind the selfhost, and the two run concurrently
# rather than in series. Locally `tier0` stays the single pre-commit gate.
tier0-gates: test-kai-namespace test-native-namespace test-module-name-ident demos-no-regression test-arena test-heap-limit test-evidence-frame test-runtime-global-audit test-wiring-audit test-perceus-position-audit test-perceus-read-audit test-tls-hoist-gate test-timeout-shim test-p2-status test-header-deps test-llvm-force-guard test-parity-preserve-native test-stage1-rejections test-rboxed-prim-scope test-namespace-matrix test-namespace-classes test-corrective-ratchet test-km-new-files test-posix-shell test-selfhost-gate-no-rebuild test-stage1-imports test-stage1-homonyms test-stage1-shadow-capture test-stage2-graph test-symtab test-resolve-sym test-symid-survives test-symid-pipeline test-decl-symid-survives test-evar-above-erase test-respelling-confined test-compile-alloc test-nest-alloc test-core-full test-light-partition test-core-cache-tid test-native-probe test-bin-kai-prereq test-walker-catchall-audit
	@echo "tier0-gates OK — every tier0 gate except the selfhost"

# A ceiling on the cells the compiler allocates compiling a fixed
# generated program: the shape it guards is a pass accumulating into a
# list inside a walk over the program, which emits correct C and only
# costs more doing it. SKIPs without a built kaic2.
test-compile-alloc:
	@./tools/compile-alloc-gate.sh

# Ceilings on the cells the typer allocates checking list literals and
# lambdas nested as deep as the parser allows: re-applying the substitution
# to a whole type at each level makes them superlinear in the depth.
test-nest-alloc:
	@./tools/nest-alloc-gate.sh

# A build compiles only the core functions the program reaches; this
# keeps the rest typed and emitted on every commit.
test-core-full:
	@./tools/core-full-gate.sh

# The native half of the namespace gate lives in stage2 (it needs $(TARGET));
# SKIPs without libLLVM, so a C-only tier0 run stays green.
test-native-namespace:
	@$(MAKE) -C stage2 test-native-namespace

# kaic1 must reject every `examples/negative/stage1_rejections/*.kai`
# with the diagnostic its `.kaic1.err.expected` pins. Bootstrap-only
# (kaic1, no kaic2), ~0.1s.
test-stage1-rejections: kaic1
	@./tools/test-stage1-rejections.sh

# kaic1 resolves `import` and compiles a package whose modules place a
# lambda at the same (line, column). Bootstrap-only (kaic1, no kaic2).
test-stage1-imports: kaic1
	@./tools/test-stage1-imports.sh

# Two modules declaring one function name: keyed on the declaring module
# the two mint distinct C symbols, so the package links and each caller
# reaches its own. Bootstrap-only (kaic1, no kaic2).
test-stage1-homonyms: kaic1
	@./tools/test-stage1-homonyms.sh

# A lambda reading a local named like a global captures the local.
# Bootstrap-only (kaic1, no kaic2).
test-stage1-shadow-capture: kaic1
	@./tools/test-stage1-shadow-capture.sh

# The import graph decides stage 2's compilation order, so an unreachable
# module is silently absent from the compiler rather than a build error.
test-stage2-graph:
	@./tools/test-stage2-graph.sh

# A symbol's owner is a module symbol's id: two homonymous modules are
# two ids, and resolution decides between their declarations by id.
test-symtab: kaic2
	@./stage2/kaic2 --symtab-selftest

# The resolver's use table must be filled by the real walk, not merely
# be constructible: the gate fails if a call to a declaration records as
# a local, which is what a table nothing consults looks like.
test-resolve-sym: kaic2
	@./stage2/kaic2 --resolve-sym-selftest

test-symid-survives:
	@./tools/symid-survives-gate.sh

# Unbox and Perceus run above the erasure and `KPerform` carries the
# effect it performs: a dump of the ids each one reads, decoded against
# the symbol table, is the only thing that fails when one is dropped.
test-symid-pipeline: kaic2
	@./tools/symid-pipeline-gate.sh

# The same property for the declaration forms: a pass that rewrites a
# body must carry the identity the resolver stamped, not restamp the slot
# empty. Dropping it is invisible to every tier — the name still resolves
# to something — which is why it takes a gate rather than a fixture.
test-decl-symid-survives:
	@./tools/decl-symid-survives-gate.sh

# The same property for value references: between the resolver and the
# erasure a reference names a declaration, so it carries that
# declaration's id. Below the erasure a name is a C symbol and the bare
# form is correct, which is why this is a window and not a ban.
test-evar-above-erase:
	@./tools/evar-above-erase-gate.sh

# The respelling is a stand-in for identity, so it must not spread while
# it is being retired. Effects stay spelled — the runtime dispatches an
# effect by its bare label, so that spelling is ABI — but the type half
# retires once `TyCon` carries a `SymId` instead of a name string.
test-respelling-confined:
	@./tools/respelling-confined-gate.sh

# A prim whose forwarder returns an RC-managed String (llvm_backend_tag,
# RBoxed in stage 1's rprelude) must keep its shared-let binder in Perceus
# scope: every consuming read carries a dup. Misclassified as a raw handle,
# the binder is Perceus-exempt, the first consuming use frees the value and
# a later read dangles — silently, as an empty string. Asserts the emitted
# C of BOTH emitters (kaic1 and self-hosted kaic2), positive and negative:
# the dup-wrapped comparator read must exist, the raw one must not.
test-rboxed-prim-scope: kaic1 kaic2
	@set -e; \
	tmp=$$(mktemp -d); trap "rm -rf $$tmp" EXIT; \
	./stage1/kaic1 examples/perceus/rboxed_prim_shared_let_1648.kai > $$tmp/s1.c; \
	grep -q 'kai_op_ne_v(kai_internal_dup(kai_ntag)' $$tmp/s1.c \
	  || { echo "rboxed-prim FAIL kaic1: comparator read of the llvm_backend_tag binder lost its dup"; exit 1; }; \
	if grep -q 'kai_op_ne_v(kai_ntag,' $$tmp/s1.c; then \
	  echo "rboxed-prim FAIL kaic1: raw (Perceus-exempt) llvm_backend_tag binder in a consuming slot"; exit 1; \
	fi; \
	$(KAIC2) --path stdlib examples/perceus/rboxed_prim_shared_let_1648.kai > $$tmp/s2.c; \
	grep -q 'kai_op_ne_v(kai_internal_dup(kaiv_ntag)' $$tmp/s2.c \
	  || { echo "rboxed-prim FAIL kaic2: comparator read of the llvm_backend_tag binder lost its dup"; exit 1; }; \
	if grep -q 'kai_op_ne_v(kaiv_ntag,' $$tmp/s2.c; then \
	  echo "rboxed-prim FAIL kaic2: raw llvm_backend_tag binder in a consuming slot"; exit 1; \
	fi; \
	echo "rboxed-prim OK — llvm_backend_tag shared let stays in Perceus scope (both emitters)"

# C-emission namespace gate. User identifiers ride their own namespaces
# (`kaiu_` fns, `kaiv_` locals), so every `kai_*` token in kaic2-emitted C
# must be a name runtime.h itself contains. A binder that leaks into `kai_*`
# with a runtime helper's name breaks the selfhost cc build; this catches
# the rest of the class — any leak, colliding or not — over the compiler's
# own thousands of binders. The reverse check keeps future runtime helpers
# out of the user namespaces, so no waived case can silently reopen.
# String literals are stripped first: the compiler's own strings name the
# kai_* symbols it emits, which would read as false leaks. The same sweep
# runs over a fixture whose user fn is spelled like a runtime helper.
test-kai-namespace: kaic2
	@set -e; \
	tmp=$$(mktemp -d); trap "rm -rf $$tmp" EXIT; \
	( cd stage2 && ./kaic2 $(EDITION_FLAG) main.kai ) > $$tmp/self.c; \
	perl -pe 's/"(?:[^"\\]|\\.)*"//g' $$tmp/self.c \
	  | tr -c 'A-Za-z0-9_' '\n' | grep -E '^kai_' | sort -u > $$tmp/emitted.txt; \
	tr -c 'A-Za-z0-9_' '\n' < stage2/runtime.h | grep -E '^kai_' | sort -u > $$tmp/runtime.txt; \
	leaks=$$(comm -23 $$tmp/emitted.txt $$tmp/runtime.txt); \
	[ -z "$$leaks" ] || { echo "test-kai-namespace FAIL: user-derived identifiers emitted into the runtime kai_ namespace:"; echo "$$leaks"; exit 1; }; \
	rev=$$(tr -c 'A-Za-z0-9_' '\n' < stage2/runtime.h | grep -E '^(kaiu_|kaiv_)' | sort -u); \
	[ -z "$$rev" ] || { echo "test-kai-namespace FAIL: runtime.h names symbols inside the user namespaces:"; echo "$$rev"; exit 1; }; \
	fx=examples/namespace-collisions/adv_name_internal_prefix; \
	( cd stage2 && ./kaic2 $(EDITION_FLAG) ../$$fx/main.kai ) > $$tmp/adv.c; \
	perl -pe 's/"(?:[^"\\]|\\.)*"//g' $$tmp/adv.c \
	  | tr -c 'A-Za-z0-9_' '\n' | grep -E '^kai_' | sort -u > $$tmp/adv-emitted.txt; \
	leaks=$$(comm -23 $$tmp/adv-emitted.txt $$tmp/runtime.txt); \
	[ -z "$$leaks" ] || { echo "test-kai-namespace FAIL: a user fn named after a runtime helper reached the kai_ namespace:"; echo "$$leaks"; exit 1; }; \
	grep -q 'kaiu_ha__kai_internal_drop' $$tmp/adv.c \
	  || { echo "test-kai-namespace FAIL: $$fx did not emit ha.kai_internal_drop as kaiu_ha__kai_internal_drop"; exit 1; }; \
	echo "test-kai-namespace OK — emitted kai_* stays inside runtime.h; runtime stays out of kaiu_/kaiv_; user names shaped like runtime helpers stay in kaiu_"

# A root file whose basename is not a C identifier still mints valid symbols.
# Both backends, since the name reaches C emission and KIR lowering alike.
test-module-name-ident: kaic2
	@./tools/test-module-name-ident.sh

# A header-only edit must rebuild every artifact that embeds it, and must not
# rebuild the ones that do not. Both directions are invisible to CI, which
# always builds from scratch. Static query of the make database — no recipe
# runs, no kaic2 dependency, ~0.1s.
test-header-deps:
	@bash tools/test-header-deps.sh

# A target that runs bin/kai but depends only on kaic2 passes wherever an
# earlier build left bin/kai behind and fails on a clean runner. Pure text.
test-bin-kai-prereq:
	@./tools/audit-bin-kai-prereq.py --self-test
	@./tools/audit-bin-kai-prereq.py

# Every #!/bin/sh script must parse under dash, not just macOS's
# bash-in-POSIX-mode sh — CI runs these under dash and a bashism dies
# at parse time there while local sh -n stays silent. Seconds, no
# kaic2 dependency.
test-posix-shell:
	@./tools/test-posix-shell.sh

# Forcing KAI_LLVM=1 with an unresolvable llvm-config must stop at parse
# time with the actionable error, never hand -DKAI_LLVM to cc without the
# LLVM include dirs. Make-level, no kaic2 dependency, ~0.1s.
test-llvm-force-guard:
	@out=$$($(MAKE) -s -C stage2 KAI_LLVM=1 LLVM_CONFIG=/nonexistent/llvm-config kaic2 2>&1); rc=$$?; \
	[ $$rc -ne 0 ] || { echo "llvm-force-guard FAIL — forced KAI_LLVM=1 without llvm-config did not stop"; exit 1; }; \
	echo "$$out" | grep -q "was not found" || { echo "llvm-force-guard FAIL — stop lacked the actionable error:"; echo "$$out"; exit 1; }; \
	echo "llvm-force-guard OK — forced KAI_LLVM=1 without llvm-config stops with the actionable error"

# `tier1-backend-parity`'s rebuild must never LOWER the tree's backend
# capability: a keg-only llvm-config (the mac default) makes stage2's
# auto-detect answer C-only, and the harness then SKIPs with exit 0 over a
# native backend the same invocation just relinked away. Hermetic sandbox,
# seconds, no kaic2 dependency.
test-parity-preserve-native:
	@bash tools/test-parity-preserve-native.sh

# The native self-host gate must run the kaic2 build-native published, not
# rebuild it: the extra `cc -O2` pass shared a runner with a multi-GB
# self-compile. Hermetic, no kaic2 dependency, milliseconds.
test-selfhost-gate-no-rebuild:
	@bash tools/test-selfhost-gate-no-rebuild.sh

# Exit-code contract of the bounded-run shim every M:N gate classifies hangs
# with: deadline -> 124, child that survived SIGTERM -> 137. Seconds, no
# kaic2 dependency, and it covers both backing implementations (coreutils on
# Linux, perl on macOS).
test-timeout-shim:
	@bash tools/test-timeout-shim.sh

# `gen-runtime-bc.sh --status` must tell "no clang 18 here" apart from
# "clang 18 is here, the bitcode was never generated": the second is one
# command from active, and reporting it as a plain opt-out is what let a
# ~2x slower native compiler pass for a codegen defect. Hermetic, seconds,
# no kaic2 dependency.
test-p2-status:
	@bash tools/test-p2-status.sh

# A KAI_LLVM=1 link keeps kaic2 only if it emits a native object. Hermetic,
# fake compilers, no kaic2 dependency, milliseconds.
test-native-probe:
	@bash tools/test-native-probe.sh

# #820 — KAI_EVIDENCE_FRAME_ONLY gate. A user effect's named instance must
# resolve through its capability slot, never the by-name walk (retained only for
# fiber-local + Ffi builtins). Binary grep-oracle over the emitted C.
test-evidence-frame: kaic2
	@bash tools/evidence-frame-gate.sh

# issue #120 — opt-in Perceus regions: P0 runtime arena gate. Plain +
# ASAN build of the C-level bump-arena fixture. Fast (~1s), no kaic2
# dependency, so it runs in tier0 and again under tier1-asan.
test-arena:
	@echo "== test-arena: bump-arena runtime gate (plain + ASAN) =="
	@bash tools/run-arena-c-fixture.sh

# issue #878 — KAI_MAX_HEAP host-safety gate. A bounded-growth fixture
# aborts clean under a low cap and completes under a high/unset cap.
# Host-safe: every run is wrapped in `timeout` inside the stage2 target.
test-heap-limit: kaic2
	@$(MAKE) -C stage2 test-heap-limit

# Issue #1012 — the whole compiler links under --emit=c-modular (shared RC
# free-list pools). Builds stage2/main.kai through kai's KAI_MODULAR
# path and smokes --version; a regression of the shared-pool guard trips it
# at link time (multi-GB .bss). Costly (a full modular self-compile), so it
# rides tier1-shard-1 with the other self-compiles, not the light pool.
test-modular-selfhost: kaic2
	@$(MAKE) -C stage2 test-modular-selfhost

# Issue #1131 — sep-comp regression gate for the relaxed read-path borrow.
# The modular-built compiler compiles a `#[derive(Show)]`-over-unit fixture
# (the inspect-then-escape shape) byte-identically to single-TU; the reverted
# inference UAF'd here. A full modular self-compile like test-modular-selfhost,
# so it rides tier1-shard-1 with the other self-compiles, not the light pool.
test-perceus-1131-modular-escape: kaic2
	@$(MAKE) -C stage2 test-perceus-1131-modular-escape

# Issue #1207 F1 — M:N scheduler ThreadSanitizer + determinism gate. Builds
# the cross-thread concurrency fixture with -fsanitize=thread and runs it at
# KAI_THREADS=4: zero data races AND N=1==N=4 output. TSAN is slow, so this
# rides a dedicated CI job (tier1-tsan in tier1.yml), never TEST_LIGHT.
test-mn-tsan: kaic2
	@bash tools/run-mn-tsan.sh

# Issue #1207 F1 — M:N determinism-only gate (no TSAN, fast). The multi-actor
# bench and the cross-thread copy stress print the same total at N=1 and N=4;
# a scheduling-order divergence trips it. Cheap enough for tier1.
test-mn-determinism: kaic2
	@bash tools/run-mn-determinism.sh

# Deadlock-banner determinism: the wedged-scheduler report is claimed by one
# worker, so a deadlocking fixture prints exactly one banner and exits 1 on
# every run. Repetition IS the gate — a race passes a single run by luck.
test-mn-deadlock-banner: kaic2
	@bash tools/run-mn-deadlock-banner.sh

# M:N corpus determinism: the whole fixture corpus at N=1 vs N>1, both
# backends. Deliberately NOT in `tier1` — it is the heaviest gate in the
# repo and owns its own CI jobs (.github/workflows/tier1-mn-corpus.yml,
# called from tier1-native.yml), where it shards across runners. This target is the local entry point;
# scope it with MN_CORPUS_DIRS while iterating. A C-only kaic2 drops the
# native arm with a loud line rather than silently halving the coverage.
test-mn-corpus: kaic2
	@bash tools/run-mn-corpus-determinism.sh

# F2 no-starvation regression gate: a sleeper on the reactor timer wheel
# wakes on time while CPU hogs occupy the other scheduler threads. Fails on
# the F1 inline reactor (the poll is pinned to thread 0), passes once the
# reactor runs on its own thread. Timing-based, so it rides the dedicated
# concurrency job (tier1-tsan in tier1.yml), not the fast tier1 path.
test-mn-reactor-bench: kaic2
	@bash tools/run-mn-reactor-bench.sh

# Idle-worker CPU gate: a program whose fibers all sleep must leave the M:N
# workers blocked, not polling. Measures the process's CPU time against a
# fraction of its wall-clock; an output-only check cannot see a busy idle
# loop. Timing-based, so it rides tier1-tsan beside the reactor bench.
test-mn-idle-cpu: kaic2
	@bash tools/run-mn-idle-cpu.sh

# M:N throughput comparison against the BEAM (issue #1207 step 5). Prints
# the kaikai KAI_THREADS=1..16 scaling curve beside Elixir's, for the same
# multi-actor workload. REPORTED, NEVER A GATE: wall-clock on a developer
# box is noisy, and a perf regression should be visible without blocking
# merges on host noise. Skips the BEAM columns when elixirc is absent.
bench-mn-throughput: kaic2
	@bash benchmarks/mn-throughput/run.sh

# Tier 1: pre-PR gate. ~2-4 min. Run before opening / merging a PR.
# PR description should include the trailing line of this output (or
# a CI link) — without it, the merge does not happen.
tier1: test rc-leak-gate test-partition-linearity test-binserialize-linearity demos-no-regression test-fmt test-fmt-package test-fmt-width test-fmt-selfhost test-fmt-help-scope test-fmt-property test-migrate test-bench test-check test-typecheck test-check-parity test-library-mode test-lsp test-diagnostics-collected test-native-diag-path test-watch-survives-error test-negative test-stdlib-modules test-core-text test-http-redirects test-independence-oracle test-packages test-editions test-modular-selfhost test-perceus-1131-modular-escape test-private-type-shadow-audit test-private-record-shadow-audit test-canonical-aliases test-runtime-global-audit test-mn-determinism test-info test-doc test-upgrade-resolver test-release-platforms test-kaic-boot test-cli-flags test-kai-cli
	@echo "tier1 OK — full make test + demos baseline + fmt fixtures + fmt self-hosting ratchet (issue #786) + bench smoke + check smoke + library-mode probes + diagnostics-collected fixtures + negative-space fixtures + stdlib modules compile clean + independence oracle (#962 soundness gate) + package-mode harness (issue #569) + whole-compiler c-modular link (issue #1012) + private-type shadow audit + private-record shadow audit + canonical-only alias audit + M:N determinism (N=1==N=4) + kai info smoke + kai doc smoke + Perceus RC leak ledger (240 fixtures pinned)"

# CI sharding (docs/ci-time-analysis.md §7). The tier1 work is split across
# SEPARATE runners (each with its own memory bus — in-job `-j` is
# bandwidth-capped). These shards PARTITION the `tier1` work: every phase of
# `tier1` above appears in exactly one shard, so the union is the full gate
# with identical coverage. The CI workflow runs them in parallel on a shared
# pre-built kaic2 and an aggregator job (`tier1`) gates on all of them — the
# Required check name is unchanged.
#
# Every shard carries one light slice. The slices are planned by measured
# cost (tools/tier1-light-plan.sh), and each shard's fixed work below is its
# `base` in tools/tier1-light-costs.txt, so the planner packs the light pool
# around it. Each shard prints `tier1-shard-wall <shard> <seconds>` when it
# finishes; tools/tier1-light-costs-refresh.sh reads the bases back from
# those lines, so moving a phase between shards needs one refresh.
#
#  shard 1 — the 4 GB self-compiles + stateful caches (memory-bound), and
#            the second half of the Perceus RC leak ledger.
#  shard 2 — the namespace-collision corpus (C axes but the C-modular two).
#  shard 3 — partition + BinSerialize linearity, core text, HTTP redirects,
#            fmt meaning-preservation corpus part 1/3, and the two C-modular
#            axes of the namespace-collision corpus.
#  shard 4 — the whole-compiler modular self-host.
#  shard 5 — fmt fixtures + fmt self-hosting, packages, editions, kai info.
#  shard 6 — the modular-escape gate, fmt meaning-preservation part 2/3.
#  shard 7 — the CLI/tooling tail.
#  shard 8 — fmt meaning-preservation part 3/3 + the demos baseline.
#
# The Perceus RC leak ledger is split in two (KAI_LEAK_SHARD): the tier0 CI
# job runs the first half after its gates, shard 1 the second. It is not
# part of `tier0` locally.
#
# The two modular self-hosts (shards 4 and 6) only detect regressions in the
# sources they compile, so CI passes TIER1_SELFHOSTS=0 on PRs that touch no
# compiler source. The light slices on those shards run either way. The
# plan reads the flag (shards 4 and 6 carry less fixed work without their
# self-hosts), so CI passes the same value to every shard: shards that saw
# different values would compute different slices.
#
# Coverage invariant (do not break): the set
#   { test-costly-parallel, test-heap-limit, test-user-cache,
#     test-core-cache, test-modular-selfhost, test-perceus-1131-modular-escape,
#     light(1/8) .. light(8/8), test-fmt-property, demos-no-regression,
#     test-fmt, test-fmt-package, test-fmt-width, test-fmt-selfhost,
#     test-fmt-help-scope, test-bench, test-check, test-typecheck,
#     test-check-parity, test-library-mode, test-lsp,
#     test-diagnostics-collected, test-native-diag-path,
#     test-watch-survives-error, test-negative, test-stdlib-modules,
#     test-independence-oracle, test-packages, test-editions,
#     test-private-type-shadow-audit, test-private-record-shadow-audit,
#     test-canonical-aliases, test-info, test-doc, test-upgrade-resolver,
#     test-release-platforms, test-kaic-boot, test-cli-flags, test-kai-cli,
#     test-partition-linearity, test-binserialize-linearity,
#     test-core-text, test-http-redirects }
# plus both halves of rc-leak-gate cover the prerequisites of `tier1` (the
# light slices union to TEST_LIGHT_TARGETS, asserted by
# `test-light-partition` in tier0).
# Adding a phase to `tier1` means adding it to a shard.
TIER1_LIGHT_SLICES := 8
TIER1_SELFHOSTS ?= 1
tier1-light-slice = $(MAKE) -C stage2 test-light-shard SHARD=$(1) SHARDS=$(TIER1_LIGHT_SLICES) TIER1_SELFHOSTS=$(TIER1_SELFHOSTS)
tier1-shard-start = @date +%s > stage2/build/tier1-shard.start
tier1-shard-wall = @echo "tier1-shard-wall $(1) $$(( $$(date +%s) - $$(cat stage2/build/tier1-shard.start) )) selfhosts=$(TIER1_SELFHOSTS)"
TIER1_CLI_TAIL := test-bench test-check test-typecheck test-check-parity test-library-mode test-lsp test-diagnostics-collected test-native-diag-path test-watch-survives-error test-negative test-stdlib-modules test-private-type-shadow-audit test-private-record-shadow-audit test-canonical-aliases test-doc test-upgrade-resolver test-release-platforms test-kaic-boot test-cli-flags test-kai-cli

test-core-cache-tid: kaic2
	$(MAKE) -C stage2 test-core-cache-tid

test-light-partition:
	$(MAKE) -C stage2 test-light-partition SHARDS=$(TIER1_LIGHT_SLICES)

tier1-shard-1: kaic2
	$(tier1-shard-start)
	$(MAKE) -C stage2 test-costly-parallel
	$(MAKE) -C stage2 test-heap-limit
	$(MAKE) -C stage2 test-user-cache
	$(MAKE) -C stage2 test-core-cache
	KAI_LEAK_SHARD=2/2 $(MAKE) rc-leak-gate
	$(call tier1-light-slice,1)
	$(call tier1-shard-wall,1)
	@echo "tier1-shard-1 OK — costly self-compiles + caches + light slice 1/$(TIER1_LIGHT_SLICES)"

# The matrix gate runs right after the C-only namespace-collision axes so it
# reads their ratchet status (tests/namespace_matrix.sh refuses green over a
# red axis). The C-modular axes run on shard 3; each fails its own target
# when red, and the matrix gate reads only the statuses on its runner.
tier1-shard-2: kaic2
	$(tier1-shard-start)
	$(call tier1-light-slice,2)
	$(MAKE) -C stage2 test-namespace-collisions-c-axes
	$(MAKE) test-namespace-matrix
	$(call tier1-shard-wall,2)
	@echo "tier1-shard-2 OK — light slice 2/$(TIER1_LIGHT_SLICES) + namespace-collision corpus (C axes) under ratchet"

tier1-shard-3: kaic2
	$(tier1-shard-start)
	$(call tier1-light-slice,3)
	$(MAKE) test-partition-linearity test-binserialize-linearity test-core-text test-http-redirects
	$(MAKE) test-fmt-property FMT_PROPERTY_SHARD=1/3
	$(MAKE) -C stage2 test-namespace-collisions-modular-axes
	$(call tier1-shard-wall,3)
	@echo "tier1-shard-3 OK — light slice 3/$(TIER1_LIGHT_SLICES) + partition + BinSerialize linearity + core text contracts + fmt meaning-preservation (corpus part 1/3) + namespace-collision C-modular axes"

# The two modular self-hosts sit in separate shards: each rebuilds the whole
# compiler, and a PR touching stage2/compiler/** misses the warm cache by
# construction, so the pair does not fit one job's budget.
tier1-shard-4: kaic2
	$(tier1-shard-start)
ifneq ($(TIER1_SELFHOSTS),0)
	$(MAKE) -C stage2 test-modular-selfhost
endif
	$(call tier1-light-slice,4)
	$(call tier1-shard-wall,4)
	@echo "tier1-shard-4 OK — whole-compiler c-modular link (TIER1_SELFHOSTS=$(TIER1_SELFHOSTS)) + light slice 4/$(TIER1_LIGHT_SLICES)"

tier1-shard-5: kaic2
	$(tier1-shard-start)
	$(MAKE) test-fmt test-fmt-package test-fmt-width test-fmt-selfhost test-fmt-help-scope test-independence-oracle test-packages test-editions test-info
	$(call tier1-light-slice,5)
	$(call tier1-shard-wall,5)
	@echo "tier1-shard-5 OK — fmt fixtures + fmt self-hosting + packages + editions + kai info + light slice 5/$(TIER1_LIGHT_SLICES)"

tier1-shard-6: kaic2
	$(tier1-shard-start)
ifneq ($(TIER1_SELFHOSTS),0)
	$(MAKE) -C stage2 test-perceus-1131-modular-escape
endif
	$(MAKE) test-fmt-property FMT_PROPERTY_SHARD=2/3
	$(call tier1-light-slice,6)
	$(call tier1-shard-wall,6)
	@echo "tier1-shard-6 OK — modular-escape gate (TIER1_SELFHOSTS=$(TIER1_SELFHOSTS)) + fmt meaning-preservation (corpus part 2/3) + light slice 6/$(TIER1_LIGHT_SLICES)"

tier1-shard-7: kaic2
	$(tier1-shard-start)
	$(MAKE) $(TIER1_CLI_TAIL)
	$(call tier1-light-slice,7)
	$(call tier1-shard-wall,7)
	@echo "tier1-shard-7 OK — CLI/tooling tail + light slice 7/$(TIER1_LIGHT_SLICES)"

tier1-shard-8: kaic2
	$(tier1-shard-start)
	$(MAKE) test-fmt-property FMT_PROPERTY_SHARD=3/3
	$(MAKE) demos-no-regression
	$(call tier1-light-slice,8)
	$(call tier1-shard-wall,8)
	@echo "tier1-shard-8 OK — fmt meaning-preservation (corpus part 3/3) + demos baseline + light slice 8/$(TIER1_LIGHT_SLICES)"

# `kai info` smoke (no kaic2 required; pure shell + awk + python3 for
# JSON validation). Guards against deleted .md, broken cmd_info
# dispatcher, JSON-escape regressions in the awk converter.
# Also runs the fenced-code-block compile audit so every example in
# `docs/info/*.md` and `docs/grammar.md` is guaranteed to type-check
# against the current stage 2 compiler — `kai info` + grammar.md are
# the two LLM-authoritative surface references (CLAUDE.md Tier 2 #5
# + Tier 3 #8).
test-info: kaic2
	@tools/test-info.sh
	@tools/test-info-blocks.sh
	@tools/test-grammar-blocks.sh

# `kai doc` smoke (pure shell + awk + python3 for JSON validation).
# Guards the human reader over the stdlib #[doc(...)] attributes and
# the `kai --help` heredoc against the #807 unescaped-backtick
# regression. Needs kaic2 (the extractor) and bin/kai.
test-doc: kaic2
	@tools/test-doc.sh

# `kai upgrade` / install.sh tag resolver (pure shell, no compiler).
# Guards tag-based version discovery, rate-limit handling, and the
# minified-JSON greedy-sed trap against regression.
test-upgrade-resolver:
	@tools/test-upgrade-resolver.sh

# Driver flag hygiene: unknown flags fail fast in every subcommand
# instead of becoming a source path or being silently ignored; also
# pins `kai check`'s C-only --backend contract. Needs kaic2 for the
# one positive `kai check --backend=c` case.
test-cli-flags: kaic2
	@tools/test-cli-flags.sh

# The kai binary (tools/kai): `kai env`, dispatch of an unknown verb to a
# `kai-<verb>` plugin on PATH, and prefix resolution in the installed layout.
test-kai-cli: kaic2
	@tools/test-kai-cli.sh
	@tests/kai_test_jobs.sh
	@tests/guard_memo.sh
	@tests/bin_memo.sh
	@tests/run_memo.sh

# Operand evaluation order under gcc on x86_64, which evaluates call
# arguments right to left. The script refuses any other host.
test-eval-order-gcc: kaic2
	@tools/test-eval-order-gcc.sh

# Release platform tokens (pure shell, no compiler). The release matrix
# names the tarball; install.sh and `kai upgrade` rebuild that name from
# uname to fetch it. A one-character disagreement ships a release whose
# every download 404s, so the correspondence is asserted, not assumed.
test-release-platforms:
	@tools/test-release-platforms.sh

# KAIC_BOOT resolution and the stage2.c identity record (pure shell, no
# compiler, no network: stub boots and a file:// release). The boot must run
# only when make builds: a recipe line naming $$(MAKE) runs even under -n/-q.
# The link of a boot's C rides along: its object cache, with the real cc.
test-kaic-boot:
	@tools/test-kaic-boot.sh
	@tools/test-kaic-link.sh
	@out=$$($(MAKE) -n -C stage2 kaic2 KAIC_BOOT=/nonexistent/kaic2 2>&1); \
	case "$$out" in *"kaic-boot: "*) echo "test-kaic-boot: FAIL: make -n ran the boot:"; echo "$$out"; exit 1 ;; esac; \
	echo "test-kaic-boot: ok — make -n prints the boot, never runs it"

# Issue #643 — institutional regression gate for the private-type
# leak fix. `tools/audit-prelude-private-types.sh` walks every
# non-`pub type` declaration in `stdlib/` and verifies a minimal
# user program that redeclares the same name still compiles. The
# arity-aware variant walker discriminates diff-arity collisions
# (the canonical `Tree` book ch5 case); same-arity collisions
# (`SplitN`, `NB_FragR`) remain a documented limitation in the
# audit script's skip-list and are tracked as a follow-up.
test-private-type-shadow-audit: kaic2
	@./tools/audit-prelude-private-types.sh

# Issue #648 — record-side companion to the audit above. Walks every
# non-`pub type X = { ... }` record declaration in `stdlib/` and
# verifies a minimal user program that redeclares `X` with a
# distinct field set still compiles. Failure means the typer's
# record-table walker (`rec_find_with_field`, issue #648) regressed
# and leaked the prelude's field set into the user's scope.
test-private-record-shadow-audit: kaic2
	@./tools/audit-prelude-private-records.sh

# Option E canonical-name audit (m14 follow-up, closed 2026-05-17).
# Walks every m14 follow-up module and confirms the canonical
# `<module>.<op>` surface exists with no surviving `<prefix>_<op>`
# legacy aliases. The qualified-call resolver's legacy-prefix
# fallback was retired together with this gate flip.
test-canonical-aliases:
	@./tools/audit-canonical-aliases.sh | tail -6

# M:N scheduler §1 — static classification gate for the runtime's mutable
# file-scope globals. Every global in stage2/runtime.h, stage0/runtime_llvm.c,
# and stage0/runtime.h must appear in tools/runtime-globals.allow with its
# partition class (tls / immutable / immortal / shared-locked / reactor-owned /
# scratch). A new unclassified global fails the build, keeping the TLS
# partition total by construction. `--self-test` proves the gate can fail.
# Pure shell, no compiler — runs fast, gates a source property.
test-runtime-global-audit:
	@./tools/runtime-global-audit.sh --self-test

# Structural coverage guard: every test-* target and fixture directory
# must be reachable from a tier — a defined-but-never-run gate reads as
# coverage while its corpus rots. Pure text, no compiler.
test-wiring-audit:
	@./tools/audit-test-wiring.sh

# Ownership walkers descend through expr_positions.kai instead of
# enumerating ExprKind by hand; the baseline is the ratcheted list of
# walkers not yet migrated. Pure text, no compiler.
test-perceus-position-audit:
	@./tools/perceus-position-audit.sh

# One read table per declaration, built by perceus_reads.kai: its entry
# shape stays private to that file and its callers are pinned in the
# baseline. Pure text, no compiler; `--self-test` proves it can fail.
test-perceus-read-audit:
	@./tools/perceus-read-audit.sh --self-test

# A new hand-rolled ExprKind walker closed by a catch-all fails: walkers
# descend through xp_positions. Existing sites are pinned in the baseline.
# Pure text, no compiler; `--self-test` proves it can fail.
test-walker-catchall-audit:
	@./tools/walker-catchall-audit.sh --self-test

# The other half of the KAI_HOT_ONLY soundness argument. gen-runtime-bc.sh
# proves no function IN the hot bitcode reaches swapcontext; this proves no
# thread-local address is materialised by a function the optimiser can fold
# into an emitted kaikai frame, whose activation does span a park and can
# resume on another OS thread. `--self-test` is hermetic — hand-written IR, no
# compiler — so the discriminator is gated on hosts where P2 is opted out too.
#
# The generator runs in the middle deliberately. It is a no-op when the .bc is
# fresh and a clean exit when there is no clang 18, but when the gate rejects
# what it built it DROPS the .bc — which would otherwise leave the third step
# with nothing to inspect and the failure invisible. Here it propagates.
test-tls-hoist-gate:
	@./tools/tls-hoist-gate.sh --self-test
	@./tools/gen-runtime-bc.sh
	@./tools/tls-hoist-gate.sh

# `kai fmt` fixture suite. Verifies that every fixture in
# examples/fmt/ formats to its `.expected.kai` and is idempotent,
# plus that every formattable example under examples/minimal/ /
# quickstart/ / phase4/ round-trips through the formatter without
# breaking re-parse. Cheap enough to gate every Tier 1 run.
test-fmt: kaic2
	@./tests/fmt_fixtures.sh

# `kai fmt` package mode. The other fmt gates drive one file at a
# time, so none of them sees `kai fmt .`: that the enumerator's
# dropped entries do not become the run's exit status, and that
# --check reports file NAMES rather than reformatted source.
test-fmt-package: kaic2
	@./tests/fmt_package.sh

# `kai fmt` line-width gate. The golden suite pins bytes and the
# property harness pins meaning; neither can see whether the writer
# keeps lines inside the budget. This measures two numbers over
# examples/fmt/width/ — over-budget lines the writer could have broken,
# and lines it removed from hand-wrapped source — and requires both at
# zero, pinned in tools/fmt-width-baseline.txt. Runs in ~1 s: 18
# fixtures, one fmt each.
test-fmt-width: kaic2
	@./tests/fmt_width.sh

# The formatter's quality ledger: the width numbers above alongside
# `km score` per formatter module. A REPORT, not a gate — run it before
# and after a lane, the PR carries the delta.
test-fmt-ledger: kaic2
	@./tests/fmt_ledger.sh

# Issue #1047 — `kai migrate` edition-migration fixtures. Asserts the
# rewrite matches the golden, is idempotent, re-parses, and reports
# un-migratable changes as `manual:` (see examples/migrate/).
test-migrate: kaic2
	@./tests/migrate_fixtures.sh

# Issue #786 — `kai fmt` self-hosting ratchet. For every source under
# stdlib/ + stage2/compiler/, asserts `kai fmt` exits 0 (no refusal)
# and is byte-identically idempotent (fmt(fmt) == fmt). Locks in the
# full-surface coverage shipped in #784 so it cannot regress. The
# skip-list inside the script must stay empty in steady state.
test-fmt-selfhost: kaic2
	@./tests/fmt_selfhost.sh

# Keeps `kai fmt --help` honest: asserts every construct the help
# claims to format actually formats, and that the help does not
# re-acquire the stale "refused with an explicit error" promise.
test-fmt-help-scope: kaic2
	@./tests/fmt_help_scope.sh
# `kai fmt` meaning-preservation gate. The fixture suite covers shapes
# somebody wrote down; the selfhost ratchet covers shapes that occur in
# stdlib/ + stage2/compiler/. Neither checks that fmt PRESERVES MEANING,
# which is how a run of writer bugs shipped that left files unparseable
# after an in-place, silent rewrite. Over every examples/**/*.kai this
# asserts: fmt exits 0, its output re-parses, the parsed AST matches the
# input's modulo position, and fmt is idempotent. examples/ is the point
# — it exercises surface (rest-patterns, kind-annotated type params,
# delimited record literals in condition position) that the selfhost
# corpus never uses. The exception lists inside the script are numbered
# by issue and must reach zero.
#
# Cost: four kaic2 invocations per file over ~1800 files. Serial that is
# ~18 min, so the script fans out over $(nproc) workers
# (FMT_PROPERTY_JOBS to override).
#
# Shard placement has two constraints, and both bite. It cannot ride
# shard-1 (its sibling fmt gates): that shard already runs ~30 min, the
# job's ceiling, and this gate pushes it over. It cannot ride shard-4 or
# shard-6 either: those are gated on `compiler-touch`, so on a PR that
# changes no compiler source they skip — and a formatter gate that goes
# quiet exactly when the formatter was not touched still has to run,
# because the corpus it checks can regress from a stdlib or fixture
# change. shard-3 is unconditional and has headroom.
test-fmt-property: kaic2
	@./tests/fmt_property.sh

# Coverage of the namespace collision corpus is a checked property, not
# a judgement call: every cell of the matrix names a fixture that exists
# or carries a written reason, every fixture on disk is claimed by a
# row, and every row's `axes` column — the list the stage2 corpus
# targets read their fixtures from — names a harness with its golden on
# disk. Needs no compiler, so it runs standalone.
test-namespace-matrix:
	@./tests/namespace_matrix.sh

# How much of the matrix passes right now. A REPORT, not a gate: the
# corpus is written against the model, so most cells fail until the
# implementation reaches them. Exits 0 whatever the count — a step that
# closes a class should move this number, and a step that breaks a
# passing cell shows up here. `VERBOSE=1` lists the failing cells;
# `native` measures the other backend.
test-namespace-matrix-status: bin/kai
	@./tests/namespace_matrix_status.sh

# Structural debt: km score on the compiler and on the files the name
# resolution refactor threads through, plus how many corrective scope
# passes survive. Run before and after a lane; the PR reports the delta.
test-km-ledger:
	@./tests/km_ledger.sh

# The design->matrix class contract: every name class docs/namespaces-design.md
# enumerates has rows in the matrix, with the mapping visible in the gate.
test-namespace-classes:
	@./tests/namespace_matrix_classes.sh

# Corrective scope passes may only disappear (baseline in
# tools/corrective-pass-baseline.txt); a new one fails the build.
test-corrective-ratchet:
	@./tools/corrective-pass-ratchet.sh

# Every NEW compiler/stdlib file on the branch meets the differential
# quality floor. Binds where `km` exists; CI skips with a note.
test-km-new-files:
	@./tools/km-new-files-gate.sh

# bench v1.x (issues #40 + #437) — smoke for `kai bench`. Builds +
# runs the `examples/stdlib/bench_basic.kai` and
# `examples/stdlib/bench_median_mad.kai` fixtures and verifies the
# new median + MAD output format. Numeric values vary per run so we
# grep for the pattern rather than diffing against a golden file.
# Also exercises the --iters CLI flag and the KAI_BENCH_WARMUP env
# var to make sure both paths feed through to the runtime.
# Failure modes:
#   - kai bench exits non-zero (compile / runtime error)
#   - any expected bench result line is missing or malformed
#   - the trailing `<N> benches` summary line is missing
#   - --iters override fails to change the iteration count
test-bench: kaic2
	@./bin/kai bench examples/stdlib/bench_basic.kai > /tmp/kaikai-bench-basic.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — kai bench exited $$rc"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	matched=$$(grep -E ': 1000 iter / median [0-9]+ ns / MAD [0-9]+ ns / mean [0-9]+ ns / range \[[0-9]+, [0-9]+\]$$' /tmp/kaikai-bench-basic.out | wc -l | tr -d ' '); \
	if [ "$$matched" != "3" ]; then \
	  echo "test-bench FAIL — expected 3 bench result lines in new format, got $$matched"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	if ! grep -qE '^3 benches$$' /tmp/kaikai-bench-basic.out; then \
	  echo "test-bench FAIL — missing '3 benches' summary line"; \
	  cat /tmp/kaikai-bench-basic.out; exit 1; \
	fi; \
	./bin/kai bench --iters 200 examples/stdlib/bench_median_mad.kai > /tmp/kaikai-bench-mm.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — kai bench --iters 200 exited $$rc"; \
	  cat /tmp/kaikai-bench-mm.out; exit 1; \
	fi; \
	matched_mm=$$(grep -E ': 200 iter / median [0-9]+ ns / MAD [0-9]+ ns / mean [0-9]+ ns / range \[[0-9]+, [0-9]+\]$$' /tmp/kaikai-bench-mm.out | wc -l | tr -d ' '); \
	if [ "$$matched_mm" != "2" ]; then \
	  echo "test-bench FAIL — expected 2 bench result lines @ 200 iter (--iters override), got $$matched_mm"; \
	  cat /tmp/kaikai-bench-mm.out; exit 1; \
	fi; \
	KAI_BENCH_ITERS=128 KAI_BENCH_WARMUP=4 ./bin/kai bench examples/stdlib/bench_median_mad.kai > /tmp/kaikai-bench-env.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-bench FAIL — KAI_BENCH_ITERS=128 kai bench exited $$rc"; \
	  cat /tmp/kaikai-bench-env.out; exit 1; \
	fi; \
	matched_env=$$(grep -E ': 128 iter / median [0-9]+ ns / MAD [0-9]+ ns / mean [0-9]+ ns / range \[[0-9]+, [0-9]+\]$$' /tmp/kaikai-bench-env.out | wc -l | tr -d ' '); \
	if [ "$$matched_env" != "2" ]; then \
	  echo "test-bench FAIL — expected 2 bench result lines @ 128 iter (env override), got $$matched_env"; \
	  cat /tmp/kaikai-bench-env.out; exit 1; \
	fi; \
	echo "test-bench OK — basic ($$matched), --iters ($$matched_mm), env ($$matched_env) bench formats verified"

# check v1 (issue #44) — smoke for `kai check`. Builds + runs the
# `examples/stdlib/check_basic.kai` fixture and verifies the output
# format. The fixture is constructed so every property holds for
# every value the generators can produce; if a counterexample
# appears it indicates a real regression in the runtime, the
# emitter, or the generator range, not just an unlucky seed.
# Failure modes:
#   - kai check exits non-zero (compile / runtime / counterexample)
#   - any of the four check result lines is missing or malformed
#   - the trailing `4/4 checks passed` summary is missing
test-check: kaic2
	@./bin/kai check examples/stdlib/check_basic.kai > /tmp/kaikai-check-basic.out 2>&1; \
	rc=$$?; \
	if [ $$rc -ne 0 ]; then \
	  echo "test-check FAIL — kai check exited $$rc"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	matched=$$(grep -E ': 100 iter, OK$$' /tmp/kaikai-check-basic.out | wc -l | tr -d ' '); \
	if [ "$$matched" != "5" ]; then \
	  echo "test-check FAIL — expected 5 OK lines, got $$matched"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	if ! grep -qE '^5/5 checks passed$$' /tmp/kaikai-check-basic.out; then \
	  echo "test-check FAIL — missing '5/5 checks passed' summary"; \
	  cat /tmp/kaikai-check-basic.out; exit 1; \
	fi; \
	./bin/kai check examples/stdlib/check_shrinking.kai > /tmp/kaikai-check-shrinking.out 2>&1; \
	rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "test-check FAIL — shrinking fixture must exit non-zero (every block fails by design)"; \
	  cat /tmp/kaikai-check-shrinking.out; exit 1; \
	fi; \
	if ! grep -qE '^0/3 checks passed$$' /tmp/kaikai-check-shrinking.out; then \
	  echo "test-check FAIL — shrinking fixture missing '0/3 checks passed' summary"; \
	  cat /tmp/kaikai-check-shrinking.out; exit 1; \
	fi; \
	shrunk=$$(grep -E ', shrunk to ' /tmp/kaikai-check-shrinking.out | wc -l | tr -d ' '); \
	if [ "$$shrunk" -lt 2 ]; then \
	  echo "test-check FAIL — expected >=2 'shrunk to' lines (Int + List), got $$shrunk"; \
	  cat /tmp/kaikai-check-shrinking.out; exit 1; \
	fi; \
	echo "test-check OK — $$matched property blocks passed; shrinking produced $$shrunk minimised counterexamples"

# `kai typecheck` (issue #1427) — verb smoke. Front-end only: a clean
# program exits 0 with silent streams, a broken one exits non-zero
# with the same diagnostic a build prints (the corpus-wide identity
# gate is test-check-parity), and the JSON report mounts ride the verb.
# The --holes-json step reuses the clean fixture: its convention-pipe
# dispatch pins that the hole driver sees the build's front-end.
test-typecheck: kaic2
	@./bin/kai typecheck examples/typecheck/clean.kai > /tmp/kaikai-typecheck.out 2> /tmp/kaikai-typecheck.err; \
	rc=$$?; \
	if [ $$rc -ne 0 ] || [ -s /tmp/kaikai-typecheck.out ] || [ -s /tmp/kaikai-typecheck.err ]; then \
	  echo "test-typecheck FAIL — clean fixture: rc=$$rc (want 0, silent streams)"; \
	  cat /tmp/kaikai-typecheck.out /tmp/kaikai-typecheck.err; exit 1; \
	fi; \
	neg=examples/negative/type_invariants/arith_string.kai; \
	./bin/kai typecheck $$neg > /dev/null 2> /tmp/kaikai-typecheck-neg.err; \
	rc=$$?; \
	if [ $$rc -eq 0 ]; then \
	  echo "test-typecheck FAIL — negative fixture accepted (want non-zero exit)"; exit 1; \
	fi; \
	if ! grep -qF "$$(head -1 $${neg%.kai}.err.expected)" /tmp/kaikai-typecheck-neg.err; then \
	  echo "test-typecheck FAIL — negative diagnostic missing the golden first line"; \
	  cat /tmp/kaikai-typecheck-neg.err; exit 1; \
	fi; \
	mono_neg=examples/negative/protocols/bound_imported_fn/main.kai; \
	./bin/kai typecheck $$mono_neg > /dev/null 2> /tmp/kaikai-typecheck-mono.err \
	  && { echo "test-typecheck FAIL — a bound violated at a mono instantiation was accepted"; exit 1; }; \
	grep -qF "$$(head -1 $${mono_neg%.kai}.err.expected)" /tmp/kaikai-typecheck-mono.err \
	  || { echo "test-typecheck FAIL — mono-time diagnostic differs from the build's"; cat /tmp/kaikai-typecheck-mono.err; exit 1; }; \
	./bin/kai typecheck examples/multi-module/bound_impl_other_module/main.kai \
	  || { echo "test-typecheck FAIL — bound satisfied by an impl in another module was rejected"; exit 1; }; \
	./bin/kai typecheck examples/typecheck/clean.kai --diags-json > /tmp/kaikai-typecheck-dj.out 2>&1 \
	  && grep -q '"diagnostics": \[\]' /tmp/kaikai-typecheck-dj.out \
	  || { echo "test-typecheck FAIL — --diags-json mount broken"; cat /tmp/kaikai-typecheck-dj.out; exit 1; }; \
	./bin/kai typecheck examples/typecheck/clean.kai --holes-json > /tmp/kaikai-typecheck-hj.out 2>&1 \
	  && grep -q '^\[\]$$' /tmp/kaikai-typecheck-hj.out \
	  || { echo "test-typecheck FAIL — --holes-json mount broken"; cat /tmp/kaikai-typecheck-hj.out; exit 1; }; \
	eff=examples/typecheck/effects_file.kai; \
	./bin/kai typecheck $$eff --effects-json > /tmp/kaikai-typecheck-ej.out 2>&1 \
	  && python3 scripts/effects_json_files.py /tmp/kaikai-typecheck-ej.out $$eff > /tmp/kaikai-typecheck-ej.got \
	  && diff $${eff%.kai}.out.expected /tmp/kaikai-typecheck-ej.got \
	  || { echo "test-typecheck FAIL — --effects-json records must name their file"; head -c 2000 /tmp/kaikai-typecheck-ej.out; exit 1; }; \
	echo "test-typecheck OK — clean exit 0, negative rejected with build-identical diagnostic, JSON mounts live"

# Check-vs-build diagnostic identity (issue #1427): every compile-time
# negative fixture must be rejected by `kaic2 --check` with the same
# exit code and byte-identical stderr as a full build.
test-check-parity: kaic2
	@./tools/test-check-parity.sh

# Issue #454 — `--library-mode` regression. Each fixture under
# examples/library_mode/ embeds `# @probe <kind> L:C` markers; kaic2
# emits a single JSON object per file with the resolved type / def /
# enclosing-node answer for each probe. A diff against `.out.expected`
# pins the JSON byte-for-byte so regressions in the typer (positions
# drift, ty_to_string format changes, lower passes lose source spans)
# fail this gate before they reach the LSP / cache lanes downstream.
test-library-mode: kaic2
	@set -e; \
	root=$$(pwd); \
	cd "$$root"; \
	for fx in type_at_basic def_at_basic \
	          def_at_local_basic def_at_local_shadow \
	          def_at_param_basic def_at_param_shadow \
	          def_at_pattern_list def_at_pattern_variant \
	          def_at_pattern_record def_at_pattern_shadow \
	          def_at_pattern_as def_at_closure_capture \
	          def_at_nested_let def_at_match_arm \
	          def_at_lambda_param symbols_root_only; do \
	  src="examples/library_mode/$$fx.kai"; \
	  exp="examples/library_mode/$$fx.out.expected"; \
	  out=$$(mktemp); \
	  "$$root/stage2/kaic2" $(EDITION_FLAG) --library-mode "$$src" > "$$out" 2>/dev/null \
	    || { echo "library-mode $$fx FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "library-mode $$fx DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "library-mode $$fx OK"; \
	done; \
	src="examples/library_mode/def_at_imports.kai"; \
	exp="examples/library_mode/def_at_imports.out.expected"; \
	out=$$(mktemp); \
	"$$root/stage2/kaic2" $(EDITION_FLAG) --path examples/library_mode --library-mode "$$src" > "$$out" 2>/dev/null \
	  || { echo "library-mode def_at_imports FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	diff -q "$$exp" "$$out" > /dev/null \
	  || { echo "library-mode def_at_imports DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	rm -f "$$out"; \
	echo "library-mode def_at_imports OK"

# LSP smoke gate. Builds tools/kai-lsp (a kaikai package) against the
# freshly built kaic2, then drives it through the scripted JSON-RPC
# sessions in examples/lsp/ (hover, completion, goto-def, diagnostics
# push, document symbols, signature help, hole diagnostics). Needs
# python3 (stdlib only, no pip). The drivers' exit-2 "binary missing"
# escape cannot fire here — the recipe builds the binary first — so
# every non-zero exit is a failure.
test-lsp: kaic2
	@set -e; \
	root=$$(pwd); \
	( cd tools/kai-lsp && "$$root/bin/kai" build . --backend=c > /dev/null ); \
	for drv in examples/lsp/*.lsp.py; do \
	  out=$$(mktemp); \
	  python3 "$$drv" > "$$out" 2>&1 \
	    || { echo "lsp $$drv FAIL"; cat "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "lsp $$(basename $$drv .lsp.py) OK"; \
	done

# The native paths compile a copy of the entry file, so a diagnostic
# attributed to the entry must still name the user's source and not the
# copy — and a failed native-modular compile must report it once, not
# once per fallback.
test-native-diag-path: kaic2
	@./tests/native_diag_path.sh

# A compile error prints Build FAILED and leaves `kai watch` waiting for
# the next save, rather than ending the session with it.
test-watch-survives-error: kaic2
	@./tests/watch_survives_error.sh

# `--diags-json` goldens. Each examples/library_mode/diags_*.kai is a
# deliberately broken source whose JSON document is diffed against its
# .diags.expected, pinning the shape and wording LSP consumers read.
test-diagnostics-collected: kaic2
	@set -e; \
	root=$$(pwd); \
	cd "$$root"; \
	for fx in diags_t1_type_mismatch diags_t2_non_exhaustive \
	          diags_t3_unbound_name diags_t4_wrong_arity \
	          diags_t5_missing_effect diags_multiple_errors \
	          diags_mono_bound; do \
	  src="examples/library_mode/$$fx.kai"; \
	  exp="examples/library_mode/$$fx.diags.expected"; \
	  out=$$(mktemp); \
	  "$$root/stage2/kaic2" $(EDITION_FLAG) --diags-json "$$src" > "$$out" 2>/dev/null \
	    || { echo "diags-collected $$fx FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "diags-collected $$fx DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "diags-collected $$fx OK"; \
	done

# Negative-space test suite (issue #511). Every fixture under
# examples/negative/** must be rejected by kaic2 (non-zero exit)
# AND must surface the expected diagnostic substring stored in the
# sibling `.err.expected` golden. Existence of this target is the
# point: positive tests alone proved insufficient (#510 — `pub` was
# silently unenforced for the full life of the language because no
# negative test asserted the contract).
test-negative: kaic2
	@./tools/test-negative.sh

# Validate every stdlib module compiles cleanly when loaded.
# stdlib is normally pulled into user programs via core auto-load or
# `import`, both of which route the file through `expand_imports`.
# The same-module name-collision validators (`validate_fn_name_collisions_decls`
# et al.) now run inside `load_prelude` and `resolve_module` (per-
# module, before the decls join the global stream), so a duplicate
# `fn foo` (or `type T`, `effect E`, `const N`, `axiom A`) inside a
# stdlib file is caught at load time — wherever the module is loaded
# from.
#
# This target exercises that gate. For every `stdlib/**/*.kai` we
# build a one-line trampoline `import <mod>` `fn main() = 0` and
# compile it; the act of importing forces kaic2 to parse and
# validate the module. Any failure (validator rejection, parse
# error, type error) is reported per-module and the target exits
# non-zero. Belongs in tier1 so stdlib drift cannot land via PR
# without surfacing.
test-stdlib-modules: kaic2
	@./tools/test-stdlib-modules.sh

.PHONY: test-core-text test-http-redirects
KAI_TEST_DRIVER ?= $(CURDIR)/bin/kai
KAI_TEST_BACKEND ?= c

test-http-redirects: bin/kai
	KAI_TEST_DRIVER="$(KAI_TEST_DRIVER)" KAI_TEST_BACKEND="$(KAI_TEST_BACKEND)" python3 tests/http_redirects.py
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" test --backend=$(KAI_TEST_BACKEND) stdlib/net/http.kai

test-core-text: bin/kai
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" test --backend=$(KAI_TEST_BACKEND) tests/stdlib/char_test.kai
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" test --backend=$(KAI_TEST_BACKEND) tests/stdlib/char_unicode_test.kai
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" test --backend=$(KAI_TEST_BACKEND) tests/stdlib/string_test.kai
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" test --backend=$(KAI_TEST_BACKEND) tests/stdlib/string_boundaries_test.kai
	# Rename the inline-test entry to avoid colliding with auto-loaded core.string.
	@tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	cp stdlib/core/string.kai "$$tmp/string_subject.kai" && \
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" test --backend=$(KAI_TEST_BACKEND) "$$tmp/string_subject.kai"
	KAI_STDLIB="$(CURDIR)/stdlib" "$(KAI_TEST_DRIVER)" check --backend=c tests/stdlib/core_text_properties_test.kai

# Differential independence oracle (#962): proves core's typecheck is
# byte-identical with and without an adversarial user file — the
# soundness gate behind reusing a typechecked stdlib TyEnv. RED if a
# user `protocol` or root fn shifts (or breaks) core's typed AST.
# Belongs in tier1: a contaminated core typecheck is silent
# incorrectness, so it must not land via PR.
test-independence-oracle: kaic2
	@$(MAKE) -C stage2 test-independence-oracle

# Package-mode harness (issue #569). The compiler's self-host
# never exercises kaikai-as-a-package — stdlib lives flat under
# `stdlib/`, not behind manifests. Without this harness the entire
# package-manager surface (manifest discovery, kai-pkg paths,
# transitive imports, cross-package effects, auto-install) stays
# a CI blind spot, surfacing regressions only when downstream
# consumers (ahu, henua, kohau) try to integrate — which is how
# #565 and #567 shipped to main. The harness lives in tools/ and
# delegates the 9-category matrix from the issue plus the
# pre-existing driver-level checks (lockfile_reproducibility,
# add_failure, init_invalid_names, manifest_parse_error).
test-packages: kaic2
	@tools/test-packages.sh

# Edition-selection surface: per-edition behaviour, repo-EDITION
# fallback, unknown-edition diagnostic.
test-editions: kaic2
	@tools/test-editions.sh

# ASAN+UBSan memory-safety gate. Rebuilds the demos/ probe set with
# `-fsanitize=address,undefined` and runs each binary; fails on any
# sanitizer diagnostic or if the demos baseline regresses under
# instrumentation. Apple clang lacks LSAN support, so leak detection
# stays disabled (`detect_leaks=0`) for portability with the Linux
# runner; if a leak ratchet ever becomes useful, gate it separately
# on Linux.
#
# CI runs the two shards as parallel jobs (the tier1-asan job in
# tier1.yml) on the shared kaic2 build, so the split point balances the
# legs around the demos block. Each shard's legs run under `make -j`:
# every leg writes its own build/ files, and the core cache is warmed
# before the fan-out. Locally `make tier1-asan` runs both shards in
# sequence.
TIER1_ASAN_JOBS ?= 4
TIER1_ASAN_LEGS_A := \
	test-mn-sigaltstack-asan \
	test-namespace-collisions-asan \
	test-trace-asan \
	test-runtime-shadow-asan \
	test-signal-trap-asan \
	test-log-asan \
	test-trap-exit-cancel-asan \
	test-monitor-ref-asan \
	test-process-basic-asan \
	test-perceus-issue82-asan \
	test-ffi-extern-c-asan \
	test-perceus-issue118-asan \
	test-perceus-issue298-asan \
	test-perceus-issue350-asan \
	test-perceus-trmc-spread-asan
TIER1_ASAN_LEGS_B := \
	test-cancel-clause-ubsan \
	test-perceus-issue703-asan \
	test-issue-779-asan \
	test-perceus-enum-slot-asan \
	test-perceus-int-cache-asan \
	test-int-field-inline-asan \
	test-perceus-nested-reuse-asan \
	test-match-pbind-catchall-asan \
	test-http-client-asan \
	test-stdlib-crypto-asan \
	test-stdlib-regex-predicate-asan \
	test-env-mutate-asan \
	test-securerandom-asan \
	test-perceus-1151-vec-push-growth-asan \
	test-perceus-1153-modcall-linear-asan \
	test-perceus-1150-vec-surface-asan \
	test-perceus-1180-range-lazy-asan \
	test-perceus-1295-borrow-slot-nested-arg-asan \
	test-perceus-1303-single-use-branch-leak-asan \
	test-perceus-1315-borrowed-match-release-asan \
	test-perceus-1328-selftail-borrowed-local-asan \
	test-issue-1331-op-arg-release-asan \
	test-perceus-1355-closure-temp-release-asan \
	test-perceus-1324-char-binder-goto-asan \
	test-perceus-1395-char-param-raw-asan \
	test-issue-1394-byte-box-asan \
	test-perceus-1410-byte-raw-ops-asan \
	test-perceus-1457-fixed-raw-ops-asan \
	test-perceus-1637-int-wrap-ops-asan \
	test-perceus-1464-fixed-raw-ops-asan \
	test-issue-1331-borrowed-op-arg-asan \
	test-perceus-1758-cond-exit-drop-asan \
	test-perceus-1765-binop-base-tail-asan \
	test-perceus-1768-move-gate-positions-asan \
	test-perceus-1770-arm-birth-leak-asan \
	test-perceus-1784-variant-rebuild-asan \
	test-perceus-1786-record-rebuild-asan \
	test-perceus-1791-arm-use-scope-asan \
	test-perceus-1302-tcrec-goto-drops-asan \
	test-perceus-1635-goto-move-collision-asan \
	test-perceus-1801-capability-param-asan \
	test-perceus-1803-op-arg-free-asan \
	test-perceus-1902-block-let-move-asan \
	test-perceus-2032-bang-operand-use-asan \
	test-perceus-owned-scope-unused-param-asan \
	test-perceus-block-let-unused-alias-asan \
	test-perceus-closure-capture-tail-asan \
	test-perceus-borrow-homonym-asan \
	test-perceus-closure-capture-selftail-asan \
	test-perceus-switch-scrutinee-selftail-asan \
	test-perceus-arm-branch-read-asan \
	test-perceus-read-identity-asan \
	test-perceus-trmc-step-ledger-asan \
	test-perceus-borrow-own-asan \
	test-perceus-spread-consumes-asan \
	test-perceus-reuse-untaken-slot-asan \
	test-perceus-branch-param-selftail-asan \
	test-perceus-trmc-raw-operand-drop-asan \
	test-perceus-pipe-borrowed-slot-alignment-asan \
	test-trmc-slot-forms-asan

tier1-asan: tier1-asan-a tier1-asan-b

tier1-asan-a: kaic2 test-arena
	@ASAN_OPTIONS="abort_on_error=0:halt_on_error=1:detect_leaks=0" \
	 UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
	 $(MAKE) -C demos verify \
	    CFLAGS="-std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wno-unused-function -Wno-unused-variable" \
	    > /tmp/kaikai-tier1-asan.log 2>&1; \
	hits=$$(grep -lE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' demos/build/*.err 2>/dev/null | wc -l | tr -d ' '); \
	if [ "$$hits" != "0" ]; then \
	  echo "tier1-asan FAIL — sanitizer diagnostics in $$hits demo(s):"; \
	  grep -lE 'AddressSanitizer|UndefinedBehaviorSanitizer|runtime error:' demos/build/*.err; \
	  echo "see /tmp/kaikai-tier1-asan.log for full kaic2 / cc output"; \
	  exit 1; \
	fi; \
	expected=$${BASELINE:-$$(cat demos/baseline.txt 2>/dev/null || echo 0)}; \
	got=$$(cat demos/build/*.status 2>/dev/null | grep -cE '^(OK|PASS)'); \
	if [ "$$got" -lt "$$expected" ]; then \
	  echo "tier1-asan FAIL — demos baseline regressed under ASAN: $$got < $$expected"; \
	  echo "see /tmp/kaikai-tier1-asan.log for per-demo status"; \
	  exit 1; \
	fi; \
	echo "tier1-asan OK — $$got/$$expected demos pass under ASAN+UBSan, no sanitizer diagnostics"
	@$(MAKE) -C stage2 core-cache-warm
	@$(MAKE) -C stage2 -j$(TIER1_ASAN_JOBS) $(TIER1_ASAN_LEGS_A)
	@echo "tier1-asan-a OK — $(words $(TIER1_ASAN_LEGS_A)) fixture legs pass under ASAN+UBSan"

tier1-asan-b: kaic2
	@$(MAKE) -C stage2 core-cache-warm
	@$(MAKE) -C stage2 -j$(TIER1_ASAN_JOBS) $(TIER1_ASAN_LEGS_B)
	@echo "tier1-asan-b OK — $(words $(TIER1_ASAN_LEGS_B)) fixture legs pass under ASAN+UBSan"

# Backend-parity: build every entry-point fixture under the documented
# example dirs + demos with the native backend AND the C-direct oracle,
# run, diff stdout + exit code. This is the same gate the tier1-native
# CI workflow runs (with NATIVE_PARITY_RATCHET=1 to forbid new gaps); the
# llvm-text backend it once also covered was removed. Local convenience
# target — not invoked from `tier1` because the cost (~15-30 min) does
# not belong on every PR locally. SKIPs when kaic2 lacks libLLVM (build
# it with `make -C stage2 KAI_LLVM=1`).
#
# The rebuild is NOT a declared prerequisite. `stage2/Makefile` auto-detects
# KAI_LLVM from llvm-config on PATH, and a keg-only LLVM (Homebrew's default)
# is off PATH — so a plain prerequisite relinks an existing native kaic2 as
# C-only, and the harness then SKIPs (exit 0) on the very capability the
# rebuild just removed. Probing first keeps the C-only SKIP intact while
# making a silent downgrade impossible.
tier1-backend-parity: bin/kai
	@bash tools/parity-preserve-native.sh
	@TARGET_BACKEND=native ORACLE_BACKEND=c NATIVE_PARITY_RATCHET=1 tools/test-backend-parity.sh

# Tier 2: daily / nightly — the slowest tier. Runs once a day on `main` HEAD,
# not per-PR. If it fails, `main` stays unbroken (Tier 0/1 gated every
# commit) but a diagnostic opens a lane the next morning.
#
# `tier1-asan` and `tier1-backend-parity` are deliberately NOT here: both run
# path-gated on PRs. `selfhost` is here rather than on the PR path: the native
# half of the byte-identity gate already runs per-PR in tier1-native, and the
# C half costs two whole compiler generations.
#
# CI splits `daily` across parallel jobs (.github/workflows/daily.yml): the
# seven tier1 shards, `tier1-unsharded`, and `daily-tail`. Their union is
# `daily`; a phase added here or to `tier1` needs an owner among them.
daily: tier1 daily-tail
	@echo "daily OK — tier1 + C selfhost + stress fixtures + coverage probe + RC budget + BinSerialize budget"

daily-tail: selfhost stress-fixtures coverage-probe rc-budget test-binserialize-budget
	@echo "daily-tail OK — C selfhost + stress fixtures + coverage probe + RC budget + BinSerialize budget"

# The `tier1` prerequisites that no tier1-shard-N runs.
tier1-unsharded: test-stage0 test-stage1 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record test-migrate test-mn-determinism test-runtime-global-audit rc-leak-gate
	@echo "tier1-unsharded OK — stage0/stage1 tests + phase4 demos + multi-module/import probes + migrate + M:N determinism + runtime-global audit + Perceus RC leak ledger"

# Stress fixtures: closed regressions that exercise patterns the
# per-feature suite does not — R3 scrutinee-reuse RC
# (interp_recursive_walk) and m4c Phase 3 polymorphic flow-through
# (m4c_flow_through). Hard gates: a failure here fails the daily.
stress-fixtures: kaic2
	@set -e; \
	for f in examples/effects/interp_recursive_walk.kai \
	         examples/effects/m4c_flow_through.kai; do \
	  name=$$(basename $$f .kai); \
	  stage2/kaic2 $(EDITION_FLAG) $$f > /tmp/stress-$$name.c 2> /tmp/stress-$$name.err \
	    || { echo "stress FAIL $$name (kaic2 errored)"; cat /tmp/stress-$$name.err; exit 1; }; \
	  cc -std=c99 -I stage2 -I stage0 /tmp/stress-$$name.c -o /tmp/stress-$$name -lm 2>> /tmp/stress-$$name.err \
	    || { echo "stress FAIL $$name (cc errored)"; cat /tmp/stress-$$name.err; exit 1; }; \
	  /tmp/stress-$$name > /tmp/stress-$$name.out 2>&1 \
	    && echo "stress OK $$name" \
	    || { echo "stress FAIL $$name (binary exit non-zero)"; cat /tmp/stress-$$name.out; exit 1; }; \
	done

# Coverage probe: every section of the runtime / language docs has a
# fixture; if not, alarm. Implemented as a shell script so it can run
# in CI without depending on the kaikai compiler itself.
coverage-probe:
	@./tools/coverage-probe.sh

# RC budget: leaked / RSS / wall vs the threshold pinned in
# docs/perceus-honesty-targets.md. Today the threshold is "no
# regression from 46.9 M leaked".
rc-budget: kaic2
	@KAI_TRACE_RC=1 KAI_THREADS=1 stage2/kaic2 $(EDITION_FLAG) stage2/compiler.kai > /dev/null 2> /tmp/rc.log; \
	leaked=$$(grep -E "alloc_total" /tmp/rc.log | head -1 | sed 's/.*leaked=\([0-9]*\).*/\1/'); \
	if [ -z "$$leaked" ]; then \
	  echo "rc-budget SKIP — KAI_TRACE_RC trace empty (kaic2 not built with trace)"; \
	  exit 0; \
	fi; \
	threshold=50000000; \
	if [ "$$leaked" -gt "$$threshold" ]; then \
	  echo "rc-budget FAIL — leaked $$leaked > threshold $$threshold (regression)"; \
	  exit 1; \
	fi; \
	echo "rc-budget OK — leaked $$leaked <= threshold $$threshold"

# BinSerialize perf gate (issue #489). Wall-time ceiling over 100
# decodes of a 500-node / ~40 KB payload. Tier 2 (not Tier 1)
# because perf telemetry belongs at daily cadence, not per-PR.
# Ceiling lives in tools/binserialize-budget.txt; ratchet down as
# future lanes improve. Guards against silent re-introduction of
# O(N) shape on the decode path that the round-trip correctness
# tests would not catch (issue #485 was 19 s; PR #487 dropped that
# to ~7 ms/decode; the budget defends that work for the cache lanes
# (#452 Phase A.0 onwards) that build on top of this substrate).
test-binserialize-budget: kaic2
	@bin/kai build tools/bench-binserialize-roundtrip.kai -o /tmp/bench-binserialize >/dev/null 2>&1 \
	  || { echo "binserialize-budget FAIL — build error"; exit 1; }
	@ceiling=$$(grep -v '^#' tools/binserialize-budget.txt | head -1); \
	wall_s=$$(/usr/bin/time -p /tmp/bench-binserialize > /dev/null 2>/tmp/binserialize.t \
	  && awk '/^real/{print $$2}' /tmp/binserialize.t); \
	wall_ms=$$(awk -v s="$$wall_s" 'BEGIN{printf "%d", s * 1000}'); \
	per_decode=$$(awk -v ms="$$wall_ms" 'BEGIN{printf "%.2f", ms / 100.0}'); \
	if [ "$$wall_ms" -gt "$$ceiling" ]; then \
	  echo "binserialize-budget FAIL — 100 decodes took $${wall_ms} ms (per decode $${per_decode} ms) > ceiling $${ceiling} ms"; \
	  echo "  expected: PR #487 baseline was ~7 ms/decode; ratchet up only with a documented justification"; \
	  exit 1; \
	fi; \
	echo "binserialize-budget OK — 100 decodes in $${wall_ms} ms ($${per_decode} ms/decode <= $$((ceiling / 100)) ms ceiling)"

# LLVM static prep lives in its own file so the release libLLVM cache key
# (release.yml) hashes only mk/llvm.mk — see the header there.
include mk/llvm.mk
