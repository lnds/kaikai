# Lane experience — issue #1131 (modular-safe relaxed borrow inference)

Re-land the relaxed read-path borrow inference that #1127 shipped and #1129
reverted after it UAF'd the sep-comp compiler, this time with the soundness
gate that closes the actual hole.

## Scope as planned vs as shipped

Planned (the issue's reopening gate): a modular fixture RED first, then
root-cause, then re-land on the fix. Shipped exactly that, plus a module
split the plan did not anticipate (the fix pushed `borrow_infer.kai` below
the `km` grade floor, so the escape analysis moved to its own module).

What ships:

- `compiler/borrow_infer.kai` restored (the relaxed inference from the #1129
  branch history), with the INSPECTION rule now guarded.
- `compiler/borrow_escape.kai` — NEW. The escape analysis that makes the
  INSPECTION rule sound under separate compilation, plus the shared
  structural `binf_absent*` var-presence walk (a leaf both readers consult;
  it sits in the lower module so neither forms an import cycle).
- `perceus.kai` / `emit_c.kai` — the `pcs_borrow_params_hybrid` signature and
  its three call sites take `pcs_borrow_key(name, mo)` again (the revert had
  dropped the self-key that self-call detection needs).
- Two gates: `test-perceus-1131-modular-escape` (the RED-first regression, on
  tier1-shard-1 beside `test-modular-selfhost`) and
  `test-perceus-1131-borrow-descent` (the read-descent RC-counter probe, in
  the light pool).

## The root cause, verified — which suspect it actually was

The brief named two suspects (asu's, from the #1127 retro): **TRMC-plant-
before-goto** and **per-partition static-ization**. Both are WRONG, refuted
by the pipeline order (`driver.kai:4671-4742`): `perceus_pass` and
`tcrec_rewrite_decls` run whole-program ONCE, *then* the code forks to
`emit_program_modular` vs `emit_program`. So the borrow map, the caller-side
`__perceus_dup` strip, and the TRMC skip_set are byte-identical in both
modes — a function-by-function diff of the self-compiled compiler confirmed
it (of 13213 common functions, only 3 differed, and those differed on the
`_donor` reuse-token, which is orthogonal to borrow and OFF in modular by
design).

The real cause is neither suspect. It is the **INSPECTION rule itself**. A
non-`pub` param used only as a bare match scrutinee was borrowed on the
theory (stated verbatim in the old `borrow_infer.kai` header) that
"arm-bound children re-incref at the bind so no component escapes borrowed."
That is Koka's dup-on-consume — **and kaikai's RC pass never implemented
it**. So a child extracted from a borrowed scrutinee and let ESCAPE
(returned bare, stored, passed owned) is an interior pointer of the borrowed
parent whose refcount was never bumped; once another path frees the parent
to its true zero, the child dangles.

Why single-TU masked it and modular did not: this is a use-after-free, and
whether it manifests depends on WHEN the parent's cell is reclaimed and
reused — which reuse-token donation (`_donor`, ON in single-TU, OFF under
separate compilation) shifts. The single-TU donation path happened to keep
the freed cell alive long enough; the modular build, with donation off, reused
it immediately and the dangling child read a foreign tag → `panic:
non-exhaustive match` in `expand_ta_decl`. So the brief's framing ("the emit
is not a pure function of the borrow map") was directionally right about
*why modular differs* but wrong about *the mechanism*: it is not
partition-linkage, it is donation shifting the free/reuse timing of a bug
the inference planted regardless of mode.

The trigger shape, reduced from portfolio: `#[derive(Show)]` over a record
with a unit-typed field. The compiler's derive+unit path runs the exact
inspect-then-escape shape on its own AST. `examples/perceus/
borrow_modular_escape_1131.kai` is that shape, minimal.

## The fix — serialize? no. Bound the inference.

The brief offered two directions: make borrow decisions partition-invariant,
or serialize inferred borrows in the TU interface. Neither is needed, because
the diff proved the borrow decision is ALREADY partition-invariant (whole-
program perceus) — the bug is that the decision was unsound *everywhere*, and
only surfaced in modular by timing. So the fix is neither: it **bounds the
inference to what the RC pass actually guarantees**. A bare scrutinee is
borrowable only if every child binder is used borrow-ably in the arm body —
inspected or borrow-through, never escaping to an owned position. The escape
walk is tail-position-aware: `EVar(child)` escapes only in tail position (it
becomes the returned value); as a binop/comparison operand it is a pure read
that retains nothing. Getting the tail flag right is what preserved the win —
a first cut treated every `EVar(child)` as an escape, which killed the borrow
of `lookup` (its bound `v` appears in `k == v`) and regressed the descent
counters back to 23097. Threading `tail` recovered 21097.

This aligns with `borrow-design.md` (borrow IS serialized in the module
interface for `pub` params) without needing to serialize the *inferred*
borrows: inferred borrows are non-`pub`, decided whole-program, and now
sound on their own — the caller/callee contract never crosses a partition
boundary the way a `pub` ABI does.

## Structural surprises

- **The oracle is the modular compiler, not a user program.** Every attempt
  to reproduce the UAF with a small two-module user program compiled *by the
  sane compiler* came back green — the escape shape in user code does not
  dangle because the surrounding RC keeps the parent alive. The bug only
  bites the compiler compiling itself modularly. The fixture is therefore
  "compile shape X with the modular-built compiler," not "run shape X."
- **`test-modular-selfhost` needs `KAI_BACKEND=c` on a native-default
  checkout.** `bin/kai` resolves the backend before `KAI_MODULAR`, so a
  kaic2 built with `make KAI_LLVM=1` routes `main.kai` to the single-TU
  native path (which link-fails: the helper link omits the libLLVM shim).
  Not a compiler bug — an env artifact. Documented so the next lane does not
  chase a phantom link error.

## Fixtures added / coverage

- `examples/perceus/borrow_modular_escape_1131.kai` (+ `.out.expected`) — the
  RED-first regression. Proven RED (panic) with the inference un-gated,
  GREEN (byte-identical to single-TU) with the escape gate. Wired to
  tier1-shard-1 beside `test-modular-selfhost`.
- The read-descent probe was re-pointed at the live inference and gated on
  its incref counter (`test-perceus-1131-borrow-descent`, light pool).
- Coverage gap: no *user-level* fixture reproduces the UAF (the shape needs
  the compiler's own reuse-timing to dangle). The modular-selfhost oracle is
  the only reliable trip-wire — logged here so nobody adds a user-program
  fixture expecting it to catch this class.

## Cost vs estimate

Root-cause dominated: the two named suspects were both dead ends, and the
diff work to refute them (function-by-function C comparison of the
self-compiled compiler, single-TU vs modular AND conservative vs relaxed)
was what pointed at the INSPECTION rule. The fix itself was small; getting
the tail-position distinction right (to keep the win) took one wrong cut.

## Follow-ups left for next lanes

- **Closure borrow (#1130, sibling lane).** Still deferred — `call_ind`
  consumes; needs `call_ind_borrow`. Untouched here.
- **Dup-on-bind in perceus (the deeper fix).** The INSPECTION rule is bounded
  conservatively today: a scrutinee whose child genuinely escapes is refused
  the borrow. If perceus grew a real dup-on-bind (incref the child at the
  pattern bind), the escape gate could relax — an escaping child would be
  owned-safe. That is the Koka model the original rule assumed existed;
  building it would recover the borrow on inspect-then-return shapes. Not
  needed for #1131; a genuine perf follow-up.
