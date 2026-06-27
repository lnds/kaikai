# Lane experience — #820 L1 (Typer: evidence obligations)

L1 of the capability-passing evidence transport (`docs/effects-capability-passing-design.md`,
lane plan `docs/effects-capability-passing-lane-plan.md`). Analysis only:
compute, per function, the distinct effect-instance evidence obligations
its row demands and which supplier discharges each — no injection, no
codegen change. The highest design-risk lane: a mis-classified instance
silently encodes a wrong evidence model that every gate except the L0
distinguishing fixtures would pass.

## Scope as planned vs as shipped

**Planned** (lane plan §B-L1): a `--dump=evidence-obligations` dump over
the existing row machinery (`infer.kai` row discharge), the three-way
supplier classification (lexical handle / forwarded caller param /
startup default), and the op-name collision surfaced as an obligation
with no supplier. Gate: byte-id green, the collision visible on the L0
`collision_*` fixtures, and two distinct obligations for one effect on
`two_instances_through_call`.

**Shipped**: all of that, plus one structural change the brief did not
anticipate — a read-only **op-resolution side-record** threaded through
`InferState` → `TypedDecl` → `TypedProgram`. The "rides the existing row
machinery" framing assumed the obligations could be read off the typed
AST after inference. They cannot for the collision fixtures (see below),
so the analysis splits into two observation points:

- a **structural walker** (`obl_walk`) over the typed body for the clean
  obligations — op calls that survive as `ECall(EVar("Eff.op"))` nodes;
- the **side-record** (`OpResolution`) captured at the resolution site
  (`try_op_call`) for the collisions, which the typed AST does not carry.

New modules `stage2/compiler/evidence.kai` (A−, cogcom avg 2.8 / max 7 —
the obligation model, structural walker, collision predicate, dump) and
`stage2/compiler/evidence_scan.kai` (A — the discharge-backed effect
scan, split out so its one-arm-per-`ExprKind` AST descent does not weigh
down the model); the dump flag and dispatch arm in `driver.kai`; the
side-record type + threading in `infer.kai`.

## The obligation model

An **obligation** is one demanded effect-instance, keyed by the full
instance label including the `@alias` suffix (`State@c1`). Two `State@c1`
/ `State@c2` are two obligations; one `State@c1` performed twice is one
(dedup by instance label + op + supplier). **Instance identity is the
discriminator, not the count** — the lane's correctness hinges on the
analysis never fusing two aliased instances into one bare `State`.

Per obligation, the supplier is one of §3's three ways, plus the
unsatisfiable fourth:

- **lexical handle** — an enclosing in-body `handle ... with E [as a]`
  discharges it → the handle's evidence (instance label preserved).
- **forwarded caller param** — no enclosing handle and the function is
  not `main` → the demand escapes to the caller's hidden param.
- **startup default** — no handle, the function is `main`, and the
  effect's `default {}` block covers the op → the startup evidence.
- **NONE / collision** — the op name is shared and the call is not
  disambiguated → no honest supplier (the #789 op-name collision).

## The three-way classification — where the model nearly went wrong

The structural walker mirrors `infer.kai`'s op-coverage discipline: a
clause body executes *outside* its own `with`, so clause and return
bodies walk under the OUTER frame stack. That single detail is why the
#789 value-corruption shape exists — an aliased `@cap` read inside an
unrelated handler clause loses its alias and resolves by bare op name.

The naive first model walked the typed AST and classified each
`EVar("Eff.op")` callee, detecting collisions structurally (≥2 live
instances of an op, or a frame of a different effect wiring the op).
**This is blind on two of the four collision fixtures**, discovered
empirically:

1. `var x = 10; x := @x + 1` does NOT desugar to a `State` handler — it
   lowers to an `array_make`/`array_get`/`array_set` slot under
   `Mutable` (the local-var optimization). So a `var`-based differential
   fixture produces no `State@x` instances at all. The correctness
   oracle must use an explicit `with State as a` handler.
2. The collision in `collision_value_corruption` / `collision_type_mismatch`
   lives inside a string interpolation (`"r=#{int_to_string(@remaining)}"`).
   The interp span is resolved by a later pass; the `@remaining` op call
   never appears as a structural node in the typed AST. `--dump-typed`
   shows ZERO dotted idents for the whole fixture. A post-resolution AST
   walk is structurally incapable of seeing it.

The fix is the side-record: `try_op_call` is the one point in the
pipeline where the op name, the candidate effects, the chosen effect,
and the alias tag coexist. After it, they fuse into a label or vanish.
Recording them there — without changing any resolution decision — is the
honest observation point.

The first collision predicate keyed on `cands` (effects that DECLARE the
op): "≥2 declarers + not disambiguated → collision." That is the WRONG
set, and it produced false positives on a dozen parity-green fixtures
(`m7b_14_state_helper`, `m8_*`, `net_*`, `tco_lambda_param_in_fiber`).
Every effect that shares an op name with a builtin (`State`/`Env` both
declare `get`; `Spawn` ops) tripped it. A dump that cries collision on
correct programs encodes the wrong evidence model just as surely as
missing a real one — the inverse false-green. The honest L1 bar is
binary: **fire on exactly the real collisions and on no green fixture.**

The correct signal is **discharge-backed vs phantom-absorbed**, not "who
declares the op." A resolution is a collision when:

- **cross-effect** (#789 segfault / type-mismatch): the chosen effect is
  never discharge-backed program-wide — no `handle with <chosen>` in any
  function and no `default {}` block — so its label is absorbed by an
  OPEN row-variable tail that no signature commits to discharge (the
  `fresh[R,e](body: ()->R/Cell+e)` tail swallows `Cell`). A sibling
  effect sharing the op name IS backed, so the perform diverts to its
  evidence. Contrast `m7b_14`: the lambda's `()->S/State[T]` is a CLOSED
  demand that `with_state` discharges — a signed contract, not a phantom
  — so `State` is backed and there is no collision, even though `Env`
  also declares `get`.
- **cross-instance** (#789 value-corruption): the chosen effect IS
  backed, but the call names an instance (`@remaining`) whose tag is
  suppressed crossing a clause/lambda boundary while a different live
  instance shadows it (`tagged=false` with an alias present).

The cross-effect test is the `obl_eff_backed` scan in `evidence_scan.kai`
(a `handle with <eff>` exists, or the effect carries a `default {}`); the
cross-instance test is the alias-without-runtime-tag flag the emitter
already computes. The signal is structural — it does NOT need a live
handler stack threaded through `InferState`. The cross-function case
(`with_state` handles the lambda) is already encoded in the fact that the
lambda's row is a closed, signed demand, not an orphan open tail.

## How instance identity distinguishes two instances

`two_instances_through_call` needs the L6 capability-as-parameter surface
to typecheck (`add(c1: Cell, ...)`; `dst.set(...)` is rejected today), so
`add`'s body does not survive inference. The dump does NOT simulate the
unbuilt surface — that would be false-green. Instead:

- `add`'s three `Cell` parameters surface as `PENDING-L6` obligations
  (read off the declared param types, not a walked body) — three
  distinct instances by parameter, the callee-side proof.
- `main` shows `State@target` as a distinct discharged obligation,
  never fused with `c1`/`c2` — the supplier-side proof.

The correctness oracle `obligations_aliased_vs_bare` is the cleaner
witness: two explicit `with State as a` / `as b` handlers, op calls
`a.set` / `b.set` / `a.get` / `b.get`, and the dump keeps `State@a` and
`State@b` as two obligations with two suppliers, never one `State`.

## Dump validation against every L0 fixture

| Fixture | Expected | Dump shows |
|---|---|---|
| `collision_value_corruption` | collision (interp-buried) | `State (via get) -> NONE (collision: get shared by State, Env)` |
| `collision_type_mismatch` | collision (interp-buried) | `State (via get) -> NONE (collision)` |
| `collision_segfault` | collision (structural, in lambda) | `Cell (via get/set) -> NONE (collision: shared by Cell, State, Env)` |
| `two_instances_through_call` | two distinct instances | `add`: 3 Cell params PENDING-L6; `main`: `State@target` distinct |
| `obligations_aliased_vs_bare` (new oracle) | instances NOT fused | `State@a` / `State@b` distinct, each lexical-handle supplier |

All four collision fixtures show the collision as an obligation with no
supplier; the two instance-distinction fixtures keep the aliased
instances distinct. The lane gates on TWO directions of falsification:
the dump must fire on the four real collisions, AND it must NOT fire on
any other fixture — a full-corpus scan of `examples/effects/` (+
`quarantine/`) reports zero spurious collisions. A dump that invents
collisions on green programs is the inverse false-green and was the bug
the `cands`-keyed first model shipped before the discharge-backed
rewrite.

## Structural surprises the brief did not anticipate

- **`InferState` has no record-update spread.** Adding `op_res` meant
  editing every `InferState { ... }` literal (~23 sites) plus the
  `TypedDecl`/`TypedProgram` aggregation (the `diags` pattern is the
  template). Field-named records made the typer catch any omission at
  compile time, which de-risked the mechanical sweep.
- **byte-id false-DIFF from a stale binary.** After editing the source,
  `selfhost` reports a DIFF until `kaic2` is rebuilt — the comparison is
  between two generations, and the un-rebuilt binary emits the old code.
  This is not non-determinism; rebuild `kaic2` first, then `selfhost`.
- **Line attribution is unreliable.** An interp re-parse rewrites source
  lines (the `@remaining` call reports `@1:15`). The side-record carries
  the enclosing function name, stamped by `infer_decl`, so the dump
  attributes resolutions by name, not by line span.

## Fixtures added and coverage

- `examples/effects/obligations_aliased_vs_bare.kai` (+ `.out.expected`)
  — the bare-vs-aliased correctness oracle asu's review demanded before
  any walker code: if the analysis cannot keep two aliased instances
  distinct, it encodes the wrong evidence model. Runs correctly today
  (golden `3`), parity-green on both backends.

The L0 `collision_*` and `two_instances_through_call` fixtures are the
inherited oracle; this lane adds the dump that reads them.

## Follow-ups for next lanes

- **L2** consumes this analysis to inject the hidden evidence params.
  The side-record (`TypedProgram.op_res`) is the per-call-site supplier
  decision L2 needs; the structural walker's obligations are the clean
  cases. L2 turns the collision (supplier=NONE) into the actual compile
  diagnostic (design §5.1).
- The `OblFrame` carries only `(eff_name, alias)` now — the collision
  detection moved to the side-record, so the frame's clause/default op
  lists (needed by the earlier structural-collision model) were dropped.
- `--dump=evidence-obligations` is the first `=`-form dump flag; the
  established convention is `--dump-X` (hyphen). The brief specified the
  `=` form; if a future lane adds sibling `--dump=` flags it groups, if
  not it is a lone stylistic outlier worth renaming.
