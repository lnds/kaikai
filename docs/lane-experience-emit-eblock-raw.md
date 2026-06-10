# Lane experience — emit a raw tail in multi-statement blocks (issue #779)

## The bug

A UFn-raw function (one returning an unboxable scalar — `Int`, a raw
`Handle`, …) whose body is a **multi-statement** block with a raw tail got
boxed then immediately unboxed:

```c
// before (pick)
return kai_intf(({ int64_t kair_x = ...; kai_int(((kair_x > 0LL) ? kair_x : kair_b)); }));
//     ^^^^^^^^                          ^^^^^^^^ box ... then outer kai_intf unbox
```

`emit_kind_raw`'s `EBlock` arm only handled the **empty-stmts** case raw
(`[] -> emit_expr_raw(tail)`); a block WITH statements (the Perceus
`__pcs_ret` exit-drop wrap, or a plain `let`-then-tail body) fell to the
`emit_expr_boxed(...)` then `unbox_boxed_scalar(...)` fallback. Symmetrically
the unbox pass marked `EBlock([], tail)` MUnboxed but kept
`EBlock(stmts != [], tail)` MBoxed.

For an `Int` tail the round-trip is benign (reversible, just wasteful). For
a raw `Handle` (a `void *` LLVM pointer) `kai_int(...)` / `(...)->as.i` would
**corrupt** the pointer — the latent soundness bug. This lane is the EBlock
sibling of the already-shipped `emit_match_raw` work (same bug-class:
unboxable result kept boxed because the unbox pass did not mark the wrapping
node and the emitter had no raw clause).

## Scope as planned vs as shipped

Planned (from #779 + brief): unbox-pass promotion of
`EBlock(stmts != [], Some(tail))` + an emit_c raw EBlock clause, mirroring
`emit_match_raw`, gated by RC trace + ASAN + selfhost byte-id. Shipped
exactly that, plus one finding that simplified the design (below).

## The promotion predicate (and why conservative)

`EBlock(stmts != [], Some(tail))` promotes to MUnboxed iff:

1. `ty_is_unboxable(e.ty)` — the gate already on every raw mode decision;
2. `child_is_raw(tail)` — the tail's mode is already MUnboxed (bottom-up);
3. `block_stmts_rc_safe(stmts)` — every statement is `SLet` or `SExprStmt`.

(3) is the conservative guard. The block's VALUE is the tail alone; the
statements run only for effects, so making the tail raw moves no drop. The
two statement forms a post-Perceus block actually contains are `SLet`
(bindings — each carries its own RC discipline, dropped at block-end by
`block_unused_lets` or dup/dropped inline) and `SExprStmt` (the
`__perceus_drop(name)` exit drops of the `__pcs_ret` wrap). `SAssert` and
the dead `SVar` / `SUse` / `SIndexAssign` forms (which should have
desugared away) keep the block boxed — correctness over the optimisation.

## Structural finding the brief did not anticipate

The brief said to change BOTH `decide_mode_aware` and `ufn_promote_calls`.
`ufn_promote_calls` turned out to be **dead code** — it has no top-level
caller in the current pipeline (only recursive self-calls; its doc comment
at unbox.kai:18-22 is stale). The live promotion is entirely in
`decide_mode_aware`, which already receives the `fns` table. I updated the
live path (the real fix) and mirrored the change in `ufn_promote_calls` for
consistency, so a future re-wire behaves identically.

The second structural point: **unbox runs BEFORE perceus**, so at unbox time
the `__pcs_ret` wrap does not exist yet — the body is the raw `let`/`if`
block. Promoting it in `decide_mode_aware` makes the inner block MUnboxed;
perceus's `pcs_wrap_with_exit` then propagates `body.mode` onto the new
outer wrap and the `EVar(__pcs_ret)` tail (perceus.kai:3501-3502), so the
emitter sees BOTH the inner block and the exit-drop wrap as MUnboxed. The
SLet-binding-mode threading (`unbox_stmt_aware` records
`LB(__pcs_ret, MUnboxed)`) makes the tail EVar read raw automatically. No
extra plumbing needed — the existing mode propagation does the work.

## The emitter clause

A dedicated helper `emit_block_raw` (mirroring `emit_match_raw`, not
inlined) reuses the EXACT statement-emission path the boxed EBlock arm uses
(`block_unused_lets` → `emit_block_stmts` → threaded `block_lcs`), and emits
ONLY the tail via `emit_expr_raw` into the statement-expression value slot.
Region blocks (deep-copy-out) route through the boxed fallback — the unbox
pass never marks one raw, so the guard is purely defensive.

## Verifying the drops thread unchanged (before/after C)

`scan(s)` — `s` (String) read twice, raw Int tail — is the canonical
leak-shape. Perceus wraps it as
`EBlock([SLet(__pcs_ret, inner), __perceus_drop(s)], Some(__pcs_ret))`.

```c
// after: drop in order BEFORE the raw tail, tail raw (no kai_int / ->as.i)
return ({ int64_t kair___pcs_ret = ({ ...raw arith... });
          { KaiValue *_ = kai_internal_drop(kai_s); (void) _; }
          kair___pcs_ret; });
```

The drop operates on the BOXED binding `kai_s`, not the raw tail — confirmed
present, in order, before `kair___pcs_ret`.

## RC gate results (the load-bearing gate)

`KAI_TRACE_RC` on `scan` in a 100k-iteration loop: `leaked=1030`; the same
loop at 200k iterations: `leaked=1030` — **constant**, not scaling with
iteration count. A skipped per-iteration drop would have made leaked scale
to ~100k/200k. The drop fires every iteration. Single-shot before/after on
the same heap program: `leaked=8` both before (boxed) and after (raw) — the
fix is RC-neutral. ASAN+UBSan on the leak fixture: 0 diagnostics (a drop run
against the raw scalar would be a UAF ASAN catches; a too-early/too-late one
likewise).

Self-compiled compiler proof (not a false-green): `kaic2b.c` contains 401
raw `__pcs_ret` tail blocks and 262 raw multi-stmt blocks with a drop
threaded before a raw tail — including **20 `void * kair___pcs_ret`**
(Handle) blocks, exactly the pointer-corruption case the issue flagged. The
byte-id gate (`kaic2b.c == kaic2c.c`) holds.

## Fixtures added

`examples/perceus/issue_779_block_raw_tail.kai` (+ `.out.expected`): `pick`
(issue repro, `let` + raw `if` tail), `scan`/`mix` (String param read twice
→ `__pcs_ret` exit drop, raw arith tail). Wired as `test-issue-779` (run +
diff, in TEST_LIGHT_TARGETS + test-fast) and `test-issue-779-asan` (in the
root `tier1-asan` target).

## Gates run

- round-trip gone: `pick`/`scan`/`mix` emit raw, 0 `kai_intf(({` round-trips
- values correct: `pick(5,9)=6`, `scan("hello")=10`, `mix(100,"abc")=106`, exit 0
- KAI_TRACE_RC: balanced, no new leak (constant across iteration counts)
- ASAN+UBSan: 0 errors on the leak fixture
- selfhost byte-id: OK (`kaic2b.c == kaic2c.c`)
- tier0: green (selfhost + demos baseline + arena)
- C↔LLVM backend parity: pass=414 fail=0
- native parity: 14/14 (unchanged — C-text-emit only)
- test-unbox-phase2 + perceus-issue350/703/298 + tco-perceus-wrap: all OK

## Follow-ups left for next lanes

None required. Out-of-scope per brief: nested blocks beyond the
post-perceus `__pcs_ret` shape stay boxed (the `block_stmts_rc_safe`
predicate is conservative by construction — any non-SLet/SExprStmt statement
keeps the block boxed). #779's blocking premise (it blocked the KIR native
walk) is MOOT — the native walk completed on the C-API-direct path, not
C-text; this lane closes the residual C-text inefficiency and the latent
Handle soundness bug.
