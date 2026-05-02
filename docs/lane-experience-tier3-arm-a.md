# Lane experience report — tier3-arm-a (Trace effect)

Best-effort retrospective by the implementing agent.

## Objective metrics (from /tmp/lane-tier3-arm-a-builds.tsv)

- Start: 2026-05-01T22:52:29-04:00
- End:   2026-05-01T23:06:45-04:00
- Wall-clock: ~14m 16s
- Build/test invocations:
  - `make tier0`:    1 invocation, 1 pass, 0 fails (baseline check
    before touching anything; demos baseline 24 holds).
  - `kaic2 --path ../stdlib <fixture>`: 4 invocations during
    fixture authoring (including 2 hits on R8 below).
  - `make test-trace`: 1 invocation, 1 pass, 0 fails (isolated
    new-target verification before tier1).
  - `make tier1`:    1 invocation, 1 pass, 0 fails. Wall-clock
    ~3m 32s; demos baseline 24 holds; selfhost byte-identical.
  - `kaic2 --effects-json`:       1 invocation (deliberate, for
    this report).
  - `kaic2 --effect-holes-json`:  1 invocation (deliberate).
  - `kaic2 --types-json`:         1 invocation — flag does not
    exist, kaic2 silently fell through to the default C-emit
    path. `--help` confirms the JSON flag set is `--holes-json`,
    `--effects-json`, `--effect-holes-json` only.

## Compiler errors I encountered

One *kaikai-language* error and one *bug found outside my lane*.

**(a) kaikai-language: `println` was the wrong surface name.** My
first draft ended `main` with `println(b)`. The kaic2 typer
reported `error: undefined name `println` (did you mean `print`?)`
and `--> trace_basic.kai:30:3 | println(b)`. Fix took ~10s: the
fixture pattern in `examples/effects/m7a_7_default_console.kai`
uses `Stdout.print(b)` and that auto-appends `\n` per Doc B
§Console. Switched and re-built clean.

**(b) bug outside my lane (R8): unbox-phase-2 vs string interp.**
After the kaic2 build went green, `cc` rejected the emitted C
with two errors:

    error: use of undeclared identifier 'kai_value'; did you mean 'kair_value'?
    error: incompatible integer to pointer conversion passing 'int64_t' to
           parameter of type 'KaiValue *'

The cause: `let value = 42` is unboxed by phase 2 (emit:
`int64_t kair_value = 42LL`), but the later use inside
`#{int_to_string(value)}` desugar still spells the boxed name
`kai_value`. Reduced minimally to a 4-line repro outside the
Trace effect, confirmed it is independent of effects, documented
as **R8** in `docs/known-regressions.md`, and worked around in
the fixture by replacing the one offending `#{...}` segment with
explicit `++` concatenation. Per CLAUDE.md lane discipline, the
fix is left for a future emit-pass lane.

## Friction points

- **No `--types-json` flag** despite the brief listing it as
  optional. Found out only by trying it: kaic2 silently emits
  C and the tooling instrumentation logged it as `NOT-A-FLAG`.
  The brief is correct that it is optional ("(si existe)") —
  noting it here for future lane briefs not to advertise it.

- **R8 wastes minutes if the workaround pattern is unknown.**
  The C-side error ("undeclared identifier `kai_value`") looks
  like a name-mangling collision rather than an unbox/interp
  interaction; I had to reduce to a 4-line repro before the
  shape was visible. The two existing helper-call patterns —
  function-parameter Int into `#{int_to_string(p)}` (works) and
  `let _ = handle { ... }` of String type (works) — happen to
  not exercise the broken case, so the codebase did not have
  prior art pointing at the workaround. Logged with full repro
  + workaround list in known-regressions.md so the next agent
  who hits it does not re-derive the diagnosis.

## Spec ambiguities or interpretive choices

1. **`Console.println` → `Stdout.print`.** The brief's spec
   sketch invokes `Console.println(b)`. The actual surface in
   `examples/effects/m7a_7_default_console.kai` is `Stdout.print`
   (atomic, post-Phase 4b split). `Stdout.print` already appends
   `\n`, so semantically `Console.println(b)` and `Stdout.print(b)`
   produce the same byte stream. I took the brief's "spec is
   indicative, not copy/paste" license and used the in-tree
   surface.

2. **stderr + timestamps → stdout, no timestamps.** The brief
   says the default handler "writes to stderr with timestamps".
   Both choices break golden-file determinism: stderr is mixed
   with diagnostics under several `make` recipes, and timestamps
   are non-reproducible by definition. Following the time/Clock
   stub-handler precedent (deterministic golden), I emit
   prefixed lines (`[trace] ...`, `[trace] checkpoint: ...`) on
   stdout via `Stdout.print` — same prefix shape, same
   information, deterministic.

3. **Trailing-lambda call in `main`.** The brief's `main` is
   `with_trace_default(() => { ... })`. I used the trailing-
   lambda sugar `with_trace_default() { ... }` to match how
   `with_writer` / `with_reader` are called in the existing
   stdlib smoke tests (`examples/writer/main.kai`,
   `examples/reader/main.kai`). Functionally identical; chose
   the form already idiomatic in tree.

4. **`Trace` is not runtime-installed.** Unlike `Console`,
   `Stdin`, `Env`, etc., `Trace` has no automatic default
   handler installed by the runtime around `main`. Callers must
   explicitly wrap their region in `with_trace_default { ... }`.
   This matches `Clock` (m8.x scope) and the `State[T]` /
   `Reader[T]` / `Writer[W]` precedents — the runtime-installed
   default handlers are reserved for capabilities that need OS
   resources (fds, randomness) and that almost every program
   transparently uses.

5. **Row signature `() -> R / Trace + e ` vs `() -> R / e + Trace`.**
   Both parse-equivalent because the typer normalises rows to
   canonical form (labels alphabetical, var last per
   `docs/effects.md` §Representation). Wrote
   `body: () -> R / Trace + e` because that is the shape used in
   `examples/effects/m7b_12_row_var_concrete_plus_var.kai`
   (`Console + e` form).

## Tier 3 LLM-friendly bet evidence — JSON tooling usage

**Did `--effects-json` / `--effect-holes-json` / `--types-json`
help me recover from a type / effects error during this lane?**

**No type errors hit, so the tools were not load-bearing.** The
single language-level error (`println` undefined) was diagnosed
in 10s from the plain-text `did you mean print?` hint — the JSON
tools would not have surfaced anything additional, since the
error is name resolution, not row-mismatch.

I still ran each tool deliberately on the green fixture to
characterise their output for the report:

- **`--effects-json`** (used 1×, post-success). Returns a single-
  line JSON array, one entry per top-level fn:

      [{"fn":"with_trace_default","line":33,"col":5,
        "effects":["Stdout"],
        "handlers_installed":[{"effect":"Trace","line":34,"col":3}]},
       {"fn":"worker_a","line":12,"col":1,"effects":["Trace"],
        "handlers_installed":[]},
       {"fn":"worker_b","line":22,"col":1,"effects":["Trace"],
        "handlers_installed":[]},
       {"fn":"main","line":29,"col":1,"effects":["Stdout"],
        "handlers_installed":[]}]

  This is genuinely useful confirmation: `with_trace_default`'s
  result row is `Stdout` (not `Stdout + Trace`), proving the
  handler stripped `Trace` correctly; `worker_a` / `worker_b`
  show pure `Trace` (no leakage of `Stdout`), proving the
  handler clauses' `Stdout.print` calls did not leak through the
  body row variable. If I had been chasing a row-leak bug, this
  is exactly the diagnostic I would want — three lines of JSON,
  no scrolling. **High value when applicable.**

- **`--effect-holes-json`** (used 1×, post-success). Returns
  `[]` because the fixture has no `?` typed effect-holes. As a
  flag for "what would the typer infer if I left this row
  open?", it would be load-bearing on a different lane (write
  the body, leave `?` in the row tail, ask the typer). My
  fixture was authored top-down with concrete rows, so the
  flag had nothing to surface. **Untested in anger.**

- **`--types-json`** (used 1×, fell through). Flag does not
  exist; kaic2 silently emits the C output to stdout. The
  reachable JSON-emitting flags per `--help` are `--holes-json`,
  `--effects-json`, `--effect-holes-json`. **Brief was advisory;
  flag is not in tree.**

**Workflow recommended for this kind of stdlib-effect lane:**

1. Author the effect declaration + helper top-down with
   concrete rows (no `?` holes). The `Trace` shape was small
   enough that the row was obvious from the start.
2. Run `kaic2 --path ../stdlib <fixture>` once. If it green-
   builds, you are in the small-bug regime (the C-emit issues,
   not the type system).
3. If `cc` rejects the C, immediately look at the C error
   *first* — it is almost always an emit-pass interaction, not
   something the JSON tools surface. R8 was diagnosed in
   seconds once I read the `kai_X` vs `kair_X` mismatch
   directly.
4. Use `--effects-json` *after* a green build as a sanity check
   that handler installation did what you expected (e.g.,
   stripped the right effect from the result row). One-line
   diff against an expectation; takes 5 seconds.

The JSON tools' cost-benefit on this lane:
**positive but small** — confirmed expected row plumbing without
having to read C output. Would have been transformative if the
lane had hit a row-mismatch error; it did not.

## Subjective summary

- **Confidence in correctness: high.** `make tier1` green;
  selfhost byte-identical (demos baseline 24 unchanged); the
  fixture's `[trace] ...` lines on stdout match the golden
  exactly; R8 documented with a self-contained workaround so
  the fixture is robust against future emit churn.

- **Hardest sub-task: M3 fixture (R8 detour).** Roughly half
  the lane wall-clock went into bisecting the unbox/interp
  interaction. Once the workaround was clear (use `++` for the
  one let-bound-Int slot) the rest was mechanical.

- **Easiest sub-task: M2 stdlib/trace.kai.** The pattern was
  visible from `stdlib/writer.kai` and `stdlib/time.kai`. ~3
  minutes including comments.

- **Did the compiler help or hinder?** Helped on (a): the
  `did you mean print?` text was already enough. Hindered on
  (b): the `cc` error pointed at the emitted C, not at the
  upstream pass that produced the bad name. A diagnostic that
  said "this `let`-bound `Int` is unboxed; the interpolation
  desugar refers to it boxed" would have saved the bisect time,
  but that is properly an emit-pass concern, not a typer one.

## Limitations of this report

- Self-report bias acknowledged. Same agent that produced the
  implementation; "high confidence" benefits from a second
  reviewer.
- Single agent (Claude); not generalisable.
- The TSV-derived elapsed_s for `tier1` (212s) is a single sample
  from this lane and not representative of CI's worst-case.

## Raw build log

    timestamp	cmd	outcome	elapsed_s
    2026-05-01T22:57:08-04:00	kaic2-fixture	BUILT	-
    2026-05-01T23:01:22-04:00	kaic2-fixture	OK	-
    2026-05-01T23:01:57-04:00	test-trace	OK	-
    2026-05-01T23:02:20-04:00	effects-json	OK	-
    2026-05-01T23:02:20-04:00	effect-holes-json	OK	-
    2026-05-01T23:02:20-04:00	types-json	NOT-A-FLAG	-
    2026-05-01T23:06:13-04:00	tier1	OK	212

Note: the first `kaic2-fixture` row reads `BUILT` (kaic2 returned
0 producing C) and the second reads `OK` (after the R8 workaround,
kaic2 + cc + run + golden all green). The interim `cc` failure
was not separately TSV'd because it was a probe inside a single
debugging cycle, not a measured build event.
