# Lane experience — issue #801: lazy `Stream[t, e]` carrier

## Scope as planned vs as shipped

**Planned (brief):** a stdlib-pure lane — `stdlib/stream.kai` with a push
carrier, pipe-canonical combinators, `read_lines`/`write_lines`, a
resumable `ReadFault` effect, fixtures, and docs. Explicit constraint:
*no touching the compiler* (emit/kir/runtime/typer), because a native
burn-down runs in parallel in that zone. Every `pub` carries `#[doc]` and
`#[unstable]`.

**Shipped:** all of the surface, plus **one load-bearing typer fix** the
brief did not anticipate (authorised mid-lane by the user). Two further
brief premises turned out false and were corrected:

1. `#[unstable]` was dropped from the whole module — the user set a new
   policy mid-lane: *stdlib pre-1.0 does not use `#[unstable]`* (no
   external users → the marker protects nobody).
2. The carrier is a single-constructor **variant**, not the bare
   function-type alias the design sketched.

The lane widened from "stdlib + fixtures" to "stdlib + fixtures + one
typer fix" because the carrier provably could not deliver its headline
ergonomics without it (details below). The widening was escalated and
approved, not taken unilaterally.

## The three typer collisions (the structural surprise)

The design assumed the typer already supported a function-shaped carrier
with effect rows in higher-order positions. It did not, in three
distinct ways. Each was reduced to a minimal repro before acting.

### Bug 1 — `#[unstable]` is not transparent (sidestepped by policy)

`#[unstable]` on a `pub type` alias suppressed its expansion; on a `pub
fn` with `var` it broke the `State` desugar (`method set/get not found`).
asu localised it to two missing `DUnstable` match arms:

- `transparent_alias_of_decl` (emit_c.kai ~10095): `DType` matches,
  `DUnstable(DType(...))` falls to `_ -> None` → the alias never enters
  the expansion table.
- `desugar_var_decl` (desugar.kai ~1432): `DFn` matches,
  `DUnstable(DFn(...))` falls to `_ -> d` → the `var` never becomes
  `State`.

The parser comment claims `DUnstable` is a transparent marker; these two
walkers violate that invariant. `http.kai` never tripped it (only
`#[unstable]` on *pure* fns). **The lane did NOT fix this** — the user's
no-`#[unstable]`-in-stdlib policy removed the need. Left documented here
for the integrator to file; the fix is two additive arms unwrapping the
wrapper, plus a regression fixture exercising both shapes (an `#[unstable]`
alias that must expand + an `#[unstable]` fn with `var`). Likely the tip
of an iceberg — worth a grep for other pre-resolve decl walkers that
`match d { DFn/DType ... _ -> }` and forget `DUnstable`.

### Bug 2 — pipes decline on a function-type alias (drove the nominal carrier)

`|` / `||` / `|?` dispatch by head type via `inf_ty_head_name`
(infer.kai:1334), which maps `TyFnT(_,_,_) -> None`. A transparent
function-type alias has no nominal head, so the pipes error with
"requires a concrete head type". The design's "pipes out of the box /
zero compiler changes" was therefore **false for a function-alias
carrier**. The fix is a design choice, not a typer change: make the
carrier a single-ctor variant `Stream(run)` so the head is
`TyCon("Stream")`. asu's call; chosen over a record because a record may
not yield a stable nominal head.

**Detour worth recording:** after switching to the nominal carrier, pipe
dispatch *still* failed with "no module declaring type `Stream`". A
trivial `Box` nominal in an imported module reproduced it. Root cause:
pipe dispatch on a user `pub type` is gated on edition ≥ `hanga-roa`
(#603), and `build_head_owner_map` falls to the tongariki (List-only)
branch when `edition` is empty. `kaic2` invoked **directly** (no
`kai.toml`) gets `edition=""`; the `kai` wrapper passes `--edition
hanga-roa` from `kai.toml`/`EDITION`, raw `kaic2` does not. This is the
#603 gate, **not a bug** — but it cost a long instrumentation detour, and
the `test-effects` harness must pass `--edition hanga-roa` for the stream
fixtures (it now does).

### Bug 3 — nominal ctor field drops its effect row (the fix that shipped)

The blocker. A nominal type (variant or record) whose field is a
higher-order function carrying an effect row **lost that row at
constructor instantiation**: the field was instantiated with an empty
row, so any effectful producer failed to unify. Minimal repro:

```kaikai
type S[t, e] = S(((t) -> Bool / e) -> Unit / e)
pub fn mk() : S[Int, Boom] = S((y) => { Boom.go(); ... })  # type mismatch: field has no /e
```

Root cause: `scheme_of_variant` (infer.kai:3655) resolved ctor fields
with the *light* `resolve_ty_with_tparams`, whose `TyFn(ps, ret, _)` arm
**discards the row with `_`** and rebuilds with `fn_ty` (empty row). The
heavier `resolve_ty_with_binds` preserves it (`row_of_expr_with_binds`),
but it needs row-variable binds. A non-nested field row var (`Thunk((t)
-> t / e)`) survived because the row was on the *outer* arrow; the bug is
specifically a row inside a higher-order field. It affected concrete rows
too (`/ Boom`, not just `/ e`).

**Fix:** `scheme_of_variant` now splits the declared tparams into
row-kind (those appearing in row position, found via
`collect_rvars_in_typeexprs`) and type-kind, resolves the ctor fields
with `resolve_ty_with_binds` (preserving `TyFn` rows), and generalises
with `scheme_full(tvar_ids, rvar_ids, ...)` instead of the old
`scheme(ids, ...)` that left `rvars` empty. ~20 lines plus two small
helpers (`names_excluding`, `tpbind_id_of`). The change is additive to
every existing ctor (a ctor with no row-position tparam computes the same
scheme as before), which is why `tier0` stayed green including selfhost
byte-determinism.

A **code-review catch (linus)** hardened the fix before it shipped: the
first cut built the row-var binds with `mk_rvbinds(rvar_names)`, which
re-numbers ids from zero. But `mk_tpbinds` had already given each name an
id (`e` → 1 if it is the second tparam), so a tparam appearing in *both*
type and row position would resolve via the type-binds to an id (1) that
landed in neither `vars` nor `rvars` — an ungeneralised free type-var
leaking into the scheme. Latent under the `Stream` fixtures (where `e` is
row-only), but a real bug. Fixed by reusing the `mk_tpbinds` ids:
`rbinds = map(rvar_names, n => RVB(n, tpbind_id_of(binds, n)))`; type-var
and row-var ids are then disjoint by construction. `tpbind_id_of` was
also changed to panic on a miss rather than return a bogus `-1`. The
regression fixture `examples/effects/ctor_field_effect_row.kai` exercises
the both-positions shape so the hardening cannot silently regress.

## Design decisions

- **Carrier = single-ctor variant**, not function alias (Bug 2) and not a
  record (asu: record head may be unstable). Combinators unwrap via one
  shared `run_with` helper, so each stays a one-liner; Perceus reuses the
  one-field constructor in place through the chain.
- **`ReadFault` with two ops, one effect** (the open design point in
  #801). `bad_chunk(msg) : Unit` is resumable (skip policy);
  `open_fault(msg) : Nothing` is abort-only by construction. Chosen over
  putting open-failure on `Fail` because one effect keeps the row to
  `File + ReadFault` and one thing to mock/handle. `Fail.fail : Nothing`
  cannot express skip (spike S2), so it was never an option for the
  chunk fault.
- **`var` in the sinks**, not an `Array` cell. The push carrier
  fundamentally needs accumulation across `yield` callbacks (yield
  returns `Bool`, not an accumulator). With `#[unstable]` gone, `var`
  desugars cleanly (Bug 1 was the only reason a cell was ever
  considered) and keeps `Mutable` masked (locally-constructed, never
  escapes), so the sink signatures stay `: a / e`, not `/ e + Mutable`.
- **Line semantics = `string.lines`**: newline stripped, a final line
  without `\n` still yielded, empty file = zero lines. `pump_lines`
  buffers a carry across chunk boundaries and `drain_lines` splits on the
  last `\n`.

## Fixtures

In `examples/effects/`, wired into `make test-effects` (with `--edition
hanga-roa --path ../stdlib`):

- `stream_early_stop.kai` — S1: `|? | take(3) |> count == 3` (producer
  stops at the third element).
- `stream_mock_disk.kai` — gallery (f): `read_lines` mocked via
  `handle ... with File`, `errors == 2`.
- `stream_skip_policy.kai` — S2: a faulting chunk performs `bad_chunk`,
  the handler resumes (skip), `count == 3`.
- `stream_shout_roundtrip.kai` — gallery (b): read→`map upper`→`write_lines`
  in one expression, round-tripped, `HELLO\nWORLD`.
- `stream_abort_leak.kai` — the negative: abort policy (handler does not
  resume) skips the producer's `close_file`, `closes == 0`. Documents the
  known fd-leak limitation as a measured fact, not a surprise.
- `demos/wc.kai` (+ `stream_wc_input.txt`) — the informal acceptance: a
  classic `wc` as a single `fold` over `read_lines`. Matches coreutils
  (`2 9 44`). The demo is the ergonomics proof: if it didn't write this
  simply, the ergonomics weren't achieved.

Coverage gap: no fixture exercises `flat_map` or `take_while` at the
behavioural level (they typecheck in the module and are covered by the
gallery in the design doc, but not run). Low risk — both are
one-liners over `run_with` — but a follow-up could add them.

## Cost vs estimate

The brief read as a tight stdlib lane. The real cost was the three typer
collisions: roughly half the lane was reducing them to minimal repros,
two escalations to the user (the `#[unstable]` contradiction, then the
nominal-carrier blocker), and the Bug-2 edition-gate detour. The actual
stdlib module + fixtures + docs were the fast part. Lesson: a "stdlib
only" carrier that puts effect rows in higher-order positions is not
stdlib-only — it stresses corners of the typer (alias expansion, ctor
scheme building, pipe dispatch) that ordinary stdlib code never reaches.
The design doc's spike "validated" the carrier using only `|>` (apply),
which is why it missed that `|` / `|?` need a nominal head.

## Follow-ups for next lanes

- **File the `DUnstable`-not-transparent bug** (Bug 1) with the two
  match-arm fix + regression fixtures, and audit other pre-resolve decl
  walkers for the same missing arm. (Integrator to open; this lane does
  not open issues.)
- **ahu**: delete its `stream.kai` and re-export the stdlib carrier
  (avoids `HLAmbiguous` on `map` for head type `Stream`); its
  `from_lines` becomes `stream.read_lines`.
- **Cancel-aware bracket** closes the abort-path fd leak (shared with
  #771's cancel-safety note).
- Behavioural fixtures for `flat_map` / `take_while`.
- `zip` / byte-level public streams / a standalone `LineWriter` handle —
  all explicitly out of scope for v1 (see design doc §Out of scope).
