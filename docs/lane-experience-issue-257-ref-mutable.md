# Lane experience — issue #257 (Ref[T] in Mutable effect)

## Objective metrics

| metric                       | value                          |
|------------------------------|--------------------------------|
| Lane duration (wall)         | 18 min (start 01:46, end 02:05) |
| Tier 0                       | OK (selfhost byte-identical, demos baseline holds) |
| Tier 1                       | OK (full make test + demos + fmt + bench + check) |
| Tier 1-ASAN                  | OK (all path-gated fixtures green) |
| `make selfhost` (stage 1+2 C) | OK — fixed point reached       |
| `make -C stage2 selfhost-llvm` | OK — fixed point reached     |
| Issue repro `prints 1`       | confirmed (exact issue snippet) |
| Compiler diff size           | +118 lines / -2 lines          |
| New fixtures                 | 1 (`examples/effects/mutable_ref_basic.kai`) |
| Coverage probe               | not affected (no `ref_*` baseline entry) |
| Calibrated budget            | 3.5h (delivered 0.09× of budget) |

Build TSV at `/tmp/lane-issue-257-ref-mutable-builds.tsv`:

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T02:01:42-04:00	tier1	OK	-
2026-05-05T02:02:39-04:00	tier1-asan	OK	-
2026-05-05T02:03:15-04:00	selfhost	OK	-
2026-05-05T02:04:17-04:00	selfhost-llvm	OK	-
```

## Diagnosis

The catalog site for `Mutable` ops is `builtin_mutable_decl()` at
`stage2/compiler.kai:43370`. The default handler installation has
two redundant emit sites that mirror each other:

- C backend: `default_mutable_shims()` at line 14712 (op
  trampolines) and `default_mutable_setup()` at 14731 (struct
  field assignments by name).
- LLVM backend: inline in the main-init emitter at line 35680
  (per-field `getelementptr ..., i32 0, i32 N` + `store`).

The runtime helpers live at `stage0/runtime.h:1130+` (private
allocators `kai_array_*`), `stage0/runtime.h:1560+` (public
prelude wrappers `kai_prelude_array_*` with the
callee-consumes-refs convention), and `stage0/runtime.h:3008+`
(default-handler trampolines `kai_default_mutable_array_*`). The
LLVM-visible C wrappers `kaix_default_mutable_*` live in
`stage0/runtime_llvm.c:305+`.

The `op_offsets` table is computed by
`compute_op_offsets_for_eff` (line 37327), which walks the EOps
list in declaration order and assigns offset = `3 + position`.
This means inserting the new ops at the **end** of the array
preserves all existing offsets, so the hardcoded LLVM `i32 N`
indices for `array_*` (3..7) need no edits.

## Runtime implementation

Choice: reuse `KAI_ARRAY` of length 1 to back `Ref[T]` rather
than introduce a new `KAI_REF` tag. Trade-off:

- **For** — zero new branches in `kai_decref`, `kai_to_string`,
  `kai_op_eq`, `kai_alloc`, ASAN paths. ~40 lines of runtime
  code instead of ~120. The surface-level `Ref` vs `Array`
  distinction lives entirely in the typer (the AST never sees
  the same value as both because the only construction path is
  `Mutable.ref_make`).
- **Against** — `kai_to_string(ref)` would render a Ref as
  `<array>`. No fixture exercises this today, and adding a
  bespoke `Ref[...]` print would require the new tag. Recorded
  as a follow-up if observability ever calls for it.

The plan brief leaned toward a struct `KaiRef { KaiHeader h;
KaiValue value; }`. I read it as a proposal ("Probable struct"),
not a mandate, and the CLAUDE.md "no premature abstraction"
guidance pushed toward the smaller change.

Helpers added in `stage0/runtime.h`:

- `kai_prelude_ref_make(init)` — wraps `kai_array_make(1, init)`.
- `kai_prelude_ref_get(r)` — wraps `kai_array_get_impl(r, 0)`.
- `kai_prelude_ref_set(r, v)` — wraps `kai_array_set_impl(r, 0,
  kai_incref(v))` and returns `kai_unit()` (Doc B's Unit shape;
  `array_set` returns `Array[T]` for stage-2 internal chaining,
  but Ref is new so it can adopt the spec shape directly).

Default-handler trampolines `kai_default_mutable_ref_*` resume
the continuation with the prelude helper's result, mirroring the
existing array clauses.

LLVM-visible wrappers `kaix_default_mutable_ref_*` and
`kaix_prelude_ref_*` were added to `runtime_llvm.c`.

## Catalog wiring (stage2/compiler.kai)

- `builtin_mutable_decl()` — three new EOps appended after
  `array_grow`:
  - `ref_make[T](init: T) : Ref[T]`
  - `ref_get[T](r: Ref[T]) : T`
  - `ref_set[T](r: Ref[T], v: T) : Unit`
- `default_mutable_shims()` — three new C trampolines.
- `default_mutable_setup()` — three new struct-field stores by name.
- LLVM `declare`s for the three `kaix_default_mutable_ref_*`
  symbols.
- LLVM `getelementptr` + `store` triplets at struct slots 8/9/10.

`Ref[T]` did not need explicit type registration: `resolve_ty`
falls through to `TyCon(name, args)` for any uppercase
identifier, so `Ref[Int]` is automatically a TyCon and the
Hindley-Milner machinery handles unification.

## Masking discipline (issue #251 + #252)

The existing `mask_local_mutable_demand` pass at
`stage2/compiler.kai:25742` only acts on **bare prelude
`array_set` / `array_grow` calls** (raised via
`add_prelude_effect_label`). Dotted op calls
(`Mutable.array_set`, `Mutable.ref_set`) flow through
`synth_op_call_with_scheme`, which always adds the effect label
unconditionally.

That means `Mutable.ref_set(c, v)` always raises `Mutable` in the
caller's row — same as `Mutable.array_set(...)` does today. No
extension was needed: Refs and Arrays inherit the same dotted-op
discipline, and the local-array masking path stays scoped to the
sugar desugar (`a[i] := v` → `array_set`). If we ever introduce
sugar for refs (`*c := v` was explicitly out-of-scope per the
issue), the masking pass would need to grow to recognise
locally-allocated refs the same way it tracks
locally-allocated arrays.

## Empirical verification

Issue repro snippet in `/tmp/issue_257_repro.kai`:

```kaikai
fn main() : Unit / Mutable + Console = {
  let c = Mutable.ref_make(0)
  Mutable.ref_set(c, Mutable.ref_get(c) + 1)
  print(int_to_string(Mutable.ref_get(c)))
}
```

`bin/kai run /tmp/issue_257_repro.kai` → `1` (matches the
acceptance gate). The committed fixture
`examples/effects/mutable_ref_basic.kai` exercises two
increments and golden-asserts `2`.

## Friction points

- The plan asked to also run `make selfhost-llvm` from the
  repo-root Makefile, but the target lives in
  `stage2/Makefile`. Trivial — invoked via `make -C stage2
  selfhost-llvm`.
- `bin/kai` does not expose a `--backend llvm` flag. The LLVM
  backend was exercised through `make -C stage2 selfhost-llvm`
  (which runs the LLVM emitter end-to-end). The fixture itself
  only runs through the C backend in CI; that is the existing
  pattern for `examples/effects/`.
- Pre-existing failure on `mini_ledger` demo also reproduces on
  `main` — unrelated to this lane (verified by stash + retry).

## Subjective summary

Wave 5 was the lightest of the five. The op-catalog machinery
is well-factored: `compute_op_offsets_for_eff` calculates field
offsets purely from declaration order, so adding three ops at
the end of `builtin_mutable_decl` propagates correctly through
the C struct layout, the LLVM struct, the dispatcher, and the
default-handler emit — no offset edits needed for existing
ops. The biggest decision was the runtime representation; the
"ref is a 1-slot array" choice cut the lane from a projected
3.5h to ~20 min wall time.

## Limitations / follow-ups

- `kai_to_string(ref)` will print `<array>` because the runtime
  tag is `KAI_ARRAY`. If any future feature surfaces refs in
  diagnostic output, introducing `KAI_REF` is the clean fix.
- No surface deref sugar (`*c`, `c.value`) — explicitly
  out-of-scope per issue #257.
- `uira/boids.kai` still uses the `rcell`/`icell` 1-element-array
  hack — separate cleanup PR per the lane brief.
- Atomic / shared-fiber refs are out of scope; future work
  belongs to the `Spawn` mailbox primitives or a new effect.
