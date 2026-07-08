# Lane experience — issue #1130: `call_ind_borrow`, the borrowing indirect call

## Scope as planned vs. as shipped

**Planned:** a runtime call variant that invokes a closure WITHOUT consuming
it, so a `^`-borrowed function-typed parameter can be borrowed through an
indirect call — unblocking the closure/HOF borrow (#1127's deferred "big
one"). Codegen routes borrowed-closure calls to the variant on both backends;
the stdlib `^` annotations (17 sites, inert since #1129) go active. Optional
gated inference extension for private HOFs.

**Shipped:** the runtime variant (`kai_apply_borrow` / `kaix_apply_borrow`),
the KIR `KCallIndirectBorrow` node + native lowering, the C sentinel
(`__perceus_apply_borrow`), the perceus flip (closure params re-enter the
effective borrow set) + the **dup-on-consume** discipline that makes it sound,
and the stdlib `^` annotations going active. The optional inference extension
was **explicitly kept out of scope** (its own issue) — the shipped mechanism
gets the win soundly without touching inference.

## The mechanism

Uniform across both backends, decision centralised in perceus:

1. **Runtime.** `kai_apply_borrow(clo, argc, argv)` — identical to `kai_apply`
   minus the closing `kai_decref(clo)`. In both `runtime.h` copies (stage0 +
   stage2). `kaix_apply_borrow` forwarder in `runtime_llvm.c` (the native
   backend cannot reference a `static kai_*`; the `kaix_` prefix is the
   external-linkage forwarder ABI). `runtime.bc` regenerated.
2. **Perceus sentinel.** A `f(args)` through a `^`-borrowed function-typed
   param is rewritten to `__perceus_apply_borrow(f, args…)`
   (`pcs_wrap_borrow_calls`), analogous to the existing `__perceus_borrow(p)`
   scrutinee wrapper. Centralising the decision in perceus means neither
   backend re-derives "is this callee a borrowed closure param". The
   apply-pipe `a |> f` (= `f(a)`) is covered too — without it the pipe
   desugared to a consuming `kai_apply` on a skip-set (never-dup'd) closure,
   a UAF on any later read (found and fixed mid-lane: `g(x, ^f) = { let a =
   x |> f; f(x); … }` crashed "non-callable value"). The map/flat/filter
   pipes route to `kai_prelude_*`, which already borrow, so only apply-pipe
   needs it.
3. **C emit.** A new sentinel arm in `emit_call_expr_default` emits
   `kai_apply_borrow(<f>, n, {args})`.
4. **Native KIR.** `__perceus_apply_borrow` classifies as `CkApplyBorrow`
   (`classify_callee`) → `lower_apply_borrow` → `KCallIndirectBorrow(fv, avs)`
   → `nemit_call_indirect_borrow` → `kaix_apply_borrow`. Dump prints
   `call_ind_borrow`.

## The soundness call — Option 1 (dup-on-consume), not relaxed inference

The wall #1129 hit was not only the missing runtime variant; it was that a
borrowed closure in the skip-set (never dup'd) flowing to an OWNED callee slot
would reach a consuming callee with no incref → UAF. The stdlib `map`/`filter`
delegate to owned private loops (`map_loop`/`filter_loop`, whose `f` is not
borrowed), so flipping `^f` alone would UAF them.

asu's call (verified against docs/borrow-design.md rule 3 = Koka
`parcBorrowApp`): **dup-on-consume**, not the relaxed inference that reopened
#1127's modular oracle. When a borrowed closure flows to an owned slot, re-
insert `__perceus_dup(f)` at THAT owned-exit (`pcs_strip_args_at_c`:
`borrowed_slot ? strip : bare-borrowed-closure-arg ? wrap_dup`). The private
loop then receives a genuinely owned ref. Crucially the decision is **local**:
it uses only the callee's `bmap` positions (which already cross partition
linkage), so the balance of the closure closes inside the annotated fn's own
body — independent of decl grouping. That locality is why it survives the
sep-comp oracle that the whole-program relaxed inference did not: the emit of
`map` is a pure function of `map`'s body plus its direct callees' bmaps, not of
the whole-program borrow map.

`foldl`/`foreach`/`any`/`all`/`take_while`/`drop_while` are self-recursive:
`^f` is called and re-passed to its OWN (borrowed) position — borrow-through,
zero RC per element, no dup-on-consume. `map`/`filter`/`count`/`max_by`/… that
delegate take one dup-on-consume at the owned loop entry (one incref per HOF
call, not per element).

## Measured (KAI_TRACE_RC)

| shape | before (#1129, owned) | after (#1130) |
|---|---:|---:|
| Probe A: `go(1000, ^f)` closure through tail loop | ~2000 incref + ~2000 decref | **incref_total=0, decref_total=0** |
| closure alloc across the loop | — | `allocs=1 leak=1` CONSTANT (N=100k → still 1) |
| HOF fixture (map/filter/2×foldl over 10 elems) | closure dup per call | correct=170, leaked=11 bounded |

**The measured 2+2/iter → 0 (isolated closure-dominated bench, N=5M, C):**

| | incref_total | decref_total | leaked |
|---|---:|---:|---:|
| owned (`go(5M, f)`, no `^`) | **10,000,000** | 10,000,001 | 7 |
| borrow (`go(5M, ^f)`) | **0** | **0** | 8 |

Exactly 2 incref + 2 decref per iteration eliminated. The native fixture
inherits the collapse from the shared KIR (`incref_total=0`). The +1 leaked is
the bounded fresh-closure caller leak.

**Wall (acceptance #3).** HOF pipeline map+filter+foldl over N=1M list: 0.04s
C, 0.04s native (identical). The workload is **memory-bound** — building and
re-building 1M-element lists dominates; the closure RC (a pointer incref/
decref) is a rounding error against the cons traffic, so the wall does not
move visibly. The closure-dominated bench above (N=5M, trivial per-iter work)
is where the elision shows: ~0.015s owned → ~0.010s borrow, and the RC-op
collapse is total (10M → 0). Honest read: the win is large in RC *ops* (which
matters under fiber RC contention and in tight closure loops), modest in wall
for a pointer-cheap closure — but never a regression.

**rb-tree bench (acceptance #5) — no-regression.** N=1M: kaikai-native 1.91x
vs C (README baseline 1.92x — identical within noise), kaikai-c 1.36x. The
rb-tree insert path uses NO `^` HOFs (the inference extension that would touch
it is out of scope), so its RC counters are unchanged by construction.

## Known imperfection: fresh-closure caller leak (bounded)

A closure literal passed to a borrow slot (`sum_via_fold(xs) = foldl(xs, 0,
LAMBDA)`) leaks ONE ref per call-site: the HOF borrows it (never drops), and
the caller does not drop-after-call. This is BOUNDED — per static call-site,
never per element (verified: N=100k `foldl` still leaks exactly 1). The full
rule-1 fix (caller drop-after-call) conflicts with mandatory TCO when the HOF
call sits in the caller's tail position (rule 6 wants dup-before/drop-before,
which a fresh rc=1 closure cannot take), and the `EBlock([…, SLet(__pcs_ret,
call), drop], __pcs_ret)` wrap that would fix it "cannot be lowered in a
closure body" on native (the #817 note). Left as a follow-up, matching the
#1129 precedent of accepting a bounded borrow leak; the win (zero per-iteration
RC) is unaffected.

## Gates

- Selfhost byte-id **C**: green.
- Modular `--emit=c-modular` (the #1127/#1131 oracle): full compiler links
  (100 TUs) AND compiles+runs `portfolio.kai` (the exact program the relaxed
  inference UAF'd) — green. This is the binary that validated the locality
  thesis.
- Native selfhost byte-id: green (`the native-built compiler emits
  byte-identical C to the oracle`).
- Serial parity subset (perceus + effects, `BACKEND_PARITY_JOBS=1`): green
  (pass=302 fail=0).
- The six `test-perceus-1127-borrow-*` gates (closure/hof/tco × C+native) all
  OK; the C ones assert `incref_total < 100` (the collapse), the native ones
  ride tier1-native.yml (never TEST_LIGHT_TARGETS — the kaic2 there is C-only).
- New `test-perceus-1130-borrow-pipe` (C, in TEST_LIGHT) + `-native`
  (tier1-native.yml) — the apply-pipe UAF regression fixture.

## Fixtures added / updated

- `borrow_closure_1127.kai` — reworked to `^f` (Probe A); the counter gate
  now asserts `incref_total < 100` (was ~2000).
- `borrow_stdlib_hof_1127.kai` — comment + gate flipped inert → active.
- **`borrow_closure_pipe_1130.kai` (+`.out.expected`)** — the apply-pipe
  UAF regression (found mid-lane): `^f` used through `x |> f` then `f(x)`.

The compiler-internal HOF that crashed #1127's C selfhost is covered by the
green selfhost byte-id.

## Follow-ups

- **Caller drop-after-call for a fresh borrowed closure** (bounded leak → 0),
  reconciled with TCO.
- **Inference extension (Option 2):** infer a private fn's fn-typed param
  whose only use is a direct call as borrowed, closing the `map_loop`/
  `filter_loop` owned-exit entirely (no dup-on-consume there). Gated on the
  modular fixture staying green — the #1131 lesson.
