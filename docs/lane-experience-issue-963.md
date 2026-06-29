# Lane experience — issue #963 Part A (cross-module monomorphization home-TU discipline)

**Slice, not closure.** This lane is `refs #963`, the first implementable slice of
Part A. It deliberately PUNTS `linkonce_odr`/COMDAT, the module-boundary signature
(`.kaii`) carrying the Perceus RC/ownership ABI, and Part B (per-module LLVM separate
compilation). What it ships is small on purpose — a behaviour-locking regression plus a
design verdict — because the design phase found the literal slice was already implemented.

## Scope as planned vs as shipped

- **Planned:** change `monomorph.kai`/`emit_c.kai` so each cross-module monomorph spec's
  body is emitted into exactly one home translation unit, externally linked, with its
  prototype in the shared whole-program `kai_decls.h`. The brief's premise was that "today
  specs effectively ride the root/whole-program TU."
- **Shipped:** no compiler-source change. The premise is false against HEAD: cross-module
  specs already land exactly once in the **defining module's** TU, not in `__root__.c`.
  The lane instead (1) records the design verdict that blesses the de-facto rule, (2) adds
  a locking regression fixture for the one-spec/one-TU/links-and-runs invariant that no
  existing fixture covered, and (3) opens follow-ups for the genuinely-deferred work,
  with the dependency hierarchy corrected per the `asu` consult.

## Empirical finding — the slice is already implemented

Built `kaic2` and ran the canonical case: a generic `box[T]` defined in user module `lib`,
instantiated at `Int` from two distinct user modules (`useb`, `usec`; neither imported by
root directly). Under `--emit=c-modular`:

- The spec body `kai_lib__box__mono__Int` is emitted **exactly once**, in the **`lib.c`** TU
  (the generic's defining module), NOT in `__root__.c`.
- Its prototype is in the shared `kai_decls.h`; `useb.c` and `usec.c` each call it cross-TU,
  resolved through that prototype at link time.
- The stdlib generic in the same program, `list.sum[Int]` → `kai_list__sum__mono__Int`,
  lands once in `list.c` likewise.
- The whole program links and runs under `KAI_MODULAR=1` with no duplicate-symbol errors;
  a transitive case (a spec body calling another generic in the same module) also links and
  runs. The only mis-attributing shape (a root-file `pub` generic instantiated from importing
  modules) is rejected earlier as an **import cycle**, so it cannot occur.

**Mechanism** (three existing pieces, converged by #748 + #898 + #962, which the #963 text
predates):

1. `specialise_decl` (monomorph.kai) rebuilds the spec `DFn` **preserving the defining poly
   decl's `mo`** — the spec carries the home module of the generic it specialises.
2. `emit_program_modular` (emit_c.kai) partitions the whole-program `[Decl]` by `mo`:
   `None` → `__root__.c`, each `Some(m)` → `m.c`.
3. `kai_decls.h` is whole-program, so every prototype is visible in every TU.

Monomorphization is whole-program and runs **before** partitioning (the MLton model, not the
C++/header-template model): there is exactly one spec per `(mangled-name, mo)` by
construction, so there is nothing for `linkonce_odr`/COMDAT to dedup.

## asu verdict (recorded)

**1. Home-TU rule — bless the two-regime rule; reject the lexicographic fallback.**
The de-facto rule is two regimes, not one:

- *Non-shared generic* (one defining module): spec hosted in the defining module via
  preserved `mo`. Single-valued by construction — no N-module ambiguity to break.
- *Shared name across modules* (the #898 path, `name_shared` in monomorph.kai): spec hosted
  in the **caller** module (`mo = caller`), duplicating identical bodies under distinct
  mangled names (`kai_<caller>__name__mono__T`). No ODR clash — the symbols differ.

The brief's candidate "lexicographically-least module on ambiguity" is not merely unnecessary
— it would be **actively unsound** in the shared case: it would dedup to a single home a
symbol that two importers legitimately resolved to *different* definitions. Bless
defining-module (non-shared) + per-caller duplication (shared); reject the lexicographic
tiebreak explicitly. The one invariant to guard: **no cross-module spec may carry
`mo = None`** (it would fall into `__root__.c` and invert the module→root dependency DAG that
Part B needs). Today the only door to that state is closed (import-cycle rejection); it is
worth a fixture.

**2. Soundness without `linkonce_odr` — confirmed.** One definition per `(name, mo)` ⇒
flat external linkage suffices. No spec's borrow discipline can differ by caller module:
(a) monomorphization erases the caller (type args, including units, are in the `MT` key);
(b) Perceus borrow signatures are caller-independent by construction (bottom-up fixpoint over
the callee body, keyed `(name, mo)`); (c) the caller-sensitive part (call-site dup/drop
adaptation) is already emitted per-caller, in each caller's own TU. Reinforcing this: `drop`
goes through the generic runtime `kai_drop` over uniform boxed `KaiValue *` — there is no
per-type destruction glue emitted in a TU — so a spec hosted in `lib.c` that drops a value of
a type defined in `useb` needs no `useb` symbol.

The corollary that must be pinned: this soundness **rests on Perceus being recomputed
whole-program every compile**. The `.kaii` RC/ownership-ABI is therefore not a punted
nice-to-have — it is a **hard prerequisite of Part B**. The moment Part B compiles a caller
against a serialized borrow signature that no longer matches the callee's emitted discipline,
the ABI-skew bug class that does not exist today reappears. Follow-up hierarchy:
`.kaii` RC-ABI **blocks** Part B; `linkonce_odr` blocks nothing.

**3. A real (likely latent) linkage-fragility gap — not attribution, not duplication.**
External linkage is decided by a fragile textual whitelist in `bin/kai` (~line 1246): an
awk strip of the leading `static ` gated on a closed set of return-type prefixes
(`KaiValue *`, `int64_t`, `double`, `int`, `uint32_t`). But `raw_c_type` (emit_c.kai) also
emits `void *` (for `TyHandle`). A cross-module generic returning an unboxed type outside
that set would stay file-local `static` and fail the cross-TU link with an undefined symbol.
We could not confirm `Handle` is constructible in a user-level cross-module generic today, so
the bug is **latent, not live** — but the whitelist is incomplete-by-construction against
`raw_c_type`'s range, and any future lane adding an unboxable return type (Float32, a small
struct-by-value, exposing `Handle` in stdlib FFI) trips it silently. The underlying smell:
the linkage decision is in the wrong place — the emitter knows `mo` and the whole-program call
graph and should emit external linkage for cross-module symbols directly, instead of always
emitting `static` and having a downstream textual heuristic revert it for a hardcoded list.
A second, link-safe debt: lifted lambdas/clauses go to `__root__.c` unconditionally, so a
cross-module spec whose body contains a lambda inverts the module→root dependency (it links
under Part A's single final link, but Part B must resolve it).

This lane does **not** ship a speculative whitelist change: the gap is untestable today (no
failing fixture can be written for an unconstructible shape), and externalizing a `static`
that should stay file-local is its own hazard. It is recorded here and filed as a follow-up
with a precise repro recipe instead. The reachable unboxed return paths (`int64_t`, `double`)
are anchored by the fixture below.

## Fixtures added

`examples/multi-module/issue-963-cross-module-mono/` — one generic, two instantiating modules,
same types, root reaching the specs only across module boundaries:

- `boxer.box[T] : [T]` instantiated at `Int` from `feature_b` and `feature_c` → one boxed
  (`KaiValue *`) spec in `boxer.c`; plus `list.sum[Int]` → one `int64_t` spec in `list.c`.
- `boxer.pick[T] : T` instantiated at `Real` from both → one unboxed `double` spec in
  `boxer.c`, anchoring the unboxed cross-TU return path asu flagged.

Wired both ways against one golden, which pins default-path == modular-path:

- `test-modular-build` (existing harness, `MODULAR_FIXTURES`) splits → compiles each `.c` →
  links → runs → diffs. The link step IS the one-spec assertion: a second definition would be
  a duplicate symbol, a missing one an undefined symbol.
- `test-issue-963-cross-module-mono` (new) builds the same fixture single-TU (default path),
  runs, diffs the SAME golden. Both targets are in tier1's `TEST_LIGHT_TARGETS`.

## What was punted, and why (follow-ups)

Ordered by the corrected dependency hierarchy:

1. **`.kaii` module-boundary signature carrying the Perceus RC/ownership ABI** — hard
   prerequisite of Part B (separate compilation cannot recompute Perceus whole-program).
2. **Part B — LLVM per-module separate compilation**, built on the KIR lowering; depends on (1).
3. **Linkage-decision fragility** — move the external-linkage decision into the emitter (it
   knows `mo` + the whole-program call graph), or at minimum make the `bin/kai` whitelist
   exhaustive against `raw_c_type` with a guard that fails on a new unhandled return type;
   also resolve the lambda→root dependency inversion.
4. **`linkonce_odr`/COMDAT** — only relevant if kaikai ever moves to the C++/header model of
   re-instantiating generics per use site; irrelevant under whole-program mono, blocks nothing.

## Cost vs estimate, and notes for next lanes

The estimate assumed an implementation lane (touch `monomorph.kai`/`emit_c.kai`). The real
work was investigation + a design verdict: the implementation was already in the tree, so the
honest deliverable is a behaviour-lock plus the verdict that future refactors of
`specialise_decl`'s `mo` handling or `emit_program_modular`'s partition must not regress.
Selfhost byte-identity is trivially intact — zero compiler/stdlib `.kai` changed; the diff is
the fixture, the Makefile wiring, and this note. The single most load-bearing thing a next
lane should carry forward: the `.kaii` RC-ABI is a Part B blocker, not an optimization, and
the linkage decision wants to move out of `bin/kai`'s awk and into the emitter.
