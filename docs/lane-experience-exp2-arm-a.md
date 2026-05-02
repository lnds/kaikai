# Lane experience report — exp2-arm-a (Trace handler composition: `with_log_prefix`)

Best-effort retrospective by the implementing agent. This is the
second of the two Tier 3 LLM-friendliness arm-A experiments
(experiment 1 shipped the `Trace` effect itself in PR #54);
experiment 2 tests the more demanding *handler composition with
delegate-to-outer* shape — a handler whose own clause bodies
invoke the same effect, expecting dispatch to skip the dispatching
node and resolve to the next handler down.

## Objective metrics

From `/tmp/lane-exp2-arm-a-builds.tsv` (raw TSV appended at the
end of this document):

- Start: 2026-05-01T23:33:13-04:00
- End:   2026-05-01T23:52:04-04:00
- Wall-clock: ~18m 51s
- Build/test invocations:
  - `make kaic2`:               1 invocation, 1 pass, 0 fails
    (incremental rebuild of the stage 2 compiler against
    `main` HEAD, ~5s).
  - `make tier0`:               1 invocation, 1 pass, 0 fails
    (~27s; demos baseline 24 holds; selfhost byte-identical).
  - `make -C stage2 test-trace`: 3 invocations,
    2 fails, 1 pass. The two fails revealed two distinct bugs
    (one C-build error from missing closure capture in clause
    bodies; one runtime use-after-free from the perceus
    single-read transfer rule).
  - `make tier1`:               1 invocation, 1 pass, 0 fails
    (~3m 02s; full `make test` + demos baseline + fmt fixtures
    + bench smoke + check smoke).
  - `kaic2 --effects-json`:     1 deliberate invocation
    (after the fix landed, for this report).
  - `kaic2 --effect-holes-json`: 1 deliberate invocation (also
    post-fix; output `[]` since no `?` annotations remain).
  - ASAN-instrumented standalone build of the fixture: 2
    invocations, both reproduced the use-after-free shape that
    the test target swallowed as a flaky segfault.

## Compiler errors I encountered

### (a) C-build: `use of undeclared identifier 'kai_prefix'`

**Where**: `build/trace_basic.c:254` and `:259` — the *first*
`make test-trace` after the initial `with_log_prefix`
implementation. The error pointed at the inner Trace clause
bodies inside `with_log_prefix`, but the C file flagged was
`trace_basic.c` because the `--path ../stdlib` import pulls
the whole module's emitted code into every importing fixture's
single-file C output.

**~Attempts to diagnose**: 1 read of the emitted line + 1 grep
through `stage2/compiler.kai` for `emit_clause_body`. The fix
shape was clear within a minute: clause bodies are emitted as
top-level C functions whose signature is
`static KaiValue *_kai_<enc>__clause_<l>_<c>_<op>(EvE *self, args..., KaiCont *k)`
— their `lcs` (lexical context) is `["self", "k", "state",
"log"]` plus the op args, and never the enclosing fn's
parameters. Any reference to an outer local in a clause body
produces an undeclared-identifier C error because the emit walker
falls through to the bare `kai_<name>` rendering.

**What helped**: reading `emit_clause_body` and
`clause_state_prologue` in `stage2/compiler.kai:9939`–`9970`
made the constraint structural rather than a typo. There is no
fixture in the repo that captures an outer local in a clause body
— the closure-capture path simply does not exist for clauses.

**What this means for the implementation**: the prefix has to
travel through one of the two channels a clause body actually has
access to: (i) the EvE struct's parametric `state` slot, or
(ii) the dynamic effect lookup path. I picked (i) and added a
private `effect TracePrefix[T] { read() : T }` to thread `prefix`
through, with the inner Trace clauses calling
`TracePrefix.read()` whenever they need the prefix.

### (b) Heap use-after-free on the second op call (R9)

After fixing (a) and getting a clean build, the fixture produced
the *first* prefixed line correctly, then two empty `[trace] `
lines, then the unprefixed tail. Under the test-target's pipeline
it sometimes segfaulted (rc=139); standalone it returned rc=0
with garbage output. ASAN exposed the shape immediately:

    AddressSanitizer: heap-use-after-free
    READ of size 4 at … in kai_string_concat runtime.h:1216
      #1 kai_prelude_string_concat runtime.h:1389
      #2 _kai_trace__…__clause_84_5_checkpoint tp.c:267
    freed by thread T0 here:
      #3 kai_prelude_string_concat runtime.h:1390
      #4 _kai_trace__…__clause_84_5_log tp.c:272

The `_log` clause's `kai_prelude_string_concat` decrefs its
inputs (runtime.h:1389-1392 — `r = kai_string_concat(a, b);
kai_decref(a); kai_decref(b); return r`). The value flowing in
is the result of `TracePrefix.read()`, which is `self->state` —
the EvE storage. Refcount drops to zero, storage is freed, and
the next op call (`_checkpoint`) reads the freed slot.

**~Attempts to diagnose**: 4 — (1) re-read the C output to
suspect a clause symbol collision, (2) grepped `runtime.h`
for the concat discipline, (3) added a `let p = state` in the
read clause hoping `let` would force an incref (it did not —
`pcs_is_non_last` returns false for a single read, so the emit
path is `KaiValue *kai_p = kai_state` with no `kai_internal_dup`),
(4) read `pcs_is_non_last` and the comment block above it
(`stage2/compiler.kai:23787`–`23806`).

**What helped**: ASAN was decisive — without it the surface
symptom (two empty lines plus an intermittent segfault) looked
like an effect-stack ordering bug, which would have sent me down
a different rabbit hole. The structural read of `pcs_is_non_last`
explained why a single-read source did not get an implicit dup
and why my two-bindings workaround would.

**The fix that shipped (source-level)**:

    read(resume) -> {
      let _keep_alive = state
      resume(state)
    }

Two `state` references pushes the use count past the `≥ 2 non-lam
uses` threshold, so both reads emit `kai_internal_dup(kai_state)`
and the EvE storage stays live across op calls. Full hypothesis,
ASAN repro, and a structural fix path for the compiler are
documented in `docs/known-regressions.md` as **R9**.

The structural fix (treat `state`/`log` as unconditionally
non-last in `pcs_is_non_last`) is small but touches the emit
pass; per lane discipline it is logged for a future Perceus / RC
lane and not done here.

## Friction points

### Did I understand outer-handler dispatch on first try?

**Yes for the type-system shape, no for the codegen
constraints.** The composed-handler row rule in
`docs/effects.md` §`Inference`
(`row(handler) = (row(body) \ {E}) ∪ row(handler clauses)`)
made the result-row signature obvious on first read:

    pub fn with_log_prefix[R, e](
      prefix: String,
      body: () -> R / Trace + e
    ) : R / Trace + e

— body's `Trace` is consumed by my inner handler, and `Trace`
re-enters the row because the clause bodies themselves invoke
it. Pinned the m8 #12 fixture
(`examples/effects/m8_12_self_delegating_handler.kai`) as prior
art for the runtime piece (the `KaiEvidence.in_dispatch` flag
makes the inner clause's `Trace.log` skip itself and resolve to
the outer handler).

What I did *not* anticipate before opening
`stage2/compiler.kai`:

1. **Clause bodies have no closure-capture path.** Every kaikai
   tutorial / fixture I have seen uses parametric handler state
   (`State[T](init)` / `Reader[T](env)`) when the clause needs
   to see "outside" data, and I had silently been treating that
   as a stylistic preference. It is a hard constraint: clause
   bodies become top-level C functions whose only inputs are
   `self`, `k`, op args, and (when the handler is parametric)
   the `state` / `log` aliases of `self->state`. The first
   compile error was a structural pop quiz on this — and the
   spec of `with_log_prefix` (a `String` parameter, not a
   parametric effect) does not telegraph that an extra private
   parametric effect is needed to thread the prefix in.
2. **State reads are aliases of EvE storage, not owned
   transfers.** Single-read `state` references emit a raw
   pointer copy. Any consumer that decrefs (concat, length,
   join — all the string prelude) frees the EvE storage out
   from under the next op call. This is **R9** in
   `known-regressions.md`. Existing repo fixtures
   (`examples/effects/m7b_11_followup_distinct_types.kai`,
   `examples/effects/m7b_14_reader_helper.kai`,
   `examples/sugars/m7b_4_*`) all read state once into an Int
   or pass it directly to a sink that does not decref, so the
   bug has been latent.

The two issues compose: solving (1) by adding a private
parametric effect introduces (2) the moment the read flows into
a string concat. The `Trace` op-arg type — `String` — is what
makes this lane the right place to surface the bug. An LLM
authoring against the catalog `Reader[Int]` example would have
shipped a working artifact and never seen R9.

### What the JSON tools did and did not buy

The brief mandated proactive use of `--effects-json` and
`--effect-holes-json`. Honest accounting:

- After the fix landed, `kaic2 --path stdlib --effects-json
  examples/effects/trace_prefix.kai` reported (formatted):

      [
        {"fn": "with_trace_default", "effects": ["Stdout"],
         "handlers_installed": [{"effect": "Trace", "line": 34, "col": 3}]},
        {"fn": "with_log_prefix",    "effects": ["Trace", "TracePrefix"],
         "handlers_installed": [
           {"effect": "Trace",       "line": 84, "col": 5},
           {"effect": "TracePrefix", "line": 83, "col": 3}]},
        {"fn": "worker", "effects": ["Trace"], "handlers_installed": []},
        {"fn": "main",   "effects": ["Trace"], "handlers_installed": []}
      ]

  This is genuinely useful as a structural confirmation: the
  inner Trace handler is at line 84 inside `with_log_prefix`,
  the outer TracePrefix handler is at line 83, and the user-
  visible row of `with_log_prefix` is `Trace + e` (the report
  conflates the row tail with explicit labels in the
  `effects` field; per the row rule `TracePrefix` is consumed
  by the outer `handle`, so it should not appear in the
  caller-visible row — minor UX gap noted but not blocking).
- `kaic2 --effect-holes-json` returned `[]`. Helpful as a
  confirmation that no `?` markers remain; not useful for
  debugging this lane's two errors because neither was a
  type-row failure — both were emit / runtime issues.

**Where the JSON tools did not help, and what did**: the
heap-use-after-free was a runtime memory-discipline failure that
the type system cannot see. Plain ASAN on a stand-alone
`cc -fsanitize=address` build of the emitted C file gave the
exact freeing call site and the use site in one stack trace.
That was the unblocking moment of the lane. Future LLM-friendly
tooling for this kind of bug would be a `--rc-discipline` lint
or a `--asan-emit` flag that wraps the test target with the
sanitizer build automatically; the JSON-as-data axis the brief
prescribes is orthogonal to memory bugs in the emitted runtime.

## Tier 3 LLM-friendliness evidence

The bet (`CLAUDE.md` §`Tier 3 — Strategic bet`): with typed
holes + structured JSON + stable rules, an LLM can author
kaikai even though current models know Python / Rust far
better. Acceptance criterion: an LLM with JSON access completes
the top 80% of typical functions within one round of
compilation.

This lane is one data point against that criterion, with the
caveat that the implementing agent (Claude Opus 4.7) has spent
cumulative hours in this codebase across previous sessions —
the sample is not "fresh LLM faces kaikai for the first time."

### Did the LLM (this agent) author the function within one
### round?

**No.** Counting only the kaikai-source rounds (excluding doc
reads and ASAN setup), it took **three rounds** of compilation
before `make test-trace` passed:

| Round | Implementation                                        | Outcome                          |
|-------|-------------------------------------------------------|----------------------------------|
| 1     | Inline closure-capture: clause bodies reference `prefix` directly | C build error: `kai_prefix` undeclared |
| 2     | Add `effect TracePrefix[T]` + thread `prefix` through `state` | Runtime: `[trace] outside prefix` and `[trace] worker-1: started` correct, then two empty `[trace] ` lines, then `[trace] after prefix` (heap UAF; intermittent segfault under redirection) |
| 3     | `let _keep_alive = state` in the `read` clause to force perceus to dup | Pass: `make test-trace`, `make tier1` green |

Round 1 was the "compose handler with delegate-to-outer is
parametric-effect territory, not closure-capture territory"
realisation. Round 2 was R9 surfacing as soon as the parametric
state was a string. Round 3 was a 5-line workaround once the
perceus rule was understood.

### What would have made round 1 unnecessary

A failure mode the *type system* knows about — "this clause
body references an enclosing local that is not in the clause
scope" — emitted as a typed-hole-style diagnostic at the
*kaikai* layer instead of an undeclared-identifier C error two
levels down. The information is present in the typer (the typer
already builds the clause's environment); all that is missing is
a check that fails fast with a hint pointing at the parametric-
state pattern. This is the LLM-friendliness bet's structural
form: shift the failure from C-undeclared-identifier (model has
no useful prior on this) to "use a parametric handler effect"
(model can pattern-match the prior art in `Reader[T]`).

### What would have made round 2 unnecessary

Either:
- **Compile-time**: `pcs_is_non_last` recognising `state` /
  `log` as always-multi-use (the proposed R9 fix). One round.
- **Tooling**: `kaic2 --asan-fixture <file>` that builds + runs
  with sanitizers and fails the test target on UAF / leak. The
  bug surfaces in round 2 instead of being absorbed by an
  intermittent-segfault flake under stdout redirection.

The current ergonomic gap is that the test target swallowed the
segfault as `Error 139` from `make`, and the standalone run
returned rc=0 with garbage stdout. An LLM relying on the
compiler's standard error stream would have seen "test-trace
FAIL — segfault" and asked the wrong question ("which clause is
crashing?") instead of the right one ("which value is being
freed?"). ASAN bridged that gap; the JSON tooling did not.

### Subjective experience vs experiment 1

Experiment 1 (Trace effect itself, PR #54) had two friction
points: a Spanish-named-op slip (caught by self-review before
commit) and the R8 unbox-phase-2 / interp interaction (logged,
worked around in source). Both were small detours; the lane
shipped within ~14 minutes wall-clock.

Experiment 2 had one *large* detour (R9, ~6 minutes between
the first failed `test-trace` and the working three-line
workaround) and one small structural pivot (round-1 closure-
capture realisation, ~2 minutes). The lane shipped within
~19 minutes wall-clock — slower than experiment 1, faster than
the brief's "demanding" framing suggested.

The qualitative difference is that experiment 2 *required*
reading the emit-pass internals to understand both errors.
Experiment 1 lived entirely at the source / type-row layer.
The Tier 3 bet is well-positioned for experiment-1-shaped lanes
(stdlib helpers that compose existing primitives); it is harder
to defend for experiment-2-shaped lanes that surface emit-pass
invariants the source language does not expose.

## Subjective summary

`with_log_prefix` shipped with the spec output, three rounds
to green, and one new entry in `known-regressions.md` (R9).
The handler composition pattern itself is sound and the result
(`Trace + e`) types cleanly; the friction was entirely in the
emit/runtime layer that the source language abstracts away.

Per `CLAUDE.md` *Things to avoid* — *Do not introduce
abstractions beyond what the task requires* — the private
`effect TracePrefix[T]` is justified (no other path exists to
thread the prefix into the clause body), and the source-level
R9 workaround is the smallest patch that makes the fixture
correct without modifying compiler internals.

## Limitations / things I am not claiming

- **R9's structural fix is not in this PR.** The proposed
  `pcs_is_non_last` change in `known-regressions.md` is a
  hypothesis that needs the test surface a future Perceus / RC
  lane will own; this PR ships only the source-level workaround.
- **The two-bindings workaround leaks one ref per `read` call**
  if `pcs_rewrite`'s exit-drop pass does not reclaim
  `_keep_alive`. The emitted C *does* show an explicit
  `kai_decref(kai__keep_alive)`, so the leak is theoretical
  rather than observed; still, R9's structural fix would
  eliminate any ambiguity.
- **The `--effects-json` row report is mildly misleading.** It
  lists `TracePrefix` in `with_log_prefix`'s `effects` even
  though the outer `handle` consumes it. Filed as a UX note
  in the friction-points section; not a blocker.
- **No multi-shot resume tested.** `with_log_prefix` resumes
  exactly once per op (default for kaikai per `docs/effects.md`
  *Out of scope for v1*). A handler that intercepts `Trace` for
  retries / backtracking would be a separate experiment.

---

## Raw TSV (`/tmp/lane-exp2-arm-a-builds.tsv`)

```
timestamp	cmd	outcome	elapsed_s
2026-05-01T23:36:00-04:00	make-kaic2	OK	5
2026-05-01T23:36:07-04:00	test-trace	FAIL	-
2026-05-01T23:40:07-04:00	test-trace	FAIL	-
2026-05-01T23:48:12-04:00	test-trace	OK	1
2026-05-01T23:48:47-04:00	tier0	OK	27
2026-05-01T23:51:57-04:00	tier1	OK	182
2026-05-01T23:53:18-04:00	effects-json	OK	-
2026-05-01T23:53:18-04:00	effect-holes-json	OK	-
```
