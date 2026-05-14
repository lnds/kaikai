# Lane experience report — issue #585 cache layer ECall panic

PR #584 (issue #452 Phase A.0 step 2) shipped the AST record serdes
layer with a synthetic round-trip self-test that only exercised
`EInt` / `EReal` bodies. The first real prelude flow through
`cache_serialize_module` panicked with a non-exhaustive match in
`cache_expr_list_loop`. Issue #585 captured the bug; this lane fixes
it, extends the self-test to prevent the same regression class, and
documents the root cause for the resolver invariant that was
violated.

## Objective metrics

- Start: 2026-05-14 (post-v0.58.0 + #584 merge).
- End: ~1 session, conversation-driven.
- LOC added (`stage2/compiler.kai`): +30 (self-test extension)
  +5 (renames `args` → `as_`).
- LOC retro: this file.
- Build / test invocations:
  - `make -C stage2 kaic2`: ~6 (incremental build per fix iteration).
  - `make -C stage2 selfhost`: 1 (verified byte-identical).
  - `make tier0`: 1 (clean run).
  - `make tier1`: 1 (clean run).
  - `stage2/kaic2 --cache-roundtrip-test`: ~5 (each fix verified).
- Selfhost byte-identical: OK after the fix.

## Repro confirmation (Step 1)

Applied the patch from the issue body — added an `ECall(EVar("g"),
[EInt(7)])` body inside `cache_roundtrip_self_test`. Built kaic2 and
ran `stage2/kaic2 --cache-roundtrip-test`:

```
panic: non-exhaustive match
```

Patched the emitted `build/stage2.c` with one-line `fprintf(stderr,
...)` traces around `cache_expr_list_loop` and `cache_expr_list_to_hex`
to capture the `_scr->tag` value. Three sequential entries:

```
ENTER cache_expr_list_loop kai_xs=0x... tag=10 rc=2, kai_acc=...
DEBUG cache_expr_list_loop _scr=0x... tag=10
panic: non-exhaustive match
```

`KAI_CLOSURE = 10` per `stage0/runtime.h:101` — the reporter's
hypothesis confirmed at the byte level. The loop's `match xs { [] ->
... ; [h, ...t] -> ... }` was type-exhaustive but runtime-incomplete
because `xs` was a closure value rather than a list.

## Bisect (Step 2): the bug is at the **call site**, not inside the
loop

After confirming the closure tag, I traced `cache_expr_list_to_hex`'s
incoming argument. The first call carried `tag=7 (KAI_CONS)` and
recursed normally; a *second* invocation (during the re-serialise
half of the round-trip) carried `tag=10 (KAI_CLOSURE)`. The closure
appeared *before* the loop ever ran, so the bug is in whichever caller
passes the wrong value, not in Perceus RC ordering around the tail
load.

Grepped for `_kai_prelude_args_thunk` in `build/stage2.c` and found
five sites; two of them sat inside the cache encoder/decoder.

### Site A — encoder ECall arm (`cache_exprkind_to_hex`)

Source (`stage2/compiler.kai:59809`, pre-fix):
```
ECall(f, args) -> concat_all(["0A", cache_expr_to_hex(f),
                                cache_expr_list_to_hex(args)])
```

Emitted C:
```c
KaiValue *kai_args = kai_incref(_scr->as.var.args[1]);
_r = kai_concat_all(... kai_cache_expr_list_to_hex(
    kai_closure(&_kai_prelude_args_thunk, 0, 0, NULL)) ...);
                          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                          shadowed: passes prelude `args()` closure
                          instead of the pattern binding kai_args
```

### Site B — decoder ECall arm (`cache_hex_to_exprkind_body`)

Source (`stage2/compiler.kai:59927`, pre-fix):
```
Some(cel) -> match cel { CExpLP(args, p3) ->
  Some(mk_stub_expr(ECall(f, args), p3))
}
```

Same symptom in the emitted C — the second argument of the
reconstructed `ECall` variant was the `prelude.args` closure rather
than the pattern's `kai_args` binding. This is why the
`serialize → deserialize → re-serialize` round-trip from #584's
self-test would *also* have failed if `EInt`/`EReal` were not the
only bodies present.

### Sites C / D — TyFn encoder and decoder

Grep for the same shadowing failure mode flagged two more sites:

- `stage2/compiler.kai:59439` — `TyFn(args, ret, row) -> ...`
- `stage2/compiler.kai:59494` / `:59501` — `CTyLP(args, p3) -> ...
  TyFn(args, ret, row)`

Both emit the prelude closure instead of the pattern binding. They
did not crash the original self-test only because the fixture had no
`TyFn` return type either. The new self-test (below) covers them.

### Sites not in the cache layer

Two more shadowings ride elsewhere in the emitted compiler
(`build/stage2.c:17202` and `:46367`, inside non-cache code):

- A `RowKind::RL(name, args)` arm in `row_label_display` — the
  resolver picks up the prelude `args()` closure when formatting row
  labels. This path is only exercised by `kai fmt` / diagnostic
  output for rows; it does not appear to crash today but is the same
  bug class.
- A `TyCon(nm)` arm in `tcrec_rule3_mask` that builds a closure
  inside its scrutinee. Less clearly broken — needs a separate
  reproduction.

Filing these as a follow-up rather than fixing inline keeps lane
discipline (one worktree fixes one thing). The fix shape is identical
to sites A/B/C/D: rename the local binding to avoid the prelude name.

## Root cause

This is the *bare-ident shadowing resolver bug* pinned in memory as
`project_bare_ident_shadowing_resolver_bug.md` and instantiated for
the binding `args` in `feedback_kaikai_param_args_shadow.md`. Stage 1's
resolver does not consistently look at the pattern-binding scope when
resolving a bare identifier in the arm's RHS: where the pattern
introduces a name like `args` (or `body`, `head`, `tail`, `map`,
`reverse`, … — any prelude builtin), the resolver sometimes prefers
the prelude function over the pattern binding.

The reporter on issue #585 had already documented that renaming
`args` → `as_` "did NOT resolve the panic" — that was true at the
time the report was written because the fix needed to land in
multiple sites (encoder *and* decoder *and* TyFn pair). Renaming
only the encoder leaves the decoder still panicking once
deserialisation occurs.

This is **not** a Perceus RC ordering bug. The reporter's three
hypotheses (A — RC ordering, B — reshape payload, C — codegen reuse)
turned out to be downstream symptoms of the resolver shadowing; the
actual fix is a local binding rename.

## Fix shape

Four renames in `stage2/compiler.kai`:

| Site | Pre-fix binding | Post-fix binding |
|---|---|---|
| `cache_exprkind_to_hex` `ECall` arm (line 59809) | `args` | `as_` |
| `cache_hex_to_exprkind_body` `tag == 10` arm (line 59927) | `args` | `as_` |
| `cache_tykind_to_hex` `TyFn` arm (line 59439) | `args` | `as_` |
| `cache_hex_to_typeexpr_fn` `CTyLP` arm (line 59494) | `args` | `as_` |

`EIntrinsic` (line 59859 / line 60188) was already using `as_` per
PR #584 — the report mentions the reporter renamed it as a
shadowing-dodge that *did* work, but never propagated the same rename
to the other three sites because none of them crashed without an
extended self-test fixture. This lane completes the propagation.

## Roundtrip test extension

Added four new shapes to `cache_roundtrip_self_test()`:

1. `DFn` whose body is `ECall(EVar("g"), [EInt(7)])` — exercises
   encoder + decoder ECall arms (sites A + B).
2. `DFn` whose body is nested `ECall(EVar("f"), [ECall(EVar("g"),
   [EInt(7)])])` — exercises ECall recursion through the list loop.
3. `DFn` whose body is `EIntrinsic("foo", [EInt(7)])` — exercises the
   sibling `cache_expr_list_to_hex` callsite and confirms it did not
   regress.
4. `DFn` whose declared return type is `TyFn([TyName("Int", [])],
   TyName("Int", []), REmpty)` — exercises sites C + D.

Result: `OK 8 decls round-trip; blob=1976 hex chars` (was 4 decls /
802 hex chars).

These four shapes pinpoint the four shadowing sites above. Any
regression that re-introduces prelude-name shadowing in those four
arms will reproduce the panic immediately at `--cache-roundtrip-test`.

## Selfhost behaviour

Selfhost remains byte-identical (`make -C stage2 selfhost` →
`self-hosting fixed point: OK`). This is expected: the cache layer is
dormant outside the explicit `--cache-roundtrip-test` driver, so the
rename changes nothing in the compile pipeline. The cache layer is
the only consumer of `cache_*` symbols and the only consumer of the
`--cache-roundtrip-test` mode.

## Real cost vs estimate

| Activity | Brief estimate | Actual |
|---|---|---|
| Read issue body + retro + memory | n/a | ~10 min |
| Confirm repro + tag print | 15 min | ~15 min |
| Bisect (codegen vs RC) | not bounded | ~25 min |
| Rename four cache sites | n/a | ~5 min |
| Extend self-test (4 new shapes) | n/a | ~15 min |
| Selfhost / tier0 / tier1 gates | n/a | ~10 min |
| Retro + PR body | n/a | ~15 min |
| Total | 2-4 hours | ~95 min |

The reporter's framing pushed toward Perceus RC theories; the actual
bug was shallower than expected. Time on bisect was well spent — the
TAG=10 confirmation + the byte-level `fprintf` of `kai_xs` at the
two callsites pointed at the closure shadowing in minutes once the
emitted C was in view, before any speculative RC fix could be tried.

## Recommendation: lessons for future cache work

1. **Mirror dispatch on every constructor at write-time.** PR #584
   noted in its retro's "Limitations" section that the synthetic
   4-decl fixture missed 90% of `ExprKind` variants. The bug landed
   on the very first variant the fixture skipped (`ECall`). The new
   self-test now covers `ECall`, `EIntrinsic`, nested-`ECall`, and
   `TyFn` — but `EMatch`, `ELambda`, `ERecordLit`, `EHandle`, pipes,
   `EBlock`, holes, and most patterns still ride uncovered. Step 3
   should add a fuller exemplar fixture or a real prelude flow before
   the cache goes hot.

2. **Beware of bare-ident shadowing across pattern bindings.** The
   resolver has a known quirk (memory
   `project_bare_ident_shadowing_resolver_bug.md`) where pattern
   bindings whose names collide with prelude functions sometimes
   resolve to the prelude. Any new code that uses `args`, `body`,
   `map`, `reverse`, `head`, `tail`, etc. as pattern bindings inside
   a match arm is a candidate for the same bug.

   The lasting fix is a resolver pass that detects pattern-binding
   shadows of prelude names and warns or errors at build time. Until
   then, the convention to follow is the one already informally used
   in `stage2/compiler.kai`: rename `args` → `as_`, `body` → `bod_`,
   etc. The naming is ugly but unambiguous. Filing as a follow-up
   issue is worthwhile.

3. **Two remaining non-cache shadowing sites** (`build/stage2.c:17202`
   `RL(name, args)` arm; `build/stage2.c:46367` `TyCon(nm)` arm) need
   separate reproduction. They are not in the #585 lane scope but
   should be tracked.

## Limitations / follow-ups

- No real-prelude verification was performed: Step 3 of #452 wires
  the `--prelude-cache` CLI driver, without which there is no
  shell-level mechanism to round-trip a real prelude file. The
  extended self-test covers the bug shape and the variants it touches;
  a real-prelude flow is for the next lane.
- The two non-cache shadowing sites identified above are not fixed.
  Per lane discipline ("one worktree fixes one thing"), file a
  follow-up issue with the two repro pointers.
- A general resolver invariant check ("no pattern binding may shadow
  a prelude name without warning") would prevent this whole bug class.
  Not in scope here.
