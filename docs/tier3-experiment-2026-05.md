# Tier 3 LLM-friendliness — A/B experiment 1 (2026-05-02)

First empirical A/B test of the Tier 3 strategic bet pinned in
`CLAUDE.md`:

> "with typed holes + structured JSON + stable rules, LLMs can author
> kaikai, even though current models know Python / Rust far better
> than effect-typed languages."

Acceptance criterion (per the same pin): "an LLM with JSON access
completes the top 80% of typical functions within one round of
compilation".

This is the first of an experiment series motivated by
`docs/lane-experience-retro-2026-05-01.md` §*Recovery protocol*: the
17-PR retro could not validate the bet because no per-lane
instrumentation captured JSON-tooling usage. This experiment is the
designed A/B that the retro deferred to the post-Tongariki window.

## Design

Two agents launched in parallel on identical baseline (`main` @
`e6ba1c3`, post Tongariki MVP closure). Identical prompts modulo a
single section: **Tooling guidance**.

| Arm | Prompt instruction |
|---|---|
| **A** | Use `--effects-json`, `--effect-holes-json`, typed holes proactively. Document each invocation in the lane-experience report. |
| **B** | Use ONLY plain-text compiler output. JSON flags and typed-hole structured candidates are explicitly forbidden. |

Both arms implemented the same feature: a minimal `Trace` effect in
`stdlib/trace.kai` with two ops (`log`, `checkpoint`) plus a default
handler that writes to stderr with a `[trace]` prefix, plus a fixture
under `examples/effects/trace_basic.kai`.

Both arms had identical:

- Feature spec (sketched in the brief; agent free to adapt to current
  stdlib idioms)
- Instrumentation snippet (TSV log per `make` / `kaic2` invocation;
  `start.txt` / `end.txt` timestamps; lane-experience report at end)
- Constraints (English commits/docs, VERSION untouched, CHANGELOG
  under `[Unreleased]`, lane discipline, CI as gate)
- Reference materials list (`docs/effects.md`, `effects-stdlib.md`,
  `stdlib/effects.kai`, `stdlib/core/io.kai`)

Worktrees and branches were `kaikai.tier3-arm-{a,b}` and
`tier3-arm-{a,b}` respectively. Agents had no awareness of each
other and no shared CI access during the experiment window.

## Raw data

### Wall-clock

Source: `start.txt` and final TSV entry per arm.

| Arm | Start | First green tier1 | Wall-clock |
|---|---|---|---|
| A | 2026-05-01T22:52:29-04:00 | 2026-05-01T23:06:13-04:00 | **13m 44s** |
| B | 2026-05-01T22:52:34-04:00 | 2026-05-01T23:05:00-04:00 | **12m 26s** |

Arm B finished **1m 18s faster** than Arm A. The opposite of the
direction the bet predicted.

### Build invocations

Source: TSV logs.

| Arm | `make`/`kaic2` builds | JSON tooling invocations |
|---|---:|---:|
| A | 5 (`kaic2-fixture` ×2, `test-trace`, `tier1`, `tier0-final`) | 3 (`effects-json` OK, `effect-holes-json` OK, `types-json` NOT-A-FLAG) |
| B | 4 (`test-trace`, `tier0`, `tier1`, `tier1-final`) | 0 (banned) |

If we exclude Arm A's deliberate JSON invocations (which the agent
did "for this report", not for the work — see qualitative below), A
made 5 actual builds and B made 4. Effectively identical.

### Compiler errors hit

| Arm | Errors during development |
|---|---|
| A | 2 (`println` did-you-mean `print` + R8 unbox×interp regression) |
| B | 1 (`println` did-you-mean `print`) |

The R8 detour is what pushed Arm A's wall-clock to ~13m 44s. Without
R8, both arms would have closed in ~5–6m post-implementation.

### PR diff size

| Arm | Files | +lines | −lines |
|---|---:|---:|---:|
| A | 7 | 458 | 3 |
| B | 6 | 419 | 3 |

Comparable. Arm A had one extra file (the R8 known-regression entry).

## Qualitative findings

Source: lane-experience reports in
`docs/lane-experience-tier3-arm-a.md` and
`docs/lane-experience-tier3-arm-b.md`.

### Arm A — "JSON tooling invocations were performative, not load-bearing"

Verbatim from `lane-experience-tier3-arm-a.md` §*Tier 3 — JSON
tooling usage*:

> `kaic2 --effects-json`: 1 invocation (deliberate, **for this
> report**). `kaic2 --effect-holes-json`: 1 invocation (deliberate).
> `kaic2 --types-json`: 1 invocation — flag does not exist, kaic2
> silently fell through to the default C-emit path.

The agent made the JSON invocations **after the work was already
complete**, to fulfill the experimental instrumentation requirement.
For both errors hit during the work itself:

- Error (a): `undefined name 'println' (did you mean 'print'?)` —
  resolved in ~10 seconds by reading the plain-text suggestion.
- Error (b): R8 unbox/interp regression — diagnosed via reading the
  emitted C, bisecting with a 4-line repro. JSON tools (which target
  effect rows and holes, not emit-pass bugs) would not have helped.

### Arm B — "the plain-text path was sufficient end-to-end"

Verbatim from `lane-experience-tier3-arm-b.md` headline:

> The headline is that the plain-text path was sufficient
> end-to-end for this lane, with one non-effect detour worth
> flagging.

Single error hit, resolved by the compiler's "did you mean" hint:

> `undefined name 'println' (did you mean 'print'?)` — first kaic2
> error after writing the fixture. The "did you mean" suggestion
> immediately resolved it. **Plain-text sufficient. JSON would not
> have helped here**; the diagnostic already named the correct fix.

Arm B never felt blocked. Did not encounter R8 because it built the
fixture differently (no `let value = 42; #{int_to_string(value)}`
construct that exercises the bug).

### Arm A — bonus: discovered R8

While building the fixture, Arm A hit R8 — a Phase 2 unboxing ×
string interpolation regression. `let value = 42` unboxed to
`int64_t kair_value`, but the desugared `#{int_to_string(value)}`
still spelled the boxed name `kai_value`. C compile error.
Diagnosed, minimally reproduced, documented in
`docs/known-regressions.md` as **R8**, worked around by switching
the fixture to use `++` concatenation. Lane discipline preserved
(no inline fix; pinned for a future emit-pass lane).

## Synthesis

### What this experiment establishes

For **straightforward stdlib mirror lanes with abundant prior art**
(in this case, `Trace` mirroring `Console`, `Clock`, `Writer`,
`with_mailbox` patterns):

- Plain-text compiler diagnostics with "did you mean" suggestions
  are **sufficient**. Both arms shipped tier1 green in ~13 minutes
  with 1–2 errors each.
- JSON tooling does **not** accelerate development in this scope.
  Arm A's invocations were performative, not load-bearing.
- The acceptance criterion ("80% of typical functions in one round
  of compilation") **is met by both arms** — both required only a
  handful of build cycles to converge.

### What this experiment does NOT establish

The bet's broader claim — that JSON tooling unlocks LLM authorship
of effect-typed languages — is **neither validated nor refuted**:

- **n=1 per arm** — direction-setting only, not statistically
  significant.
- **Single feature scope** — stdlib mirror with abundant prior art is
  the easiest possible case. Areas where the JSON tools should
  plausibly differentiate (effect-row composition without prior art,
  hole-driven type discovery in larger codebases, polymorphic
  call-site debugging) were **not exercised**.
- **Single agent personality** (Claude) — variance across LLMs
  unmeasured.
- **Same agent didn't run both arms** — within-subject vs
  between-subject experimental design choice was made for parallel
  speed, but trades off control over agent-specific variance.
- **`--types-json` does not exist** despite the brief listing it as
  optional (with `(si existe)` hedging). Arm A discovered this
  empirically. The bet's actual surface is `--effects-json` +
  `--effect-holes-json` + typed holes (`?` / `?name`).

### Operational recommendations

1. **Continue the experiment series.** Schedule a second A/B with a
   feature scope that genuinely stresses effect-row inference. Three
   candidate scopes documented in the post-experiment-1 chat: (a)
   pattern-match guards (parser + typer + exhaustiveness), (b)
   refinement-type narrowing in match arms (m12.6 followup
   territory), (c) **handler composition — `with_log_filter(level,
   body)` that wraps the just-shipped `Trace` effect with a level
   filter, requiring delegate-to-outer-handler-of-same-effect
   patterns with no obvious prior art**. (c) was selected for
   experiment 2.
2. **Do not invest in m11 / new JSON tooling solely on the basis of
   the Tier 3 bet** — the evidence so far is that plain-text
   diagnostics with helpful suggestions carry the work for typical
   stdlib lanes. m11 should be justified on its own merit
   (Elm/Rust-grade error UX for human developers), not as bet
   validation.
3. **Update the brief format**: drop `--types-json` from agent
   prompts (it does not exist). Document the actual JSON tooling
   surface in `docs/lane-instrumentation.md`.
4. **The "did you mean X?" suggestions in kaikai's typer are
   load-bearing for LLM productivity**. Both arms relied on them
   for the only kaikai-language error encountered. Future typer
   work should preserve and extend this pattern.

### Implications for `docs/llm-authorship-baseline.md`

The baseline doc (n=6 lanes, 2026-04-26) deferred the acceptance
question. This experiment is the first formal A/B answer for one
slice of that question. The headline: for this slice, **the plain-
text path is sufficient and the bet is not directionally
supported**. Whether the bet holds for other slices is the subject
of subsequent experiments. An addendum to
`llm-authorship-baseline.md` should include the data above and the
caveats; the doc should remain agnostic until n is meaningfully
larger.

## Limitations

- **Self-report bias** — agents wrote their own retros. They may
  rationalise tooling choices post-hoc. Counter-evidence: both arms
  converged on the same conclusion ("plain-text sufficient"), which
  suggests low bias in this direction.
- **Both agents are the same Claude version** — within-model variance
  small; between-model variance unmeasured.
- **Feature scope was easy by design** — to bound the experiment to
  ~1–2h per arm. A harder scope might surface tooling differences
  that this experiment cannot.
- **TSV granularity is coarse** — invocation timestamps but no
  per-error wall-clock attribution. Cannot distinguish "spent 30 s
  reading error" from "spent 5 m". Refining the instrumentation is
  pinned for the next experiment.

## Appendix: full retros

The two lane-experience reports are committed alongside this
synthesis:

- `docs/lane-experience-tier3-arm-a.md` — full Arm A retro
  (instructed to use JSON tools)
- `docs/lane-experience-tier3-arm-b.md` — full Arm B retro
  (forbidden from JSON tools)

Both include their TSV instrumentation logs verbatim.
