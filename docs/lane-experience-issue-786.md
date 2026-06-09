# Lane experience — issue #786: `kai fmt` self-hosting ratchet (Tier 1 gate)

**Branch:** `fmt-ratchet-786`
**Scope shipped:** a Tier 1 gate that formats every `stdlib/**/*.kai` +
`stage2/compiler/*.kai` and asserts `kai fmt` exits 0 (no refusal) and is
byte-identically idempotent (`fmt(fmt(file)) == fmt(file)`). Empty skip-list.

## What this lane is — and what it is deliberately not

This is a *ratchet*, not a feature. #784 already made `kai fmt` round-trip the
full language surface across the whole corpus, and #788 fixed #785 (the last
trailing-comment R7 edge). The coverage exists; this lane's only job is to
**lock it in so it cannot silently regress**. There is no new formatter code —
the deliverable is a test harness + its CI wiring.

The original #781 phase plan (Phase 2 effects/handlers, Phase 3
protocols/impls/derive, Phase 4 statements/units/refinements) was premised on
those constructs being unformattable. They are not — the premise came from a
stale `fmt.kai` scope header, as the #781 retro documents. So "implement the
phases" was the wrong frame; "gate the coverage that already shipped" is the
right one, which is exactly what #786 asks for.

## Scope as planned vs. as shipped

| Planned (issue / brief)                              | Shipped                                          |
|------------------------------------------------------|--------------------------------------------------|
| Make target: fmt every corpus file, check idempotent | ✅ `tests/fmt_selfhost.sh` + `test-fmt-selfhost`  |
| Wire to Tier 1                                        | ✅ added to `tier1:` chain (CI runs `make tier1`) |
| Skip-list, should start empty                         | ✅ empty `SKIP=""`; file-by-file verified         |
| Retro in the implementation commit                    | ✅ this doc                                       |

## The corpus is 100% clean today — empty skip-list, verified file-by-file

Brief instruction: verify each file; only add to the skip-list what is genuinely
non-idempotent, with an issue reference. I ran the idempotency check over all
101 files (`find stdlib -name '*.kai'` = 56 + `stage2/compiler/*.kai` = 45)
before writing the harness:

- **0 refusals** — every file exits 0 under `--fmt`.
- **0 non-idempotent** — `fmt(fmt(f))` is byte-identical to `fmt(f)` everywhere.

So the skip-list ships **empty**, which is the issue's acceptance target ("CI
gate green with an empty (or single, documented) skip-list"). #785's
trailing-comment edge, which the issue anticipated needing a skip entry, was
already fixed in #788 — there is nothing to skip.

## Count drift: the issue says 27 compiler files, today there are 45

The issue text (and #784) cite `stage2/compiler/` as 27 files. The #781 split
plus the KIR lanes (#787/#790/#791/#792/#794/#795) grew it to 45. The gate uses
`find` over the live tree, not a frozen list, so it auto-tracks new files —
exactly what a ratchet wants: a new compiler module that is not idempotent fails
the gate the moment it lands, rather than slipping through a stale enumeration.

## Design decision 1 — `--fmt` is stdout, not in-place, so no source-copy dance

The lane brief (and the harness-safety memory) warned that `kai fmt` is
destructive in-place and that the corpus must be copied to a namespaced temp dir
under `build/`. I checked `driver.kai:1402` and the `--help` text first: `--fmt`
**pretty-prints to stdout** — it does not mutate the file it reads. (The
in-place destructiveness is a property of `kai fmt <dir>` as a user command, not
of the `--fmt` flag the compiler exposes.) That collapses the safety problem:
the harness only ever *reads* the real sources and redirects stdout into a
private `mktemp -d`. The real tree is never touched. This mirrors the existing
`tests/fmt_fixtures.sh` precedent exactly, so there is no new safety surface.

## Design decision 2 — per-file counter keys defeat the `make -j` race

The memory on parallel-Make races (`make -j` runs sibling targets concurrently;
shared `build/` paths collide) applies to *fixed* scratch paths. The harness
sidesteps it two ways: (1) the whole run gets one `mktemp -d` (so two concurrent
invocations of the script never share a directory), and (2) within a run each
file's artefacts are keyed by an incrementing counter (`$tmp/$n.p1`), so even a
future refactor that parallelises the inner loop stays collision-free. I
stress-tested 5 concurrent runs of the script — all 5 reported `101 passed, 0
failed`, confirming the `mktemp` isolation holds.

## Why a separate target instead of folding into `test-fmt`

`test-fmt` (`fmt_fixtures.sh`) tests the *canonical-output* contract:
`fmt(input) == expected.kai` against hand-written goldens in `examples/fmt/`,
plus a round-trip sanity sweep on a small example corpus. #786 tests a different
invariant — *idempotency + no-refusal over the production corpus* — with no
goldens (the corpus files are their own expected output after the first pass).
Different contract, different corpus, different failure story, so it is a
sibling target with its own name in the `tier1:` chain, not a bolt-on to the
fixtures script. The two are complementary: fixtures pin *what canonical looks
like* on curated cases; the ratchet pins *that the real tree stays stable*.

## Fixtures / coverage

No `examples/` fixture is added — the "fixture" for this gate **is the live
corpus** (101 files), which is the strongest possible regression surface for the
invariant. A synthetic fixture would be strictly weaker. The gate itself is the
coverage artefact; it lives in `tests/fmt_selfhost.sh` and runs on every PR via
`make tier1`.

## Real cost vs. estimate

Small and mechanical, as expected for a ratchet. The only investigation was
confirming the corpus is genuinely clean (so the skip-list is empty) and
confirming `--fmt` is stdout (so no source-copy is needed). Both took one
empirical pass each. No formatter code changed.

## Follow-ups left for next lanes

- **None blocking.** The skip-list is empty and the issue's acceptance is met.
- If a future lane reintroduces a non-idempotent construct, the gate will fail;
  the correct response is to fix the formatter (separate lane) or, if the fix is
  genuinely deferred, add the file to `SKIP` with a `# refs #N` note pointing at
  a tracking issue — never to weaken the gate.
- The `kai fmt <dir>` *user-facing* in-place writer is untested here (this gate
  exercises the `--fmt` stdout path only); a `--fmt-check`-based CI guard on the
  user command could be a later increment, but is out of scope for #786.
