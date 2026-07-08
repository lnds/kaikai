# Lane retro — #1112 native backend leaks discarded owned values

## Scope as planned vs as shipped

**Planned (brief):** the native `KDo` arm evaluates a discarded op for effect
and drops the LLVM Handle without a runtime decref, so any owned value dropped
in statement position leaks. Emit `kaix_internal_drop` on the runtime value when
the discarded op produces an OWNED result, mirroring the C oracle's `SExprStmt`
lowering `{ KaiValue *_ = <e>; kai_decref(_); }`, without dropping the LLVM
Handle in the way that fired the kaic1-Perceus UAF documented at
`emit_native_term.kai:289`.

**Shipped:** exactly that, plus the sharper diagnosis below. The fix is ~10
lines of codegen (`nemit_do` + `ndo_discards_owned_box`) routing the `KDo` arm
through a boxed-slot gate. The issue framed this as an `array_make`/array-drop
bug; it is not — it is the general discarded-owned-value bug, and the array case
is one (worst) victim.

## The one diagnosis the brief got right that the issue got wrong

The issue (#1112) hypothesised the leak "likely lives in the same
`array_make`/array-drop codegen path on native." It does not. The asu bisection
in the brief nailed it: a minimal repro with **no array at all** —
`fn mk(v: Int): P = P{x:v}; fn main() = { mk(7); ... }` — leaks on native and
frees on C. The KIR for the discard is identical on both backends:

```
.t0 = mk(7)      # KLet(.t0, SBoxed, KCall)
do .t0           # KDo(KLitOp(KVar(.t0)))
```

C's `emit_stmt`/`SExprStmt` lowers the `do` to `{ KaiValue *_ = kai_mk(7);
kai_decref(_); }` — the decref rides the discard. The native `KDo` arm only
evaluated for effect and dropped the LLVM Handle. **Arrays are the worst victim
because `array_set` returns an owned `incref(a)` that is discarded in statement
position** (`array_set(a, i, ..); rest`); the discarded ref pinned the array,
so it never reached rc0, so its element slots were never walked → 7 GB RSS.
Fixing the general discard fixes the array case for free.

## Design decisions and alternatives considered

**Where to gate the decref.** The brief warned to distinguish "ops with an
owned result (need decref)" from "pure-effect ops (nothing to drop)". Reading
the C oracle showed the distinction is simpler than the brief implied: the C
oracle does **not** branch on op kind — it `kai_decref`s *every* `SExprStmt`
result (except a `__perceus_drop` call, which already decref'd internally). This
is safe because `kai_decref` short-circuits on the Unit/small-Int/Bool
singletons a pure-effect discard (`Stdout.print` → Unit) yields. So the native
decision is a repr gate, not a semantic one:

- A discard always lowers as `KDo(KLitOp(KVar(n)))` — the ctor/call landed in a
  prior `KLet` register, so the `KDo` carries the register *read*, never the
  call. The gate is therefore `native_ctx_reg_slot(n) == 0` (a boxed slot).
- A raw-slot register (`SInt64`/`SReal`/`SBool`, tag ≥ 1) owns nothing, and
  `kaix_internal_drop` on a raw scalar reinterpreted as a `ptr` is an over-drop
  (UAF). The boxed-slot gate excludes it — this is the load-bearing soundness
  check, not an optimisation.
- A `KLitOp(KUnitV)` (a `__perceus_drop`'s residual `do unit`, an inlined
  constant) is not a `KVar` and is excluded — the real decref already fired as
  the prior `KRC(KDrop)` statement, so decref'ing here would double-count.

Considered and rejected: replicating the C oracle's "decref everything"
literally. On native, a `KDo` can carry a raw scalar (the C IR has no raw
statement-position values the same way), so an unconditional decref would
over-drop raws. The boxed-slot gate is the native-faithful translation.

**Respecting the kaic1-Perceus Handle rule.** The `:289` comment documents that
leaving a bare handle in statement position lets the type-blind kaic1 Perceus
`kai_decref` the raw LLVM `Value*` (a phantom-`phi` UAF). The fix captures the
value Handle in a `let h = ...` before passing it to `kaix_internal_drop`, and
captures the drop-call result in `let _d = ...` — the same handle-binder
exclusion `nemit_rc`/`KDrop` already uses. No new UAF surface.

## Structural surprises the brief did not anticipate

- **`docs/native-slot-rc-analysis.md` does not exist in the tree.** The brief
  cited it as the "complete analysis" to read first; it was brief context, not
  a committed doc. The brief's inline description plus the code was enough.
- **The `KDo` never carries a `KCall`/`KCon` directly.** The brief's mental
  model ("ops with an owned RESULT like `mk(7)`") suggested the `KDo` might hold
  the ctor. It never does — `bind_op` always names a fresh `KLet` register and
  the `KDo` reads it. This is what makes the slot gate trivial: inspect the
  register's slot, not the op's shape.

## Fixtures and coverage

`examples/perceus/native_discard_decref_1112.kai` (+ `.out.expected` golden)
exercises two discard shapes: a bare ctor result discarded per loop iteration,
and the issue's `Array[Record]` via a discarded `array_set` across 100
build-and-drop rounds. Gated by `test-perceus-1112-native-discard-decref`
(native, KAI_TRACE_RC): asserts `free_total >= alloc_total - 10`. Wired into
`.github/workflows/tier1-native.yml`. A regression that drops the discard decref
leaves `free_total` near zero and `leaked` growing linearly.

Coverage gap: the fixture asserts near-total reclaim, not exact
backend-for-backend `free_total` equality — the str/print infrastructure
allocates a few backend-specific residuals (native `leaked=1`, C `leaked=7` on
the same source), so an exact-equality assertion would be brittle. The
`free_total` figure IS identical across backends (101202) for the owned work;
the ratchet is on the near-total-reclaim invariant.

## Verification

- KAI_TRACE_RC minimal repro (`mk(7)` discarded): native `record free_total`
  0 → 1, `decref_total` 0 → 1, matching C.
- Issue `Array[Record]` repro: native `leaked` 1000002 → 1, `free_total` 1 →
  1000002 (identical to C). `Array[Wrap(Int)]` variant case identical.
- selfhost byte-id C: OK (`kaic2b.c == kaic2c.c`).
- native self-host gate: COMPILE + LINK + RUN + SELF-COMPILE OK — the compiler,
  full of statement-position discards, self-compiles byte-identically under the
  native backend at -O2. An over-drop would SIGSEGV here; it does not.
- native perceus regression suite (#1048/#860/#817/#747/#872): all green.
- `km cogcom` on the edited file: avg 0.9, max 2 — the two new functions are
  trivial, no degradation.

## Real cost vs estimate

Small. The bulk of the time was reading the C oracle and the KIR lowering to
confirm the slot-gate is the faithful translation (and that a `KDo` never holds
a raw scalar without a raw slot register behind it). The fix itself is ~10 lines.

## Follow-ups

None load-bearing. The `str` residual leak both backends show on the print path
is orthogonal I/O-infrastructure noise, not this bug — out of scope.
