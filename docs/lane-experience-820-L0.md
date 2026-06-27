# Lane experience — #820 L0 (foundational fixtures, ⛔ BARRIER)

## Scope as planned vs as shipped

**Planned (lane plan §B L0):** land the model-distinguishing fixtures
(three #789 repros + the two-instances flagship), a spawn-audit baseline,
and the dispatch-model honesty target — against today's compiler, touching
no compiler code. This lane is the barrier that gives L1–L6 their only real
oracle (selfhost byte-id is false-green for the whole effort).

**Shipped:** exactly that, with the repro states determined by measurement,
not assumption:

- `examples/effects/quarantine/collision_value_corruption.kai` — Repro 1.
- `examples/effects/collision_type_mismatch.kai` (+ `.err.expected`) — Repro 2.
- `examples/effects/quarantine/collision_segfault.kai` — Repro 3.
- `examples/effects/quarantine/two_instances_through_call.kai` — flagship.
- `examples/effects/m8_3_spawn_console.out.expected` + 5 sibling goldens — spawn-audit baseline.
- `docs/dispatch-honesty-targets.md` + one discovery line in CLAUDE.md.
- `tools/test-backend-parity.sh` — explicit `quarantine/` prune (documented).

No compiler code touched. byte-id is irrelevant here and was not chased.

## Each repro's measured today-behaviour and the state chosen

The repros are the three from #789's issue body **verbatim** (not
extrapolated). I first tried to construct them from the design's prose
(two distinct effects sharing an op name; a row variable absorbing a
phantom effect) and every such attempt either returned the *correct* value
or was *correctly rejected* by today's typer — because the by-name walk
keys on the **effect label** (`kai_evidence_lookup_node`,
`stage2/runtime.h`), so two distinct effects never collide, and the
row-variable cases surface a clean `effect not handled: E`. The collision
only bites in the exact shape #789 documents: a `var`'s `State[Int]` read
through a *handler clause* while a same-op-named `State[[Int]]` runner is
live, and the `Cell`/`State` shared-op-name absorption. Lesson reinforced:
**use the issue's repros, do not re-derive the bug from the design prose.**

Measured on **both** backends (C-direct and in-process libLLVM native),
`KAI_MAX_HEAP=4g timeout 60`:

| Repro | Measured today (C === native) | State chosen | Why |
|---|---|---|---|
| 1 — silent corruption | exit 0, prints `r=0` (correct: `r=10`) | **quarantine**, no golden | a `.out.expected` of `r=0` would golden-lock corruption |
| 2 — type mismatch | exit 1, `kai: type mismatch in -` | **`.err.expected`** + `# DOCUMENTS BROKEN BEHAVIOUR` header | aborts stably; the abort string is a stable golden |
| 3 — segfault | exit 139, no stable output | **quarantine**, no golden | a segfault has no stable golden |
| flagship | exit 1, `method `set` not found for type `Cell`` (compile reject) | **quarantine**, L3-TARGET in comment | capability-as-parameter op-call surface (§6.2) does not exist yet; dies at the call boundary as the design predicts |

C and native produced **identical** behaviour for all four — the dispatch
drift is not backend-specific, confirming design §1's claim that the model
(not a codegen bug) is the root.

## How the quarantine glob exclusion works

The parity harness (`tools/test-backend-parity.sh`) collects entry points
with two finds per dir: `find -maxdepth 1 -name '*.kai'` (flat shape) and
`find -mindepth 2 -name 'main.kai'` (package shape). A `quarantine/`
subdir holding files **not** named `main.kai` is therefore already excluded
by construction: `-maxdepth 1` never descends into it, and the package walk
only matches `main.kai`. I verified this empirically (the quarantine files
do not appear in the collected entry-point set).

To make the exclusion **intentional rather than accidental** — so a future
`quarantine/main.kai` cannot leak in — I added `-not -path '*/quarantine/*'`
to the package walk and a comment stating why. The flat fixture
`collision_type_mismatch.kai` *is* collected as an entry point, but the
harness's own `is_skipped()` skips any fixture with a sibling
`.err.expected` (negative-by-design), so it never enters the parity diff.

Other harnesses confirmed clear:
- `coverage-probe.sh` globs `examples/effects/*.kai` (no recursion) → quarantine excluded; new fixtures can only *close* coverage gaps, never open them.
- `test-negative.sh` walks only `examples/negative/**` → does not touch these.
- `test-effects` (stage2/Makefile) enumerates fixtures by explicit name and asserts output with inline `grep -q`, not against `.out.expected` → the 6 spawn-audit goldens are inert for it (which is the point — they are L3's before-picture) and consistent with what it already asserts.

## The honesty-target wording

The load-bearing row is the lane-plan's verbatim sentence, kept on a single
greppable line:

> Dispatch model: dynamic-scoping-by-name (NOT the capability-passing the
> docs claim) — accidental equivalence holds only while each effect has ≤1
> live handler.

The table mirrors `fibers-honesty-targets.md`: *Shipped* / *Accidental
(works only because ≤1 live handler)* / *Broken (#789)* / *Broken (#820
flagship)* / *Deferred*. I split "Broken" into the #789 collision row and
the flagship capability-through-call row because they fail for different
reasons (op-name collision vs. capability-as-parameter not existing) and
flip in different lanes (L2 vs L3). The discovery line lives in CLAUDE.md's
primary-docs list next to the fibers/perceus honesty targets.

## Structural surprises the brief did not anticipate

1. **The design-prose repros do not reproduce; the issue-body repros do.**
   The brief said "construct these repros — base them on #789's three
   cases" and "do not invent the behaviour — measure it." Measuring is what
   caught that my first constructions were wrong. The authoritative source
   turned out to be `gh issue view 789` (the verbatim programs), not the
   design doc's paraphrase.
2. **The wrapper auto-rebuilds C-only.** `bin/kai` silently rebuilds a
   C-only `kaic2` if the binary is missing or stale, which repeatedly
   clobbered the native build. The fix is to keep `llvm-config` on PATH for
   every invocation and never `make clean` mid-measurement. `make
   KAI_LLVM=1 kaic2` from the root says "up to date" when a C-only kaic2
   already exists — only `make -C stage2 clean && make -C stage2 KAI_LLVM=1
   kaic2` forces the native relink.
3. **No exclusion mechanism was actually needed** — the harness's existing
   find shape already excludes a non-`main.kai` quarantine subdir. The added
   prune is belt-and-suspenders, not a fix for a real leak.

## Fixtures added and coverage gaps

Added: 4 fixtures (3 repros + flagship), 1 `.err.expected`, 6 spawn-audit
goldens, 1 honesty doc, 1 CLAUDE.md line, 1 harness prune. The coverage gap
this lane *cannot* close: the corpus still has **no** fixture that performs
a **user** (non-builtin) effect inside a fiber against an **outside** user
handle — escape-vector-4, the exact pattern L3's spawn audit migrates. Every
existing spawn fixture performs a builtin effect (Console/Spawn/Actor, which
have runtime defaults and do not inherit parent evidence). This absence is
itself the §1 false-green made concrete and is flagged for L3 to construct
the missing case rather than assume the baseline covers it.

## Follow-ups for next lanes

- **L1** must validate `--dump=evidence-obligations` against these fixtures —
  the obligation dump for the collision fixtures must show the collision (an
  obligation with no supplier), or L1 is blind (lane plan §C).
- **L2** flips `collision_*` from quarantine/`.err.expected` to the real
  op-name-collision diagnostic as `.err.expected`.
- **L3** flips `two_instances_through_call.kai` out of quarantine to its
  `.out.expected` (`3`) and constructs the missing user-effect-in-fiber
  spawn-audit case noted above.
- **L6-docs** collapses the honesty table to a single *Shipped:
  capability-passing* row.
