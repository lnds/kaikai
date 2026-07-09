# Lane experience — issue #1139: purity/aliasing attributes on emitted functions

## Scope as planned vs as shipped

**Planned** (issue): rows nominate purity candidates, the post-perceus
lowered body confirms; stamp `nounwind` + `memory(none)` on native LLVM
functions and `__attribute__((const))` on C functions; never `willreturn`;
binary acceptance via IR-grep + CSE check; coverage table over the
self-compiled compiler; wall on a CPU-bound bench.

**Shipped**: all of it, plus three fixes the gates forced (below). Out of
scope by design, recorded in `purity.kai`'s module doc: `memory(read)` /
`__attribute__((pure))` and return `noalias` — both need an aliasing
oracle (a "read-only" body can still write through `kai_op_field`'s
incref; a "fresh" return can be a recycled reuse cell), a separate lane.

## Mechanism

- `stage2/compiler/purity.kai` (new, ~180 LOC of code): a pure side-table
  pass over the post-perceus/post-tcrec `[Decl]`, the `rawsafe.kai`
  pattern. A candidate must have an all-raw ABI (`classify_unbox_sig`,
  every param + return an unboxed scalar) AND a lowered body with no
  allocation, no RC op, no effect op, no pointer read. Interprocedural
  verdict by least fixed point from `PImpure` bottom, so a recursive SCC
  with one impure member never confirms itself.
- The `__kai_tcrec|` self-tail-call sentinel counts as memory-clean (the
  fn's own back-edge), so tail-recursive scalar loops — kaikai's loop
  idiom — confirm const. A `__kai_trmc|` step allocates and stays impure.
- Native: `kai_llvm_add_nounwind` / `kai_llvm_add_memory_none` runtime
  helpers on the byval/sret enum-attribute pattern; stamped in
  `nemit_declare_fns`. `nounwind` broadly (kaikai has no EH unwinding;
  traps abort), `memory(none)` only on confirmed fns, `willreturn` never
  (divergence is not modeled — a `memory(none)` fn that traps must
  survive DCE). FFI shims get nothing.
- C: `KAI_CONST` (= `__attribute__((const))` under GCC/Clang) prefixed on
  BOTH the forward decl and the definition through one helper, same
  `c_sym`. Whole-program and c-modular both stamp; under c-modular the
  shared `kai_decls.h` carries the same promise as each definition —
  `((const))`, unlike reuse donation, does not change the ABI.

## What the gates caught — three finisher fixes

The three feature commits were complete and green on the light gates. The
heavy oracles then surfaced three distinct defects, each invisible to a
single-TU C-backend run:

1. **Missing native-shim forwarders.** The two new LLVM prims were
   registered in stage1 + stage2 + runtime.h, but not forwarded in
   `stage0/runtime_llvm_native_shim.c` — the natively-built compiler
   failed to link. Two-line fix on the existing forwarder pattern.
2. **Native miscompile of the mark filter (the scary one).**
   `purity_const_syms` filtered `PConst` marks with a cons pattern that
   discriminates a nullary variant in a nested slot
   (`[PM(n, mo, PConst), ...rest]`). The native backend does not
   discriminate that nested tag — the first arm matches both tags — so
   the NATIVELY-built compiler extracted every mark as const and stamped
   `((const))` on every function it emitted, `println` included: silent
   wrong-answers in every downstream program. `test-native-selfhost-gate`'s
   byte-identity oracle caught it (746 stamps vs the oracle's 90 on the
   probe). Worked around by binding the verdict and testing it with a
   predicate; the backend bug is minuted with a 20-line two-backend repro
   for its own issue (kept out of this lane per one-worktree-one-fix).
   The `--dump-purity` golden could never see this: the analysis was
   correct; only the natively-executed *filter* over its output lied.
3. **c-modular linkage stripping missed the prefixed form.**
   `externalize_modular_linkage` drops a column-0 `static ` to give
   cross-TU symbols external linkage; `KAI_CONST static ...` no longer
   matched, confirmed-pure pimpls stayed TU-local, and the modular
   selfhost link failed with undefined `__pimpl_*` symbols. Fix: strip
   the linkage keyword, keep the attribute.

Lesson repeated from #1127 and now twice confirmed: **single-TU green is
not sound**. The native selfhost byte-identity gate and the modular
selfhost link are different oracles from the fixture suite, and both bit
on this lane. Any pass whose output feeds the emitters must be validated
by executing the compiler ON both backends, not only by compiling
fixtures WITH both backends.

## Coverage — self-compiled compiler

Confirmation over the compiler itself (stage2 `main.kai`, whole program
with stdlib):

| Attribute | Functions | Of total emitted |
|---|---|---|
| native `nounwind` + `memory(none)` | 77 | 13,556 defines (0.57%) |
| native `nounwind` only | 6,519 | 48.1% |
| native no attribute (FFI shims, thunks, lambdas) | 6,960 | 51.3% |
| native `willreturn` | 0 | — |
| C `KAI_CONST` | 77 | 13,607 defs (0.57%) |

The 77 are the same set on both backends (one confirmation table): char
classifiers, scalar `Numeric`/`Ord`/`Sub`/`Rem` pimpls, `int_pow` loops,
`list.int_cmp`. The low rate is expected — the compiler is
allocation-heavy; the all-raw ABI gate excludes anything touching a boxed
value, and that is most of a compiler.

## Wall — CPU-bound benches (before = origin/main kaic2, after = this lane)

Method: same source, one binary per (compiler × backend) cell, identical
outputs verified across all cells, median of 7 runs on an otherwise idle
mac (arm64, cc -O2).

| Bench | Build | Before | After | Delta |
|---|---|---|---|---|
| rb-tree 1M inserts | C, single-TU | 0.43 s | 0.46 s | flat (jitter; run ranges overlap) |
| rb-tree 1M inserts | native | 0.63 s | 0.62 s | flat |
| pure-call loop 3×1e8 calls | C single-TU / native | 0.00 s | 0.00 s | flat — fully folded both sides |
| pure-call loop 3×1e8 calls | **C c-modular (cross-TU)** | **0.40 s** | **0.00 s** | **calls hoisted + loop folded** |

Honest reading, and it matches the issue's "shape-dependent" caveat:

- **rb-tree is flat.** Its hot path is boxed and allocating — impure by
  construction — and the one confirmed-const fn in it (`lcg_next`) takes
  a fresh argument every iteration, so there is nothing to hoist or CSE.
- **Single-TU is flat.** With the callee's body visible, `cc -O2` / LLVM
  inline and fold the pure loop to a constant with or without the
  attribute — whole-program inlining subsumes `((const))`.
- **The cross-TU shape is the win.** Under c-modular the callee's body is
  invisible to the calling TU; the `KAI_CONST` on the shared `kai_decls.h`
  prototype is the ONLY purity information `cc` has. Before: 3×1e8 opaque
  calls (0.40 s). After: the invariant calls hoist out of the loop and the
  accumulation folds to closed form (0.00 s). This is precisely the
  boundary the attribute was designed to cross, and it required fix 3 to
  even link.

## Fixtures

- `examples/perceus/purity_scalar.kai` + `.out.expected` — `--dump-purity`
  golden pinning the confirmation shapes (raw arithmetic, divide,
  interprocedural call, recursive self-call → const; alloc / boxed param /
  effect → impure). Rides `test-purity` in tier1's light targets.
- `examples/perceus/purity_licm.kai` + `.out.expected` — binary
  acceptance: `memory(none)` present on `heavy` and absent on allocating
  `noisy` in the native IR, no `willreturn` anywhere, and `opt -O2` CSEs a
  duplicated pure call to one. `test-native-purity-attrs`, gated to
  tier1-native (needs the IR dump + `opt`).
- Coverage gap left open: no fixture pins the c-modular `KAI_CONST`
  externalization (fix 3); `test-modular-selfhost` exercises it end to end
  on every tier1 run, which is the gate that caught it.

## Cost vs estimate

Predecessor session: the three feature commits plus fixtures, green on
light gates. Finisher session: rebase over #1140/#1142 (three-way
Makefile list conflicts), the three fixes above, coverage + bench, this
retro. The two silent-miscompile hunts dominated the finisher's cost;
both were localized quickly because each oracle names its divergence
(byte-diff of emitted C; undefined symbols at link).

## Follow-ups

- Native backend: nested nullary-variant tag in a cons pattern arm is not
  discriminated (fix 2's root cause) — needs its own issue + lane; the
  workaround in `purity_const_syms` can revert to the nested pattern once
  fixed.
- `memory(read)` / `((pure))` and return `noalias` once an aliasing
  oracle exists.
- Recognize more memory-clean runtime builtins (the whitelist is three
  entries: `kai_intf`, `kai_realf`, `kai_boolf`) if profiles show
  confirmed-const coverage is leaving wins on the table.
