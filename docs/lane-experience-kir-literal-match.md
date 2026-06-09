# Lane experience — KIR literal-pattern match lowering

Branch off `kir-native-walk`. This lane fixes a frontend bug in the
AST→KIR lowering: a `match` discriminated by **literal patterns**
(`match n { 0 -> 100; 1 -> 200; n -> n + 1 }`, or `'c'` / `"s"` /
`true`/`false`) lowered to a malformed KIR — an EMPTY `KSwitch` routing
to a default block that kept only the FIRST non-variant arm's body and
never emitted the catch-all bind. The C-direct backend (which emits from
the AST, never the KIR) was unaffected; the bug was confined to the
`--emit=kir` / `--emit=native` path, where it SEGFAULTED (exit 139).

This is the literal-pattern sibling of the list-pattern bug fixed in
PR #792 (`kir_lower_match.kai`): the same root shape (empty `KSwitch` +
binds never emitted), the same fix shape (a fail-chain decision tree
instead of the variant tag-switch).

## Scope as planned vs as shipped

Planned (brief): make literal-discriminated arms lower like the C-direct
oracle — discriminate by equality, bind the catch-all name, handle the
literal types C-direct supports (Int, Bool, Char, String). Add a native
regression fixture, verify the two named perceus fixtures pass under
native, keep `km` A−/B+, do not regress variant- or list-match.

Shipped: exactly that, via a dedicated equality fail-chain (a `KCondBr`
chain over `kai_eq_raw`), in a new A++ module (`kir_lower_lit.kai`) + a
thin driver in the Expr-SCC (`kir_lower_walk.kai`). All four literal
types reach native↔C-direct parity. No scope cuts.

## Root cause (confirmed)

`lower_match` lowered every non-list match the same way: read
`KTagOf(scrv)`, build one `KSwitch` case per arm keyed on the
constructor tag (`plan_arms` / `arm_tag`), bind variant sub-patterns by
slot. A literal pattern is none of those:

- `arm_tag(PLit)` returned `-1`, so `plan_arms` gave every literal arm
  NO switch case → an empty `KSwitch`.
- `lower_default_arm` + `first_default_arm` lowered only the FIRST
  default-eligible arm's body into the (single) default block; the rest
  vanished, and the catch-all `n -> ...` arm's `n` bind was never emitted.

So `match x { 0 -> 100; 1 -> 200; n -> n + 1 }` lowered to
`switch tagof(x) [] default L1 { L1: t<-100 }` — `200` and `n+1` gone,
`n` unbound. The native backend then built an LLVM `switch` with zero
cases over the variant tag (meaningless for an Int) and segfaulted.

## Lowering shape chosen: CondBr equality chain (not switch-on-value)

The brief offered two shapes: (a) an integer `KSwitch` over the
scrutinee's raw int value, or (b) a `KCondBr` equality chain. I chose
**(b)**, for three reasons:

1. **Generality.** The C-direct oracle's literal patterns span Int, Bool,
   Char, and String (`kai info match`). Shape (a) only works for an Int
   (and would need the *raw* int, not the `tagof` the lowering already
   computes — a different scrutinee repr). Shape (b) is one code path for
   every literal type: the equality prim is polymorphic.
2. **It mirrors the oracle's boxed path.** KIR is all-boxed today; the
   C-direct boxed-scrutinee path is exactly `kai_op_eq(scr, lit)` chained
   in `if`s (`emit_pat_test`'s `PLit` arm). The switch-on-value path is
   C-direct's `MUnboxed` fast path (`emit_match_switch`) — an optimisation
   the KIR has no unboxing for yet.
3. **The machinery already existed.** It is structurally identical to the
   list-match fail-chain from PR #792 — `KCondBr` over a boxed-Bool
   predicate, the same `lower_branch_into` / `match_preamble` / `reopen`
   helpers, the same `LMArm` result type.

### The load-bearing detail: NON-consuming equality

The equality prim must be **non-consuming**. The scrutinee atom is shared
across every arm in the chain (re-read after each failing test). The `==`
prim maps to `kaix_eq` = `kai_op_eq_v`, which CONSUMES both operands (the
m5.x flip) — it would decref the scrutinee on the first failing test and
corrupt the rest of the chain. The runtime already ships the right shim:
`kaix_eq_raw` (stage0/runtime_llvm.c) = `kai_bool(kai_op_eq(a, b))`, which
reads both pointers, consumes neither, and returns the immortal Bool
singleton — so the ephemeral condition needs no drop, exactly like the
list-match `kaix_is_cons_b` / `kaix_is_nil_b` predicates.

The KIR prim is named `kai_eq_raw`; the native `nprim_runtime_sym` already
maps `kai_*` → `kaix_*` by stripping the prefix, so `kai_eq_raw` →
`kaix_eq_raw` with NO backend change. The KIR dumper prints it generically.
The C-direct path never consumes the KIR, so this prim is native-only and
invisible to the selfhost byte-id.

## Structural surprises

- **No KIR→C translator.** The KIR is consumed only by the native backend
  (`emit_native_ops.kai`) and the dumper. The selfhost byte-id gate emits
  from the AST via `emit_c.kai` and never touches the KIR — so my changes
  are byte-id-invisible by construction (confirmed: selfhost stayed
  deterministic). Native parity, not byte-id, is the gate that catches a
  KIR-lowering bug.
- **`main : Int` exit code.** A program whose `main` returns an `Int`
  exits 0, not the returned value — the value-to-exit-code convention is
  not wired for an `Int`-returning main. The native fixtures use
  `exit(value)` to make the value observable; the regression fixture does
  the same (`exit(232)`).

## Fixtures added

- `examples/native/literal_match.kai` — one program exercising all four
  literal types in a single exit-code observable: Int with a binding
  catch-all (`classify(1)=200`, `classify(7)=8` via `n -> n+1`), Bool
  (`pick(true)=3`), Char (`digit('1')=1`), String (`col("green")=20`).
  Total 232 (< 256, carried by the exit code). Wired into
  `tools/test-native-parity.sh` by the harness's `examples/native/*.kai`
  glob — native rc must equal C-direct rc.
- The two perceus fixtures the brief named
  (`examples/perceus/match_pbind_catchall_raw.kai`,
  `unbox_phase2_match.kai`) already existed and exercise this exact shape;
  verified they now run on the native path (`42|200|8` and `200`,
  matching the C path) — before this fix they segfaulted natively.

## Gates run (all green)

- **Native parity** (`tools/test-native-parity.sh`): 10 passed, 0 failed
  — including the new `literal_match` (rc=232, == C-direct).
- **Selfhost byte-id** (`make -C stage2 selfhost`): "selfhost determinism:
  OK (kaic2b.c == kaic2c.c)".
- **tier0** (`make tier0`): OK — selfhost deterministic, demos baseline
  holds (35 passing), arena gate passes.
- **C↔LLVM backend parity** (`tools/test-backend-parity.sh`): pass=413,
  fail=0 (96 skip). Unchanged by an AST→KIR-only edit, as expected.
- **ASAN+UBSan** on the native `literal_match` object + runtime
  (`-fsanitize=address,undefined`): rc=232, 0 sanitizer errors.
- **`km`**: new module `kir_lower_lit.kai` A++ (99.6), cogcom avg 1.1 /
  max 2. `kir_lower_walk.kai` B+ (84.5 → 84.2) — same grade band, the
  three new driver fns are operator-light; the file's weak dimension
  (Halstead F, the Expr-SCC is operator-dense) is pre-existing, not new
  debt. No new duplicate groups (Duplication A++ on both files).

## Follow-ups for next lanes

- **Record-pattern discrimination** (`match p { { x: 0, y } -> ...; { x, y }
  -> ... }`) — FIXED in the kir-record-match lane (`kir_lower_rec.kai`, see
  `docs/lane-experience-kir-record-match.md`). It was the analogue of this
  lane: a field-test decision tree. The field read uses the existing
  `kai_op_field` / `kai_op_field_borrow` prims (name-keyed, NOT a
  `KProj`-by-index — records are name-keyed; the foreseen new node was
  unnecessary), exactly as the C-direct `emit_pat_test_record` /
  `emit_pat_binds_record` oracle does.
- **Guards** (`k if k < 0 -> ...`) are still dropped by every match path in
  the KIR lowering (the `Arm(p, _, body)` destructure discards the
  `Option[Expr]` guard slot, the same as the variant path). The literal
  fixtures avoid guards; a guarded literal arm would lower as if
  unguarded. Out of scope here, but the first thing a guard-supporting
  lane touches.
- **Switch-on-value optimisation.** Once the KIR has Int unboxing, a
  pure-Int literal match could lower to a real `KSwitch` over the raw
  value (C-direct's `MUnboxed` fast path) instead of the equality chain —
  a perf refinement, not a correctness one.
