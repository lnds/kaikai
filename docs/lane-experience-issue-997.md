# Lane experience — issue #997 (spawned fiber reads/writes an enclosing `var`)

## Scope as planned vs. as shipped

**Planned (brief):** a fiber spawned inside a `nursery` that reads or writes a
`var` (a State cell) from the enclosing scope type-checked but segfaulted (rc
139, both backends). The brief framed it as the same family as #978 — an
effect/capability handled by the parent fiber, unreachable in the child — and
recommended decision (A): turn the segfault into a clean diagnostic, since a
`var`/State cell is fiber-local and does not cross a `spawn` (#820).

**Shipped:** decision (A), realised as **two layers**, not one:

1. **Compile-time reject (primary).** The cell escape check that already
   rejects a closure that captures a cell and outlives its handler now also
   rejects a closure that captures a cell and is *deferred to another fiber by
   `spawn`* — the same defect by another route. This is the diagnostic a user
   sees for every common shape: reading the cell, writing it, or applying a
   captured closure that reads it.
2. **Runtime floor (safety net).** The by-id cell lookup
   (`kai_evidence_lookup_node_by_id`) on the child fiber, which returned NULL
   and was dereferenced into a segfault, is now guarded by
   `kai_evidence_require_reachable` — a NULL becomes
   `kai: capability not reachable in this fiber: <name>` + exit 1. This catches
   the residue the syntactic check cannot see statically (a cell read reached
   through a rebound closure alias inside the spawn body).

## Decision A vs. B, and why two layers

B (copy/propagate the cell into the child fiber) was rejected on the same
grounds the brief anticipated: #820 makes fiber-local capabilities *not* cross a
`spawn` by construction; a parent handler crossing a spawn is the hazard the
deleted clone introduced. So the cell genuinely must not cross — the question
was only *where* to report it.

Within A, the brief pointed at #978's mechanism (a runtime guard on the
frame-slot fill). The architect review (asu) sharpened this: #978 and #997 sit
on opposite sides of a line. #978's NULL is *dynamically* dependent — a child
fiber performing an effect the parent *might* handle is a reasonable
composition the typer cannot statically refute, so it defers to runtime. #997's
NULL is *statically total* — a `var` cell is fiber-local by construction, so
*no* program that touches a parent cell across a spawn is ever valid. Where the
hazard is static and total, reject at compile time; where it is
dynamic-conditional, defer to runtime. Copying #978 wholesale would have left a
program that type-checks but *always* crashes — the exact "type-checks but
always UB" anti-pattern Tier 1 ("safe at compile time") exists to kill.

The correct precedent was therefore not #978 but **the cell escape check that
already existed** (`lcr_escape_check`/`escaping_closure_pos` in `desugar.kai`):
it already rejected "a closure that captures the cell and outlives its handler",
with the diagnostic, the cell-access detection, and the justification text. A
spawn deferring the closure to another fiber is the same closure surviving the
handler teardown, by another route — extending the net, not weaving a new one.

The runtime floor is still needed because the escape check is **syntactic and
conservative**, like Koka's own `var`/`ref` escape analysis. It cannot follow a
cell read reached through an aliased closure rebound inside the spawn body
(`spawn(() => { let g = reader; g() })`) without reimplementing interprocedural
escape analysis in a desugar pass — an unbounded tail of edge cases (alias via
record field, via list element, via a closure returned from a local fn) for
marginal coverage. The mature split is "static catches the common, honest
shapes; runtime guarantees the invariant a type-checked program never
segfaults." That is exactly the static-reject + runtime-net combination the
architect proposed at the outset.

## Where the fix lives

- `desugar.kai` — a new spawn-aware pass (`spawn_block_escape_pos` and the
  `spawn_escape_*` / `task_touches_cell` family) wired into `lcr_escape_check`.
  It scans the whole block (descending into eager lambdas like a `nursery`
  body, which runs in place but may itself spawn) for a `<recv>.spawn(task)`
  whose task touches the cell. It runs *before* the nursery-cap rewrite, so the
  surface form `n.spawn(...)` (`EField(EVar(n), "spawn")`) is still visible.
- `stage2/runtime.h` + `stage0/runtime.h` (twins) —
  `kai_evidence_require_reachable`.
- `stage0/runtime_llvm.c` — the `kaix_evidence_require_reachable` forwarder.
- `emit_c.kai` + `emit_native_fx2.kai` — the by-id perform path wraps its
  lookup in the guard on both backends.

## Structural surprises the brief did not anticipate

1. **The bug is not on the #978 code path.** The brief said "reuse #978's
   mechanism". #978 guarded the *frame-slot fill* (capability-passing
   transport). Cells are dispatched **by-id** (`kai_evidence_lookup_node_by_id`
   over the fiber's evidence stack), an entirely different perform path. The
   guard had to be *added* to the by-id dispatch, not reused. The C emit made
   this visible: the bare-walk perform branch already wrapped its lookup in
   `kai_evidence_require` (the #978 fix), but the by-id branch right above it
   did not — that asymmetry was the segfault.

2. **The escape check could not see the spawn at first.** The existing
   `escaping_closure_pos` treats every `ECall` argument as run-eagerly ("a
   lambda passed as an argument runs eagerly inside the block"), so it never
   descended into the lambda passed to `nursery`, let alone the spawn nested
   inside it. A new whole-block scan that descends into eager lambdas was
   required; bolting the spawn check onto `escaping_closure_pos`'s value-escape
   logic was the wrong shape and was reverted.

3. **Shadowing was a real false positive, caught only by testing.** The first
   spawn scan was shadow-blind (it reused `contains_cell_access`, which matches
   `n.get`/`n.set` by name with no scope tracking). A `var counter` declared
   *inside* the spawn body — perfectly legal, its handler lives on the child
   fiber — was wrongly rejected because it shares the outer cell's name. The fix
   made the whole task scan (`task_touches_cell` and friends) shadow-aware:
   a nested `var n` / `let n` / lambda param `n` / arm pattern binding `n` masks
   the cell from that point on. This subsumed an earlier `applies_tainted`
   helper (which was shadow-blind and scored cogcom 19), which was deleted.

## Fixtures added

In `examples/effects/`, run by `test-issue-997-spawn-cell-fiber-local`:

- `issue_997_spawn_cell_read_rejected` — the verbatim #997 repro (read).
- `issue_997_spawn_cell_write_rejected` — the write variant.
- `issue_997_spawn_cell_transitive_rejected` — reads via an applied captured
  closure (`spawn(() => { reader(); () })`).
- `issue_997_spawn_cell_runtime_floor` — the rebound-alias residue the static
  check cannot see; asserts a clean runtime diagnostic + exit 1, never a
  segfault.
- `issue_997_spawn_inner_cell_ok` — a fiber's own inner `var` (positive).
- `issue_997_spawn_no_cell_ok` — a spawn that never touches the cell, plus the
  parent mutating it from a non-spawned position (positive).
- `issue_997_spawn_shadowed_cell_ok` — an inner `var` shadowing the outer one
  (the shadowing false-positive regression).

## Coverage gaps

The runtime floor names the cell but does not point at the source line of the
spawn (the diagnostic is emitted from the runtime, which has no span). The
compile-time reject does carry the span. This is acceptable: the common shapes
all reject at compile time with a span; only the rare rebound-alias residue
falls through to the spanless runtime diagnostic.

## Verification

Both backends, serial backend-parity (runtime change). The five reject/floor
fixtures and three positive controls behave identically under `--backend=c` and
`--backend=native`. Selfhost byte-id green. tier0 green.
