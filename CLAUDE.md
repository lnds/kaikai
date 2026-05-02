# kaikai

A functional programming language with static typing, compiled to native code via LLVM, with algebraic effects as a first-class primitive and Elixir-style pipelines.

The project is in a **full redesign phase**. The previous history (a partial compiler in Go) was discarded. The current design lives in `docs/design.md`.

## Project language conventions

- **Commit messages in English. No exceptions.** This includes
  commit subjects, bodies, PR titles, and PR descriptions. No
  Spanish loanwords either, even if they were used as technical
  jargon during agent briefing — translate them. Examples that
  have leaked in past sessions and must be avoided: "mentira"
  (use "structural lie" or "false claim"), "letra chica"
  (use "fine print"), "trampa" (use "trap" or "pitfall"),
  "aterrizó" (use "landed"). If a brief in Spanish reaches you,
  the commit message you produce is still English.
- **All documentation in English.** Including `README.md`, `docs/`, code comments, and any user-facing text.
- Conversation with the user (Spanish) is not documentation and does not appear in the repo.

## Cross-cutting principles

Three tiers, ordered by how non-negotiable they are. When principles conflict, the higher tier wins.

### Tier 1 — Load-bearing

1. **Safe at compile time**
   - Memory safety by default.
   - No null; `Option[T]` always.
   - Effects visible in types: every effect a function uses appears in its row (`/ Console + File + Cancel`, etc.). A function cannot be called from a context that does not handle its effects. Catalog and defaults pinned in `docs/effects-stdlib.md` (Doc B).
   - Explicit runtime escapes, audited not incidental: `panic`, unfilled `?`, `todo!`, unbound `axiom`, FFI crossings, **opaque mutable `Array[T]`**.
   - The Array escape is provisional: `array_make / get / set / grow` mutate in place and the mutation is not visible in the type. It exists so the stage 2 inferencer can index its substitution by TyVar id in O(1). Migration specified in `docs/effects-stdlib.md` §`Mutable`: `array_*` retrofits behind the `Mutable` effect in m7a, with the array-indexing sugar (`a[i]`, `a[i] := v`) shipping in m7b. Must not be used as a general-purpose container in new code.

2. **Runtime-efficient**
   - Monomorphisation of generics.
   - Mandatory tail-call optimisation.
   - Primitives unboxed inside fibers; heap boxing only for compound immutables.
   - Effects compiled with one-shot continuations as the zero-cost default; multi-shot pays on use.

3. **Fast compilation**
   - Single-pass parse, LL(1) grammar with minor bookkeeping.
   - HM-extended types with effect rows; decidable; **no Haskell-style type-class resolution** (no HKT, no constraint propagation in signatures, no functional dependencies, no type families). Single-dispatch protocols Go/Clojure/Elixir-style — `O(1)` impl-table lookup, no constraint solver — are permitted (m12.8, see `docs/protocols.md`).
   - Pipeline `lex → parse → resolve → infer → monomorph → perceus → lower`, dumpable between any two passes.

### Tier 2 — Aspirational (trade-offs allowed; Tier 1 wins ties)

4. **Structured compiler output**
   - Diagnostics and queries come as stable JSON alongside human text. Typed holes + `--holes-json` are the prototype; `kai type --json` and siblings extend the contract.
   - **Not** "one canonical form per construct". The language has intentional redundancies (`=` vs `{}` bodies, `|>` vs `|`, multiple lambda forms) — each form signals intent. The real rule is *few forms, each with clear intent*.

5. **Approachable core, novel where it pays off**
   - Day-to-day syntax (declarations, `let`, `if`, `match`, pipes, pattern matching) stays close to Python / JS / Elixir.
   - Advanced surface — effects, handlers, nursery, fibers, holes — is **novel on purpose**. Expect a one-day ramp for sequential kaikai and a week to internalise the effect model.

6. **Few visible concepts, layered**
   - Floor: ~10 concepts for basic code (types, functions, `let`, `match`, `if`, records, sum types, lists, pipes, strings).
   - Advanced features stack on top; no program pays for every concept.

### Tier 3 — Strategic bet (depends on future conditions)

7. **LLM authorability**
   - Bet: with typed holes + structured JSON + stable rules, LLMs can author kaikai, even though current models know Python / Rust far better than effect-typed languages.
   - Mechanism: shift weight from *the model knowing kaikai* to *the compiler telling the model what goes where* — holes, effect queries, exhaustiveness counterexamples.
   - Acceptance criterion: an LLM with JSON access completes the top 80% of typical functions within one round of compilation.

## Tie-breakers when principles conflict

- Safety beats ergonomics.
- Fast compilation beats generality.
- Runtime efficiency beats expressive novelty.
- Approachability beats one-canonical-form.
- LLM-friendliness is not a veto: a feature good for LLMs but bad for humans does not ship.

## Not principles

Do not cite these in design arguments:
- *One canonical form per construct* — already violated four times deliberately; see #4.
- *Never surprise a Python programmer* — effects surprise them, by design; see #5.
- *Zero-cost abstractions* — effects, fibers, and RC have small but non-zero costs.
- *Backward compatibility* — not promised until post-MVP.

## Baseline architectural decisions

- **Backend**: LLVM directly (stage 2). Stages 0 and 1 emit portable C.
- **Memory**: hybrid **Perceus** (compile-time optimized RC, Koka-style) inside each fiber + **isolated fibers** BEAM-style (private heap, messages copied). No borrow checker.
- **Effects**: capability-passing **Effekt + inference**. Effects as an explicit set; inferred in local bodies; mandatory annotation in public signatures. Three pinned design docs: `docs/effects.md` (Doc A — semantics), `docs/effects-stdlib.md` (Doc B — catalog and defaults), `docs/effects-impl.md` (Doc C — CPS transform and runtime). Sugars (trailing lambdas, `@cap` / `cap := v`, `var`, `a[i]`) live in `docs/syntax-sugars.md` and ship in m7b.
- **Concurrency**: fibers and actors live entirely inside the effect system.
  - `spawn` / `await` / `select` / `cancel` are ops of the **`Spawn`** effect (`docs/structured-concurrency.md`).
  - `send` / `receive` / `self` are ops of the parameterised **`Actor[Msg]`** effect (`docs/actors.md`).
  - `Cancel` is a separate effect for cooperative cancellation.
- **FFI**: crossing to C via the `Ffi` effect capability. Declarations use `extern "C" fn name(args) : T`.
- **Tooling**: single `kai` binary with subcommands (`build`/`run`/`test`/`fmt`/`repl`/`lsp`/`doc`).
- **Tests**: builtin syntax (`test "..." { ... }` + `assert`), integrated with `kai test`.

## Three-stage bootstrap

- **Stage 0** — minimal C compiler. Zero dependencies. Compiles **kaikai-minimal** → portable C.
- **Stage 1** — intermediate compiler in kaikai-minimal. Compiles **enough of full kaikai to compile stage 2** (basic effects + handlers + basic Perceus + monomorphisation) → C. Full effect catalog, m7b sugars, fibers, and actors land in stage 2 (`docs/stage2-design.md`).
- **Stage 2** — definitive compiler in full kaikai. Direct LLVM backend. Self-hosted.

Any machine with `cc` can bootstrap from scratch:

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -I stage0 -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

## Testing discipline (load-bearing — read before opening a PR)

Three test tiers run at three cadences. Pin spec lives in
`docs/testing-tiers.md`. CI runs in GitHub Actions
(`.github/workflows/tier1.yml`, ubuntu-latest); it is the source
of truth for tier1 on every PR and on every push to `main`.
Defaults that every agent must follow without being told:

- **Tier 0 — pre-commit fast sanity** (~30–60s): `make tier0`
  (`make selfhost && make demos-no-regression`). Run before each
  commit when iterating on compiler code; you may skip when the
  change is documentation-only or trivially text-edit. CI catches
  what local skips miss; this rule is now a velocity tool, not a
  gate.
- **Tier 1 — gated by CI on every PR**: GitHub Actions runs
  `make tier0 && make tier1` on every PR and on every push to
  `main`. **CI green is the merge gate** — the PR description no
  longer needs to paste the trailing line of `make tier1`
  verbatim, the check status replaces that ceremony. Agents may
  run `make tier1` locally for faster feedback (~2–4 min on mac)
  but it is not required. The R2 / R3 / R4 regressions that
  motivated the manual ceremony (commit messages claiming
  `make test clean` without anyone having run `make test`) are
  the kind of failure CI now makes structurally impossible.
- **Tier 2 — `make daily`**: only the maintainer / cron runs this on
  `main` HEAD at end-of-day. Tier 1 + stress fixtures + coverage
  probe + RC budget. Agents do not run Tier 2 inside their lanes —
  it would burn velocity. ~10-20 min.

Adjacent rules every agent must apply:

- **Working tree must be clean** at every commit. No `git status`
  output, no stashed work tied to a different lane. The
  2026-04-29 audit caught `stage1/compiler.kai` dirty for hours
  because nobody noticed; that hides which gate actually ran.
- **Lane discipline**: a worktree fixes one thing. If you find a
  bug outside your lane, **open a GitHub issue** with repro +
  hypothesis (label `regression` plus the topic-specific labels
  — `runtime`, `compiler`, `perceus`, `typer`, `stdlib`,
  `unboxing`, `refinements`), do not fix it inline. The L2 wrong-
  lane revert from 2026-04-29 is the precedent. (Pending work
  used to live in `docs/*-followup.md` + `docs/known-regressions.md`;
  those were retired in PR #99, see "Where pending work lives"
  below.)
- **Lane handoff — push + PR is authorized**: an agent spawned
  in a worktree (e.g. via `/wt-claude`) **is authorized to push
  its lane branch to origin and open a pull request via
  `gh pr create` once its acceptance gate is green** (Tier 0 +
  Tier 1, plus `tier1-asan` if the lane touches the runtime or
  emit pass). This is the standing exception to the global
  Claude Code rule "do not push to the remote repository unless
  the user explicitly asks." Rationale: the worktree spawn
  itself is the explicit authorization for the lane's full
  push-to-PR loop; making the user also re-confirm push at the
  end of every lane burns turns and breaks the parallel-lane
  flow this repo's worktree pattern is built around. **Merge
  stays with the integrator** — agents do NOT run
  `gh pr merge`, do NOT bump `VERSION`, and do NOT promote
  `## [Unreleased]` (those are integrator steps per the
  workflow below). If CI fails, the agent fixes the failure on
  the same branch and pushes again; force-push is fine for
  lane branches that have not been reviewed yet.
- **VERSION + CHANGELOG (agent side)**: do NOT bump the version
  yourself. Add your closing-commit entry to `CHANGELOG.md` under
  the existing `## [Unreleased]` section and **leave `VERSION`
  untouched**. The integrator (the human merging the PR) assigns
  the final version number after merge. This rule exists because
  parallel lanes cannot know each other's order of merge: on
  2026-04-29 three agents (PR #24 / #25 / #26) all asked for the
  same `0.12.0` because each was opened against the same
  baseline; the integrator had to bump them to 0.12.0 / 0.13.0 /
  0.14.0 by hand. Leaving `VERSION` and the section header alone
  in the PR avoids that work. The PR description may suggest the
  *next* number, but treat it as advisory only.

- **Integrator workflow (post-CI)**: the integrator no longer
  performs a local merge dance. Once tier1 is green on the PR, the
  flow is:

      # 1. Merge via gh — gh creates the merge commit on GitHub.
      gh pr merge <N> --merge \
        --subject "Merge pull request #<N> from <branch>" \
        --body "$(cat <<EOF
      <descriptive body — same style as past merges, e.g. f9a8822>
      EOF
      )"

      # 2. Release commit on main — promotes [Unreleased] and
      # bumps VERSION. Goes direct to main (admin bypass on
      # branch protection).
      git pull --ff-only origin main
      $EDITOR VERSION CHANGELOG.md   # bump + rename header
      git add VERSION CHANGELOG.md
      git commit -m "release: 0.X.Y — promote [Unreleased]"
      git push origin main

  The release commit is its own commit, not bundled inside the
  merge commit. There is a brief window (seconds) where main
  has the new content under `[Unreleased]` and the old
  `VERSION`; immaterial for an MVP-stage compiler. The
  integrator decides the version number after seeing what
  parallel lanes already took. The advantage over the prior
  local-merge-dance: ~30s per merge instead of ~5–7 min, no
  manual tier1 validation (CI did it), and the flow is
  mechanizable — an agent can perform it given a brief.
- **`make daily` failures are diagnostics, not blockers.** Tier 0
  / Tier 1 already gated every commit, so `main` stays unbroken.
  A `daily` failure opens a lane the next morning; do not jam
  velocity by treating it as a per-PR gate.
- **Coverage probe ratchet**: `tools/coverage-baseline.txt` is the
  current allowed gap count. New gaps fail the probe; closed gaps
  bump the baseline down. Add `<!-- coverage: skip -->` after a
  heading only when it documents a non-testable concern (design
  rationale, references), not when you forgot to write a fixture.
- **Honesty targets**: when you ship a runtime change that affects
  fibers or Perceus, update the relevant tier in
  `docs/fibers-honesty-targets.md` or
  `docs/perceus-honesty-targets.md`. The "Tier 1 #2 holds without
  footnotes" claim in this CLAUDE.md is verifiable only against
  those documents.
- **Where pending work lives**: open follow-ups and known
  regressions live in **GitHub Issues**, not in tracking docs.
  PR #99 (2026-05-02) retired `docs/m5x-followup.md`,
  `docs/m12-6x-followup.md`, `docs/unboxing-phase2-followup.md`,
  and `docs/known-regressions.md`; their open items moved to
  issues #77–#96 with the labels `regression`, `perceus`,
  `runtime`, `typer`, `compiler`, `stdlib`, `unboxing`,
  `refinements`, and the tier labels `tier1` / `tier2` / `tier3`.
  When you discover a new follow-up or regression, open an issue
  with the relevant labels — do not re-create the tracking docs.
  Closed items are recoverable from `git log` if needed.

## Things to avoid

- **Do not go back to Go** for the compiler. The prior Go frontend was discarded on purpose.
- **Do not introduce a Rust-style borrow checker**. Perceus + fibers resolves memory at compile time without cognitive cost.
- **Do not add forms whose intent overlaps with an existing one**. Two bodies for functions is fine (short vs block); two pipes is fine (apply vs map); a third way to do the same thing with no new intent is not. The standard is *few forms, each carrying distinct intent* — not *one form, full stop*.
- **Do not add dependencies to stage 0**. It must build on any system with an ANSI `cc`.
- **Do not design against WASM, Windows, or other post-MVP targets**, but do not invest effort in them now either.
- **Do not cite retired principles** in design arguments (see the "Not principles" section above).

## Current state

See `docs/design.md` for the full plan, phased roadmap, and MVP specification.
