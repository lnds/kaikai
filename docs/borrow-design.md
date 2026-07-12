# Borrow annotations — `^` on parameters (design)

**Status:** accepted design, pre-implementation. Consolidates the 2026-07-08
design conversation. Part of the Orongo scope together with the kind
system (#1108). Prim-level borrowing already shipped (#1120); this document
covers the generalization to user function parameters.

## The problem, measured

Perceus' owned calling convention makes every argument consumed: the caller
dups, the callee drops. For a parameter that is only *read*, both operations
are provably unnecessary — the same tax Koka eliminates with its `^` borrow
marker. Measured on kaikai today (`KAI_TRACE_RC`, slope method):

| shape | RC ops today | with borrow |
|---|---|---|
| closure threaded through a tail-recursive loop (`go(n, acc, f)`) | 2 incref + 2 decref **per iteration** | ~0 |
| read-only descent over a *shared* tree (lookup shape) | ~1+1 **per level per call** | ~0 |
| `a[i]` / `array_length` container traffic | 0 since the prim borrow landed | — |

The first row is the big one: every user of `map`/`filter`/`fold` pays it per
element, invisibly. Annotating the stdlib HOFs once hands that win to all
existing code.

What borrowing is NOT for: rebuilding traversals. A function that consumes its
argument to reuse its cells (`insert`, `map`'s list spine) *wants* ownership;
Koka's own rb-tree bench uses no `^` at all, and kaikai's insert path is
already at Koka counter-parity. Borrow is a read-path and HOF lever, not a
reuse lever.

## Prior art already in-tree

Perceus already carries an interprocedural borrow map keyed by callee name +
parameter position: `pcs_build_borrow_map` / `pcs_strip_borrow_args` /
`BorrowEntry` in `stage2/compiler/perceus.kai`. Two producers feed it today:

- **Inference, ultra-conservative:** a parameter is borrowed only when the
  function's match arms bind *nothing* that is later read (the `is_red`
  shape). The moment an arm binds a child that the body uses, the parameter
  falls back to owned — that is precisely the case this design relaxes.
- **The prim table:** `array_get`/`array_length` position 0, with `_borrow`
  runtime variants and the non-shadowed gate.

The generalization rides this machinery; it does not build a parallel one.

## Koka's mechanics (verified against the Koka source)

- Per-parameter `ParamInfo = Borrow | Own` on the *definition*
  (`Common/Syntax.hs`); a name → param-info map (`Core/Borrowed.hs`).
- At a call to a known borrowing function, borrowed arguments are let-floated
  and dropped **after** the call (`parcBorrowApp`, `Backend/C/Parc.hs`); the
  caller skips the dup; the callee neither consumes nor drops.
- **Sound by construction — no escape checker.** If the callee does consume a
  borrowed parameter (returns it, stores it), Parc inserts a dup at that point
  (dup-on-consume). A wrong annotation costs performance, never memory safety.
  The annotation is a calling-convention hint, not a contract.
- Borrow information attaches to *known top-level functions only*. A function
  value flowing through a variable is called with the owned convention — the
  annotation is not part of the function type.
- Koka has no borrow inference (annotation-only). Its stdlib uses `^` in two
  families: indexed-read prims (vector/string) and HOF closure parameters
  (`map(xs, ^f)`, `partition(xs, ^pred)`, `is-empty(^xs)`).

## The kaikai model — hybrid (inferred private, explicit public)

The same discipline kaikai already applies to effect rows:

1. **Non-`pub` functions: inferred.** Relax the conservative rule with the
   Koka callee discipline — a parameter whose children are bound and read can
   still be borrowed; each child use dups instead of consuming the parent.
   No annotation, no surface; the whole-bundle view makes it safe.
2. **`pub` functions: explicit `^`.** Borrow-ness changes the calling
   convention, i.e. it is ABI. Public signatures state it; separate
   compilation serializes it in the module interface alongside the effect row.
   Inference never flips a `pub` boundary silently.

Known trade-off inherited from inference: a refactor can silently flip a
private parameter back to owned (a perf cliff without a diagnostic). Koka's
answer is annotation-everywhere; ours is the `pub` boundary plus, later, an
opt-in report of inferred borrows. Not a v1 gate.

## Surface

```kaikai
pub fn map(xs: [a], ^f: (a) -> b) : [b]
fn lookup(^t: Tree, k: Int) : Option[Int]
```

- `^` prefixes the parameter *name*, only inside a `fn` declaration's
  parameter list.
- **Grammar (verified):** the parameter-start position accepts only an
  identifier today (`parse_fn_params_group`), so a leading `^` is a currently-
  rejected token in a position no expression can begin — and no expression can
  begin with `^` (power is infix-only; the unit exponent lives inside `<...>`).
  The contexts are disjoint at every parse point; LL(1) is preserved with one
  token of lookahead. Precedent for positional glyph reuse: `<` (comparison
  vs `Real<m>`), `|` (variant separator vs map-pipe).
- **Grouped parameters:** `^` marks only the name it precedes and does not
  back-fill across a group — `^a, b: Tree` borrows `a` alone. Write
  `^a, ^b: Tree` for both.
- **Not on lambdas.** Lambda parameters are local; inference covers them.
- **Not in function types.** `(a) -> b` carries no borrow marker: calls
  through a function-typed value use the owned convention (matching where the
  information actually lives — see Koka mechanics above).

## Semantics

1. At a call to a known function with a borrowed position, the caller does
   not dup the argument and places any pending drop **after** the call
   (dropping before the call would be a use-after-free).
2. The callee neither consumes nor drops a borrowed parameter.
3. **Dup-on-consume fallback:** a borrowed parameter that is returned, stored,
   captured by an escaping closure, or passed to an owned position is dupped
   at that point. Never an error.
4. **Children of a borrowed parameter:** pattern-binding a field/child of a
   borrowed value binds it borrowed; a child flowing to an owned position dups
   there. This is the piece the current conservative inference lacks.
5. **Reuse exclusion:** a borrowed parameter is never a reuse-in-place
   candidate, never donated as a reuse token, and never participates in
   dup/drop fusion as a consumed value.
6. **TCO:** mandatory TCO is preserved. Borrow-through (passing one's own
   borrowed parameter on) needs no drop, so self-recursion stays tail. An
   owned value dying into a borrowed position of a tail call takes the
   dup-before + drop-before pair instead of a drop-after (one RC pair at the
   entry frame, O(1), tailness kept).
7. Effects are orthogonal: `^` changes no row, no type, no handler behavior.

## Staging

1. **Callee discipline + relaxed inference (no surface).** Implement rules
   1–6 for inferred borrows on non-`pub` functions. Measurable alone: the two
   probe shapes above drop to ~0 without any annotation.
2. **`^` surface + `pub` ABI + stdlib sweep.** Parse `^` in parameter
   position, serialize in interfaces, annotate the stdlib HOFs and read-only
   container/inspection functions. `kai info syntax` gains one line; `kai fmt`
   normalizes `^` hugging the name.

The array *write*-path RC (`array_set` re-threading, ~4 ops/element measured
post-#1120) is a sibling concern tracked separately; it is a prim/runtime
matter, not part of this surface.

## References

- `docs/perceus-honesty-targets.md` — RC discipline tiers this extends.
- `stage2/compiler/perceus.kai` — `BorrowEntry`, `pcs_build_borrow_map`,
  `pcs_strip_borrow_args`, the conservative `pcs_borrow_params` rule.
- Koka source: `src/Core/Borrowed.hs`, `src/Backend/C/Parc.hs`
  (`parcBorrowApp`), `lib/std/core/vector.kk` / `list.kk` (`^` usage).
- Issue #1120 — prim-level borrow (shipped); the measurement method
  (`KAI_TRACE_RC` slope) reused here.
