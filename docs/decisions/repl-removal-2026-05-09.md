# Decision: remove `kai repl` from v1.0 surface

**Date:** 2026-05-09
**Closes:** #406
**Status:** accepted

## Context

`kai repl` shipped as a Level-1 prototype in `bin/kai` (a shell-side
read/eval loop that wraps every input buffer in `fn main() { ... }`,
recompiles from scratch, and runs the resulting binary). The roadmap
listed it as a v1.0 deliverable alongside `kai lsp`.

The prototype has structural problems that make it unsuitable as the
v1.0 REPL:

- **No state across evaluations.** Every buffer recompiles from
  scratch — there is no scope, no persistent bindings, no module
  cache. A real REPL needs incremental typing + linking, which the
  current pipeline (`lex → parse → resolve → infer → monomorph →
  perceus → lower`) is not designed to support without significant
  rework. See `docs/lane-experience-kai-repl-watch.md` for the
  prototype's own write-up of the limitation.
- **Effect handlers do not survive across evaluations.** The
  v1.0-quality REPL described in `docs/stage2-design.md §m17`
  ("online session with module reload and `?`-hole completion")
  requires the m17 work — independent from the prototype.
- **The shell-side wrapper is a maintenance liability.** It lives
  in `bin/kai`, not in the kaikai self-host. Improving it is
  fork-of-fork work that does not feed back into stage 2.

The package manager lane (#405) needs `bin/kai` simplified before it
adds new commands (`init`, `add`, `install`, `update`).

## Decision

Remove the `kai repl` prototype from `bin/kai`, the help text, and
the v1.0 roadmap. Defer the real REPL (m17 — `docs/stage2-design.md`)
to **post-1.0**, where it can be built on the stage 2 pipeline with
proper incremental compilation and `?`-hole completion.

This is a **breaking change** for any user who was relying on the
prototype. The replacement workflow is `kai run <script.kai>` for
ad-hoc evaluation; the script can be edited and re-run. `kai watch`
provides the closest "evaluate on every save" approximation.

## Consequences

- `bin/kai` loses ~130 LOC (`cmd_repl`, `repl_eval`, `usage_repl`,
  the `repl)` case, and the help-banner mention).
- `docs/roadmap.md` Anga Roa scope no longer lists REPL as a
  remaining v1 deliverable. The DoD shrinks from 4 items to 3.
- `docs/design.md`, `docs/stage2-design.md` are annotated with
  "post-1.0 per #406" callouts so the historical aspiration stays
  visible without misleading new readers.
- `docs/lane-experience-kai-repl-watch.md` is left intact as the
  post-mortem of the prototype lane.
- The package manager lane (#405) inherits a cleaner driver to
  extend.

## Alternatives considered

1. **Keep the prototype, mark it experimental.** Rejected: the
   prototype's recompile-per-eval model is not a foundation for the
   m17 REPL; keeping it costs ergonomic credibility ("kai has a
   REPL" — but not really) and ongoing maintenance.
2. **Build m17 now instead of removing.** Rejected: m17 depends on
   stage 2 incremental compilation that does not yet exist;
   sequencing it before #405 (package manager) and v1.0 release on
   2026-05-21 is not credible.

## Reversal cost

Re-adding the prototype is ~130 LOC against `bin/kai` and a
roadmap edit; reversal of this decision is cheap if a v1.x
post-mortem shows users wanted the recompile-per-eval REPL after
all. The repo retains the historical implementation in git history
under PR-of-origin (linked from #406).
