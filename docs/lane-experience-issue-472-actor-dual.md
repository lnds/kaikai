# Lane retro — Actor[T] dual instantiation in effect rows (issue #472)

## Branch / scope

Branch: `lane-issue-472-actor-dual`. Closes #472.

A function declaring two distinct `Actor[T]` instantiations in its
effect row (`/ Actor[Request] + Actor[Reply] + Console`) was
rejected by the typer with `effect not handled: Actor[Reply]` —
the second instantiation was silently dropped from the declared
row. This made the canonical request/reply pattern from
`docs/actors.md` §Request/reply literally unimplementable.

## Scope as planned

The brief sketched a single-dispatch story: extend effect-row
matching to be aware of parametric type-args so the op-call
dispatcher could pick the right `Actor[T_i]` based on the
receiver's signature. Estimated 1-2 days, with the bulk in the
op-call resolver.

## Scope as shipped

One six-line patch in `stage2/compiler.kai` plus fixtures. The
op-call resolver did NOT need changes — it already keys on the
op's signature via `find_matching_declared` which calls
`labels_args_unify` on the ty_args. The bug was much earlier in
the pipeline: parametric labels were being collapsed by the row
DEDUP step before dispatch even got a chance to look at them.

### The fix

`stage2/compiler.kai:22824 dedup_row_labels_loop` was keying every
row label by its bare effect name, with an in-source TODO that
explicitly acknowledged the gap:

```kai
let key = match args {
  [] -> n
  _ -> n   # for now we dedup by name only; same caveat as alias expansion
}
```

`Actor[Request]` and `Actor[Reply]` both produced `key = "Actor"`,
so the second was filtered out as a duplicate. The fix keys
parametric labels by `name[arg1,arg2,...]` using the existing
`type_expr_display` renderer, so each instantiation has a distinct
key. Bare-name labels (no args) keep using just the name and the
original aliasing-collapse semantics for `Stdout`, `Stderr`, etc.

### Stage-0 shadow gotcha

Caught the long-standing `args` shadowing bug from
`project_kaikai_param_args_shadow` on the first attempt: I named the
pattern binder `args` (`RL(n, args) ->`) and stage 1 silently
resolved every reference to the prelude `args()` builtin instead of
the local parameter. The generated C had
`KaiValue *kai_args = kai_incref(...)` immediately followed by
`kai_internal_dup(kai_closure(&_kai_prelude_args_thunk, ...))` —
visible only when inspecting the emitted C, not the source. Renamed
to `targs` and the fix worked first try.

This is now ~5 lanes that have lost ~30 min to the `args` shadow.
A stage-1 fix would pay off — but that's a separate lane.

### Op-call dispatch was already correct

Once both labels survive dedup, the existing dispatch path in
`check_body_row_label_loop` / `find_matching_declared` handles the
single-dispatch protocol exactly as the brief hoped:

- `Actor.receive() : Msg / Actor[Msg]` — fresh-instantiated
  `Msg = ?t`, the row label `Actor[?t]` unifies against the first
  declared `Actor[Request]` (binding `?t = Request`); subsequent
  context (match arm, `let req: Request = ...`) drives the
  disambiguation.
- `Actor.send(pid, msg)` — `pid: Pid[Msg]` argument pins the
  instantiation directly. The `labels_args_unify` step picks the
  declared label whose ty_args match.

No new code was needed for either path — they had been quietly
working on parametric effects all along, but were unreachable
because dedup ate one of the labels.

## Fixtures

Four new files under `examples/actors/`:

1. `dual_actor_request_reply.kai` — exact reproducer from issue
   #472, inlined onto a single mailbox so v1's eager scheduler
   can drive it without fibers.
2. `dual_actor_send_only.kai` — two `Actor.send` calls with
   different message types; pid argument disambiguates.
3. `dual_actor_receive_only.kai` — type-annotation on the result
   pins which `Actor[T]` is being read.
4. `dual_actor_missing_one.kai` — negative fixture: function that
   omits `Actor[Reply]` from its declared row but tries to send
   to a `Pid[Reply]`. Still rejected with the existing diagnostic,
   pinning regression behaviour.

The .out.expected / .err.expected goldens are short (one line
each). Each fixture is wired into `stage2/Makefile`'s
`test-effects` target after the existing actor block.

### Runtime caveat the fixtures bake in

v1's inline-eager scheduler can route through only the closest
installed `Actor[T]` handler. The fixtures therefore demonstrate
the typer accepting both effects in the row without trying to
exercise true multi-mailbox routing — that requires the m8.x
cooperative scheduler. The point of the lane is the typer fix; the
runtime story for multi-actor lands elsewhere.

## Structural surprises

Two:

1. **Dedup keyed by name was a documented shortcut, not a bug
   waiting to be discovered.** The comment in
   `dedup_row_labels_loop` ("same caveat as alias expansion")
   actively flagged the limitation. Search would have surfaced
   this in seconds — the brief's diagnosis hypothesis was right
   for the wrong layer. Lesson: when the brief points at "the
   typer", grep for the obvious dedup/normalize sites before
   diving into unification.

2. **The diagnosis-vs-fix layer cost an hour.** I spent 30 min
   reading row unification code (`partition_walk`,
   `find_remove_label`, `bind_row_tails`) on the hypothesis that
   row-matching itself needed parameterisation. None of that code
   was wrong — it never saw both labels because dedup ran first
   on the declared row at decl-elab time. The error message
   "enclosing row: Actor[Request]" pointed straight at the
   collapsed list; I should have trusted what the diagnostic
   said.

## Real cost vs estimate

- Estimated: 1-2 days.
- Actual: ~1.5 hours (diagnose, fix, fixtures, tier1, retro).
- Fix surface: 6 source lines in `stage2/compiler.kai`, no new
  helpers, no new types, no new passes.

Selfhost stays byte-identical — the dedup change does not affect
the compiler's own rows (the compiler uses bare-name effects
throughout, and the new key path is only taken for parametric
labels). Tier1 green (local), test-effects passes including the
four new actor fixtures.

## Follow-ups (not in this lane)

- **`args` shadow bug**: still bites; lane to fix stage 1's
  resolver order so user bindings win over prelude builtins.
- **Aliases with parametric bodies**: the `expand_row_labels`
  comment notes that aliases over parametric effects
  (`type DbReader = Reader[DbConfig]`) are out of scope. That's
  a separate gap from #472 and the dedup change doesn't unblock
  it — `find_alias` still gates on bare-name match.
- **Multi-mailbox routing at runtime**: the request/reply
  fixture uses a single mailbox because v1 cannot route between
  two `Actor[T]` handlers in the same fiber. Real two-actor
  request/reply needs the m8.x scheduler — tracked in
  `docs/m8x-followup.md`.
- **Diagnostic clarity for parametric labels**: the error message
  now correctly lists both `Actor[T1]` and `Actor[T2]` in
  `enclosing row`, but renders `Pid[Reply]` and similar argument
  types via `type_expr_display`, which has no notion of "this is
  the bare-name vs full form". Today this happens to look right
  for `Actor[Foo]`; future doc-aliased effects may need a more
  explicit renderer.
