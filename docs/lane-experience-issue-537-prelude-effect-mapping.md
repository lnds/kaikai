# Lane experience — Issue #537 (prelude effect mapping)

Closes (partial): https://github.com/lnds/kaikai/issues/537

## Scope-as-planned vs scope-as-shipped

**Planned** (per brief):

1. Add three entries to `prelude_effect_for`: `args → Env`,
   `program_name → Env`, `exit → Process`.
2. Sweep bare call sites and annotate their enclosing fn rows.
3. Promote `program_name_no_row.kai` + `exit_no_row.kai`
   from `silent_contract/` to `effects_phase2/` with
   `.err.expected` goldens; create `args_no_row.kai` to match.

**Shipped**:

1. Two entries added (`args → Env`, `exit → Process`) — NOT
   three. `program_name` deliberately left unmapped (see
   *Design decisions* below).
2. Zero call sites needed annotation. The only bare call sites
   in the repo (`stage2/compiler.kai:56460-56461`) sit inside
   `fn main()` whose REmpty row already absorbs builtins with
   default handlers (the discipline pinned by Bug 6 + issue
   #517). `stdlib/os/args.kai:33` and `stdlib/os/process.kai:93`
   already declared the correct rows.
3. Two enforced fixtures shipped (`args_helper_no_row.kai`,
   `exit_helper_no_row.kai`) in `effects_phase2/`, anchored on
   helpers — NOT on `main`. The original silent-contract files
   were main-anchored, which is wrong post-fix: main's REmpty
   absorbs Env+Process. The enforcement only bites in helpers,
   which is where the real silent contract lived.
   `program_name_no_row.kai` stays in `silent_contract/` with
   an updated README note pointing at issue #127.

## Design decisions

### `program_name` is NOT in `prelude_effect_for`

The brief assumes `program_name → Env`. The repo already
decided otherwise, in two places:

- `stdlib/os/args.kai` lines 14-19 and 35-41 explicitly
  document `program_name()` as a pure read of `kai_g_argv`
  (the runtime snapshot installed at `kai_set_args` time),
  NOT a handler-mediated capability. The wrapper carries no
  effect row by design.
- Issue #127 (closed) routed argv[0] through the runtime
  snapshot precisely so it would not need handler plumbing.

If `program_name → Env` were added, the wrapper's signature
would have to change to `pub fn program_name() : String / Env`,
which contradicts the issue-#127 decision. Doing so would
require either reopening #127 or splitting the wrapper into
"`Env`-rowed surface + bare-prelude pure read" — neither
worth the churn for a value that is process-wide constant
state.

Reported back in the silent_contract README: the
`program_name_no_row.kai` fixture stays as documentation of
the gap until the design decision is revisited.

### Fixtures anchored on helpers, not main

The brief proposed promoting the existing main-anchored
fixtures (`fn main() : Int = { exit(0); 0 }`). Post-fix that
file compiles cleanly: main's REmpty row absorbs the new
`Process` demand and the runtime installs the default
handler. The exit code propagates correctly (verified:
`exit(7)` produces a process exit of 7). So the original
fixtures stop being silent contracts — they become positive
runtime tests.

The real silent contract is in helper functions: a helper
that calls `exit` bare without declaring `/ Process` used
to slip through. Post-fix it raises an explicit
`error: effect not handled: Process`. The new fixtures
exercise that path.

## Structural surprises

- **`main`'s REmpty absorption interacts with the fix.**
  Adding the two table entries does not affect main-anchored
  code. The bug-bash assumption "silent at main" was an
  artifact of `prelude_effect_for` returning None — main
  absorbed nothing because nothing was demanded. After the
  fix the demand exists, main absorbs it, and the default
  handler installs. The interesting enforcement is in
  helpers.
- **Selfhost stayed byte-identical.** The only bare
  `args()`/`exit()` calls in `stage2/compiler.kai` are
  inside `fn main()`. The change to `prelude_effect_for` is
  additive (it raises labels that were previously dropped),
  and main absorbs them, so the inferred row for `main` gains
  `Env+Process` — which `emit_main_wrapper` installs default
  handlers for. No signature in any helper needed annotation.

## Fixtures added

- `examples/negative/effects_phase2/exit_helper_no_row.kai`
  + `.err.expected` (golden: `error: effect not handled: Process`).
- `examples/negative/effects_phase2/args_helper_no_row.kai`
  + `.err.expected` (golden: `error: effect not handled: Env`).

`tools/test-negative.sh` count: 77 → 79 PASS.

## Real cost vs estimate

- Brief estimate: 3-4 hours.
- Real: ~1 hour. The audit pre-lane was accurate; the
  fix was a 9-line table extension; the sweep found zero
  call sites needing annotation; the fixture work was two
  small files plus README touch. The judgment call about
  `program_name` consumed the most thinking time.

## Follow-ups for next lanes

- `program_name_no_row.kai` remains in `silent_contract/`
  as documentation of the unmapped status. Closing it would
  require revisiting issue #127's design — out of scope for
  #537.
- The lane brief mentioned issue #531 (qualified `Effect.op`
  not propagating through ordinary fn calls). That family
  is distinct and untouched by this lane.
- The main-absorption behavior is correct, but it means
  silent contracts anchored on main need to be rewritten as
  helper fixtures to enforce. Other phase-2 silent contracts
  may need the same treatment when their underlying issues
  close.
