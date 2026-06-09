# Lane experience — `KFn.stateful` flag (kir-clause-stateful-flag)

## Scope as planned vs. shipped

**Planned:** propagate the existing `ClauseInfo.stateful` boolean onto the
lowered clause `KFn` so KIR consumers (`KAI_BACKEND=kir`, native backend)
read statefulness off the structure instead of re-deriving it from the AST.
Make the flag visible in the `--emit=kir` dump. Touch no backend emission.

**Shipped:** exactly that, plus a regression fixture in the KIR golden
suite. No backend consumption — the native state-prologue / `KResume2`
write is the next lane (see follow-up below).

## The gap

A stateful handler clause (`handle { ... } with Eff(init) { ... }`) reads a
`state` register and resumes via the 2-arg `resume(value, new_state)`. The
typer already computes this and stores it in `ClauseInfo`'s 7th positional
field (`CI(line, col, eff, op, params, body, stateful, enc_fn, caps)`). The
C-direct (`emit_c.kai`) and LLVM-text (`emit_llvm.kai`) emitters read
`stateful` straight off `ClauseInfo` because they emit from the AST. But
the KIR lowering (`lower_clause_fn` in `kir_lower_fns.kai`) bound that field
as `_` and dropped it: the resulting `KFn` had no statefulness signal, so a
KIR consumer saw a clause whose body does `dup state; resume[tail] k state`
referencing a free `state` register the `KFn` never declared or marked.

Verified before changing anything: `--emit=kir` on
`examples/effects/m7b_14_state_helper.kai` showed
`clause fn _kai_clause_10_3_get(resume: box @resume) : box { ... dup state;
resume[tail] k state ... }` — `state` free, no marker.

## The one-line fix

1. `kir.kai`: add `stateful: Bool` as the last field of the `KFn` record
   (+ doc comment explaining it applies to `KFnClause` only, and that
   emit_c/emit_llvm still read `ClauseInfo.stateful` since they emit from
   the AST not the KIR).
2. `kir_lower_fns.kai`: in `lower_clause_fn`, change the `CI(...)`
   destructure's 7th `_` to `stateful` and thread `stateful: stateful` into
   the `KFnClause`. Set `stateful: false` at the two non-clause sites.
3. `kir_dump.kai`: print a ` stateful` marker after the return slot when the
   flag is true (`kir_stateful_mark`).

## KFn construction sites touched (grep `KFn {` across `stage2/`)

All three live in `kir_lower_fns.kai`; no construction site exists anywhere
else in the stage2 tree (the "native main-spine builder" the brief warned
about does not construct a `KFn` literal):

- `lower_fn` (~line 80, `KFnNormal`) — `stateful: false`.
- `lower_lam_fn` (~line 105, `KFnLambda`) — `stateful: false`.
- `lower_clause_fn` (~line 137, `KFnClause`) — `stateful: stateful` from the
  `CI` destructure.

## Dump spelling chosen

`clause fn <sym>(<params>) : <ret> stateful {` — the word `stateful`
appended after the return slot, before the opening brace, only when the
flag is true. A stateless clause / normal fn / lambda keeps its exact prior
shape (no marker), so every existing golden is byte-unchanged. This matches
the dump's existing terse-attribute convention (the `kind` keyword prefix,
the `@resume` / `@cap` origin marks): a one-word marker, no punctuation.

## Fixtures added

`examples/kir/stateful_clause.kai` (+ `.kir.expected` + `.kir.fns`) — a
self-contained parametric effect `Slot[T]` handled with `with Slot[Int](42)`,
so the clause is stateful. Wired into the `test-kir` golden suite (stage2
Makefile). Its golden carries the `: box stateful` marker on the clause and
`install Slot ... state=42` on `main`, exercising the new flag end to end.
Effect name `Slot` chosen to avoid the stdlib collision the first draft hit
(`Env` and `State`/`Reader` are stdlib/builtin effects, and the harness runs
with `--path ../stdlib`).

## Gates run (all green)

- `make -C stage2 test-kir` → all 7 fixtures OK incl. `stateful_clause`;
  existing 6 goldens byte-unchanged (no stateful clause among them, so the
  marker never perturbs them).
- KIR dump check: `m7b_14_state_helper` get/set now `: box stateful`;
  stateless `m7b_2a_op_id_basic` Box.id clause stays `: box`.
- `make -C stage2 selfhost` → `selfhost determinism: OK (kaic2b.c == kaic2c.c)`.
- `make tier0` → OK (32 demos, arena gate, selfhost determinism).
- `m7b_14_state_helper.kai` runs exit 0 under C-direct AND `KAI_BACKEND=kir`;
  same for `stateful_clause.kai`.
- `bash tools/test-backend-parity.sh` → pass=413 fail=0.
- `km score`: kir.kai 97.9 A++ → A++; kir_lower_fns.kai 93.8 A+ → 93.7 A+;
  kir_dump.kai 97.3 A++ → A++. Grade-neutral, all A+/A++ (these KIR files
  are well-factored, not the F monoliths).

## Structural surprises

None. The KIR files are small and clean (kir.kai is types-only, 73 LOC).
The only adjustment from the brief's hypothesis: the brief warned of a
"native main-spine builder" constructing a `KFn`; no such literal exists —
only the three lowering sites construct `KFn`.

## Follow-up (the next lane)

This lane is behaviour-preserving: it only adds a field + makes it visible.
The native backend's *consumption* of `KFn.stateful` is the next lane:

- the state prologue reading `self->state` under the free names `state`
  (and the legacy alias `log`) at the top of a stateful clause body;
- `KResume2` writing `self->state` via `kaix_clause_state_set`;
- seeding the `state=` slot of `KInstall` with the handler's `init`.

The native backend still aborts `KResume2 unsupported`; that is expected and
out of scope here.
