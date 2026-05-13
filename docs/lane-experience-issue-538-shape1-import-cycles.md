# Lane retro — issue #538 shape 1 (import cycle detection)

## Scope as planned

Close shape 1 of #538: import cycles between modules (`a.kai` imports
`b.kai`, `b.kai` imports `a.kai`, `main.kai` imports `a.kai`) were silently
accepted. The loader walked both modules, marked each `visited`, and produced
a partially-loaded `a` whose body referenced names from `b` that referenced
names from `a` that had not yet been declared — surfacing as an "unknown
name" diagnostic far from the actual cause.

Shapes 2 + 3 of #538 already closed in PR #542; the audit recommended an
`in_progress: [String]` stack on `ResolveState` separate from the existing
`visited` set, with an estimate of "3–5 days, touches ~6 callers".

## Scope as shipped

Matches the plan. No scope creep.

- `ResolveState` grew an `in_progress: [String]` field, distinct from
  `visited`. `visited` stays semantically "this path is fully loaded, don't
  reload" (the diamond-import dedup path); `in_progress` is the stack of
  paths whose `resolve_module` call has not yet returned.
- Three new helpers next to the existing `rs_*` family: `rs_in_progress`,
  `rs_push_in_progress`, `rs_pop_in_progress`.
- `resolve_module` reordered:
  1. If `abs_path` is `in_progress`, emit cycle diagnostic with the chain
     and `rs_bump_errs`. No push, no recurse.
  2. Else if `abs_path` is `visited`, return rs unchanged (diamond).
  3. Else push, process all four exit branches (missing file, lex errors,
     parse errors, success), then pop and mark visited.
- The constructor at `expand_imports` pre-seeds `in_progress: [root_path]`
  alongside `visited: [root_path, ...prelude_paths]`. This makes any cycle
  that loops back to the root file an explicit cycle error rather than a
  silent `visited` short-circuit. Prelude paths are NOT seeded into
  `in_progress` because preludes are loaded sequentially before the user
  graph and never re-enter; only the user root needs both.
- Chain formatting: `format_cycle_chain` walks `in_progress` in reverse
  (root → deepest) and appends the re-entered target once more, producing
  e.g. `main.kai -> a.kai -> b.kai -> a.kai`. Uses `concat_all` plus a
  small tail-recursive `join_with_arrow` helper rather than pulling in a
  generic `string_join` (kaikai's stdlib doesn't have one in scope for
  the compiler at this layer).

## Caller sweep

Linus' pre-lane estimate was ~6 callers of `ResolveState`. The actual count
was **5 record-literal sites** in `stage2/compiler.kai`, all centralised in
the 30-line block at 46867–46903 plus the constructor at `expand_imports`:

- `rs_push_in_progress` (new)
- `rs_pop_in_progress` (new)
- `rs_mark`
- `rs_append_decls`
- `rs_add_module`
- `rs_bump_errs`
- The constructor in `expand_imports` (only literal outside the helper
  block)

No callers thread the literal through anywhere else — every consumer goes
through the helpers, which is exactly the property that made this lane
small. The other type named `*ResolveState` in the file is
`AliasResolveState` (line 23429), a separate concern (typer-side alias
resolution) and untouched.

## Design decisions and alternatives considered

**Stack vs. separate parameter.** Considered passing `in_progress` as a
parameter independent of `ResolveState` since it's contextual to the active
path, not accumulated state. Rejected: would break the symmetry with how
`visited` flows through `process_imports` recursion, and the rs is already
monadic (returned by value through every step). Field on rs is the
consistent shape.

**Pop policy.** Considered "push on entry, never pop, dedup at the end".
Rejected: balanced push/pop keeps the chain short and accurate. A
non-balanced stack would report `main -> a -> b -> c -> a` for a cycle
even when `c` already finished and popping `c` is correct. Balanced stack
also makes the data structure intuitive: at any point during recursion,
`in_progress` is precisely the path from the root call site to the current
frame.

**Chain richness.** The brief allowed simplifying to "cycle detected at
path X" if rich-chain formatting needed new error-reporter infrastructure.
It didn't — `eprint` plus string concat sufficed. The current diagnostic is
single-line, prefixed `kaic:` to match the surrounding family
(`kaic: cannot open module …`), no source-anchor caret. If we want a rich
multi-line diagnostic later, the existing `Diagnostic` plumbing can wrap
this without touching the detection logic.

**Self-import of root.** A `main.kai` that imports itself is still
silently deduped because `root_path` is in both `visited` and
`in_progress`. The cycle-check branch fires first
(`rs_in_progress(root_path)` is true), so self-import now emits the cycle
error as well. Verified by hand: the diagnostic reads
`kaic: import cycle detected: main.kai -> main.kai`.

## Structural surprises

None. The block of `rs_*` helpers was already well factored — every
existing caller went through the helpers, so widening the field set was
purely mechanical. The reordering of `resolve_module` to check
`in_progress` before `visited` was the only non-trivial change to control
flow, and the four exit branches collapse cleanly into a single
post-recursion `pop + mark_visited` pair.

## Selfhost behaviour

Byte-identical. The change is additive: it inserts a new prefix check
that never fires on non-cyclic graphs (the compiler's own import graph has
no cycles), and the post-recursion `rs_mark` is the same operation as
before — just relocated from "before recurse" to "after pop". `visited`
semantics are preserved for legitimate diamond imports.

`make tier0` reports `selfhost byte-identical, demos baseline holds`.

## Fixtures + coverage

- Promoted `examples/negative/silent_contract/import_cycle/{a,b,main}.kai`
  to `examples/negative/modules/import_cycle/` and added
  `main.err.expected` containing `kaic: import cycle detected:`. The
  fixture exercises the canonical 3-file shape from the issue: `main`
  imports `a`, `a` imports `b`, `b` imports `a`.
- `tools/test-negative.sh` count: 93 → 94 PASS. The multi-file fixture
  counts as 1 (it routes through the `main.err.expected` discovery branch
  in the script).

No latent cycles were discovered in the compiler or stdlib during the
sweep — `make tier0` runs `kaic2` against `stage2/compiler.kai` and the
full demo set, and all paths green. If a real cycle existed in the import
graph today it would now fail-loud; it doesn't.

## Real cost

~45 minutes start to finish. The estimate of 3–5 days assumed the field
addition would touch many callers; the actual blast radius was 5
record-literal sites all inside one 30-line block. The brief's caller
inventory (`grep -nE "ResolveState\b"`) accurately predicted the scope —
following it directly was correct.

## Follow-ups left for next lanes

- **Source-anchor caret on the cycle diagnostic.** The current diagnostic
  is a single `kaic:`-prefixed line, no `--> file:line:col` block, no
  caret pointing at the cycling `import` statement. Matches the
  surrounding `kaic: cannot open module …` style. Worth elevating both
  to richer `Diagnostic` plumbing as a single follow-up; tracking value
  would be small (the error is already clear) — defer until someone
  hits a multi-line cycle chain in practice and asks for the anchor.

- **Cycles via selective imports.** The fixture uses `import a`; a cycle
  via `import a.{foo}` exercises the same `IkSelective` arm of
  `process_imports` which routes through the same `resolve_module`, so
  the detection covers it. No additional fixture today; could be added
  if a future lane needs to differentiate the path.

- **Alias cycles.** `import a as x` cycles through the same code path
  (only the surface name differs). Same coverage argument.

- The `silent_contract/import_cycle/` placeholder was renamed, not
  preserved. Anyone reading `silent_contract/` for outstanding contracts
  now finds it absent — that's the intended state. The directory still
  contains the four other contracts that have not yet closed.
