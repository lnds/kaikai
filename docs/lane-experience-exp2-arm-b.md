# Lane experience report — exp2-arm-b (with_log_prefix + JSON-tooling embargo)

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## TL;DR

**Did not ship.** The lane's nominal target was
`with_log_prefix(prefix, body)` in `stdlib/trace.kai` — a
`Trace` effect handler that prepends a prefix to every message and
re-raises to the outer `Trace` handler. The lane discovered two
runtime/emitter bugs that together make handler-composition helpers
of this shape non-viable on the current compiler+runtime:

- **R9** — handler clauses do not capture parameters of the
  enclosing fn (`use of undeclared identifier 'kai_<name>'` at
  `cc` time). The spec in `docs/effects-impl.md` §*Op clause as
  ordinary function* says the channel for closure captures is
  `self.env`; that path is unimplemented in stage2's emitter.
- **R10** — the canonical-today workaround (push the value into a
  parameterised handler's `state` slot, read via `Eff.op()` from
  the inner clause) crashes the runtime on the second op
  invocation. Reproduced with `Reader[String]` (SIGBUS) and
  `State[String]` (SIGSEGV); both share the same shape.

Both findings are pinned in `docs/known-regressions.md` with
repro, hypothesis, and fix-path notes. The lane's substantive
output is the discovery + documentation; the helper itself is
**not** in the tree, no fixture, no Makefile target — `tier1`
stays byte-identical except for the doc edits.

The lane operated under the Tier 3 LLM-friendliness experiment 2
embargo (`docs/tier3-experiment-2026-05.md` synthesised
experiment 1; experiment 2 is the harder-scope follow-up). Arm B
is forbidden from `--effects-json`, `--effect-holes-json`, and
typed-hole structured candidates. The tooling-experience section
below is detailed; the headline is that **plain-text was
sufficient end-to-end** for the *diagnosis* — no point in the lane
did I feel a JSON dump would have unblocked anything, because the
blockers were `cc` build errors and runtime crashes, not type-row
mysteries.

## Objective metrics (from `/tmp/lane-exp2-arm-b-builds.tsv`)

- Start: 2026-05-01T23:33:19-04:00
- End:   see TSV tail at the bottom
- Build / test invocations:
  - 3 × `make test-trace-prefix` (with the candidate helper):
    - 1st: FAIL (cc rejects `kai_prefix` — direct capture, R9)
    - 2nd: FAIL (output diff: blank lines for invocations 2/3 —
      Reader workaround tripping R10's corruption mode)
    - 3rd: FAIL (SIGSEGV — State workaround tripping R10's crash mode)
  - Several `kaic2 + cc + run` direct invocations to build the
    minimal repros for R9 and R10 (also captured in TSV).
  - 1 × final `make tier1` to confirm docs-only changes leave
    selfhost / fixtures byte-identical.
- JSON-tooling invocations: **0** (banned by the experiment).
- Typed-hole invocations: **0** (banned).

## Investigation (M1)

Read of `stdlib/trace.kai` confirmed the existing surface:
`with_trace_default[R, e](body) : R / Stdout + e` substitutes
`Trace` for `Stdout`; ops are `log(msg) : Unit` and
`checkpoint(name) : Unit`. The brief's target signature
**preserves** `Trace` in the result row (the helper transforms
messages and re-raises, it does not consume the effect).

Read of `examples/effects/m8_12_self_delegating_handler.kai`
confirmed that an inner `Trace.log(...)` call from inside a
`Trace`'s `log` clause body resolves to the next outer `Trace`
handler, courtesy of the `in_dispatch_node` flag set/clear
around clause execution (commit 4a77d49, R2 fix).

Read of `docs/effects.md` §*Composed handlers* established the
nesting semantics; read of `docs/effects-impl.md`
§*Op clause as ordinary function* surfaced the load-bearing line
that closure captures of the enclosing scope go through
`self.env`. This last line turned out to be the spec's promise
that R9 violates.

Plus: read of `docs/lane-experience-tier3-arm-b.md` (the prior
arm-B retro from experiment 1) and `docs/tier3-experiment-2026-05.md`
to set up the JSON-embargo discipline. The synthesis doc explicitly
calls out experiment 2's harder shape:

> **handler composition — `with_log_filter(level, body)` that wraps
> the just-shipped `Trace` effect with a level filter, requiring
> delegate-to-outer-handler-of-same-effect patterns with no obvious
> prior art**

Same pattern as `with_log_prefix`. The synthesis predicted this
shape would stress effect-row inference; it turned out to stress
the *runtime* and *codegen*, not the typer.

## Implementation attempts (M2 + M3)

### Attempt 1 — direct closure capture

Shape:

    pub fn with_log_prefix[R, e](
      prefix: String, body: () -> R / Trace + e
    ) : R / Trace + e = handle {
      body()
    } with Trace {
      log(msg, resume) -> {
        Trace.log(prefix ++ ": " ++ msg)
        resume(())
      }
      checkpoint(name, resume) -> {
        Trace.log(prefix ++ ": checkpoint: " ++ name)
        resume(())
      }
      return(x) -> x
    }

`kaic2` accepted it cleanly (typed correctly: row arithmetic
checks). `cc` rejected the emitted C with four
`use of undeclared identifier 'kai_prefix'` errors at the four
references inside the clause bodies. The emitter spelled
`kai_prefix` (boxed alias) inside a stmt-expr that has no
`prefix` in scope; handler clauses are emitted as standalone C
functions whose only inputs are `(*EvE self, op_args..., k)`.

**Diagnosis path**: the C error message is direct. Scrolled to
the offending line in the emitted C, recognised the stmt-expr
wraps a clause body, located `docs/effects-impl.md` §*Op clause
as ordinary function* (lines 612–616) which says capture goes
through `self.env`, then concluded the path is unimplemented.
Took roughly 5 minutes from FAIL to root-cause. Plain-text was
sufficient.

### Attempt 2 — Reader[String] outer + delegating Trace inner

Per R9's documentation, the workaround is to put the value into a
parameterised handler's state slot. Reader is a built-in
parametric effect (`effect Reader[T] { ask() : T }`, auto-injected
by the compiler — confirmed at `stage2/compiler.kai:33172`).

First tried `with_reader(prefix) { ... }` from `stdlib/reader.kai`,
but that helper's signature is not row-polymorphic
(`body: () -> S / Reader[T]` — no `+ e`), so the inner Trace
handler's body row (`Trace + Reader[String] + e`) does not unify.
The error message from the typer was direct:

    type mismatch in function call
      = note: expected: (String, () -> ?t4 / Reader[String]) -> ?t4
      = note: found:    (String, () -> ?t4 / Trace + Reader[String] + ?e5) -> ?t15 / ?e6

Switched to an inline parameterised handler:

    pub fn with_log_prefix[R, e](
      prefix: String, body: () -> R / Trace + e
    ) : R / Trace + e = handle {
      handle { body() } with Trace {
        log(msg, resume) -> {
          Trace.log(Reader.ask() ++ ": " ++ msg); resume(())
        }
        checkpoint(name, resume) -> {
          Trace.log(Reader.ask() ++ ": checkpoint: " ++ name); resume(())
        }
        return(x) -> x
      }
    } with Reader[String](prefix) {
      ask(resume) -> resume(state); return(x) -> x
    }

Compiles cleanly. Runs partially: the first `Trace.log` produces
the correct line, the next two produce blank lines (just `\n`),
then the post-region `Trace.log` from `main` produces correctly
again. Hex-dumped the output to confirm — the corruption is
literal blank `\n`, not whitespace, suggesting the outer
`Stdout.print` is being called with an empty or freed-memory
string.

**Negative control** (small fixture, no parameterised outer):

    fn delegating_helper[R, e](
      body: () -> R / Trace + e
    ) : R / Trace + e = handle {
      body()
    } with Trace {
      log(msg, resume) -> { Trace.log("X: " ++ msg); resume(()) }
      ...
    }

Three `Trace.log` invocations from the body, all three lines
correct, exit 0. So self-delegation alone is fine. The bug is
the **combination** of self-delegation with a parameterised
outer.

### Attempt 3 — State[String] outer (in case Reader is the issue)

Same shape as attempt 2, with `Reader[String](prefix) { ask -> ... }`
swapped for `State[String](prefix) { get -> ...; set -> ... }`
and `Reader.ask()` swapped for `State.get()`. Compiles cleanly.
Runs and **SIGSEGVs on the second op invocation** (ran via the
wired-up `make test-trace-prefix` target which captures exit 139).

Same shape, same root cause, different runtime symptom — Reader's
crash signature is SIGBUS / blank corruption, State's is SIGSEGV.
Both are R10.

### Decision: revert + document

After three attempts each blocked by a different facet of the
same combined runtime/emitter limitation, reverted all
implementation changes (`stdlib/trace.kai`, `stage2/Makefile`,
deleted `examples/effects/trace_prefix.{kai,out.expected}`).
The deliverable became:

- R9 entry in `docs/known-regressions.md` (closure capture in
  clauses).
- R10 entry in `docs/known-regressions.md` (parameterised outer ×
  self-delegating inner).
- This lane-experience report.
- CHANGELOG `[Unreleased]` entry that lists the documented
  regressions and notes the lane did not ship the helper.

Not shipping a partially-working helper is the honest call. A
panicking placeholder `pub fn with_log_prefix[R, e](...) =
panic("blocked on R9/R10")` would be lying about the API surface;
a no-op pass-through would silently strip the prefix. Both are
worse than absence.

## Surprises / pitfalls

### `with_reader` is not row-polymorphic in v1

`stdlib/reader.kai`'s `with_reader[T, S](env, body: () -> S /
Reader[T]) : S` predates the row-polymorphic helpers
(`with_mailbox` is the canonical row-polymorphic shape). For the
attempt-2 wrapping I needed `body: () -> S / Reader[T] + e`. I
did not extend `with_reader` (out of lane scope and would not have
unblocked R10 anyway), but if a future lane lands the
row-polymorphic version it should ship as a separate helper or as
a signature widening — both `with_mailbox` and the `with_reader`
v1 callers should keep working.

### `state` keyword is the only safe channel for clause-body data

R9 makes this hard rule explicit: anything a handler clause needs
to read that *isn't* an op argument and *isn't* a re-entry into
another effect must come through `state`. That's a tight surface,
and it only works for parameterised handlers — non-parameterised
handlers literally cannot thread enclosing-scope data into their
clauses today.

### R10's "first invocation works" pattern is misleading

The Reader-variant of R10 corrupts on the *second* op, so a
fixture that exercises a single op call would pass and look like
the helper works. The Trace `worker()` example for the brief uses
three ops (`log`, `checkpoint`, `log`); had the brief shape only
called `log` once, a partial implementation would have shipped
silently broken. This is a strong argument for the brief-style
"three invocations end-to-end" smoke fixture even when the API
suggests a single-call shape would suffice.

## Tier 3 — JSON tooling usage (experiment 2 embargo)

The Tier 3 LLM-friendliness experiment 2 forbade
`--effects-json` / `--effect-holes-json` / `--types-json` and
typed-hole structured candidates. Per the brief, the report must
document where I would have asked for JSON, and whether the
embargo lost time.

### Errors encountered (all plain-text resolutions worked)

1. **`use of undeclared identifier 'kai_prefix'`** (cc, attempt 1).
   Plain-text was complete: error pinpointed the missing
   identifier and its file:line, and the spec doc
   (`docs/effects-impl.md` §*Op clause as ordinary function`)
   named the channel that *should* exist (`self.env`). Diagnosis
   ~5 min. **JSON would not have helped** — the bug is in codegen
   not in the type system, and `--effects-json` reports the
   inferred row, not the closure-env layout.

2. **`type mismatch in function call`** (kaic2, attempt 2 — the
   `with_reader` row-polymorphism issue). Plain-text gave the
   exact two row signatures side by side
   (`expected (String, () -> ?t4 / Reader[String]) -> ?t4` vs
   `found (String, () -> ?t4 / Trace + Reader[String] + ?e5) -> ?t15 / ?e6`).
   Resolution: read the helper's source, see it isn't
   row-polymorphic, switch to inline `with Reader[String](prefix)`.
   ~2 min. **JSON would arguably have been faster here** —
   `--effects-json` would have dumped the row as structured data,
   making the missing `+ e` immediately obvious. But the
   plain-text diagnostic already showed both rows next to each
   other; the difference jumped out without parsing JSON. Net:
   negligible.

3. **Output diff: blank lines for invocations 2 and 3** (runtime,
   attempt 2). The `make test-trace-prefix` failure showed a unified
   diff, but I had to `xxd` the output file to confirm the blank
   lines were literal `\n` and not whitespace. This is not a
   diagnostic class JSON addresses at all (JSON dumps from kaic2
   describe the type system / effect graph; runtime corruption is
   downstream of all of them). Plain-text + hex-dump was the right
   tool. ~3 min to root-cause to "the third op crashes the runtime
   path", then directed the next attempt.

4. **SIGSEGV (exit 139)** (runtime, attempt 3). Same class as #3 —
   runtime crash with no plain-text diagnostic at all (the
   process just exits with the signal). JSON tooling does not
   address this. The lane-discipline call to **stop attempting and
   document the bug** was easy to make from this signal alone.

### Where I noticed I would have asked for JSON

Once, briefly, in attempt 2's row-mismatch error (#2 above). Even
there, plain-text was sufficient because the typer prints both
row signatures side by side. In a deeper row-polymorphism puzzle
(say, three nested helpers each contributing a different effect
to an inferred row that's not what the user expects), the
`--effects-json` dump would shine. **The shape I hit did not
exercise that scope.**

The R9 / R10 blockers are downstream of the type system entirely;
no JSON tool addresses them. The experiment 2 embargo cost me
nothing on the *blocker* class of error, by construction.

### Where the embargo did matter

Indirectly: the brief framed this as a "handler composition"
experiment with the implicit hypothesis that the JSON tooling
would help with effect-row composition challenges. The lane's
finding is that handler composition is **not currently bottlenecked
by the type system** — it's bottlenecked by the codegen and
runtime paths. A JSON-enabled arm A run on the same brief would
likely converge on the same blocker (R9 surfaces at `cc` time, not
in the typer; no flag the agent could have invoked would have
flagged it earlier).

This is a **falsifiable prediction** the experiment series can use:
if a parallel arm A run on the same brief discovers R9 / R10 at the
typer stage instead of cc / runtime, the prediction is wrong; if
arm A reaches the same blockers via the same path, the prediction
is supported. Either way, the experiment 1 conclusion ("plain-text
sufficient for stdlib mirror lanes") generalises to "plain-text
sufficient for the *type-system* portion of handler composition
lanes" — a stronger claim than the brief framed.

### Time lost (>5 min iterating without understanding the error)

None. The longest single hold was ~5 min on attempt 1 (deciding
whether the cc error was a misuse on my part vs. a real emitter
bug — resolved by reading the spec doc and the canonical
delegating-handler fixture). No iteration without understanding;
each attempt was a deliberate hypothesis test against the next
candidate workaround.

### Bottom line for the experiment

For a **handler-composition lane that needs to thread a value into
clause bodies** — the experiment 2 brief's harder shape — plain-
text diagnostics carried the entire lane *because the blockers were
not in the type system*. JSON tooling would have been redundant;
typed holes would have been redundant. The actual bottleneck is
two compiler/runtime bugs (R9, R10) that no compile-time
introspection tool could surface earlier than `cc` and `./binary`
do today.

This is **one data point** — n=1 on the second-most-demanding
shape from the experiment-1 candidate list. It does not generalise
to:

- effect-inference debugging where the typer infers but the row
  doesn't match expectations (the case `--effects-json` and
  `--effect-holes-json` were built for — and which my lane never
  exercised, because R9 + R10 fired first).
- complex multi-helper composition where the row arithmetic gets
  involved and the typer surfaces a mismatch deep in the stack.
- lanes where the agent can complete the implementation under
  current compiler limits (i.e., the helper *does* ship and the
  agent debugs row issues end-to-end).

A useful next experiment would run the same embargo on a lane
**after R9 + R10 are fixed**, so the row-polymorphism work the
brief originally targeted actually surfaces. Until those bugs
land, every "handler composition" lane will hit the same wall.

## Followups parked

- **R9 fix** — emitter pass to detect free vars in clause bodies
  and thread them through `self.env`. Touches the lower-effect
  pass + Perceus (handler-install incref / handler-pop decref of
  captured values).
- **R10 fix** — audit `kai_evidence_lookup_node` and the
  `in_dispatch_node` save/restore around resume entries from
  parameterised handlers. Likely needs to clear/reinstate
  `in_dispatch_node` relative to the handler that owns the
  suspended continuation, not the fiber's current top.
- **`stdlib/reader.kai` row-polymorphism** — extend `with_reader`
  to `body: () -> S / Reader[T] + e) : S / e`, mirroring
  `with_mailbox`. Independent of R9 / R10; would unblock the
  attempt-2 outer wrapping shape if R10 is fixed first.
- **Mixed-handler regression coverage** — once R10 is fixed, add
  fixtures covering each cell of the matrix (parameterised outer
  × self-delegating inner; non-parameterised outer ×
  self-delegating inner; parameterised outer × non-self-delegating
  inner; etc.). The current `m8_12_self_delegating_handler`
  fixture covers one cell.
- **`with_log_prefix` itself** — once R9 and R10 are both fixed,
  the helper becomes a ~15-line shim (the Attempt 2 shape will
  work directly). Re-open the lane then.

## Limitations of this report

- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- One lane, one shape (handler composition with value-threading).
  The "JSON would not have helped" claim is bounded to this
  shape; the experiment 1 caveat (n=1, single feature scope) also
  applies here.
- The R9 + R10 hypotheses are agent-derived from emitted C
  inspection; they have not been confirmed by an emitter / runtime
  audit. The fix-path notes in the regression entries are
  best-guess directions for the next agent.
- Wall-clock includes the agent's reasoning and tool-call
  overhead; no profiling separates the kaic2 build cost from
  the agent decision cost.
- The "Tier 3 embargo cost me nothing on this lane" claim is a
  counter-factual; it cannot be verified without re-running the
  same lane with JSON access enabled. The plausible counter-
  argument is that with JSON access I would have *attempted* the
  same three workarounds in the same order, hit the same
  blockers, and concluded the same thing — no time saved either
  way.

## Raw build log

Appended in a separate commit per the brief's instrumentation
discipline.

**Note on instrumentation**: the per-build TSV script in the
brief used `${PIPESTATUS[0]}` against a chain that didn't expose
the make exit code reliably (the make target itself runs in a
sub-shell pipeline that changes directories); several FAILs were
initially recorded as OK in `/tmp/lane-exp2-arm-b-builds.tsv`.
The TSV appended below has been corrected to reflect the actual
outcomes, cross-checked against my command history. Future
agents: use `make ... ; rc=$?` (no pipeline) or capture exit code
inside the make recipe itself.
