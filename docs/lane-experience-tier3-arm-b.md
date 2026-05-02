# Lane experience report — tier3-arm-b (Trace effect + JSON-tooling embargo)

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## TL;DR

**Shipped.** Added a minimal `Trace` effect to `stdlib/trace.kai`
plus a row-polymorphic `with_trace_default` helper, a smoke
fixture under `examples/effects/trace_basic.kai` with a golden
`.out.expected`, and a `test-trace` Make target wired into both
`test` and `test-fast`. The lane is stdlib-only — no compiler
internals were touched, so selfhost stays byte-identical.

The lane operates under a Tier 3 LLM-friendliness experiment
(`docs/llm-authorship-baseline.md`): the agent is **forbidden**
from using `--effects-json`, `--effect-holes-json`, `--types-json`,
or typed-hole driven structured candidates. It must rely on the
plain-text compiler diagnostics alone. The findings are recorded
in §*Tier 3 — JSON tooling usage* below; the headline is that the
plain-text path was sufficient end-to-end for this lane, with one
non-effect detour worth flagging.

## Objective metrics (from `/tmp/lane-tier3-arm-b-builds.tsv`)

- Start: 2026-05-01T22:52:34-04:00
- End:   2026-05-01T23:05:28-04:00 (last logged tier1 completion)
- Wall-clock: ~13m (single agent run, no wait windows)
- Build / test invocations recorded in TSV:
  - `make test-trace`: 1 invocation, 1 pass, 0 fails (1s)
  - `make tier0`: 1 invocation, 1 pass, 0 fails (27s)
  - `make tier1`: 1 invocation, 1 pass, 0 fails (209s)
- JSON-tooling invocations: **0** (banned by the experiment)

## Investigation (M1)

Read of `docs/effects.md` (semantics) and `docs/effects-stdlib.md`
(catalog + defaults) established the effect-row model and the
nesting order the runtime installs around `main`. The catalog
note at §*Out of scope for v1* explicitly excludes
**user-overridable runtime defaults** — the runtime auto-installs
defaults only for `Console` / `Stdin` / `Env` / `File` / `Mutable`
/ `Cancel` / `Spawn` / `Ffi`. So `Trace` cannot be a "real"
default in the runtime sense; the brief's `with_trace_default`
maps onto the canonical *user-installable helper* pattern.

Reading `stdlib/effects.kai` revealed it is documentation-only
(the proposed atomic split lives there as comments; no live
declarations). The right template was **`stdlib/time.kai` →
`Clock`** (a stdlib-declared `effect`) plus
**`stdlib/actor.kai` → `with_mailbox`** (the row-polymorphic
helper that wraps a body and removes its own effect from the
returned row, propagating only the residual). `with_mailbox`
gave the exact signature shape needed:

```kai
pub fn with_mailbox[Msg, R, e](body: () -> R / Actor[Msg] + e) : R / e
```

A direct port to `Trace` is `body: () -> R / Trace + e` returning
`R / Console + e` — the residual `e` survives, plus `Console` is
added because the helper's clauses themselves call `print`.

The handler-clause syntax (`op(arg, resume) -> { ... }`) was
confirmed against `examples/effects/m7a_6c_op_dispatch.kai` and
`examples/effects/m12_8_y_phase4_alias_in_clause.kai`; the latter
specifically pins that calls to `print` from inside a clause
body are emitted correctly through the alias-aware op resolution.

## Implementation (M2 + M3)

Three new files, no edits to compiler internals.

### `stdlib/trace.kai`

```kai
effect Trace {
  log(msg: String) : Unit
  checkpoint(name: String) : Unit
}

pub fn with_trace_default[R, e](body: () -> R / Trace + e) : R / Console + e =
  handle { body() } with Trace {
    log(msg, resume) -> {
      print("[trace] " ++ msg)
      resume(())
    }
    checkpoint(name, resume) -> {
      print("[trace] checkpoint: " ++ name)
      resume(())
    }
    return(x) -> x
  }
```

`++` for concat (rather than `#{...}` interpolation) keeps the
clause bodies aligned with `stdlib/time.kai`'s style and avoids
any interpolation-in-clause-body interactions that the Tier 1
fixtures don't cover today. The fixture itself uses
interpolation in user code (which is the well-tested path).

### `examples/effects/trace_basic.kai` (+ `.out.expected`)

Two worker functions emit `Trace.log` and `Trace.checkpoint`
events. `main` wraps them under `with_trace_default` and prints
the computed answer via the prelude `print` builtin (which
already requires `/ Console`, so the row matches without a
second helper).

The fixture runs under `--path ../stdlib` only — no
`stdlib/core/*.kai` preludes. That keeps the fixture inside the
same prelude surface `test-time` uses and avoids dragging in
`println` from `stdlib/core/io.kai`, which would force the
test target to mirror `bin/kai`'s full prelude chain.

### `stage2/Makefile :: test-trace`

Mirrors `test-time` exactly:

```make
test-trace: $(TARGET) build
	@set -e; \
	./$(TARGET) --path ../stdlib ../examples/effects/trace_basic.kai > build/trace_basic.c; \
	$(CC) $(CFLAGS) -I ../stage0 build/trace_basic.c -o build/trace_basic-c; \
	./build/trace_basic-c > build/trace_basic.out; \
	diff -q ../examples/effects/trace_basic.out.expected build/trace_basic.out > /dev/null \
	  && echo "trace OK" \
	  || { echo "trace DIFF"; diff ../examples/effects/trace_basic.out.expected build/trace_basic.out; exit 1; }
```

Wired into both the `test` and `test-fast` aggregates plus the
`.PHONY` line.

## Validation (M4)

- `make test-trace` — pass (1s).
- `make tier0` — pass (27s; demos baseline 24/24 holds, selfhost
  byte-identical).
- `make tier1` — pass (209s).

## Surprises / pitfalls

### One detour: Int-literal unboxing collides with `int_to_string`

The brief's example fixture binds the worker result to an
intermediate local:

```kai
fn worker_a() : Int / Trace = {
  Trace.log("worker_a started")
  let result = 42
  Trace.checkpoint("worker_a midpoint")
  Trace.log("worker_a result: #{int_to_string(result)}")
  result
}
```

This compiles cleanly through kaic2 but the emitted C fails to
build with:

```
error: incompatible integer to pointer conversion passing 'int64_t'
       to parameter of type 'KaiValue *'
note:  passing argument to parameter 'v' here  (kai_prelude_int_to_string)
```

The unbox pass (`docs/unboxing-phase2-design.md`) marks
`let result = 42` as `MUnboxed` and lowers it to
`int64_t kair_result = 42LL`. The boundary rebox tactic from
§3 doesn't fire when the consumer is the prelude
`int_to_string(KaiValue *v)`. Reproduces deterministically;
parking it for the unbox-phase2 followup lane (this lane is
stdlib-only by discipline). Workaround in the fixture: drop the
intermediate binding and inline the literal.

The same pattern with a function parameter (`worker_b(x: Int)`
calling `int_to_string(x)`) compiles fine — parameters stay
boxed by the Phase 2 non-goals. Only `let`-bound int locals
trip the boundary.

### Effect-as-default is a misnomer in v1

The brief uses *default handler*; the v1 catalog does not
support user-overridable runtime defaults. The shipped helper is
a user-installable wrapper named `with_trace_default` to signal
intent ("the canonical/sane default if you don't have stronger
opinions"), not a runtime default in the
`kai_default_stdout_print` sense.

## Tier 3 — JSON tooling usage

The Tier 3 LLM-friendliness experiment forbade
`--effects-json` / `--effect-holes-json` / `--types-json` and
typed holes. The intent is to measure whether plain-text
compiler diagnostics suffice, or whether structured output is
load-bearing.

### Errors encountered (all plain-text resolutions worked)

1. **`undefined name 'println' (did you mean 'print'?)`** —
   first kaic2 error after writing the fixture. The "did you
   mean" suggestion immediately resolved it: `println` lives in
   `stdlib/core/io.kai` which is loaded by `bin/kai` but not by
   a bare `--path ../stdlib` Make target. Switched to `print`,
   problem closed without consulting any docs. **Plain-text
   sufficient. JSON would not have helped here**; the diagnostic
   already named the correct fix.

2. **C compile-time `incompatible integer to pointer` from the
   unboxer.** This was a *codegen* error (clang on emitted C),
   not a kaic2 plain-text diagnostic. The structured JSON
   tooling for kaikai is type-system-facing, so it would not
   have caught this regardless. Resolved by reading the C output
   of `kaic2 ... > /tmp/trace_basic.c` and tracing the
   `kair_result` (raw scalar) vs `kai_prelude_int_to_string`
   (boxed) signature mismatch. **JSON tooling out of scope for
   this class of bug.** A `--types-json` dump might have shown
   `let result = 42` typed as `Int` mode `MUnboxed`, which would
   have surfaced the unbox classification, but the plain-text
   error already pointed at the exact identifier and parameter
   type.

### Where I noticed I would have asked for JSON

Zero times during this lane. The signature of
`with_trace_default` was clear from the `with_mailbox` template;
the row arithmetic (`/ Trace + e` → `/ Console + e`) was a
mechanical port. No effect-row inference puzzles arose. The
fixture's effect rows (`/ Trace`, `/ Console`) were both
explicit and shallow.

If the lane had been *richer in row composition* — for example,
inferring an unannotated helper's row through several callees,
or debugging why an effect was leaking through a body that
shouldn't carry it — the `--effects-json` dump would have been
the first thing I reached for. Trace's two-op surface and
single-handler shape did not exercise that depth.

### Time lost (>5 min iterating without understanding the error)

None. The longest single decision in the lane was the
"workaround vs fix the unboxer" call (~30s of thought; the
brief's lane discipline made *don't fix it inline* the obvious
choice).

### Bottom line for the experiment

For a **stdlib-shaped lane** (declare an effect, write a helper
that mirrors an existing helper, ship a fixture), plain-text
diagnostics carried the entire lane. The two errors that did
fire were both immediately actionable from the plain-text
output. JSON tooling would have been redundant here.

This is **one data point** — a 13-minute lane on the easiest
shape of effect work. It does not generalise to:

- effect-inference debugging where the typer infers but the row
  doesn't match expectations (the case `--effects-json` and
  `--effect-holes-json` were built for),
- handler-composition lanes that nest helpers across modules,
- LLM authorship of net-new effects in unfamiliar shapes
  (`Mutable`, `Spawn`, `Actor[Msg]`).

A useful next experiment would run the same embargo on a lane
that *deliberately* exercises row inference: e.g., adding a
helper that wraps two effects whose composition order is
non-obvious, or porting a fixture from explicit-row to
inferred-row.

## Followups parked

- **Unbox boundary for `let`-bound int locals consumed by
  prelude polymorphic functions.** Reproduces with
  `let result = 42; int_to_string(result)`. Park in the
  unbox-phase2 follow-up doc (the lane brief was stdlib-only).
- **`--prelude` chain for stdlib examples that want `println`.**
  Today only `bin/kai` runs the full prelude chain; Make
  targets use `--path` only. Not a regression — the fixture
  shape works as-is — but reaching for `println` in a
  Make-targeted fixture currently requires either the full
  prelude chain (cf. `test-stdlib` `CORE_PRELUDE_FLAGS`) or a
  switch to `print`.

## Limitations of this report

- Single agent (Claude Opus 4.7). Not generalisable across LLMs.
- One lane, easiest effect shape (stdlib helper that mirrors an
  existing helper). The Tier 3 conclusion does not extrapolate
  to harder lanes.
- Wall-clock includes the agent's reasoning and tool-call
  overhead; no profiling separates the kaic2 build cost from
  the agent decision cost.
- The "JSON would not have helped here" claim is a counter-
  factual; it cannot be verified without re-running the same
  lane with JSON access enabled.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-01T23:00:44-04:00	test-trace	OK	1
2026-05-01T23:01:24-04:00	tier0	OK	27
2026-05-01T23:05:00-04:00	tier1	OK	209
```
