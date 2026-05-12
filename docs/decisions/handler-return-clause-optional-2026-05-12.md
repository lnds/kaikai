# Decision: handler `return` clause stays optional, defaults to identity

**Date:** 2026-05-12
**Closes:** #539
**Status:** accepted

## Context

Issue #539, surfaced by the negative-space audit phase 2 (#520), asked
whether `handle { body } with E { ops }` that omits the `return(x) ->
...` clause should be rejected by the typer. The original framing
quoted `docs/effects.md` "every handler must declare a `return` clause
to transform the body's final value" as evidence that the omission was
a silent contract.

Re-reading the doc more carefully revealed that the line cited is
contradicted by a more specific paragraph on the same page (lines
311–313):

> `return(x)` is an optional clause that runs on `body`'s normal
> return path. If absent, the return value of `handle` is the
> return value of `body`.

So the "must declare" line was an exaggeration in the audit; the doc
already says optional and already specifies identity-as-default. The
typer's current behavior (`check_return_clause_opt` returning
`RetTyped { ty: body_ty, ret_opt: None }` when the clause is omitted)
matches that paragraph. No silent contract.

The remaining design question is whether to keep this default or to
enforce explicit `return` clauses.

## Options considered

### Option 1 — Always require `return`

Reject every handler that omits the clause. Forces retrofit of every
existing stateless handler in stdlib and demos (`Console`, `Log`,
`Stderr`, etc.) — likely hundreds of sites. Predictable, simple typer
rule, but breaks the common case for no clear benefit: stateless
identity is the most common shape and writing it explicitly is pure
noise.

### Option 2 — Reject if-and-only-if the effect "requires" a transformation

Only emit an error when the effect's declared return shape is
non-identity (e.g. `State[S]` whose `return` transforms `x` to `(x,
state)`). Sound in theory. Two practical problems:

1. **Effects have no place to declare a return shape today.** The
   `effect E { ops }` syntax names ops but not the return-of-handler
   shape. Two handlers of the same effect can have different return
   types; the typer has no way to know "this effect requires a
   transformation" without inventing new syntax.
2. **Predicate definition is the trap.** "Requires a transformation"
   is a property of the handler's intent, not the effect's definition.
   `Console.print` is stateless; a hypothetical handler that buffers
   to a list and exposes the list via `return` is stateful. Same
   effect, different intent.

### Option 3 — Synthesize identity and emit a warning

If `ret_opt == None`, synthesize `return(x) -> x` silently and emit
a warning suggesting the user might have intended a transformation.
The audit's first instinct.

**Blocker:** kaikai's warning infrastructure is intentionally minimal.
There is exactly one runtime call (`diag_warning`, stage2/compiler.kai
line 11540) used in two places (issue #243 ambiguity warnings, and
typed-hole non-strict mode). Adding a third use case opens the door
to "let's have warnings as a feature," which requires:

- A lint-code taxonomy (e.g. `unused_variable`, `unhandled_return`).
- Suppression annotations (`#[allow(...)]` or similar).
- `--warn-as-error` / `--deny-warnings` flags.
- Documentation on which warnings exist, when they fire, how to
  silence them.

That is several sprints of infrastructure work — disproportionate to
closing #539. Worse, it commits kaikai to a "we have warnings now"
direction that conflicts with the existing posture: every other gap
the audits caught was promoted to error, not warning. The two existing
warnings are transitional states (typed holes during development,
import ambiguity during refactor) — qualitatively different from
"handler omits a clause that has an obvious default."

### Option 4 — Tie to #533 (user-declared default handlers)

If `effect E { ops; default { clauses } }` ships (#533), the `default`
block can declare a `return` clause. Handlers of users that omit the
clause inherit the effect's default `return`; if the effect declares
no default and the user omits the clause, the typer rejects.

This is internally consistent with the rest of #533's merge semantics
(see issue body, "Decision 3 — `return` is uniform"). But it forces
every effect that wants identity-default to write `default { return(x)
-> x }` explicitly in its declaration. The user vetoed this
explicitly: "obliga a escribir siempre el default, no queremos eso."

### Option 3b — Synthesize identity, no warning, no diagnostic

The actual chosen option. Identical to Option 1 of the original audit
list: keep current behavior, document the decision, close #539 as
wontfix.

**Rationale:**

- The doc already promises identity-as-default (effects.md:311-313).
- The typer already implements it.
- Most handlers are stateless; making the common case verbose
  serves no one.
- Stateful handlers that genuinely need a non-identity `return` write
  it explicitly. The author of a `State[S]` handler typing
  `return(x) -> (x, state)` is making an intentional choice and is
  unlikely to forget. The bug shape "I meant to write a transforming
  return and forgot" is theoretical; no concrete instance was named
  by the audit.
- Future-proof: if a specific effect (e.g. `State`, `Writer`) later
  needs to require an explicit `return`, that constraint can land
  per-effect under #533 without making it a global typer rule.

## Decision

**`handle { body } with E { ops }` without an explicit `return` clause
is legal and equivalent to `return(x) -> x`. No diagnostic, no
warning. The typer's existing behavior is correct as designed.**

`docs/effects.md` already documents this on lines 311–313; no doc
change beyond making the intent more emphatic. The audit's framing
that the omission was a "silent contract" was a misread of the doc.

## What changes

- `docs/effects.md`: light edit on the `return(x)` paragraph to
  reinforce that identity-as-default is intentional, not an
  oversight. Reference this decision doc.
- `examples/negative/silent_contract/handle_missing_return_clause.kai`:
  delete (the fixture's premise is wrong) or convert to a positive
  fixture demonstrating identity default.
- `#539` closes as `wontfix` with a comment linking here.

## Why this is not future debt

If a future lane discovers a real instance of "I wrote a `State[S]`
handler, omitted `return`, lost the state" — file a fresh issue with
the repro. The right fix at that point is either:

1. Add a per-effect annotation that says "this effect's handlers must
   provide a `return` clause" (post-#533, narrow scope).
2. Promote the user's specific handler to declare `return` explicitly
   (already legal, just verbose).

The decision today is "don't preempt a problem that has no concrete
report." It can be revisited per effect, not globally.

## Cross-references

- `docs/effects.md` §`handle ... with` lines 298–314 — the
  authoritative spec for handler shape.
- `docs/effects.md` §"State as an effect" lines 492–532 — the
  worked example showing both identity and non-identity `return`.
- Issue #533 — user-declared default handlers, the right vehicle
  for any future per-effect "requires return" enforcement.
- Issue #531 — analogous deferred design question (Ref masking),
  also blocked on #533.
- Audit history: Linus + Eric 2026-05-12 (in conversation log) flagged
  warning infrastructure cost; this decision adopts that finding.
