# Bootstrap: relax byte-identical fixed point to per-compiler determinism (2026-05-22)

## Decision

Replace the byte-identical fixed point gate that conflates `stage1` and
`stage2` output with a per-compiler determinism gate. The current
`make selfhost` chain stays in place, but the invariant being checked is
explicitly narrowed: `kaic2b` (stage2 compiled by stage2-via-stage1)
must produce the same C as `kaic2c` (stage2 compiled by itself again).
The implicit equivalence between `kaic2a` (stage2 compiled by stage1)
and `kaic2b` is dropped.

This is Option C as presented to the user on 2026-05-22 after a
three-way review by Linus, asu, and Eric. Linus and asu recommended C
directly. Eric recommended B (drop the fixed point entirely and rely on
tier1). The compromise lands on C because it preserves the canary that
detects non-determinism (hashmap iteration order, leaked pointer
addresses, etc.) at the point where it is genuinely cheap to keep, and
loses only the part of the invariant that was structurally false from
the start.

## Context

### What the gate has been doing

The selfhost chain produces three compilers from one source file:

```
stage2/compiler.kai ──[ kaic1   ]──> kaic2a.c ──[ cc ]──> kaic2a
stage2/compiler.kai ──[ kaic2a  ]──> kaic2b.c ──[ cc ]──> kaic2b
stage2/compiler.kai ──[ kaic2b  ]──> kaic2c.c
```

The CI gate is `diff -q kaic2b.c kaic2c.c`. Two compilers must agree:

1. `kaic2a → kaic2b`: stage1 wrote kaic2a; kaic2a writes kaic2b. If
   stage1 and stage2 take the same emission decisions, the second
   pass stabilises here.
2. `kaic2b → kaic2c`: kaic2b is already "real stage2". A second run of
   the same compiler over the same source should be byte-deterministic.

Although the explicit check only compares `kaic2b.c` against `kaic2c.c`,
the gate **implicitly** requires `kaic2a.c == kaic2b.c` as well, because
if kaic2a and kaic2b diverge, the chain does not converge on the second
pass.

### Why the implicit invariant is structurally false

`stage1` is a transpiler written in kaikai-minimal. It has no typer, no
effect rows, no Perceus, no protocol resolution. `stage2` has all of
these. The two compilers share input grammar but not the internal
machinery that drives emission. The only way they agree byte-for-byte is
if `stage2` deliberately downgrades itself to emit code that `stage1`
can also emit — i.e., refrains from every optimisation that depends on
information `stage1` does not have. The current monolithic
`stage2/compiler.kai` and its strcmp-based match dispatch are concrete
artefacts of this restriction.

This was the structural blocker behind the rb-tree perf lane on
2026-05-22: replacing strcmp with `variant_tag == N` comparison in match
dispatch is correct and gives a ~14% wall improvement on the canonical
benchmark, but `stage1` cannot emit it (no typer means it does not know
which variant table to consult). Under the implicit invariant, the
change is unshippable without porting a typer into `stage1` — a cascade
that was explicitly rejected as out of scope.

### What other compilers do

No serious self-hosted language with a non-trivial type system enforces
byte-identical fixed point as a CI gate:

- **GHC** uses `stage0` (an existing GHC binary on the system) →
  `stage1` (boot artefact) → `stage2` (shipped binary). The gate is
  testsuite green, not byte equivalence. Stage1 and stage2 are allowed
  to differ.
- **rustc** uses the same pattern: `stage0` is a downloaded beta,
  `stage1` is built by it, `stage2` is built by `stage1`. Optional
  `stage3` is compared behaviorally with `stage2`, not byte-for-byte.
- **OCaml** ships `boot/ocamlc` as a checked-in bytecode and uses a
  bootstrap-stability check that allows the boot artefact to be
  refreshed deliberately.
- **Idris2** and **Lean 4** are both self-hosted with rich type systems
  and both reject byte-identical bootstrap as the gate, citing the same
  structural reason.

kaikai is the outlier. The byte-identical gate was inherited from the
phase where `stage1` and `stage2` were nearly the same compiler in
different surface languages, and never revisited after `stage2` grew its
typer.

### What the gate costs today

The gate forces three structural taxes on every change to the
compiler:

1. **Optimisations that depend on type information are blocked.** Match
   tag-compare, niche-filling in `Option[Ref[T]]`, devirtualisation of
   protocols, type-driven inlining, monomorphisation of effect rows,
   layout decisions. Every one of these has been deferred or
   half-shipped because `stage1` cannot replicate the emission.
2. **Refactors with global structural impact pay a coordination tax.**
   Changing the internal representation of `Expr`, `Ty`, or `Env` in
   `stage2` requires either porting the new representation into
   `stage1` first, or downgrading `stage2` to emit the legacy shape.
   Either path is expensive and the bootstrap punishes work in progress
   with an unreadable 200k-line diff.
3. **Modularisation of `stage2/compiler.kai` is blocked.** The monolith
   is 69k lines partly because any independent compilation order across
   `lexer + parser + resolver + typer + emit` modules becomes a source
   of divergence between stages. The gate vetoes modularisation as long
   as it stands.

The runtime cost of a gate failure is also bad UX. `diff -q
kaic2b.c kaic2c.c` failing produces no actionable signal — a 200k-line
diff with no fixture name, no line of source. The 2026-05-19 infinite
loop that consumed several hours of debugging was exactly this failure
mode.

### What the gate genuinely buys

One thing, and only one: it detects non-determinism in the compiler.
Hashmap iteration order leaks, timestamps, pointer addresses used as
identifiers, race conditions. This is real value and worth preserving.

The byte-identical between `kaic2b` and `kaic2c` already captures all of
that signal, because they are the same compiler running twice on the
same input. The byte-identical between `kaic2a` and `kaic2b` adds no
deterministic signal — it adds a *structural equality* signal that was
never load-bearing.

## The relaxed invariant

After this decision, the gate documents two distinct invariants:

1. **Per-compiler determinism (load-bearing)**: `kaic2b` running on
   `stage2/compiler.kai` must produce the same output every time. The
   `diff -q kaic2b.c kaic2c.c` check enforces this and stays as the CI
   gate.
2. **Stage1 → stage2 functional correctness (load-bearing)**: `kaic2a`
   must be a valid compiler — i.e., the chain `kaic1 → kaic2a → kaic2b`
   must produce a `kaic2b` that passes tier1. The check that enforces
   this is tier1 itself, not a structural diff.

Anything outside these two invariants — the structural shape of
`kaic2a.c` vs `kaic2b.c`, the order of declarations, the internal
representation choices, the specific optimisations applied — is
explicitly out of scope of the gate.

## What this unlocks

The blocked items move from "structurally impossible" to "ordinary
engineering work":

- **Tag-compare in match dispatch** (the ~14% rb-tree perf lane that
  motivated this revisit).
- **Type-driven layout optimisations**: niche-filling, packed tags for
  small variants, struct field reordering.
- **Whole-program devirtualisation** of protocols where the receiver is
  knowable.
- **Cross-module inlining** based on monomorphisation results.
- **Modularisation of `stage2/compiler.kai`** into `lexer.kai +
  parser.kai + resolver.kai + typer.kai + effects.kai + perceus.kai +
  emit.kai`, with independent compilation and arbitrary link order.
- **Internal representation changes** (HAMT environments, arena-based
  AST, indexed ID types) without bootstrap drama.

None of these are automatic consequences. Each is its own lane. But
each becomes proportional to its intrinsic complexity rather than
inflated by the bootstrap tax.

## What this does NOT unlock

Four structural debts visible in the codebase are independent of the
gate. The decision recorded here makes them cheaper to address — by
removing the coordination cost between stages — but it does not address
them directly.

### 1. Synthetic sites with hardcoded variant tags

`stage2/compiler.kai` contains sites where the compiler builds AST
nodes of itself via emission with hardcoded literal tags
(`kai_variant_u(0, "EMatch", ...)`). The literal `0` was harmless under
strcmp dispatch because the matcher ignored `variant_tag`. Under
tag-compare dispatch, these sites collide with builtins.

Fix shape: every constructor-emission site must resolve the tag through
`evar_find_tag(variants, name)` against the module's variant table, not
hardcode a literal. There are two known sites (lines ~13006 and
~13228) and an unknown number of indirect cases.

### 2. Three-way duplication of builtins across stages

The variant tables for `Some`, `None`, `Ok`, `Err`, `Signal*`, `Exit*`
are declared three times in three languages (`stage0/emit.c`,
`stage1/compiler.kai`, `stage2/compiler.kai`). They must agree on the
atom-style assignment (`Some=0, None=1, Ok=2, Err=3, Sig 4..8, Exit
9..10`) by hand.

Fix shape: centralise builtins in a single source file that all three
stages consume. Stage0 generates C from it; stage1 includes it as
literal data; stage2 reads it the same way. This was unimplementable
under the old gate because changes to the central file produced
diverging internal orderings across stages; under the new gate it is
ordinary refactoring.

### 3. Typer paths that leave `scrutinee.ty` as unresolved TyVar

Several match-typing paths build `EMatch` without ensuring the
scrutinee's type has been substituted into its concrete form. Without
the concrete `TyCon`, the emitter has no variant table to consult for
tag-compare.

Fix shape: cirugy in `synth_match` and `check_match` around
`apply_ty(sub, ty)`, ensuring the EMatch node carries a resolved type.
This was reverted on 2026-05-22 because the change to the EMatch shape
broke synthetic construction sites simultaneously (item 1 above). The
two need to be sequenced.

### 4. Two variant representations in the C runtime

`stage0/runtime.h` exposes both `kai_variant` (boxed) and
`kai_variant_u` (slot-mask). `stage2` mostly emits `kai_variant_u` but
legacy paths still construct `kai_variant`. The runtime's
`kai_op_eq_v` and discrimination helpers carry both code paths.

Fix shape: audit emission sites, migrate stragglers to
`kai_variant_u`, drop the boxed representation from the runtime.

## Sequencing

The intended order, with the relaxed gate as prerequisite:

1. **Document and adopt Option C** (this decision doc + a follow-up
   commit that adds the explicit invariants to `stage2/Makefile`
   comments).
2. **Add the stage1 non-determinism canary** in tier2 (cron daily):
   run `kaic1 compiler.kai` twice on the same source and diff the
   outputs. This recovers the determinism cover that the old gate gave
   for stage1, without coupling it to stage2's emission.
3. **Sites with hardcoded tags** (item 1 of the debt list): audit and
   fix.
4. **Atom-style global builtins** (item 2): centralise and migrate the
   three stage tables to a single source.
5. **Typer propagation of `scrutinee.ty`** (item 3): cirugy in
   `synth_match` and `check_match`.
6. **Tag-compare in match consumers**: the rb-tree perf payload.
7. **Variant runtime unification** (item 4): drop `kai_variant` boxed.
8. **Modularisation of `stage2/compiler.kai`**: separate lane,
   scheduled when the cumulative monolith cost outweighs the cost of
   the split.

Each step is its own lane with its own fixtures and retro. The lanes
1–6 can proceed in roughly this order; lane 7 is independent and can
slot in wherever; lane 8 is a project, not a lane.

## What we are accepting

By moving to Option C we explicitly accept:

- **`kaic2a.c` is allowed to look different from `kaic2b.c`.** Any
  inspection tooling that diffs the two for QA purposes will stop being
  meaningful and should be removed.
- **A class of compiler bug that is deterministic but incorrect will
  only be caught by tier1**. Under the old gate, such a bug would also
  surface as a `kaic2b.c → kaic2c.c` divergence in the iterations after
  the first. Under the new gate, tier1's fixture coverage is the sole
  line of defense. Mitigation: continue ratcheting the coverage probe
  baseline; add compiler-invariant fixtures (not just user-program
  fixtures) when introducing new emission paths.
- **Stage1 non-determinism**, if introduced accidentally, will only be
  caught by the cron canary, not on every PR. Mitigation: the canary
  runs daily and failures are escalated.

## What we are not doing

- We are **not** removing the `make selfhost` chain. The chain still
  builds three compilers and still checks per-compiler determinism. The
  change is in interpretation, not in mechanics.
- We are **not** porting a typer into `stage1`. Repeatedly explored,
  repeatedly rejected; the cascade into kaikai-minimal is unbounded.
  Option C makes this unnecessary.
- We are **not** committing to modularisation of `stage2/compiler.kai`
  by this decision. We are only removing the gate that vetoed it.
  Modularisation remains a separate decision when its own cost/benefit
  is clear.
- We are **not** weakening the Tier 1 stability commitment. The
  edition-stability invariants in `docs/decisions/editions-stability-without-stagnation-2026-05-15.md`
  apply to the user-visible surface and are orthogonal to the
  bootstrap gate. The relaxed gate has no implications for upgrade
  compatibility within an edition.

## Precedent invoked

- GHC, rustc, OCaml: standard stage1/stage2 bootstrap with behavioural
  equivalence, no byte-identical gate.
- Idris2, Lean 4, F\*: self-hosted compilers with non-trivial type
  systems, all rejecting byte-identical as a gate for the same
  structural reason kaikai now does.
- Linus and asu (review 2026-05-22): both recommended Option C
  directly. Eric recommended the more aggressive Option B (no fixed
  point, tier1 only) and preferred the resulting clarity, but agreed
  that C is the most conservative path that still unblocks the work.

## Reviewers

- Linus: "Move to Option C. Change the Makefile today. The tag
  optimization unblocks, non-determinism stays detected, and the
  bootstrap stops being the dead weight blocking the modularisation of
  the 69k lines."
- asu: "C is the correct reading of the invariant. A and B are the
  whole invariant or nothing. … No language with non-trivial types and
  serious self-hosting uses byte-identical fixed point."
- Eric: "The byte-identical gate protects a false invariant (stage1 ≡
  stage2) at a real onboarding and evolution cost. C is the most
  conservative path; B is the honest path." Voted B but acknowledged
  C as a valid intermediate.
