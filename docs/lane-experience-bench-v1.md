# Lane experience report — bench-v1

Best-effort retrospective by the implementing agent.

## Objective metrics (from /tmp/lane-bench-v1-builds.tsv)

- Start: 2026-05-01T19:02:28-04:00
- End:   2026-05-01T19:21:59-04:00
- Wall-clock: ~19m 31s
- Build/test invocations:
  - `make all`:      0 invocations.
  - `make kaic2` (under stage2): 2 invocations, 2 passes, 0 fails.
  - `make selfhost`: 1 invocation, 1 pass, 0 fails (ran via the
    umbrella driver after M3+M4 to confirm byte-identical
    bootstrap).
  - `make tier0`:    1 invocation, 1 pass, 0 fails.
  - `make tier1`:    2 invocations, 1 pass, 1 fail. The first run
    failed in `test-stdlib`: the loop in `stage2/Makefile`
    iterates every `examples/stdlib/*.kai` and links each one as
    a regular program, but `bench_basic.kai` has no `main`
    outside `--bench` mode. Fixed by adding a `bench_*` skip
    pattern to the loop. Second run green end-to-end.
  - `make test-bench`: 1 invocation, 1 pass, 0 fails.

## Compiler errors I encountered

None visible at the kaikai level. Every kaic2 build during the
lane succeeded on the first attempt — no type errors, no
non-exhaustive match warnings, no name resolution errors. This is
consistent with the work being structurally a mirror copy of an
already-working pattern (DTest).

The single failure during the lane was a *Make / linker* error,
not a compiler error: `test-stdlib` tried to link
`bench_basic.kai` as a regular program (no `--bench`) and the
linker reported the missing `_main` symbol. Diagnosis took
~30 s; fix took another ~30 s.

## Friction points

- **Stdout/stderr redirect during smoke test.** First end-to-end
  run captured `2>&1` into the temporary `.c` file, which baked
  the `KAI_TRACE_RC` ANSI report into the middle of a struct
  declaration and produced misleading clang errors. Recovering
  meant re-reading the redirect rather than chasing clang's
  diagnostic. Immediate fix; cost ~1 minute.

- **Walker site count is the dominant cost.** The DBench mirror
  needed an arm in roughly 25 distinct functions across
  `stage2/compiler.kai`. Each is mechanical — copy a
  `DTest(desc, body, l, c) -> DTest(desc, f(body), l, c)` arm
  and rename the constructor — but a single miss would have
  produced a non-exhaustive match warning at typer time, and
  finding the missed site by build error rather than by
  systematic grep would have wasted time. Mitigation: ran a final
  `grep -n "DTest\|DBench"` pass before the first kaic2 build
  and confirmed parity column-by-column.

- **stage2/Makefile coverage drift.** The `test-stdlib` loop
  silently scoops up every `*.kai` under `examples/stdlib/`. New
  fixtures with non-default `main` semantics need an explicit
  skip; there is no convention (filename suffix, header line,
  side-table) for "this is a bench / test fixture, not a
  free-standing program". Suggestion for v1.x: extract a
  per-target fixture list rather than a glob — but this is a
  Makefile concern, orthogonal to bench v1.

## Spec ambiguities or interpretive choices

1. **`setjmp` wrap?** The test harness wraps each test body in
   `setjmp(kai_test_jmp)` so a failed `assert` longjmps out and
   the next test still runs. Bench v1 deliberately omits this:
   timing aborted code is meaningless, and an assertion failure
   inside a bench should panic the process the same way it would
   inside a normal program. Decision: panic, not recover.

2. **Mode flag orthogonality.** Could have folded bench_mode
   into the existing `test_mode: Bool` (0 = normal, 1 = test,
   2 = bench), but instead added `bench_mode` as a sibling Bool
   parameter on `emit_program` / `emit_program_llvm` /
   `emit_main_wrapper`. Reasoning: the two modes never coexist
   at runtime (CLI parsing maps to a single `Mode` enum), so
   keeping them as parallel Booleans makes the call sites
   read symmetrically.

3. **Synthetic C symbol prefix for clauses inside a bench
   body.** `collect_decl` mints `__test__<line>_<col>` for a
   `DTest`; mirrored to `__bench__<line>_<col>` for a `DBench`
   so any `EHandle` clause emitted from inside a bench body
   carries the right enclosing symbol. The runtime path is
   currently unused (no fixture exercises an effect handler
   inside a bench in v1), but the symmetry is cheap and avoids a
   future debugging surprise.

4. **Output format**. Issue #40's acceptance criterion phrases
   the line as `<name>: <N> iter / <ns/iter> ns/iter`. Read
   literally that has two `ns/iter` substrings — clearly the
   first `<ns/iter>` is the placeholder for the per-iteration
   nanosecond value and the second is a unit literal. Emitted
   format:

       <desc>: 1000 iter / <Y> ns/iter

   where `<desc>` is the raw quoted source span (matching how
   `kai_test_pass` prints `kai_test_current` verbatim) and `<Y>`
   is `total_ns / iters` as a `long long`.

## Tier 3 LLM-friendly bet evidence

**Did `--effects-json` or `--effect-holes-json` help me recover
from a type / effects error during this lane?**

No — and the reason is informative. The lane produced *zero*
type or effect errors at the kaikai level. Every kaic2 build
succeeded on the first attempt. The work was structurally a
mirror of a known-good pattern (DTest), so once the call-site
inventory was in hand the implementation reduced to pasting a
parallel match arm next to each existing one. There was nothing
for the structured JSON to surface.

The two errors I did hit — a stdout/stderr redirect mistake and
a `test-stdlib` linker failure — both lived **outside** the
kaikai compiler boundary. Plain-text C compiler / linker output
diagnosed both within seconds.

This is a useful negative datapoint for the LLM-authorability
hypothesis: structured holes are most valuable when the
implementation deviates from a known-good pattern. Mirror lanes
(test → bench, stage 0 → stage 1 backport, etc.) gain less from
JSON than from a clean grep map of the base pattern. The
bottleneck on this lane was *enumerating the sites*, not
*understanding what to put at each site*.

If `--effects-json` had been available and I had asked for "all
match arms in `stage2/compiler.kai` that destructure `DTest`",
that would have been a meaningful shortcut. The current
`--effects-json` flag does not target this question. This
suggests an adjacent capability worth scoping for v1.x of the
honesty-targets work: a structured "where is this constructor
matched?" query, distinct from typed holes.

## Subjective summary

- **Confidence in correctness: high.** The bench harness is a
  byte-by-byte structural mirror of the test harness with one
  intentional deviation (no setjmp). Every existing test
  fixture continues to pass; selfhost is byte-identical;
  `make tier1` is green; the smoke fixture exercises three
  bench bodies with materially different runtime profiles
  (constant-time arithmetic, allocation, recursion) and all
  three produce sensible ns/iter values (17, 914, 3857).
  The remaining risk is the lack of a fixture exercising an
  effect handler inside a bench — flagged as a v1.x follow-up.

- **Hardest sub-task: M2 walker plumbing.** Not difficult, but
  high-volume and high-precision. A missed arm would produce a
  non-exhaustive-match warning rather than a hard error, and
  catching it requires reading every grep hit. Spent the most
  wall-clock time here.

- **Easiest sub-task: M5 fixture + Make target.** Single small
  `.kai` file + a shell-block recipe + a `bench_*` skip in
  `test-stdlib`. ~3 minutes including writing the fixture
  comments.

- **Did the compiler help or hinder?** Helped, in a quiet way.
  Selfhost byte-identity gives a strong existence proof that
  the AST changes are internally consistent; if `DBench` had
  been mishandled in any pass that the bootstrap exercises, the
  hash would have diverged. Hindered: nothing materially. The
  one Make-level failure was caught quickly and was orthogonal
  to the language change.

## Limitations of this report

- Self-report bias acknowledged. The retro is written by the
  same agent that produced the implementation; "no errors, high
  confidence" is the kind of claim that benefits from a second
  reviewer.
- Context truncation noted. The conversation included
  substantial prior context from agent exploration outputs;
  some of the friction-point granularity (e.g. exact wall-clock
  per-milestone) is reconstructed from the TSV, not measured.
- Single agent (Claude). Not generalisable.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-01T19:09:47-04:00	build-kaic2	OK	-
2026-05-01T19:12:15-04:00	build-kaic2	OK	-
2026-05-01T19:14:40-04:00	test-bench	OK	-
2026-05-01T19:15:03-04:00	selfhost	OK	-
2026-05-01T19:15:35-04:00	tier0	OK	-
2026-05-01T19:17:33-04:00	tier1	FAIL	-
2026-05-01T19:21:06-04:00	tier1	OK	-
```
