# kaikai

A functional language with static typing, LLVM native compilation, algebraic effects as a first-class primitive, and Elixir-style pipelines. Full design lives in `docs/design.md`.

## Project language

- Commit messages, PR titles/bodies, and all documentation are **English only** — no Spanish loanwords, even when the briefing is in Spanish.
- Conversation with the user (Spanish) is not documentation and does not appear in the repo.

## kaikai syntax — `kai info` is authoritative

Before claiming a kaikai surface form exists in conversation, code suggestions, or examples, run `kai info <topic>`. `kai info` with no args lists the topics; `kai info syntax` is the one-page cheat sheet of every form kaikai actually has, including an explicit **NOT IN KAIKAI** section listing the false-friends that look plausible but do not exist (operator sections, `\x -> body`, list comprehensions, `do { }`, type classes, `throw/catch`, `return` statements, etc.). LLM agents in particular extrapolate from Haskell/Python/JS when uncertain — `kai info` is the cheap, always-correct way to check before writing. `kai info <topic> --json` returns the same content as structured JSON for programmatic consumption.

The reference pages live at `docs/info/*.md` and travel with the binary (dev checkout reads from there; installed tarball ships `share/kaikai/info/`).

## Cross-cutting principles

Three tiers; the higher tier wins on conflict.

### Tier 1 — Load-bearing

1. **Safe at compile time.** Memory-safe by default; `Option[T]` instead of null; effects visible in row types (catalog in `docs/effects-stdlib.md`); explicit runtime escapes (`panic`, `?`, `todo!`, `axiom`, FFI) are audited, not incidental. `Array[T]` writes ride the `Mutable` effect per `docs/effects-stdlib.md` §`Mutable` on the observable-effects discipline (issue #251 + #252): observable mutations require `Mutable`, locally-constructed Arrays mask it.
2. **Runtime-efficient.** Generics monomorphised; mandatory TCO; primitives unboxed inside fibers; effects compile to one-shot continuations as the zero-cost default.
3. **Fast compilation.** Single-pass parse, LL(1) with minor bookkeeping; HM extended with effect rows, decidable; **no Haskell-style type-class resolution** (no HKT, no constraint propagation, no functional dependencies, no type families). Single-dispatch protocols Go/Clojure/Elixir-style — `O(1)` impl-table lookup — are permitted (`docs/protocols.md`). Pipeline `lex → parse → resolve → infer → monomorph → perceus → lower`, dumpable between any two passes.
4. **Stability without stagnation** (Rust-inspired, [2014 manifesto](https://blog.rust-lang.org/2014/10/30/Stability/)). Upgrades between kaikai versions never break user code without a migration path. We iterate fast on internals — runtime, codegen, RC discipline, cache layers, typer refactors — but the surface a user wrote against is preserved across versions within the same **edition**. Breaking changes to the language surface require an **edition bump** (Tongariki → Hanga Roa → Orongo, geographic Rapa Nui names), which is a deliberate public commitment, not an incidental side effect of a refactor lane. Editions are tracked in the `EDITION` file at repo root, surfaced in `kai --version`, and documented in `docs/editions.md`. The lesson load-bearing here: **we owe users that they never dread upgrading kaikai**.

### Tier 2 — Aspirational (Tier 1 wins ties)

5. **Structured compiler output.** Diagnostics + queries come as stable JSON alongside human text. Typed holes + `--holes-json` are the prototype. **Not** "one canonical form per construct" — the language has intentional redundancies (`=` vs `{}` bodies, `|>` vs `|`, multiple lambda forms); the rule is *few forms, each with clear intent*.
6. **Approachable core, novel where it pays off.** Day-to-day syntax (declarations, `let`, `if`, `match`, pipes, patterns) stays close to Python/JS/Elixir. Advanced surface — effects, handlers, nursery, fibers, holes — is novel on purpose.
7. **Few visible concepts, layered.** ~10 concepts for basic code; advanced features stack on top.

### Tier 3 — Strategic bet

8. **LLM authorability.** Bet that typed holes + structured JSON + stable rules let LLMs author kaikai despite weak prior exposure. Acceptance: an LLM with JSON access completes the top 80% of typical functions within one round of compilation.

### Tie-breakers

- Safety beats ergonomics.
- Fast compilation beats generality.
- Runtime efficiency beats expressive novelty.
- Approachability beats one-canonical-form.
- LLM-friendliness is not a veto: a feature good for LLMs but bad for humans does not ship.

### Not principles (do not cite)

- *One canonical form per construct* — already violated four times deliberately; see #5.
- *Never surprise a Python programmer* — effects surprise them, by design.
- *Zero-cost abstractions* — effects, fibers, RC have small but non-zero costs.
- *Full backward compatibility forever* — within an edition, yes (per #4 stability). Across editions, breaking changes are allowed but require opt-in. Pre-1.0 we still ship `feat:` / `fix:` rapidly; the protection is at the edition boundary, not at every minor version.

## Baseline architecture

- **Backend**: LLVM directly (stage 2). Stages 0–1 emit portable C.
- **Memory**: Perceus (compile-time-optimised RC, Koka-style) inside each fiber + isolated fibers BEAM-style (private heap, messages copied). No borrow checker.
- **Effects**: capability-passing Effekt + inference. Inferred in local bodies; mandatory annotation in public signatures. Three pinned docs: `docs/effects.md` (semantics), `docs/effects-stdlib.md` (catalog + defaults), `docs/effects-impl.md` (CPS + runtime). Sugars in `docs/syntax-sugars.md`.
- **Concurrency**: fibers and actors live inside the effect system. `spawn/await/select/cancel` are ops of `Spawn` (`docs/structured-concurrency.md`); `send/receive/self` are ops of `Actor[Msg]` (`docs/actors.md`); `Cancel` is a separate cooperative-cancellation effect.
- **FFI**: crosses to C via the `Ffi` capability. `extern "C" fn name(args) : T`.
- **Tooling**: single `kai` binary (`build`/`run`/`test`/`fmt`/`lsp`/`doc`). REPL is permanently out of scope — see `docs/decisions/repl-removal-2026-05-09.md`.
- **Tests**: builtin syntax (`test "..." { ... }` + `assert`), via `kai test`.

## Three-stage bootstrap

- **Stage 0** — minimal C compiler, zero deps. Compiles kaikai-minimal → portable C.
- **Stage 1** — intermediate compiler in kaikai-minimal. Compiles enough of full kaikai to produce stage 2 → C.
- **Stage 2** — definitive compiler in full kaikai, direct LLVM backend, self-hosted (`docs/stage2-design.md`).

Bootstrap from any machine with `cc`:

```sh
cc stage0/*.c -o kaic0
./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -I stage0 -o kaic1
./kaic1 demos/fizzbuzz.kai -o fizzbuzz
./fizzbuzz
```

## Testing discipline

Three tiers, three cadences. Spec in `docs/testing-tiers.md`; CI in `.github/workflows/tier1.yml` (and `tier1-asan.yml`).

- **Tier 0** — pre-commit fast sanity (~30–60 s): `make tier0`. Run before each commit when iterating on compiler code; CI catches what local skips miss.
- **Tier 1** — gated by CI on every PR: `make tier0 && make tier1` runs on `ubuntu-latest` for every PR and every push to `main`. **CI green is the merge gate.** Optional locally for faster feedback (~2–4 min on mac).
- **Tier 1-ASAN** — path-gated CI on PRs touching `stage0/**`, `stage1/compiler.kai`, `stage2/compiler.kai`, `stage2/Makefile`, `stdlib/**`, `examples/effects/**`, or `examples/perceus/**`. Catches non-portable fixes that pass on macOS but fail on Linux.
- **Tier 2** — `make daily`: maintainer / cron only, ~10–20 min. Tier 1 + stress + coverage probe + RC budget. Failures are diagnostics, not blockers — `main` already gated by Tier 1.
- **Doc-only changes** (diff confined to `docs/`, root `*.md`, `LICENSE`) skip every tier locally AND in CI via `paths-ignore`. Code paths (`stage*/`, `stdlib/`, `examples/`, `demos/`, `.github/`, `Makefile`, `VERSION`, build scripts) always trigger tiers.

## Lane discipline

- **One worktree fixes one thing.** If you find a bug outside your lane, open a GitHub Issue with repro + hypothesis (label `regression` plus topic labels: `runtime`, `compiler`, `perceus`, `typer`, `stdlib`, `unboxing`, `refinements`). Do not fix it inline.
- **Working tree clean at every commit** — no uncommitted leftovers, no stashes tied to other lanes.
- **Regression-fixture discipline.** Every fix lane adds at least one fixture exercising the bug shape, wired into the right tier. Live in `examples/effects/` (effect-shaped), `examples/perceus/` (RC), `examples/sugars/` (sugar desugar), or the closest precedent. Negative fixtures get `.err.expected` goldens; positive fixtures get `.out.expected`. The closing PR must name the new fixture(s).
- **Lane handoff (push + PR is authorized).** Worktree-spawned agents (e.g. via `/wt-claude`) may push the lane branch and open a PR via `gh pr create` once the acceptance gate is green. Standing exception to the global "do not push without explicit ask" rule. **Merge stays with the integrator** — agents do NOT run `gh pr merge`. If CI fails, fix on the same branch and push again; force-push is fine for unreviewed lane branches.
- **Pending work lives in GitHub Issues.** Tracking docs were retired in PR #99; do not recreate them.
- **Lane retros are mandatory for non-trivial lanes.** Every lane that ships a non-trivial change (any architectural impact, any refactor, any new feature surface, anything that future lanes will reference) MUST produce `docs/lane-experience-issue-NNN.md` (or `lane-experience-<topic>.md` if no issue) BEFORE `gh pr create`. Bug fixes confined to a single file and doc-only PRs are exempt. The retro lives in the same commit as the implementation, not in a follow-up. Minimum contents: scope-as-planned vs scope-as-shipped, design decisions and alternatives considered, structural surprises the brief did not anticipate, fixtures added and coverage gaps, real cost vs estimate, follow-ups left for next lanes. ~50-150 lines is the target; longer if the lane was genuinely novel. The 91 retros in `docs/lane-experience-*.md` are the precedent; the bug-bash batch 2026-05-09→10 violated this rule (13+ merged PRs, zero retros) and surfaced the gap.

## Doc discipline

Catalog docs (`docs/stdlib-roadmap.md`, `docs/stdlib-layout.md`, `docs/effects-stdlib.md`, `docs/roadmap.md`) drift away from reality unless lanes update them at close. Issue #367 catalogued the last round of drift; this section exists to prevent the next round.

The closing PR for any feature lane MUST update:

- `docs/stdlib-layout.md` catalog entry — flip the operation's marker from `(planned: #N)` to `(shipped)`.
- `docs/stdlib-roadmap.md` *Current inventory* table — cite the closing PR number on the affected row.
- Any aspirational sentence in `docs/effects-stdlib.md` that the lane has now made factual — drop or trim the v1-status sidebar.
- The closing GitHub issue body if it lists out-of-scope work that the lane in fact covered.

Two further rules:

- **Issue numbers in roadmaps must verify.** Before citing `#N` in `docs/roadmap.md` (or any other doc), run `gh issue view N --json title` and confirm the title matches the citation context. Ghost references (#92, #120 in the pre-#367 Hanga Roa text) waste downstream readers' time.
- **Aspirational text gets a v1-status sidebar.** When a doc describes runtime behaviour that has not yet shipped (reactor, suspend, scheduler), pin a `> **v1 status (YYYY-MM-DD):** …` callout next to the aspirational paragraph stating what actually runs today and which lane closes the gap. The sidebar comes off when the lane closes.

Without this discipline, doc → reality drift recurs and downstream analysis (Linus / Eric strategic consults, new-contributor onboarding, agent prompts) draws wrong conclusions because the catalog lies. The audit triggered by #367 was launched after a strategic consultation reached a wrong recommendation because Phase 2 was documented as "not landed" while M1+M2+M3 were live in `stdlib/`.

### Primary docs vs. lane bitácora — what's authoritative

When reasoning about *current state* of the language, runtime, or stdlib, the **primary** sources are:

- `docs/design.md`, `docs/effects.md`, `docs/effects-stdlib.md`, `docs/effects-impl.md`, `docs/protocols.md`, `docs/structured-concurrency.md`, `docs/actors.md` — language semantics.
- `docs/fibers-honesty-targets.md`, `docs/perceus-honesty-targets.md` — what's shipped vs. deferred per honesty tier.
- `docs/stdlib-layout.md`, `docs/stdlib-roadmap.md` — current inventory.
- `docs/roadmap.md` — milestone state.

The **secondary / historical** sources are the lane retrospectives and audits:

- `docs/lane-experience-*.md` (~90 files) — per-lane retros: what was attempted, what reworked, what got proposed and didn't ship, what surprised the lane.
- `docs/lane-audit-*.md`, `docs/*-phase0-audit.md`, `docs/*-followups.md` — point-in-time audits, often pessimistic about features that have since shipped.
- `docs/lane-experience-*-disclaimer-sweep.md` — the sweeps that aligned other docs to reality.

These are **bitácora**: project diary, retrospective, work log. Use them for **meta-analysis** ("did this feature need rework?", "what was the original scope?", "what did the lane discover?") — **not** as source of truth for current behavior.

Common failure mode an agent should avoid:

- Reading a 4-week-old `lane-experience-X.md` that says "Phase Y not landed" and reporting Phase Y as missing, when the primary docs (catalog + honesty targets) show Phase Y shipped two weeks ago.
- Citing an audit doc's pessimistic finding ("RC is fictional") as if it described today's runtime, when the closing lane already shipped the fix.

Rule: when a lane retro / audit and a primary doc disagree, the primary doc wins. Lane retros may be stale; primary docs are the lane-close discipline's responsibility to keep current.

## Commit messages — Conventional Commits

`<type>(<scope>)?: <subject>`. Type drives changelog placement and version bump:

- `feat` → CHANGELOG "Added", MINOR bump (pre-1.0).
- `fix` → "Fixed", PATCH.
- `perf` / `refactor` → "Changed", PATCH.
- `docs`, `chore`, `ci`, `test`, `build` → excluded from changelog, no bump.

Domain areas (`typer`, `runtime`, `perceus`, `emit`, `tco`, `stdlib`, `demos`, `fmt`, `unbox`, `sigharness`, …) are **scopes**, not types: `fix(typer): correct row inference`, never `typer: correct row inference`. The legacy bare-domain-prefix style is invisible to cz.

**Breaking changes**: `feat(typer)!: drop X` plus `BREAKING CHANGE: <why>` footer. cz reads the footer to bump MINOR (pre-1.0) or MAJOR (post-1.0).

## VERSION + CHANGELOG (agent side)

Do **NOT** touch `CHANGELOG.md` or `VERSION`. Both are regenerated by `cz bump` at release time from commit messages.

## Integrator workflow (post-CI)

Once tier1 is green:

```sh
gh pr merge <N> --merge --subject "..." --body "..."
git pull --ff-only origin main
cz changelog --dry-run | head -40    # eyeball the section
cz bump --yes                         # writes VERSION + CHANGELOG.md + tag
git push origin main --follow-tags
```

cz reads `.cz.toml` (`change_type_map = { feat = "Added", fix = "Fixed", … }`, `major_version_zero = true`), walks commits since the most recent `v$version` tag, and bumps the version mechanically from commit types. `cz bump --files-only` writes the diff without committing if a hand-written narrative is wanted on top.

## Coverage + honesty

- **Coverage probe ratchet**: `tools/coverage-baseline.txt` is the current allowed gap count. New gaps fail the probe; closed gaps bump the baseline down. Add `<!-- coverage: skip -->` only for non-testable concerns (design rationale, references), not for forgotten fixtures.
- **Honesty targets**: runtime changes affecting fibers or Perceus update the relevant tier in `docs/fibers-honesty-targets.md` or `docs/perceus-honesty-targets.md`.

## Things to avoid

- **Do not go back to Go** for the compiler. The prior Go frontend was discarded on purpose.
- **Do not introduce a Rust-style borrow checker.** Perceus + fibers resolves memory at compile time without cognitive cost.
- **Do not add forms whose intent overlaps an existing one.** Two function bodies (short vs block) is fine; two pipes (apply vs map) is fine; a third way to do the same thing with no new intent is not.
- **Do not add dependencies to stage 0.** It must build on any system with an ANSI `cc`.
- **Do not design against WASM, Windows, or other post-MVP targets.**
- **Do not cite retired principles** in design arguments (see *Not principles*).
