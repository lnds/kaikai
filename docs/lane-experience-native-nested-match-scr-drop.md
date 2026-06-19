# Lane experience — native nested-owned-match scrutinee-drop aliasing

**Scope:** fix a native-only use-after-free where two NESTED owned matches shared
the fixed `__pcs_scr_drop` exit-drop binder, which the native flat register table
collapsed into one alloca — the inner store clobbered the outer, and the outer
exit drop double-freed the inner scrutinee. Surfaced as `max([T])` crashing
(EXC_BAD_ACCESS, exit 138/139) while `min([T])` survived.

## How it was found

Not from a bug report — from auditing why `tier1-native` had been **red for the
last several merged PRs** (#862/#863/#864) yet merged anyway. The investigation:

1. The only red check on those PRs was `tier1-native` (the native-vs-C parity
   ratchet); tier0/tier1/tier1-asan were green. Two merged via `autoMerge`
   because `tier1-native` is **not a required status check**.
2. The "NEW gaps" the ratchet flagged (huffman, weather, http_*, list_helpers,
   poly_ord_containers, option_collect_basic, …) had exit codes that **drifted
   between CI runs** (139 vs 124 vs 1) — initially read as flakiness.
3. Run locally and SERIALLY, they were **deterministic** (5/5 identical). The
   "flakiness" was 4-worker load turning a deterministic crash into a
   timeout-vs-segv race. The bug was real, just masked by the parallel ratchet.
4. Bisected to `095680dc` (before #858/#860/#861): the crashes were
   **pre-existing**, not introduced by the recent PRs. The PRs were correct; the
   gate was crying wolf because its baseline never recorded these gaps and its
   single-pass recheck cannot filter a deterministic failure.

## Root-cause diagnosis (the satisfying part)

Minimized `poly_ord_containers` to `max([3,1],[3,2])` + `show`. Key observations
that pinned it without ever reading the native IR (which carries no DWARF for
kaikai code, so lldb only showed `kaix_internal_dup` frame 0):

- `min([3,1],[3,2])` (returns `a`, the FIRST arg) → OK. `max([3,1],[3,2])`
  (returns `b`, the SECOND arg, since `a < b`) → crash. `max([9,1],[3,2])`
  (a > b, returns `a`) → OK. **Direction-dependent: returning the second arg
  crashes.**
- `max` over `String` (concrete impl, returns 2nd arg) → OK. So it needs the
  GENERIC `[T]` impl, whose body is the cons-spine comparator `list_cmp_loop`.
- `cmp` (returns Int, not a param) → OK.

The KIR dump (`--emit=kir`) was conclusive: `list_cmp_loop` is an owned match on
`a` nesting an owned match on `b`, and `__pcs_scr_drop` was bound `= a` then
`= b` then `drop`ped twice — the same literal name. The native register table
(`rspec_add`, name-keyed) dedupes it into one alloca; the inner `<- b` clobbers
the outer `<- a`; the outer exit drop frees `b` twice. The C oracle avoids it via
lexically-shadowed `_scr` statement-expressions (`emit_c.kai`), which the native
flat table has no equivalent for.

`min` returns `a` (still-valid after the corruption, merely leaked), so it ran;
`max` returns `b` (now dangling), so its `dup b` faulted. Exactly the symmetry
observed.

## The fix (TWO parts — the second found by the serial ratchet)

**Part 1 — fresh per-match drop binder.** Mint a `<join>$sd` exit-drop binder
(the join register is already `ls_fresh_reg`-unique) instead of the literal
`__pcs_scr_drop`, so nested matches never share one native alloca — the
structural equivalent of the C oracle's lexical `_scr` shadowing. This alone
fixed the list path (`poly_ord`, `list_helpers`, …).

**Part 2 — DUP the scrutinee into the drop binder.** Part 1 ALONE regressed the
VARIANT path: `gap3_balance_minimal` (an rb-tree rotation, `match t → match l →
match ll → match lr`) went from exit 0 to a 139 double-free. The serial ratchet
caught it (the parallel one would have hidden it as flakiness). Root cause found
by reading the C oracle's emitted source: for a nested match whose scrutinee is a
LIVE arm-alias binder, C emits `_scr = kai_internal_dup(kai_lr)` — it DUPS the
scrutinee, the match-exit `kai_decref(_scr)` frees the DUP, and the arm's own
cleanup `drop lr` frees the original. My `KLet($sd, …, scrv)` aliased `lr`
directly, so `drop $sd` and `drop lr` both freed the same cell. The fix: emit
`KRC(KDup(scrv))` before binding `$sd`, mirroring the oracle exactly. For a
fresh-value scrutinee (the list path) the dup+drop is net-zero over a discarded
value — no regression. This second part also closed `option_collect_basic` /
`result_collect_basic` (their 124 timeout was the same over-decref corrupting a
recursion guard into an infinite loop — NOT the separate bug first suspected).

Threading (the bulk of the diff):
- `match_preamble` returns the new binder (`LSPre(join, sd, st)` replacing
  `LSReg`), emits `KLet(sd, …)`, and publishes `sd` into `LowerSt.mscr`.
- `match_finish(owned, sd, r)` and `match_selftail_scr_drop(owned, sd, body, st)`
  take the binder explicitly. `lower_match` no longer wraps a single
  `match_finish` — each specialised path (`lower_{list,lit,record}_match`,
  `lower_match_blocks`, `lower_guard_chain`) plants its OWN with its own `sd`.
- The TRMC cons-modulo step (`__kai_cons_s`, map/filter) drops the consumed
  scrutinee in the EMITTER, far from the arm. Its drop register travels two ways:
  the new `KTrmcStep` field (lowering → `emit_native_term` → `nemit_trmc_step`),
  fed from `LowerSt.mscr`. The old `emit_native_trmc` literal
  `nemit_load_reg("__pcs_scr_drop")` is gone.

`LowerSt` gains an `mscr: String` field (12 constructors updated, the same
threading pattern as `locals`/`renames`/`ffi`). A TRMC step always reads the
`mscr` set by its own enclosing match before any arm body lowers, and a
sibling/outer match re-sets `mscr` in its own preamble — so no stale read can
occur and no explicit save/restore is needed (documented at the set site).

## Why threading and not a one-liner

The drop binder is consumed in THREE places that don't share a stack frame:
`match_finish` (continuation), `match_selftail_scr_drop` (arm body, tcrec
back-edge), and `emit_native_trmc` (the EMITTER, reached via the `ECall` dispatch
far from the match). The reuse-donor alias `__pcs_scr` could NOT be freshened —
perceus's recogniser emits that exact name (`perceus.kai`) for `KConReuse`
donors. Only the DROP binder is lowering-private, so only it is freshened.

## Verification

- The crash repro `max([3,1],[3,2])` → exit 0, `[3, 2]`; `poly_ord_containers`
  full → exit 0, all 17 lines.
- 4 of the 5 previously-crashing fixtures reach native↔C parity
  (poly_ord_containers, list_helpers, list_zip3_scan, http_client_basic). The
  5th, `option_collect_basic`, is a **separate** pre-existing bug — a 124 timeout
  (infinite loop in `option.collect`), not the nested-match double-free; it
  printed the first line then hung both before and after this fix. Filed
  separately, out of scope here.
- map/filter (the `__kai_cons_s` TRMC path) and the #860 cons-selftail fixture
  still pass — the TRMC drop rename did not regress them.
- selfhost byte-id OK (kaic2b == kaic2c). KIR goldens regenerated:
  `control_flow` and `list_match` diff is ONLY `__pcs_scr_drop → .tN$sd`,
  nothing else.
- New regression fixture `examples/perceus/native_nested_match_scr_drop.kai`
  (+ `.out.expected` from the C oracle), covered by the `examples/perceus`
  arm of `tools/test-backend-parity.sh`.

## Residual / follow-ups

- The `min`/`max`-over-lists path may still LEAK the escaping scrutinee — a
  benign under-decref, NOT a double-free, same class #860 attacked elsewhere. The
  crash (the load-bearing bug) is closed and parity holds.
- The gate lesson: `tier1-native` was red for REAL — pre-existing native crashes
  (this nested-match double-free) that the baseline never recorded, surfacing as
  drifting 139/124 exit codes that looked flaky under the parallel ratchet's
  load. Being a non-required check let the noise be ignored, and #862/#863/#864
  merged over it. This fix closes the whole cluster (huffman, gap3,
  poly_ord_containers, list_helpers, list_zip3_scan, http_client_basic,
  option_collect_basic, result_collect_basic). A non-required gate that is
  chronically red protects nothing — once green, it is worth making
  `tier1-native` required, or fixing the recheck to confirm gaps SERIALLY (the
  parallel recheck cannot distinguish a deterministic crash from flakiness).
- The serial ratchet earned its keep here: it caught the Part-1-only regression
  that the parallel ratchet would have passed as flaky. Any RC-touching native
  change MUST gate on `BACKEND_PARITY_JOBS=1`.
