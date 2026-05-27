# Lane experience — issue #697: check-param String body SIGTRAPs on string ops

## Scope as planned vs. as shipped

**Planned:** `kai check "..." with s: String { string_length(s) >= 0 }` SIGTRAPs
(Trace/BPT trap: 5); `{ true }` passes. The brief suspected the check-emission
path: a slot/type-tag mismatch, a KAI_STR field tripping an assert, or a trap
guard in the check runner. It pointed at runtime.h's `kai_arbitrary_string` /
`kai_check_*` as the likely site.

**Shipped:** the trap is neither in the generator nor a runtime assert. It is a
**Perceus ownership-model mismatch** between how the check body is RC-analysed
and how the generated check loop manages the param's lifetime. One-line root
cause: a single-use check-param transferred raw (move) to a decref-aware prelude
consumer, which freed it, and then the loop's own iter-drop double-freed it. The
fix lives in `stage2/compiler/perceus.kai` (emit, not runtime).

## Where the trap originated

ASAN (not lldb — the SIGTRAP surfaced as a libmalloc `mfm_free` assertion with
no useful kaikai frame) pinned it exactly:

```
heap-use-after-free in kai_decref
  freed by:  kai_prelude_string_length → kai_decref (prelude ops CONSUME their arg)
  re-read by: _kai_check_0 → kai_decref(kai_s)   (the loop's iter_drop)
  allocated:  kai_arbitrary_string → kai_str_from_bytes
```

The check loop (emit_c.kai `emit_check_fn`) hand-emits:

```c
kai_s = kai_arbitrary_string();        // RC=1, LOOP owns it
KaiValue *_body = <body_c>;            // string_length(kai_s) consumes kai_s → frees it
...
kai_decref(kai_s);                     // iter_drop: double-free
```

The check loop treats itself as the **owner** of the generated param: it drops
it in `iter_drops`/`tail_drops`, and the shrinker re-evaluates the same `body_c`
against the **same** `kai_s` across iterations. That makes the param **borrowed**
by the body, not owned.

But Perceus analysed the body with the param-names as ordinary fn-param bindings
(move-on-last-use). `string_length(s)` is the only read of `s`, so Perceus chose
a raw transfer — no `__perceus_dup`. Move + loop-owned-drop = the double free.
`{ true }` worked by accident: the body never touches `s`, so the loop drops it
once, cleanly.

This is the *same* alias-not-owned shape the code already documents for the
handler-clause names `state`/`log` (perceus.kai `pcs_is_non_last`): a binding
that aliases storage the body does not own, handed to a decref-aware consumer,
freed, then read again. That carve-out (`if nm == "state" or nm == "log"`) forces
always-dup. The fix extends the same mechanism to check-params.

## The fix

Treat check-params as **borrowed**: force `__perceus_dup` on every read, last-use
included. Threaded a `force_set: [String]` parameter alongside the existing
`skip_set` through the `pcs_rewrite_*` chain (`pcs_rewrite_expr/_kind/_arms/
_block/_stmt/_clauses/_expr_opt/_expr_list/_fields_init/_field_init/_list_elems/
_list_elem` plus the interpolated-string trio `pcs_rewrite_estr_span/_iparts/
_interp_src`). `pcs_is_non_last` returns `true` for any name in `force_set`, same
as for `state`/`log`. Only the `DCheck` branch populates it (`force_set = pnames`);
`DFn`/`DTest`/`DBench` pass `[]`.

Result for `string_length(s) >= 0`:

```c
KaiValue *_body = kai_op_ge(kai_prelude_string_length(kai_internal_dup(kai_s)), kai_int(0LL));
```

`string_length` consumes the dup; the loop keeps the original to drop. The
shrinker's re-emitted `body_c` is identically borrow-safe, so it re-reads the
same `kai_s` across iterations without consuming it. Symmetric with `{ true }`.

### Why not the alternatives

- **Skip-set was not the lever.** Even forcing `skip_set = []`, a single-use param
  still resolves to move (`pcs_count_non_lam_uses >= 2` is false). The decision is
  in `pcs_is_non_last`'s last-use branch, not skip_set.
- **Body-owns + loop-skips-drop** was rejected (asu consult): the loop cannot know
  whether the body consumed the param (control-flow dependent), and the shrinker
  re-evaluates N times on the same value — a consuming body would UAF on the 2nd
  re-eval. Loop-owned + borrow is the only model consistent with the shrinker.
- **`in_lam=true`** would force-dup everything but also wrongly borrow the body's
  own locals. A dedicated `force_set` scopes the borrow to the params only.

### asu's two guards

1. *force_set must scope to the DCheck node, not contaminate sibling fns with the
   same param name.* Satisfied: only the DCheck branch populates it; DFn passes `[]`.
2. *Don't force-dup unboxed scalars (`x: Int`).* Not applicable here: in the
   check-harness every param is a boxed `KaiValue*` (the loop uniformly does
   `kai_decref(kai_x)` on any type), so `__perceus_dup` is always well-typed.
   Verified with a `with x: Int, y: String` fixture under ASAN — clean.

## Fixtures

`examples/stdlib/check_basic.kai` gained a Tier 5 block — the first check block
whose body reads a `String` param through a consuming prelude op:

```kai
check "string length is non-negative" with s: String { string_length(s) >= 0 }
```

The earlier four blocks (Int, [Int], multi-Int) never exercised a consuming read
of a generated value, which is exactly why #697 went uncaught. `make test-check`
bumped 4→5 OK lines and `4/4`→`5/5`. Manually verified additional shapes under
ASAN (`!=`, `++`, two-use `string_length(s)==string_length(s)`, multi-param,
`[Int]`); the `!=` shape also exercises the shrinker path, all clean.

## Cost vs. estimate

Triage was fast once ASAN ran — lldb only gave a libmalloc frame; ASAN named
both ends of the double-free immediately. The diagnosis (ownership-model
mismatch, not a runtime assert) inverted the brief's runtime hypothesis. The fix
itself is conceptually one line (`force_set` in `pcs_is_non_last`) but mechanically
~200 lines of parameter-threading through the `pcs_rewrite_*` chain — invasive but
type-checked and pattern-matched to the existing `skip_set` thread. One asu consult
validated the model and surfaced the two guards.

## Follow-ups

- The `force_set` thread mirrors `skip_set`. If a future lane unifies the
  per-name dup-policy into a single record (`{skip, force}`) the threading
  collapses to one channel. Not worth it for two booleans today.
- Generator coverage for check is still v1 (Int/Bool/Char/String + lists of
  those). Structural/protocol-derived generators (issue #44 path) remain deferred;
  when they land, each new boxed param type rides the same borrow discipline for
  free (the force_set is by-name, type-agnostic).
