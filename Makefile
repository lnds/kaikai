.PHONY: bench-mn-throughput all kaic0 kaic1 kaic2 kaic2-fast kaic2-fast-verify test test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record test-fmt test-fmt-selfhost test-migrate test-bench test-check test-typecheck test-check-parity test-library-mode test-diagnostics-collected test-negative test-private-type-shadow-audit test-runtime-global-audit test-tls-hoist-gate test-stdlib-modules test-independence-oracle test-packages test-binserialize-budget test-issue-779-asan demos-verify demos-no-regression selfhost test-arena test-heap-limit test-modular-selfhost test-perceus-1131-modular-escape test-mn-tsan test-mn-determinism test-mn-corpus test-mn-reactor-bench test-upgrade-resolver clean warm-core tier0 test-header-deps test-llvm-force-guard tier1 tier1-shard-1 tier1-shard-2 tier1-shard-3 tier1-shard-4 tier1-shard-5 test-doc tier1-asan tier1-backend-parity daily coverage-probe rc-budget stress-fixtures

all: kaic1 kaic2 bin/kai

kaic0:
	$(MAKE) -C stage0 kaic0

kaic1: kaic0
	$(MAKE) -C stage1 kaic1

kaic2: kaic1
	$(MAKE) -C stage2 kaic2

# Dev fast rebuild — an EXISTING kaic2 recompiles itself modularly
# (no kaic1, no bundle). Deliberately not wired into the bootstrap
# chain: `make kaic2` stays the trust path. See docs/build-system.md.
kaic2-fast:
	$(MAKE) -C stage2 kaic2-fast

kaic2-fast-verify:
	$(MAKE) -C stage2 kaic2-fast-verify

# bin/kai is already a checked-in shell script; this target just confirms it
# is executable and visible.
bin/kai:
	@chmod +x bin/kai
	@echo "kai driver: $$(realpath bin/kai 2>/dev/null || pwd)/bin/kai"

test: test-stage0 test-stage1 test-stage2 test-demos test-multi-module test-import-stdlib test-import-prelude-dedup test-import-qualified-record

test-stage0:
	$(MAKE) -C stage0 test

test-stage1:
	$(MAKE) -C stage1 test

test-stage2:
	$(MAKE) -C stage2 test

# Build + run + test every phase 4 demo via bin/kai.
test-demos: kaic1
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
	for case in multi-module multi-module/issue-237 multi-module/issue-897-shadow multi-module/issue-898-qualified-generic multi-module/issue-962-root-fns-isolation multi-module/pipe_custom_type multi-module/pipe_two_types_same_pkg; do \
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

# Issue #279 regression — exercises bin/kai's `--path "$$STDLIB_ROOT"`
# resolution end-to-end. The bug fixed in this lane was that line 469
# of bin/kai hardcoded `--path "$$ROOT/stdlib"` instead of using the
# already-resolved `$$STDLIB_ROOT`, breaking `import loop` (and any
# non-prelude stdlib import) in the brew-installed layout where
# `$$ROOT/stdlib` does not exist. We invoke `bin/kai run` (not kaic2
# directly) so the wrapper's argument construction is exercised.
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

# m12.8 Phase 3 — Core demo gate. Delegates to stage2's
# `test-demos-core` which builds each demo under examples/portfolio/
# and examples/usd_to_eur/ on both backends, redirects stdin from the
# `.in` fixture when present, and diffs against `.out.expected`. The
# top-level alias matches the name Eric proposed in the m12.8 review.
demos-verify: kaic2
	$(MAKE) -C stage2 test-demos-core

# m12.8.x post-Core REOPEN — Eric level 3 gate. Runs the full demo
# probe set under demos/ and fails when the OK + PASS count drops
# below the baseline pinned in `demos/baseline.txt`. Used at every
# milestone close to prevent silent regressions in features the
# Core demo gate (`demos-verify`) does not exercise.
demos-no-regression: kaic2
	$(MAKE) -C demos no-regression

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
tier0: selfhost demos-no-regression test-arena test-heap-limit test-evidence-frame test-runtime-global-audit test-tls-hoist-gate test-timeout-shim test-p2-status test-header-deps test-llvm-force-guard
	@echo "tier0 OK — selfhost deterministic (kaic2b.c == kaic2c.c), demos baseline holds, arena gate passes, heap ceiling contains, evidence-frame gate holds, runtime globals classified, no thread-local escapes into an inlinable hot-bitcode function, timeout shim honours its exit-code contract, P2 status distinguishes its three states, header prerequisites declared, forced KAI_LLVM=1 without llvm-config stops loud"

# A header-only edit must rebuild every artifact that embeds it, and must not
# rebuild the ones that do not. Both directions are invisible to CI, which
# always builds from scratch. Static query of the make database — no recipe
# runs, no kaic2 dependency, ~0.1s.
test-header-deps:
	@bash tools/test-header-deps.sh

# Forcing KAI_LLVM=1 with an unresolvable llvm-config must stop at parse
# time with the actionable error, never hand -DKAI_LLVM to cc without the
# LLVM include dirs. Make-level, no kaic2 dependency, ~0.1s.
test-llvm-force-guard:
	@out=$$($(MAKE) -s -C stage2 KAI_LLVM=1 LLVM_CONFIG=/nonexistent/llvm-config kaic2 2>&1); rc=$$?; \
	[ $$rc -ne 0 ] || { echo "llvm-force-guard FAIL — forced KAI_LLVM=1 without llvm-config did not stop"; exit 1; }; \
	echo "$$out" | grep -q "was not found" || { echo "llvm-force-guard FAIL — stop lacked the actionable error:"; echo "$$out"; exit 1; }; \
	echo "llvm-force-guard OK — forced KAI_LLVM=1 without llvm-config stops with the actionable error"

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
# free-list pools). Builds stage2/main.kai through bin/kai's KAI_MODULAR
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
# rides a dedicated CI tier (tier1-tsan.yml), never TEST_LIGHT.
test-mn-tsan: kaic2
	@bash tools/run-mn-tsan.sh

# Issue #1207 F1 — M:N determinism-only gate (no TSAN, fast). The multi-actor
# bench and the cross-thread copy stress print the same total at N=1 and N=4;
# a scheduling-order divergence trips it. Cheap enough for tier1.
test-mn-determinism: kaic2
	@bash tools/run-mn-determinism.sh

# M:N corpus determinism: the whole fixture corpus at N=1 vs N>1, both
# backends. Deliberately NOT in `tier1` — it is the heaviest gate in the
# repo and owns its own workflow (.github/workflows/tier1-mn-corpus.yml),
# where it shards across runners. This target is the local entry point;
# scope it with MN_CORPUS_DIRS while iterating. A C-only kaic2 drops the
# native arm with a loud line rather than silently halving the coverage.
test-mn-corpus: kaic2
	@bash tools/run-mn-corpus-determinism.sh

# F2 no-starvation regression gate: a sleeper on the reactor timer wheel
# wakes on time while CPU hogs occupy the other scheduler threads. Fails on
# the F1 inline reactor (the poll is pinned to thread 0), passes once the
# reactor runs on its own thread. Timing-based, so it rides the dedicated
# concurrency tier (tier1-tsan.yml), not the fast tier1 path.
test-mn-reactor-bench: kaic2
	@bash tools/run-mn-reactor-bench.sh

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
tier1: test demos-no-regression test-fmt test-fmt-selfhost test-migrate test-bench test-check test-typecheck test-check-parity test-library-mode test-diagnostics-collected test-negative test-stdlib-modules test-independence-oracle test-packages test-modular-selfhost test-perceus-1131-modular-escape test-private-type-shadow-audit test-private-record-shadow-audit test-canonical-aliases test-runtime-global-audit test-mn-determinism test-info test-doc test-upgrade-resolver
	@echo "tier1 OK — full make test + demos baseline + fmt fixtures + fmt self-hosting ratchet (issue #786) + bench smoke + check smoke + library-mode probes + diagnostics-collected fixtures + negative-space fixtures + stdlib modules compile clean + independence oracle (#962 soundness gate) + package-mode harness (issue #569) + whole-compiler c-modular link (issue #1012) + private-type shadow audit + private-record shadow audit + canonical-only alias audit + M:N determinism (N=1==N=4) + kai info smoke + kai doc smoke"

# CI sharding (docs/ci-time-analysis.md §7). tier1's ~15-min light-fixture
# grout dominates the PR critical path; it is CPU-bound + independent, so we
# split it across SEPARATE runners (each with its own memory bus — in-job
# `-j` is bandwidth-capped). These four shards PARTITION the `tier1` work:
# every phase of `tier1` above appears in exactly one shard, so the union is
# the full gate with identical coverage. The CI workflow runs them in
# parallel on a shared pre-built kaic2 and an aggregator job (`tier1`)
# gates on all four — the Required check name is unchanged.
#
#  shard 1 — the 4 GB self-compiles + stateful caches (memory-bound), then,
#            after they free their RSS, demos + every non-light phase. These
#            are CPU-light, so the runner that finishes the costly compiles
#            fastest absorbs the tail instead of sitting idle.
#  shard 2 — light slice 1/3.
#  shard 3 — light slice 2/3.
#  shard 4 — the two whole-compiler modular self-hosts. Each is a full
#            compiler self-compile (the slowest single phases in the gate);
#            isolating them keeps every shard's wall-clock near the light
#            slices instead of one shard dominating the critical path.
#  shard 5 — light slice 3/3.
# Why the non-light tail lives on shard 1, NOT on a light shard: an earlier
# 3-way light split measured worse, but the culprit was the tail + demos
# piled onto a light slice, not the round-robin itself. Round-robin splits
# by target COUNT, not cost, and the families differ ~50x (stdlib 158
# fixtures vs 1-fixture targets) — if the slices measure unbalanced, weight
# the split by measured cost instead of raising SHARDS further.
#
# Coverage invariant (do not break): the set
#   { test-costly-parallel, test-heap-limit, test-user-cache,
#     test-core-cache, test-modular-selfhost, test-perceus-1131-modular-escape,
#     light(1/3), light(2/3), light(3/3),
#     demos-no-regression, test-fmt, test-fmt-selfhost, test-bench,
#     test-check, test-library-mode, test-diagnostics-collected,
#     test-negative, test-stdlib-modules, test-packages,
#     test-private-type-shadow-audit, test-private-record-shadow-audit,
#     test-canonical-aliases, test-info, test-doc,
#     test-upgrade-resolver }
# equals exactly the prerequisites of `tier1` (the three light slices union
# to TEST_LIGHT_TARGETS, proven by the round-robin partition in
# stage2/Makefile). Adding a phase to `tier1` means adding it to a shard.
tier1-shard-1: kaic2
	$(MAKE) -C stage2 test-costly-parallel
	$(MAKE) -C stage2 test-heap-limit
	$(MAKE) -C stage2 test-user-cache
	$(MAKE) -C stage2 test-core-cache
	$(MAKE) demos-no-regression
	$(MAKE) test-fmt test-fmt-selfhost test-bench test-check test-typecheck test-check-parity test-library-mode test-diagnostics-collected test-negative test-stdlib-modules test-independence-oracle test-packages test-private-type-shadow-audit test-private-record-shadow-audit test-canonical-aliases test-info test-doc test-upgrade-resolver
	@echo "tier1-shard-1 OK — costly self-compiles + caches + demos + non-light tail (fmt/bench/check/negative/stdlib-modules/audits/info/doc/upgrade-resolver)"

tier1-shard-2: kaic2
	$(MAKE) -C stage2 test-light-shard SHARD=1 SHARDS=3
	@echo "tier1-shard-2 OK — light slice 1/3"

tier1-shard-3: kaic2
	$(MAKE) -C stage2 test-light-shard SHARD=2 SHARDS=3
	@echo "tier1-shard-3 OK — light slice 2/3"

tier1-shard-4: kaic2
	$(MAKE) -C stage2 test-modular-selfhost
	$(MAKE) -C stage2 test-perceus-1131-modular-escape
	@echo "tier1-shard-4 OK — whole-compiler c-modular link + #1131 modular-escape gate"

tier1-shard-5: kaic2
	$(MAKE) -C stage2 test-light-shard SHARD=3 SHARDS=3
	@echo "tier1-shard-5 OK — light slice 3/3"

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
# regression. Needs kaic2 (the extractor) and bin/kai (the wrapper).
test-doc: kaic2
	@tools/test-doc.sh

# `kai upgrade` / install.sh tag resolver (pure shell, no compiler).
# Guards tag-based version discovery, rate-limit handling, and the
# minified-JSON greedy-sed trap against regression.
test-upgrade-resolver:
	@tools/test-upgrade-resolver.sh

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

# Tongariki — `kai fmt` fixture suite. Verifies that every fixture
# in examples/fmt/ formats to its `.expected.kai` and is idempotent,
# plus that every formattable example under examples/minimal/ /
# quickstart/ / phase4/ round-trips through the formatter without
# breaking re-parse. Cheap enough to gate every Tier 1 run.
test-fmt: kaic2
	@./tests/fmt_fixtures.sh

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
	./bin/kai typecheck examples/typecheck/clean.kai --diags-json > /tmp/kaikai-typecheck-dj.out 2>&1 \
	  && grep -q '"diagnostics": \[\]' /tmp/kaikai-typecheck-dj.out \
	  || { echo "test-typecheck FAIL — --diags-json mount broken"; cat /tmp/kaikai-typecheck-dj.out; exit 1; }; \
	./bin/kai typecheck examples/typecheck/clean.kai --holes-json > /tmp/kaikai-typecheck-hj.out 2>&1 \
	  && grep -q '^\[\]$$' /tmp/kaikai-typecheck-hj.out \
	  || { echo "test-typecheck FAIL — --holes-json mount broken"; cat /tmp/kaikai-typecheck-hj.out; exit 1; }; \
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
	          def_at_lambda_param; do \
	  src="examples/library_mode/$$fx.kai"; \
	  exp="examples/library_mode/$$fx.out.expected"; \
	  out=$$(mktemp); \
	  "$$root/stage2/kaic2" --library-mode "$$src" > "$$out" 2>/dev/null \
	    || { echo "library-mode $$fx FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	  diff -q "$$exp" "$$out" > /dev/null \
	    || { echo "library-mode $$fx DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	  rm -f "$$out"; \
	  echo "library-mode $$fx OK"; \
	done; \
	src="examples/library_mode/def_at_imports.kai"; \
	exp="examples/library_mode/def_at_imports.out.expected"; \
	out=$$(mktemp); \
	"$$root/stage2/kaic2" --path examples/library_mode --library-mode "$$src" > "$$out" 2>/dev/null \
	  || { echo "library-mode def_at_imports FAIL (kaic2 exit)"; rm -f "$$out"; exit 1; }; \
	diff -q "$$exp" "$$out" > /dev/null \
	  || { echo "library-mode def_at_imports DIFF"; diff "$$exp" "$$out"; rm -f "$$out"; exit 1; }; \
	rm -f "$$out"; \
	echo "library-mode def_at_imports OK"

# Issue #487 — `--diags-json` regression. Each fixture under
# examples/library_mode/diags_*.kai compiles a deliberately-broken
# source; the collected [Diagnostic] is serialised to JSON and diffed
# against the .diags.expected golden. The fixtures pin the m11 v1 +
# v1.x template wording (T1, T2, T3, T4, T5) so an unintended
# refactor that changes the user-facing message fails this gate
# before LSP-side consumers (#447) drift.
test-diagnostics-collected: kaic2
	@set -e; \
	root=$$(pwd); \
	cd "$$root"; \
	for fx in diags_t1_type_mismatch diags_t2_non_exhaustive \
	          diags_t3_unbound_name diags_t4_wrong_arity \
	          diags_t5_missing_effect diags_multiple_errors; do \
	  src="examples/library_mode/$$fx.kai"; \
	  exp="examples/library_mode/$$fx.diags.expected"; \
	  out=$$(mktemp); \
	  "$$root/stage2/kaic2" --diags-json "$$src" > "$$out" 2>/dev/null \
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

# Tier 2.5 — daily memory-safety gate. Rebuilds the demos/ probe set
# with `-fsanitize=address,undefined` and runs each binary; fails on
# any sanitizer diagnostic or if the demos baseline regresses under
# instrumentation. Apple clang lacks LSAN support, so leak detection
# stays disabled (`detect_leaks=0`) for portability with the Linux
# runner; if a leak ratchet ever becomes useful, gate it separately
# on Linux.
#
# Why daily and not per-PR: ASAN doubles compile + run wall, and the
# value is structural — catch new UAF / UB regressions of the R10 /
# R11 shape (heap-use-after-free in handler dispatch / Perceus
# scopes) within 24h of merge — not gating. CI: invoked from
# `make daily` so `.github/workflows/daily.yml` picks it up
# automatically.
tier1-asan: kaic2 test-arena
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
	@$(MAKE) -C stage2 test-mn-sigaltstack-asan
	@echo "tier1-asan OK — sigaltstack fixture passes under ASAN+UBSan at KAI_THREADS=1/2/8/16 (alternate-stack ownership gate)"
	@$(MAKE) -C stage2 test-trace-asan
	@echo "tier1-asan OK — trace fixtures pass under ASAN+UBSan (R10/R11 regression gate)"
	@$(MAKE) -C stage2 test-runtime-shadow-asan
	@echo "tier1-asan OK — issue #78 fixture passes under ASAN+UBSan (runtime helper rename gate)"
	@$(MAKE) -C stage2 test-signal-trap-asan
	@echo "tier1-asan OK — issue #107 fixture passes under ASAN+UBSan (Signal effect runtime gate)"
	@$(MAKE) -C stage2 test-log-asan
	@echo "tier1-asan OK — issue #141 fixture passes under ASAN+UBSan (Log default handler / clock_gettime / strftime gate)"
	@$(MAKE) -C stage2 test-trap-exit-cancel-asan
	@echo "tier1-asan OK — issue #103 fixture passes under ASAN+UBSan (trap-exit / outer Cancel handler gate)"
	@$(MAKE) -C stage2 test-process-basic-asan
	@echo "tier1-asan OK — issue #126 fixture passes under ASAN+UBSan (Process effect runtime gate)"
	@$(MAKE) -C stage2 test-perceus-issue82-asan
	@echo "tier1-asan OK — issue #82 fixture passes under ASAN+UBSan (Perceus leak-audit gate)"
	@$(MAKE) -C stage2 test-ffi-extern-c-asan
	@echo "tier1-asan OK — m12.7.x FFI fixture passes under ASAN+UBSan (extern_c shim memory gate)"
	@$(MAKE) -C stage2 test-perceus-issue118-asan
	@echo "tier1-asan OK — issue #118 fixtures pass under ASAN+UBSan (Perceus reuse-in-place gate)"
	@$(MAKE) -C stage2 test-perceus-issue298-asan
	@echo "tier1-asan OK — issue #298 fixture passes under ASAN+UBSan (closure capture lifecycle gate)"
	@$(MAKE) -C stage2 test-perceus-issue350-asan
	@echo "tier1-asan OK — issue #350 fixtures pass under ASAN+UBSan (arm-binding multi-use drop gate)"
	@$(MAKE) -C stage2 test-perceus-trmc-spread-asan
	@echo "tier1-asan OK — TRMC shared pass-through arg fixture passes under ASAN+UBSan (dropmask / wrap-skip alignment gate)"
	@$(MAKE) -C stage2 test-perceus-issue703-asan
	@echo "tier1-asan OK — issue #703 fixture passes under ASAN+UBSan (UFn-body let-bound variant double-match gate)"
	@$(MAKE) -C stage2 test-issue-779-asan
	@echo "tier1-asan OK — issue #779 fixture passes under ASAN+UBSan (multi-stmt block raw-tail drop-threading gate)"
	@$(MAKE) -C stage2 test-perceus-enum-slot-asan
	@echo "tier1-asan OK — enum-slot fixture passes under ASAN+UBSan (enum-as-int extract/re-intern soundness gate)"
	@$(MAKE) -C stage2 test-perceus-int-cache-asan
	@echo "tier1-asan OK — Phase 1.A fixture passes under ASAN+UBSan (widened small-int cache pinned-slot gate)"
	@$(MAKE) -C stage2 test-int-field-inline-asan
	@echo "tier1-asan OK — i64-inline variant Int-field battery passes under ASAN+UBSan (raw-binder soundness gate)"
	@$(MAKE) -C stage2 test-perceus-nested-reuse-asan
	@echo "tier1-asan OK — nested-pattern reuse fixture passes under ASAN+UBSan (balance rotation outer+inner cell reuse gate)"
	@$(MAKE) -C stage2 test-match-pbind-catchall-asan
	@echo "tier1-asan OK — issue #91 fixture passes under ASAN+UBSan (PBind catch-all fast-path raw-local gate)"
	@$(MAKE) -C stage2 test-http-client-asan
	@echo "tier1-asan OK — net.http client fixture passes under ASAN+UBSan (Tier S1 lane #3 gate)"
	@$(MAKE) -C stage2 test-stdlib-crypto-asan
	@echo "tier1-asan OK — issue #139 fixture passes under ASAN+UBSan (crypto byte-buffer + 64-bit modular gate)"
	@$(MAKE) -C stage2 test-stdlib-regex-predicate-asan
	@echo "tier1-asan OK — issue #85 fixture passes under ASAN+UBSan (regex sigil + matches predicate runtime gate)"
	@$(MAKE) -C stage2 test-env-mutate-asan
	@echo "tier1-asan OK — issue #127 fixture passes under ASAN+UBSan (Env set/unset/vars buffer-ownership gate)"
	@$(MAKE) -C stage2 test-securerandom-asan
	@echo "tier1-asan OK — issue #140 fixture passes under ASAN+UBSan (SecureRandom default-handler gate)"
	@$(MAKE) -C stage2 test-perceus-1151-vec-push-growth-asan
	@echo "tier1-asan OK — issue #1151 fixtures pass under ASAN+UBSan (Vec push growth-at-capacity gate)"
	@$(MAKE) -C stage2 test-perceus-1153-modcall-linear-asan
	@echo "tier1-asan OK — issue #1153 fixtures pass under ASAN+UBSan (module-call linearity gate)"
	@$(MAKE) -C stage2 test-perceus-1150-vec-surface-asan
	@echo "tier1-asan OK — issue #1150 fixtures pass under ASAN+UBSan (Vec slices / minting / collect)"
	@$(MAKE) -C stage2 test-perceus-1180-range-lazy-asan
	@echo "tier1-asan OK — issue #1180 fixture passes under ASAN+UBSan (lazy-range norm / fallback gate)"
	@$(MAKE) -C stage2 test-perceus-1295-borrow-slot-nested-arg-asan
	@echo "tier1-asan OK — issue #1295 fixture passes under ASAN+UBSan (borrowed-slot nested-arg use-count gate)"
	@$(MAKE) -C stage2 test-perceus-1303-single-use-branch-leak-asan
	@echo "tier1-asan OK — issue #1303 fixture passes under ASAN+UBSan (branchy single-use param exit-drop gate)"
	@$(MAKE) -C stage2 test-perceus-1315-borrowed-match-release-asan
	@echo "tier1-asan OK — issue #1315 fixtures pass under ASAN+UBSan (borrowed-scrutinee release gate)"
	@$(MAKE) -C stage2 test-perceus-1328-selftail-borrowed-local-asan
	@echo "tier1-asan OK — issue #1328 fixtures pass under ASAN+UBSan (multi-use borrowed-local exit-drop gate)"
	@$(MAKE) -C stage2 test-issue-1331-op-arg-release-asan
	@echo "tier1-asan OK — issue #1331 fixture passes under ASAN+UBSan (op-arg + tail-read-local release gate)"
	@$(MAKE) -C stage2 test-perceus-1355-closure-temp-release-asan
	@echo "tier1-asan OK — issue #1355 fixture passes under ASAN+UBSan (closure temp in borrowed HOF slot, no double-free)"
	@$(MAKE) -C stage2 test-perceus-1324-char-binder-goto-asan
	@echo "tier1-asan OK — issue #1324 fixture passes under ASAN+UBSan (Char-binding match on the tcrec goto ledger)"
	@$(MAKE) -C stage2 test-perceus-1395-char-param-raw-asan
	@echo "tier1-asan OK — issue #1395 fixture passes under ASAN+UBSan (raw Char param, fresh box per consuming use)"
	@$(MAKE) -C stage2 test-issue-1394-byte-box-asan
	@echo "tier1-asan OK — issue #1394 fixture passes under ASAN+UBSan (Byte literal boxes through the Byte constructor)"
	@$(MAKE) -C stage2 test-perceus-1410-byte-raw-ops-asan
	@echo "tier1-asan OK — issue #1410 fixture passes under ASAN+UBSan (raw Byte op family, fresh box per boxed border)"
	@$(MAKE) -C stage2 test-perceus-1457-fixed-raw-ops-asan
	@echo "tier1-asan OK — issue #1457 fixture passes under ASAN+UBSan (raw Int32/UInt32/UInt64 op family, fresh box per boxed border)"
	@$(MAKE) -C stage2 test-perceus-1464-fixed-raw-ops-asan
	@echo "tier1-asan OK — issue #1464 fixture passes under ASAN+UBSan (raw Int128 op family, boxed div/mod, fresh box per boxed border)"
	@$(MAKE) -C stage2 test-issue-1331-borrowed-op-arg-asan
	@echo "tier1-asan OK — issue #1331 fixture passes under ASAN+UBSan (borrowed binder into an op arg)"

# Backend-parity: build every entry-point fixture under the documented
# example dirs + demos with the native backend AND the C-direct oracle,
# run, diff stdout + exit code. This is the same gate the tier1-native
# CI workflow runs (with NATIVE_PARITY_RATCHET=1 to forbid new gaps); the
# llvm-text backend it once also covered was removed. Local convenience
# target — not invoked from `tier1` because the cost (~15-30 min) does
# not belong on every PR locally. SKIPs when kaic2 lacks libLLVM (build
# it with `make -C stage2 KAI_LLVM=1`).
tier1-backend-parity: kaic2
	@TARGET_BACKEND=native ORACLE_BACKEND=c NATIVE_PARITY_RATCHET=1 tools/test-backend-parity.sh

# Tier 2: daily / nightly. ~10-20 min. Runs once a day on `main` HEAD,
# not per-PR. If it fails, `main` stays unbroken (Tier 0/1 gated every
# commit) but a diagnostic opens a lane the next morning.
#
# `tier1-asan` and `tier1-backend-parity` are deliberately NOT in this
# target: both already run path-gated on PRs (and thus on every merge to
# main that touches their paths), and together they pushed the cron past
# its 30-min budget — every scheduled run was silently `cancelled` at the
# timeout, producing no diagnostic at all. Keep the daily under its budget
# so it actually completes; the two heavy gates keep their per-PR coverage.
daily: tier1 stress-fixtures coverage-probe rc-budget test-binserialize-budget
	@echo "daily OK — tier1 + stress fixtures + coverage probe + RC budget + BinSerialize budget"

# Stress fixtures: closed regressions that exercise patterns the
# per-feature suite does not — R3 scrutinee-reuse RC
# (interp_recursive_walk) and m4c Phase 3 polymorphic flow-through
# (m4c_flow_through). Hard gates: a failure here fails the daily.
stress-fixtures: kaic2
	@set -e; \
	for f in examples/effects/interp_recursive_walk.kai \
	         examples/effects/m4c_flow_through.kai; do \
	  name=$$(basename $$f .kai); \
	  stage2/kaic2 $$f > /tmp/stress-$$name.c 2> /tmp/stress-$$name.err \
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
	@KAI_TRACE_RC=1 KAI_THREADS=1 stage2/kaic2 stage2/compiler.kai > /dev/null 2> /tmp/rc.log; \
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
