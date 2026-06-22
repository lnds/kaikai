# Lane experience — issue #882 (native rotation-reuse SIGSEGV)

## Scope as planned vs as shipped

**Planned:** fix the native SIGSEGV (exit 139) on the rb-tree rotation/balance
shape introduced by #880. The brief framed it as "#880's two-branch
`kai_incref_if_shared` reuse emission does not cover the gap3 NESTED rotation
shape on a SHARED donor" and pointed at the `KConReuse` lowering / the C
oracle's `kai_check_unique` SHARED branch.

**Shipped:** a one-token fix in `arm_body_uses_reuse` (`kir_lower_match.kai`),
the #858 structural-dup gate predicate. The brief's mechanism diagnosis was on
the wrong layer — see "Structural surprises" — but its *symptom* model (a kept
child of a SHARED donor embedded with no protecting incref → double-free →
cycle → `size` recursion → SIGSEGV) was exactly right.

## Root cause (as actually found)

gap3 is NOT lowered through the reuse path at all. perceus's
`pcs_try_reuse_variant` only stamps `__perceus_reuse_variant` on a 1:1 arm
(same ctor/arity, donor == this arm's scrutinee). The gap3 rebuild
`Node(Node(lll,llk,llr), lk, Node(lr, k, r))` is non-bijective, so perceus
leaves every rebuild as a plain `KCon` (fresh-alloc) and decides RC entirely
via `dup`/`drop`. The `--emit=kir` dump confirmed it: zero `KConReuse`, zero
`IncrefIfShared` — all `con Node`.

So `KConReuse` / `kai_incref_if_shared` / `reuse_gc` (the #880/#872 machinery
the brief pointed at) are dead code for this shape. The bug lives one layer up,
in the **#858 structural dup** of an owned arm's pointer children.

The #858 dup is gated `owned ∧ ¬arm_body_uses_reuse(body)`. The C oracle
(`emit_c`) unconditionally `kai_incref`s an owned arm's pointer children and
only the reuse arm itself borrow-binds. The native path approximated that with
the `arm_body_uses_reuse` gate — but the predicate descended into **nested
`EMatch`** arms. So the OUTER arm `Node(l, k, r)`, whose body is
`match l { match ll { Node(..) -> reuse } }`, was classified as "uses reuse"
and had its dup of `l`/`k`/`r` suppressed. But the outer arm does NOT consume
its scrutinee via reuse — it deconstructs `t` and drops it at arm exit. Its
child `r`, embedded into the rebuilt `Node(lr, k, r)`, got no protecting incref
and was double-freed by the `t`-drop cascade. The freed cell was recycled by a
later `con` into a node that aliased a live ancestor → a cycle → `size`
recursed forever → stack overflow → SIGSEGV.

A `__perceus_reuse_*` consumes the scrutinee of the NEAREST enclosing match
(its donor is that match's `__pcs_scr`, re-aliased per match-preamble). A reuse
inside an inner match consumes the inner scrutinee, never the outer one. So the
predicate must stop at the `EMatch` boundary. The fix: `EMatch(s, _arms) ->
arm_body_uses_reuse(s)` (was `... or arm_reuse_arms(arms)`), and delete the now-
dead `arm_reuse_arms`.

## Design decisions / alternatives considered

- **Add per-child increfs to the `KConReuse` SHARED branch** (the brief's
  literal instruction). Rejected after the KIR dump: this shape never reaches
  `KConReuse`. Patching there would have been a no-op for gap3.
- **Reinstate #871's scrutinee dup-gate.** Rejected per the brief and on
  merit — it only masks the UAF as a leak (re-inflates the parent RC). The fix
  is the correct *per-arm* incref, not a blanket parent dup. The dup-gate stays
  removed.
- **Stop at `EMatch` (chosen).** Mirrors the oracle structurally: an owned arm
  increfs its own kept children; only the directly-reuse arm borrow-binds. The
  reuse arm itself (`Node(lll,llk,llr) -> __perceus_reuse_variant(...)`, body =
  a direct `ECall`, no `EMatch` to cross) still suppresses its dup correctly.
  The middle arm (`Node(ll,lk,lr) -> match ll {...}`) now correctly gets the
  dup of `ll`/`lr` it always needed.

## Structural surprises the brief did not anticipate

- **C-direct does NOT consume KIR.** The C oracle emits RC straight from the
  AST (`emit_program` → `emit_c.kai`); only the native backend goes through
  `lower_to_kir`. Two independent RC disciplines over the same post-perceus
  AST. That's *why* native can crash where C is clean — and why the bug had to
  be in the KIR-only path, not in perceus (a perceus bug would break both).
- **gap3 is not a reuse shape.** The brief's whole `KConReuse` framing assumed
  reuse lowering; the actual fix is in the structural-dup gate. The KIR dump
  was the decisive tool — it showed `con` everywhere and no reuse ops.
- **The crash is a hang, not a fast fault.** Under lldb the process timed out
  instead of faulting immediately: the corruption is a *cycle*, and the SIGSEGV
  is the stack overflow of the recursive `size` walking it. Matches the
  oracle's own comment in `emit_c.kai` ("aliasing → CYCLE → gap3 stack-
  overflows in `size`").

## Fixtures added / coverage

- `examples/perceus/gap3_balance_minimal.kai` already existed (the #350
  regression fixture). Wired it as a **native crash gate**:
  `test-perceus-882-native-rotation-reuse-crash` (build native, run under
  `timeout`, assert exit 0 + output `8` + bounded leak via `KAI_TRACE_RC`). A
  regression manifests as a hang, so the gate runs under `timeout`. Added to CI
  `tier1-native.yml` shard 2 next to the #860 ledger gate, and to `.PHONY`.
- Second rotation shape verified by hand: `nested_pattern_reuse_balance.kai`
  (native==C, exit 0, leaked=4 < C's 10) and `rb_tree_bench.kai` (1M inserts,
  native leaked=20 == C leaked=20, constant — no O(inserts) leak).

## Gate results (mac, local)

- gap3 native: `8` exit 0 (== C oracle). N-sweep 5..8 all green (N≤4 never hit
  the LL branch). huffman native: round-trip ok, exit 0, output == C.
- #872 leak gate: green, unchanged (`leaked=4 / alloc=9121`). Crash fixed
  without reopening the leak.
- `KAI_TRACE_RC` balanced: gap3 native leaked=2 (< C's 8); rb_tree_bench native
  leaked=20 == C; nested balance native leaked=4 (< C's 10). All bounded
  end-of-program live values, no per-iteration leak.
- selfhost byte-id: OK (`kaic2b.c == kaic2c.c`).
- C-backend ASAN on all three #350 fixtures: clean.

## The ASAN-on-native gap (mac-only)

The brief asked for ASAN on the rotation shape. ASAN+UBSan on the **native**
backend HANGS on mac — confirmed environmental, not the fix: a trivial
`fn main() = print("hi")` native object linked against `runtime_llvm.c` under
`-fsanitize=address,undefined` hangs identically (a hello-world exercises none
of `arm_body_uses_reuse`). The bitcode-link native path can't take ASAN flags;
the legacy `runtime_llvm.c` link does but hangs at startup under ASAN on
Darwin. ASAN coverage for this shape is therefore via the **C backend** (the
`test-perceus-issue350-asan` gate, green) on the same post-perceus fixture, and
the native ASAN run is left to CI Linux where the path works.

## Real cost

A small change with a large diagnosis. The brief's mechanism pointer was wrong,
so the time went into the KIR dump + C-oracle read that relocated the bug from
the reuse path to the structural-dup gate. The fix itself is two lines.

## Follow-ups for next lanes

- **Native heap-bound perf residual.** native still fresh-allocs where the C
  oracle reuses-in-place (rb_tree_bench: 26M allocs native vs 6.3M C, ~2.35× C
  wall). Not a leak (free≈alloc, leaked=20 const) — the reuse-in-place transform
  for the non-bijective rotation shape on native is the open perf work, not this
  lane's correctness scope.
- The `arm_body_uses_reuse` predicate now matches the oracle's per-arm
  borrow/own split exactly; if the reuse recogniser ever gains a cross-`EMatch`
  shape, this boundary must be revisited (it cannot today — perceus re-aliases
  `__pcs_scr` per match).
