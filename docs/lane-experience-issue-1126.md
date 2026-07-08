# Lane experience — issue #1126: the read-loop must drop its re-threaded Array container

## Scope as planned vs. as shipped

**Planned (integrator comment, which corrected the issue body):** in the
tail-recursive read loop the re-threaded array parameter never receives
its final drop at the base case, so the container — and transitively its
N boxed elements — outlives its last use. Fix it in the front-end RC
(Perceus), both backends, `closes #1126`.

**Shipped:** exactly that, as a Perceus dup-suppression, not a new drop.
The container drop was never missing at the base arm; a redundant *dup*
on the recursive re-thread inflated the container's refcount by one per
iteration, so the single base-arm exit drop could never reach zero. The
fix suppresses that dup while keeping the exit drop. Both backends, one
file (`perceus.kai`).

## The cause, verified first (the issue body was inverted twice)

The issue body pinned it on the element incref (`array_get` returns
`kai_incref(items[i])` with no balancing decref for a boxed record). The
integrator's comment already corrected that to the *container* drop. I
verified before believing either.

Scoped repro (array dies inside a fn, `let total = read(...)` so
exit-liveness cannot mask it), `KAI_TRACE_RC`, both backends:

| shape (N=1000) | before | after |
|---|---|---|
| `Array[P]` fill + read `a[i].x` | leaked=1004 (1001 records), free=3 | leaked~3–9, free=1004 |
| `Array[Int]` same | leaked=4, array never freed | leaked~1, array freed |

`leaked` was constant across N=1000/10000/100000 after the fix (9 on C,
independent of N) — unbounded → bounded, the exact "pins its array
forever" signal gone.

The KIR made the mechanism unambiguous. Pre-fix `read`:

```
L1:  dup a                       # <- the leak: re-thread dup, never balanced
     .t4 = array_get_borrow(a, i)   # borrow, does NOT consume a
     tcrec-goto {p0 <- a, ...} dropmask=6   # bit0=0: goto does NOT drop old a
L2:  drop a                      # exit drop, base arm only
```

Each iteration: `dup a` (+1), `array_get_borrow` (no consume), goto
re-binds `a` to itself without dropping the old (dropmask bit0=0). Net
+1 per iteration; the base arm's lone `drop a` (−1) leaves RC=N. The
array's RC never hits zero → it and its N boxed records leak.

Pre-#1120, `array_get` *consumed* `a`, so the `dup a` was exactly the
copy that fed that consume — balanced. #1120 made the read a borrow
(`array_get_borrow`, no consume) and the caller-side strip removed the
dup on the *borrow arg*, but the *re-thread* dup was decided by a
borrow-UNAWARE count and stayed. That dup is now pure leak.

## The fix

`pcs_is_non_last`'s fallback dups a read when the flat non-lam use count
is ≥ 2. For `a` in `read` the count is 2 (borrow read + re-thread), but
the borrow read does not consume — the borrow-aware max-path count is 1.
The re-thread is the sole consumer and can transfer raw.

`skip_set` already models "transfer raw, no dup" — but it *also*
suppresses the exit drop (the branch-aware skip assumes the value is
consumed on every path). Here the base arm does NOT consume `a`, so its
exit drop is load-bearing. The two obligations must split:

- `pcs_collect_borrow_move_last_params` collects params with this shape
  (flat count ≥ 2, borrow-aware max-paths == 1, body has a self-tail-call,
  and `nm` absent from every non-recursing tail).
- `rewrite_skip_set = skip_set ∪ borrow_move_params` feeds the dup pass —
  suppresses the re-thread dup.
- `skip_set` (unchanged) feeds `pcs_collect_exit_drops` — keeps the base
  arm's drop.

Distinct sets for dup-suppression vs. exit-drop-suppression is the whole
trick. Union them and you'd re-leak the base arm.

## Structural surprises

1. **The last-use is the borrow read, not the re-thread.** My first
   attempt reused `move_last_set` (issue #817), which suppresses the dup
   at the *lexically-last* use. But `a[i]` sits to the right of `read(a,…)`
   on the same line, so `last_use_for` returns the borrow read's position.
   Moving that is a no-op; the re-thread kept its dup. Suppressing the dup
   on *every* read (skip-set style, minus the exit drop) is the right
   model — the borrow read's dup is stripped anyway.

2. **The scoped repro exposed a second shape.** A `workload()` whose tail
   IS `read(a)` leaves `a` in tail position, so `pcs_collect_block_let_exit_drops`
   skips its exit drop (assuming tail-use transfers raw) while the fallback
   still dups it — the local binding then leaks its array. That is the
   same bug class one scope up (a let-binding, not a param) and is NOT
   fixed here; the canonical issue repro (`Stdout.print(read(a))`, tail is
   `0`) does not hit it. Left as a follow-up shape, not widened into this
   lane (one worktree, one thing).

3. **`pcs_last_use_consuming` gave inconsistent verdicts** for `a[i].x`
   (record, EField over EIndex) vs `a[i]` (int, EIndex) — true for one,
   false for the other, on identical RC shape. It asks about the wrong
   position (the lexical last use = the borrow read). Replaced with the
   structural gate `nm absent from non-recursing tails`, which is
   position-independent and consistent across both.

## Fixtures + gates

- `examples/perceus/array_read_loop_drop_1126.kai` (record) +
  `array_read_loop_drop_int_1126.kai` (int), each with an `.out.expected`.
- `test-perceus-1126-read-loop-drop` (C) and `-native` gate both fixtures
  under `KAI_TRACE_RC`: output exact + `leaked < 100` (the O(N)
  regression floor is ~1004). Wired into `.PHONY`, `TEST_LIGHT_TARGETS`
  (C), and `tier1-native.yml` (native).

## Regression surface exercised

All RC-sensitive perceus gates green post-fix, unchanged: #1120
(incref_total=0), #1117 map-pipe, #817 filter, #1112 discard-decref,
#860 cons cascade, #1048 record, #1053 / #1104 / #872 (rb-tree reuse),
#995 shared/dead-donor reuse, #747 real-slot UAF, #1069 projection, #1083
variant spine. tier0 green (selfhost C byte-id deterministic).

## Real cost

The diagnosis (KIR bisection to the exact `dup a`) was fast; the trap
was the two false starts on *which* mechanism suppresses the dup
(move-last-set position gate → skip-set-minus-exit-drop). Both were
caught by re-running the scoped repro after each attempt, not by
reasoning — the register-level RC accounting is where the intuition
failed.
