# Lane experience — issue #785: trailing comment on a multi-line construct's last line round-trips inline

**Branch:** `fmt-r7-trailing`
**Scope shipped:** make `kai fmt` idempotent on a trailing comment that sits on
the LAST source line of a multi-line construct (a sum-variant arm). After #784
this was the only non-idempotent file in the whole `stdlib/` + `stage2/compiler/`
corpus; this lane closes the residual R7 tail of #93 that #781 explicitly left
unchanged.

## The bug, precisely

```
pub type KOpThunk = KOT(String, String)   # op name -> clause-body thunk sym
```

is the LAST declaration of `kir.kai`. `kai fmt` always expands a sum to its
two-line form:

```
pub type KOpThunk
  = KOT(String, String)  # op name -> clause-body thunk sym
```

Pass 1 keeps the trailing inline (correct). Pass 2 demotes it to a leading
(full-line) comment above — non-idempotent.

## Root cause — one coordinate, one off-by-one

`fmt_drain_trailing_after(s, limit)` drains a pending trailing comment inline iff
`is_tr and (cl < limit)`, where `cl` is the comment's source line. The decl
walker computes that `limit` via `decls_trailing_limit(d, rest)`:

- **non-last** decl → `decl_line(next)` (the next decl's opening line). For a
  multi-line decl the last arm sits well below `decl_line(d)` but still below
  `decl_line(next)`, so the bound covers it. This case was already idempotent.
- **last** decl → `decl_line(d) + 1`. `decl_line(d)` is the decl's **opening**
  line (the `pub type`), not its last line. A trailing on the LAST line of a
  multi-line last decl sits at `decl_line(d) + 1` or below — exactly on the
  exclusive bound — so `cl < limit` is false and the comment demotes.

So the failure is exactly: *last decl + multi-line construct + trailing on its
final line*. Single-line last decls (`fn double(x) = x * 2  # …`) are unaffected
because there `decl_line(d)` IS the last line.

## Design decision — symmetric unbounded drain, not AST line-walking

The issue header (and `fmt.kai`'s old scope note) flagged the "proper" fix as
**option (a): walk the token stream alongside the AST** so the formatter knows a
construct's true last line. I did not do that, and an asu design consult plus a
direct empirical check on the tree both pointed away from it:

- **Option B (derive `decl_last_line(d)` from the AST).** `Variant` and
  `FieldDecl` carry no `line` field; you'd reach through to the last
  `TypeExpr.line`, with a different traversal per decl shape — fragile,
  cogcom-heavy, and it only works because pass-1's reflow happens to align the
  2nd-pass input lines with the AST. Correct today by coincidence, a trap the
  day the reflow changes. Rejected.

- **The chosen fix.** For the last decl, drop the upper bound entirely:
  `decls_trailing_limit` returns the sentinel `0` and `fmt_drain_trailing_after`
  treats `limit == 0` as "no upper bound". Source lines are 1-based, so `0` can
  never be a real `cl` — the sentinel is unambiguous. **Soundness:** a *trailing*
  comment (code preceded the `#` on its line) pending after the last decl can
  only belong to that decl — no code follows it — so dropping the bound cannot
  mis-anchor it against anything below. Leading end-of-file comments are not
  trailing and are handled by `fmt_drain_rest`, untouched.

asu's initial objection to the unbounded form was that it could mis-attribute a
comment in the multi-arm case (several arms each carrying a trailing). The
empirical check refuted that the change *introduces* the hazard: that
mis-attribution **already exists** for non-last decls. `pub type KOp` has 12 arms
each with its own trailing; pass 1 already drains only the FIRST pending (arm 1's
comment) inline after the LAST arm and drops the other 11 — the documented R7
multi-arm loss, a one-time pass-1 event, idempotent thereafter. The fix extends
the *same* (already-live) behaviour to the last decl; it does not create a new
class of loss, and #785 is strictly about *idempotency of the last line*, not
multi-arm fidelity (that stays the broader open R7 edge of #93).

## Scope as planned vs. as shipped

| Planned (brief)                                  | Shipped                                          |
|--------------------------------------------------|--------------------------------------------------|
| Fix last-line trailing idempotency (scoped)      | ✅ sentinel `0` in two functions, ~no churn      |
| Keep change in the drain/limit logic only        | ✅ `fmt_writer.kai` + `fmt.kai`, no walker rewrite|
| Fixture under `examples/fmt/` wired to harness    | ✅ `trailing_last_line.{input,expected}.kai`     |
| selfhost byte-id intact + tier0 green            | ✅ (see gates)                                    |
| Retro in the same commit                         | ✅ this doc                                       |

## Fixture + coverage

`examples/fmt/trailing_last_line.input.kai` carries two single-arm sums: one mid-
file (the case that already round-tripped, as a guard) and one as the file's last
decl (the #785 shape). The harness (`tests/fmt_fixtures.sh`) runs both its checks:
`fmt(input) == expected` and `fmt(expected) == expected` (idempotency) — the
second is what the bug broke. Without the fix, `fmt(expected)` demotes the last
trailing and the idempotency check fails; with it, both pass.

A multi-arm sum with per-arm trailings is deliberately **not** in the fixture: it
would exercise the unrelated R7 multi-arm loss and produce a non-idempotent-
looking expected, muddying the #785 signal. That edge remains tracked under #93.

## Gates

- `kaic2 --fmt kir.kai` then re-format: byte-identical (was the sole diff).
- Whole-corpus sweep: 96 files (stdlib + stage2/compiler), **0 non-idempotent**.
- `examples/fmt/` harness: 37 passed, 0 failed (incl. the new fixture).
- `make -C stage2 selfhost`: byte-id intact.
- `make tier0`: green.
- `km score`: `fmt.kai` A++ (99.1), `fmt_writer.kai` B+ (83.4, preexisting —
  83.5 before; the +12 LOC are the doc comment, no logic-grade change).
  `km cogcom`: both avg < 5 / max < 25. `km dups`: 0 new groups.

## Follow-ups left for next lanes

- **R7 multi-arm trailing loss (#93).** Several trailings on the arms of one sum
  still collapse to one (the first, re-anchored to the last arm) in pass 1. The
  real fix is option (a) — a token-stream-aware walker that knows each arm's
  span — and is a larger refactor than #785 warranted. Not regressed here.
- **Trailings on inner record fields when the record collapses to one output
  line.** Same option-(a) class, untouched.
