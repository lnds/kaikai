# Testing tiers

Decision pinned 2026-04-29 (post fibers + perceus honesty-targets).
Three tiers of test coverage with explicit cadence: per-commit, per-PR,
and daily. The point is that **velocity at the lane level (parallel
agents in worktrees, tag at lane close, semver per release) does not
require running every test on every commit — it requires being honest
about which tier ran and when**.

Sister docs:
- `docs/fibers-honesty-targets.md` — what "fibers as advertised" means.
- `docs/perceus-honesty-targets.md` — what "RC as advertised" means.
- This file — what "tested as advertised" means.

## The problem

Today's session uncovered three preexisting bugs whose commit messages
all claimed `make test clean`:

- R2 — `m8x_2_yield_interleave` SIGSEGV from v0.4.0 (R2 scheduler land).
- R3 — `interp.kai` panic from `9fe6f6d` (match-scrutinee leak plug).
- R4 — `let _ = fiber_spawn(...)` deadlocks (never had a fixture).

The pattern: **the gate the commit message named was not the gate that
ran**, or the fixture for the case did not exist. `make selfhost` was
honest in its day (then a byte-identical fixed point across stage 1
↔ stage 2; today narrowed to per-compiler determinism — see
`docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md`);
`make test` was honest in name but partial in practice;
`make demos-no-regression` was honest about its baseline but the
baseline did not include the regressions.

The fix is not "run more tests per commit" — that murders velocity
when 510 commits land in 10 days. The fix is to **name three tiers
explicitly, run each at its own cadence, and require the commit /
PR description to declare which tier ran**.

## Tier 0 — pre-commit (every agent, every commit)

~30-60 seconds. Runs inside each worktree, before any commit.

```
make selfhost              # per-compiler determinism (C + LLVM)
make demos-no-regression   # baseline N
```

If Tier 0 fails, the commit does not happen. This is what keeps `main`
unbroken across parallel lanes — every commit lands on a green
self-host with the demo baseline holding. `make selfhost` checks that
`kaic2b` and `kaic2c` (stage 2 compiled by itself, twice) are
byte-identical; stage 1's output (`kaic2a.c`) is not required to match
stage 2's (see `docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md`).

**Disciplina**: agents never push commits that fail Tier 0. The
3-level gate's "Level 1 mechanical" maps onto Tier 0.

## Tier 1 — pre-PR (gate before merge)

~2-4 minutes. Runs once per PR before opening / before merge.

```
make test    # full target — see stage2/Makefile §test
```

Includes test-tokens / test-ast / test-types / test-env / test-infer /
test-infer-check / test-dump-typed / test-dump-mono / test-check /
test-run / test-blocks / test-llvm / test-modules / test-modules-path /
test-modules-qualified / test-modules-qualified-neg / test-holes /
test-effects / test-effect-runtime / test-link-runtime / test-sugars /
test-loop / test-reader / test-writer / test-time / test-intervals /
test-stdlib / test-protocols / test-demos-core / test-aspirational
+ test-m4c.

**Disciplina**: PR description includes the trailing line of `make
test` output (or a link to a CI run on the PR branch). The reviewer
verifies it before merging. **Without that line, the merge does not
happen** — "make test clean" as prose is not the same as
`make test exit 0` as fact.

The 3-level gate's "Level 2 invariant verifier + audit" and most of
"Level 3 demo gate" map onto Tier 1.

## Negative-space discipline (issue #511)

Positive tests assert "this valid program compiles and runs". They do
NOT assert "this invalid program is rejected with the expected
diagnostic". That blind spot let `pub` go silently unenforced for the
full life of the language (issue #510): the contract was advertised
in `docs/design.md` and 672 `pub` lines across `stdlib/` assumed
otherwise, but no test ever asserted "a non-`pub` decl called across
modules is rejected", so the gap survived three years.

**Every contract advertised in primary docs must have a negative
fixture that asserts the rejection.** Without that, the contract is
not load-bearing — it's an aspiration the compiler may or may not
have implemented.

Negative fixtures live under `examples/negative/<category>/` and are
gated by `tools/test-negative.sh`, wired into Tier 1 via the
`test-negative` target. Three modes are supported:

- **Compile-time**: `<name>.kai` + `<name>.err.expected`. The harness
  runs `kaic2 <name>.kai`, asserts non-zero exit, and greps stderr
  for the first line of the golden — same convention as the existing
  `test-modules-qualified-neg` target. Multi-file fixtures use
  `<dir>/main.kai` + `<dir>/lib.kai` + `<dir>/main.err.expected`.
- **Stage 1 rejection**: `<name>.kai` + `<name>.kaic1.err.expected`
  routes to `stage1/kaic1` instead of `kaic2` (used for stage-2-only
  features like `protocol`, `impl`, `effect`).
- **Runtime-time**: `<name>.kai` + `<name>.run.err.expected`. The
  harness compiles via kaic2 → cc, runs the binary, asserts non-zero
  exit, and greps stderr for the panic message. Used for contracts
  that `docs/effects-impl.md` declares runtime-only (one-shot resume
  is the prototype).

Two optional siblings:

- `<name>.core.kai` — implicit `--core` invocation.
- `<name>.flags` — extra CLI args (e.g. `--core stdlib/protocols.kai`
  for derive fixtures, `--path stdlib` for actor fixtures).

When a fixture is authored and the language SILENTLY ACCEPTS the
prohibited construct, the audit has succeeded — the language has a
silent contract. The fixture moves to
`examples/negative/silent_contract/` (excluded from the harness via
path filter), the lane files a `bug,pre-1.0` issue, and
`silent_contract/README.md` gets a row linking the fixture to the
issue. When the issue closes, the closing lane moves the fixture
back out and adds the `.err.expected` golden.

The #511 audit surfaced 14 silent-contract fixtures clustered into
3 issues (#516, #517, #518) — see
`docs/lane-experience-issue-511-negative-tests.md` for the matrix.

## Backend-parity discipline (issue #575)

Positive tests assert "this program compiles and runs on the C
backend" (or, in a hard-coded handful, on both). They do NOT assert
that *every* fixture compiles + runs identically on both backends.
That blind spot let five LLVM-only failures ship to main in two
weeks (#513, #522, #524, #570, #571), each discovered by a
downstream consumer (notably ahu) rather than by CI.

The C backend is the spec-by-construction (it is the bootstrap
path; it must work). The LLVM backend is the production target.
They must agree on every program kaikai can express. When they
disagree, that is a backend bug, not a language ambiguity.

`tools/test-backend-parity.sh` walks every entry-point fixture
under:

- `examples/effects/`, `examples/actors/`, `examples/spawn/`,
  `examples/perceus/`, `examples/refinements/`, `examples/llvm/`,
  `examples/packages/`, `examples/minimal/`, `examples/quickstart/`,
  `examples/stdlib/`, `examples/attributes/`, `examples/unstable/`
- `demos/`

For each fixture, the harness runs `KAI_BACKEND=c kai build …`,
`KAI_BACKEND=llvm kai build …`, executes both binaries (with a
30-second per-binary timeout), and diffs stdout + exit code. Any
divergence is a failure. `examples/negative/` is intentionally
excluded — those fixtures must reject at compile time, so
backend-parity (which assumes both build cleanly) does not apply.

Entry-point detection: kaikai fixtures live in two shapes —
**flat** (`<dir>/<name>.kai` is a standalone program) and
**package** (`<dir>/<pkg>/main.kai` is the entry-point; sibling
files are libraries loaded by main). The harness walks `*.kai`
directly under each listed dir for the flat shape and `**/main.kai`
for the package shape. Library files are never compiled standalone
— they would always C-FAIL at link time (no `kai_main` symbol) and
that's not a backend-parity concern.

The harness is wired into CI as the `tier1-backend-parity` job,
which runs on the same `paths` filter as `tier1-asan` (any change
to `stage*/`, `stdlib/`, `examples/`, `demos/`, `bin/kai`, the
script itself, or the workflow). Locally it runs as
`tools/test-backend-parity.sh` from the repo root after `make all`.

The `make tier1-backend-parity` convenience target rebuilds `kaic2`
through `tools/parity-preserve-native.sh` rather than as a plain
prerequisite. `stage2/Makefile` resolves an unset `KAI_LLVM` from
`llvm-config` on PATH, and a keg-only LLVM (Homebrew's default) is
off PATH — so a plain prerequisite relinks an existing *native*
`kaic2` as C-only, after which the harness probes native, finds it
gone, and exits 0 with SKIP: a green that verified nothing, over a
capability the same invocation destroyed. The wrapper probes the
tree's backend capability *before* the rebuild and never lowers it:
a native tree rebuilds with `KAI_LLVM=1` (resolving a keg-only
`llvm-config` via `brew --prefix`) or stops loud; a C-only tree
rebuilds C-only and reaches the intended SKIP; and `KAI_LLVM=1`
that does not yield native fails rather than reporting SKIP over an
explicit request. `make test-parity-preserve-native` (tier 0,
hermetic) locks all four.

**Skip discipline.** Two skip mechanisms, used differently:

- `tools/backend-parity-skips.txt` — one line per fixture:
  `<relative-path>:<issue-number>:<one-line-reason>`. Every entry
  must reference an *open* tracking issue. The skip is the
  bookmark; the issue is the work. When the issue closes, the
  closing lane removes the line and re-runs the harness to confirm
  parity.
- Inline annotation — a fixture whose first line contains
  `// skip-backend-parity: <reason>` is skipped silently. Reserved
  for fixtures that are *intentionally* backend-specific by design
  (e.g. a feature only one backend implements, with no expectation
  the other ever will).

**What to do when this gate fails.** The failure output names the
fixture and the divergence (build-failure on one backend, exit-code
mismatch, or stdout diff). Two valid responses:

1. **Backend bug.** The divergence is unintended — fix the
   offending backend (usually LLVM, since the C backend is the
   spec). The fix is in scope for the same PR if small; otherwise
   file a tracking issue and add the fixture to the skips file in
   the same PR that introduced it.
2. **Intentional divergence.** The fixture exercises a
   backend-specific feature. Add the inline `// skip-backend-parity`
   annotation with a reason. This is rare — the language is
   designed for the two backends to be observationally equivalent.

Adding a new fixture to any of the walked dirs implicitly opts it
into this gate. New fixtures that fail on one backend but not the
other should not merge until the divergence is resolved or
explicitly skipped with a tracking issue.

## M:N corpus-determinism discipline (issue #1207)

Backend parity asks whether two backends agree. This gate asks a
different question of the same corpus: whether **one** backend agrees
with **itself** when the scheduler is given more than one thread.

Nothing asked it before. The `KAI_THREADS` default was 1, so every
gate in the repo measured the single-threaded scheduler; the M:N
coverage was ~9 hand-authored `mn_*` stress fixtures, none of which
asks whether an ordinary program prints the same thing at N=4 that it
prints at N=1. The consequence was self-referential: shipping the
multi-threaded default was blocked on "no known M:N bug", and the only
thing discovering M:N bugs was the PR raising the default. The blast
radius was never confined to scheduler code either — at
`KAI_THREADS>1` `main` itself becomes a fiber, so allocator and
scheduler paths change for **every** program.

`tools/run-mn-corpus-determinism.sh` walks the same entry-point corpus
as backend parity (shared walk: `tools/lib/corpus.sh`) on both
backends. Per fixture it takes the `KAI_THREADS=1` run as the
reference, then runs at each higher thread count and requires the
stdout **and** the exit code to reproduce it.

**Six buckets, never collapsed.** A hang, an empty output, an
exit-code mismatch and a stdout divergence are four different bugs;
merging them into "failed" cost an earlier lane three useless rounds.

| bucket | meaning |
|---|---|
| `ok` | stdout and exit code reproduce the N=1 reference |
| `hang` | the bounded run hit its deadline (124/137) |
| `crash` | exit code differs from the reference (SIGSEGV, trap, abort) |
| `empty` | exit code matched, stdout empty, reference non-empty |
| `diverge` | exit code matched, stdout non-empty, contents differ |
| `reorder` | same lines as the reference, different order, on a fixture that declares its interleaving unspecified |

**`diverge` and `empty` clear two adjudications before they are believed.**
First N=1 self-stability: a fixture whose own N=1 output varies cannot
witness anything about thread count. Then reproduction — the divergence must
reappear on at least one of N further runs at the same thread count. That
second check exists because `Stdout.print` is not line-atomic across
scheduler threads (#1388), so any multi-fiber program can occasionally emit
a torn line; it tears a different fixture every run, and a byte-exact gate
that believed each one would be red at random. A held flake is still
announced loudly, with a CI annotation. `crash` and `hang` clear neither
adjudication: re-running a one-in-many SIGSEGV until it behaves is how a
gate learns to lie.

**Two observation conditions are load-bearing.** stdout goes to a
FILE, never a pipe — a pipe (like a tty) changes the buffering enough
to close the window on a whole family of races. And every run is
bounded via `tools/lib/timeout.sh`, so a wedged scheduler is counted
as a hang instead of being absorbed into the job ceiling, where it
reads as a cancelled run.

**Order-dependence, declared not assumed.** A demo that prints from
two fibers has no specified line *order*: at N=1 the round-robin
scheduler alternates, at N=4 the fibers really run at once. Such a
fixture is listed in `tools/mn-corpus-order-dependent.txt` with a
reason, and the gate then compares the **multiset** of its output
lines instead of the sequence. Every line must still appear exactly
once, so lost work, a duplicated message and a torn line all remain
findings. The file forgives interleaving and nothing else; a fixture
that prints a *result* does not belong in it, and neither does one
that gains or loses a line — that is a real divergence, fixed in the
runtime or by making the fixture's delivery point deterministic, not
declared here. Every entry names an open issue — the declaration is
the bookmark, the issue is the work, and these goldens also break
tier1's own byte-for-byte comparison once `KAI_THREADS` defaults to
ncpu (#1390).

**Known findings ratchet.** `tools/mn-corpus-baseline.txt` holds the
(fixture, backend) pairs that were already broken above one thread
when the gate first walked the corpus, each line naming an open
issue. A pair failing that is not listed fails the gate; the queue
may only shrink. These defects are timing- and platform-dependent —
a pair reproduces on one runner and not another — so the file is the
union over the platforms measured, and a quiet run is not evidence
an entry is fixed.

**What the gate refuses to judge, it names.** Fixtures that fail to
build, fixtures whose N=1 reference does not complete, and fixtures
whose N=1 output varies between runs are each listed by name in the
summary rather than folded into the pass. The walk also checks its own
arithmetic: if it did not reach every fixture on every arm it fails
outright, because a truncated walk otherwise produces a tidy and
entirely false "no findings".

**Where it runs.** `.github/workflows/tier1-mn-corpus.yml`, always-on
(a path filter would encode the false assumption that the blast radius
is local), sharded per `tools/mn-corpus-shards.txt`. The shard split is
asserted to partition the corpus before any fixture runs — a shard list
that quietly stopped naming a directory would leave the gate green over
a smaller corpus. PR path: N=4, 2 repeats. Nightly cron and manual
dispatch: N in {2,4,8}, 3 repeats. Locally: `make test-mn-corpus`,
scoped with `MN_CORPUS_DIRS` while iterating.

## Formatter meaning-preservation discipline (issue #1605)

`kai fmt` had two nets and neither checked the property that matters.
`tests/fmt_fixtures.sh` compares 54 fixtures against goldens — it covers
what somebody thought to write down. `tests/fmt_selfhost.sh` requires
exit 0 and byte-identical idempotency over `stdlib/` + `stage2/compiler/`
— which sounds total but only ever sees **code that already exists in
the repo**. A surface form no repo file happens to use is invisible to
both. Six writer bugs shipped through that gap in a single week
(#1597, #1599, #1600, #1601, #1602, #1603); three of them left the file
**unparseable**, after a rewrite that is in-place, silent, and inherited
by `kai migrate` through the shared writer (`kai migrate` re-emits via
`fmt_program`, so a package with nothing to migrate takes pure damage).

The first run of the property gate found four more the two older nets
could not see, because all four produce output that **parses**: #1606
(a `requires`/`ensures` clause erased from the signature into a body
`assert`, so the compiler stops checking the contract at call sites —
a compile-time error becomes a runtime failure), #1607 (hex and binary
literals re-rendered as decimal: `0xFFFFFFFFFFFFFFFF` comes back as
`-1`), #1608 (`3i` emitted as its `complex.mk` desugar) and #1610 (a
`~r/.../` regex literal emitted as a malformed desugar). The last two
produce files that no longer compile.

The gate then caught the fix for those: #1614 restored the `requires`
clause but emitted it on the signature line ahead of a block body, a
shape the parser rejects, so eleven `examples/refinements/` files went
from failing (c) to failing (b) — #1617. That is the loop working as
intended: the net moves a silent corruption into a loud one, and then
holds the fix to the same standard.

The missing assertion is the central property of a formatter: **`fmt`
preserves meaning**. `tests/fmt_property.sh` (target `test-fmt-property`)
asserts four things per source:

- **(a) exit 0** — fmt does not refuse the file.
- **(b) re-parse** — the output is still valid kaikai. Catches a writer
  emitting an internal encoding (`s#Shape`, `...__list_rest_10_10__`).
- **(c) AST equality** — `parse(fmt(src))` equals `parse(src)` modulo
  source position. Catches output that parses but denotes something
  else.
- **(d) idempotency** — `fmt(fmt(x)) == fmt(x)`.

**The corpus is `examples/**`, and that is the whole point.** It is
~1780 files against the selfhost corpus's 214, and more importantly it
exercises surface the compiler's own sources never use — rest-patterns,
kind-annotated type parameters, record literals delimited in condition
position. That is where the six bugs lived. A run covers 1573 files;
193 are dropped because the parser rejects them (negative fixtures,
by design) and the rest are excused against an open issue.

Cost is four `kaic2` invocations per file, ~18 minutes serial, so the
script fans out over `$(nproc)` workers (`FMT_PROPERTY_JOBS` overrides) —
the work is per-file independent. Even fanned out it costs minutes, so it
rides `tier1-shard-6` rather than shard-1 with its sibling fmt gates:
shard-1 was already the long pole at ~30 minutes and adding this to it
hit the job's ceiling, cancelling the shard. Shard-6 ran in well under a
minute. Note
the harness passes `--path stdlib`: without it most of the corpus fails
to resolve its imports and is silently dropped as unparseable, which
costs about a quarter of the coverage.

(c) compares `kaic2 --ast` output with `@line:col` suffixes stripped;
normalising position is mandatory, since reformatting moves every token
and an unnormalised dump would differ for every file. The dumper is
structural and exhaustive over Expr/Pattern/TypeExpr/Decl with no
catch-all arm, but it is a **proxy**, not structural equality on the AST
type, and it drops three slots: the `TyRefine` where-predicate, the
`DDoc` text, and lambda parameter annotations. Comments are not in the
AST at all, so (c) cannot see a comment re-attached to the wrong element
(#1597) except when the move also perturbs structure. **This is why the
property gate does not retire the golden fixtures** — goldens remain the
only net over comment placement and layout, and the two are complements.

Exception lists inside the script are keyed by issue number and waive
**one property for one file**; an excused file must still satisfy the
other three, so an exception cannot mask a total refusal. A file that
stops failing while still listed fails the run as a stale exception —
that is what forces the list to empty instead of rot. **The lists must
reach zero**, exactly like the skip-list in `tests/fmt_selfhost.sh`.

## Tier 2 — `make daily` (end of day / cron)

~10-20 minutes. Runs once a day on `main` HEAD, not on each PR.

```
make tier1
make stress-fixtures      # see "Stress fixtures" below
make coverage-probe       # see "Coverage probe" below
make rc-budget            # leaked / RSS / wall vs baseline
make demos-extended       # demos/9d9l + demos/vs + aspirational rerun
```

If Tier 2 fails, **`main` is not broken** — Tier 0 / Tier 1 already
gated every commit. Tier 2 produces a diagnostic that opens a lane
the next morning. Velocity is preserved.

The 3-level gate's "Level 3 programa no contemplado" maps onto Tier 2:
fixtures that were not anticipated when a feature shipped, exercised
nightly to surface latent regressions.

## Stress fixtures

Live under `examples/effects/` and `examples/stress/` (the latter
created on demand). They cover patterns that the per-feature suite
does not exercise but that REAL programs hit:

- **Concurrency**: `large_concurrent_drain.kai` (100 fibers + mailboxes
  + leak budget assertion).
- **Refcount**: `rc_leak_budget.kai` (programmatic check that
  `KAI_TRACE_RC` reports leaked < N for a known-shape program).
- **Polymorphic flow-through**: `m4c_flow_through.kai`
  (`fn f[a](x) = g(x)` chain that exercises body type substitution
  after Phase 3 m4c lands).
- **Negative regressions**: `m8_fiber_discard.kai`, `interp_recursive_walk.kai`
  (each documents a closed regression with an executable repro that
  panics if the fix regresses).

A stress fixture is graduated to Tier 1 once its bug is fixed and the
fixture stops being aspirational.

## Coverage probe

A script under `tools/coverage-probe.sh` that walks every `## ` heading
in the runtime / language design docs (`docs/effects.md`, `docs/
structured-concurrency.md`, `docs/actors.md`, `docs/fibers-impl.md`,
`docs/effects-impl.md`, `docs/effects-stdlib.md`) and matches each
heading against `examples/effects/*.kai` looking for a fixture whose
prefix or comment names the section. Headings without a fixture are
reported as **coverage gaps**.

This is preventive: when a doc grows a new section but no fixture
follows, the probe catches it before someone hits the bug at runtime.
A heading is allowed to lack a fixture only if it explicitly carries
`<!-- coverage: skip -->` (e.g. design discussion, not a feature).

## RC budget probe

A script that runs `KAI_TRACE_RC=1 stage2/kaic2 stage2/compiler.kai
2>&1 | grep alloc_total` and compares the numbers against
`docs/perceus-honesty-targets.md` Tier 2 targets. If `leaked` rises
above the threshold (today: 50 M; goal: 5 M post Tier 2), alarm.

The threshold is updated when Perceus Tier 2 sub-items land. Today the
threshold is "no regression from current 46.9 M"; once perceus_pass
multi-read dup ships, threshold drops to ~32 M; etc.

## Cadence summary

| Tier | When it runs | Owner | Gate |
|---|---|---|---|
| 0 | Every commit | The committing agent / human | Commit blocked |
| 1 | Pre-PR (and pre-merge if rebased) | The PR opener | Merge blocked |
| 2 | End of day / cron | The maintainer | Diagnostic opens a lane next AM |
| (Pre-release) | Before tag bump to a *.0 | Release captain | Release blocked |

Pre-release adds nothing new yet — Tier 2 is the de-facto pre-release
gate until 1.0 puts a stricter floor in place.

## What this document is NOT

- Not a CI configuration. The cadences above are the contract; CI
  (`gh actions` etc.) implements them mechanically when the project
  publishes a public repo. Today the work is done locally.
- Not a list of all `make` targets — only the ones that map to a
  tier.
- Not an excuse to skip Tier 1 on a PR because "the change is small".
  The R2 / R3 regressions that surfaced today were each a 30-line
  diff; both passed Tier 0 but broke Tier 1 silently because Tier 1
  was not run.
