# Phase 0 audit — m14 bootstrap split + stdlib qualified-call migration (issue #203)

Status: **audit complete** (2026-05-03). Doc-only lane.

This audit informs Phase 1+ of issue #203 — the m14 milestone
that migrates kaikai's stdlib from flat-prefix names
(`list_map`, `string_concat`, `opt_unwrap_or`) to module-relative
names (`list.map`, `string.concat`, `option.unwrap_or`) per the
schedule pinned in `docs/stdlib-layout.md` (lines 80–83, 406–415).

The issue body proposes a **bootstrap split** (option D): a new
`stdlib_minimal/` consumed by stage 1 plus a `stage2-bootstrap`
intermediate compiler, on the premise that stage 1 cannot parse
qualified calls and currently consumes 205+ flat-prefix stdlib
references.

The audit walks the actual code to test that premise. Section §1
quantifies stage-by-stage stdlib consumption; §2 counts the
flat-name surface; §3 surveys options A–D plus a fifth option that
falls out of the empirical findings; §4 sketches the per-PR
selfhost strategy; §5 inventories risk; §6 names the recommended
verdict.

Precedents: `docs/perceus-anga-roa-phase0-audit.md` (PR #204),
`docs/unions-phase0-audit.md` (PR #189). Same shape adapted to
the bootstrap-chain / namespace-migration domain.

---

## §1 — stdlib usage by stage

### Method

For each stage, walked the source verbatim and checked: (a) what
its build invocation actually loads as preludes, (b) what
flat-prefix names it references, (c) whether each referenced name
is defined inside the source itself, baked into stage 0's prelude
table, or genuinely a stdlib call.

The flat-prefix name set was constructed from `stdlib/core/*.kai`
public exports — 117 `pub fn` declarations, of which 69 carry the
deprecated flat prefix (`string_*`, `list_*`, `opt_*`,
`result_*`, `ch_*`). The remaining 48 already use
module-relative names (`length`, `reverse`, `map`, `filter`, …);
`stdlib/core/list.kai` is partway migrated and exports both
forms today.

Separately, **stage 0's PRELUDE table** in `stage0/emit.c:425–467`
hardcodes 37 names that the stage 0 C compiler lowers directly to
`kai_prelude_*` runtime calls — `print`, `string_concat`,
`string_length`, `string_slice`, `char_at`, `char_to_int`,
`list_length`, `list_append`, `list_reverse`, `string_to_int`,
etc. **These are not stdlib calls** in any meaningful sense:
they're built-in operators with prefix-y names, baked into the
bootstrap C compiler. They cannot move to `stdlib_minimal/`
without being demoted from "builtin" to "user-level fn" plus a
stage 0 rewrite.

### Stage 0 (`stage0/*.c`)

Pure C bootstrap compiler (~5 K LoC). Does not load kaikai
stdlib. Defines the 37-entry PRELUDE table that maps source-level
flat-prefix names to runtime helpers.

**Stdlib calls: 0.** **Stage-0 builtin definitions: 37.**

### Stage 1 (`stage1/compiler.kai`, 6,134 LoC)

`stage1/Makefile` invokes `kaic0 compiler.kai` with **no
`--prelude` flags**. Stage 1 receives no stdlib at compile time.

Every flat-prefix name referenced in stage 1 source resolves to:

1. A self-defined helper at the top of the file
   (`stage1/compiler.kai:12–17` defines `ch_is_digit`,
   `ch_is_lower`, `ch_is_upper`, `ch_is_alpha`, `ch_is_alnum`,
   `ch_is_space`; `:2813` defines `list_has`; `:3652` defines
   `list_nth`).
2. A stage 0 PRELUDE entry (`string_concat`, `string_length`,
   `char_at`, `list_reverse`, etc. — lowered by the stage 0 C
   compiler, not by any kaikai-level lookup).

Filtered to **stdlib-only flat names** (i.e. names that exist as
`pub fn` in `stdlib/core/*.kai` AND are *not* in stage 0's
PRELUDE table — 68 names total), stage 1 has:

| Name | Refs | Defined where |
|---|---:|---|
| `ch_is_digit` | 9 | self (line 12) |
| `list_nth` | 3 | self (line 3652) |
| `ch_is_alpha` | 3 | self (line 15) |
| `ch_is_upper` | 2 | self (line 14) |
| `ch_is_space` | 2 | self (line 17) |
| `ch_is_lower` | 2 | self (line 13) |
| `ch_is_alnum` | 2 | self (line 16) |

**Total: 23 references, all resolving to self-inlined helpers.**

**Stage 1 has zero genuine stdlib dependencies.** It does not
load `stdlib/core/*.kai`; it does not require any function from
those files to be present at compile time. The flat-prefix
helpers it uses are inlined as ~6 `fn` declarations at the top of
the file.

### Stage 2 (`stage2/compiler.kai`, 41,451 LoC)

`stage2/Makefile` invokes `kaic1 compiler.kai` with **no
`--prelude` flags**. Stage 2 receives no stdlib at compile time
either.

Same audit applied. Filtered to stdlib-only flat names plus
literal-string filtering (the m6.2 resolver in stage 2 carries
documentation comments mentioning names like `list_take`,
`opt_map`, `result_and_then` that do not resolve to call sites):

| Name | Real call sites | Defined where |
|---|---:|---|
| `list_is_empty` | 12 | self (`stage2/compiler.kai`) |
| `ch_is_digit` | 11 | self (line 26) |
| `ch_is_alpha` | 5 | self (line 29) |
| `ch_is_alnum` | 4 | self (line 30) |
| `list_nth` | 3 | self (line 10338) |
| `ch_is_space` | 3 | self (line 31) |
| `ch_is_lower` | 3 | self (line 27) |
| `string_ends_with` | 2 | self (line 20263) |
| `ch_is_upper` | 2 | self (line 28) |

Names that appear only in comments or string literals
(`list_take`, `list_sort`, `opt_map`, `result_and_then`, etc.
inside the m6.2 resolver's documentation): zero real call sites.

**Total: 45 real references, all resolving to self-inlined
helpers.** Like stage 1, stage 2's compiler source has **zero
genuine stdlib dependencies**.

### Examples + demos + stdlib (genuine consumers)

These are the actual stdlib consumers — the targets of the rename.

| Tree | Files containing flat-prefix refs | Total flat refs |
|---|---:|---:|
| `examples/` | 77 | 393 |
| `demos/` | 8 | 42 |
| `stdlib/` (self-references) | 23 | 677 |

Stdlib's 677 self-references are the largest single bucket — many
internal helpers use flat names to call other internal helpers.
Most of these become trivially-renamable once the module
boundary is settled.

### Headline

**The "stage 1 cannot understand qualified calls" premise from
the issue body is true; the dependent claim "stage 2's source
must therefore be parseable by stage 1's parser, which forbids
qualified calls" is also true. But the empirical bridge —
"stage 2's source uses 205+ flat-prefix stdlib calls" — is
false.** Stage 2's compiler.kai uses zero stdlib calls. Every
flat-prefix name in stages 1 and 2 is either self-inlined or a
stage 0 builtin.

The 205+ figure in the issue body counted every flat-prefix
identifier in stage 1 source, conflating stage 0 builtins
(`string_concat`, `char_at`, …) and self-defined helpers
(`ch_is_digit`, …) with genuine stdlib calls (zero of which
exist).

The migration surface is **`stdlib/` itself + `examples/` +
`demos/`** — total ~1,112 flat references across ~108 files. The
bootstrap chain (`stage0/`, `stage1/`, `stage2/`) is structurally
insulated from the rename.

---

## §2 — Flat-name reference count

Numbers below are produced by grep over the canonical
flat-prefix name set (the union of stage 0 PRELUDE entries and
stdlib `pub fn` names with deprecated prefix). They include both
genuine stdlib calls and stage 0 builtin invocations — i.e. they
match the issue body's counting methodology.

| Tree | Refs (issue's methodology) | Refs (stdlib-only, our methodology) |
|---|---:|---:|
| `stage1/compiler.kai` | 182 | 23 |
| `stage2/compiler.kai` | 874 | 45 |
| `examples/` | 393 | 179 |
| `demos/` | 42 | 18 |
| `stdlib/` | 677 | 280 |
| **Total** | **2,168** | **545** |

Three observations:

1. The issue body's "today: 205+" for `stage1/compiler.kai`
   matches our 182 figure when the broader methodology is used
   (stage 0 builtins + self-helpers + stdlib calls bundled
   together). Numbers diverge once stage 0 builtins are
   subtracted: stage 1 has 23 stdlib-name references, all
   self-defined.
2. Stage 2 has the largest raw count (874) but the same property:
   essentially zero of those references go through any kind of
   stdlib loader. They go through self-defined helpers and
   stage 0 builtins.
3. The genuine migration surface is **545 references across ~108
   files** in `stdlib/` + `examples/` + `demos/`. This is the
   number that drives Phase 1+ work, not 2,168.

The split between **"stage 0 builtins"** and **"stdlib functions
that happen to share a flat-prefix style"** is the load-bearing
distinction; the issue body did not make it.

### Per-module breakdown of stdlib-only references

| Prefix | Names exported | Refs in stage1 | Refs in stage2 | Refs in examples | Refs in demos | Refs in stdlib self |
|---|---:|---:|---:|---:|---:|---:|
| `ch_*` | 8 | 20 | 28 | ~30 | 0 | ~25 |
| `list_*` | 28 | 3 | 17 | ~85 | 18 | ~95 |
| `opt_*` | 13 | 0 | 1 | ~30 | 0 | ~70 |
| `result_*` | 12 | 0 | 1 | ~25 | 0 | ~70 |
| `string_*` (stdlib-only set) | 7 | 0 | 2 | ~9 | 0 | ~20 |

Stage 1 + stage 2 combined consume only `ch_*` and a handful of
`list_*`/`string_*`. Everything else lives in
`examples/`/`demos/`/`stdlib/`-self.

---

## §3 — Bootstrap-bootstrap option survey

### Option A — Backport qualified-call parser to stage 1

**Cost.** ~400–600 LoC in `stage1/compiler.kai`: add an
`EModCall` variant and the `expand_imports` ModuleTable + the
resolver shadowing rule from `docs/m6.2-design.md`. Stage 0
already lexes `import` and parses `a.b` as `EField`, so the
parser proper needs no change; the additions are post-parse
(resolver + emit dispatch).

**Risk.** **High.** Stage 1's role is *kaikai-minimal* —
deliberately small, parser-LL(1)-with-bookkeeping, no module
machinery beyond M6.1 concat (which stage 0 already implements
via `import`). Adding m6.2 to stage 1 reintroduces the
chicken-and-egg the issue body warns about, just one stage
earlier: stage 1 would now want stdlib loaded as preludes (to
exercise the new dispatch), which means stage 1's Makefile gains
`--prelude` flags, which means stage 1 source becomes coupled to
`stdlib/`'s shape — exactly the coupling the bootstrap split
seeks to avoid.

**Resulting state.** Stage 1 grows a half-typer. The
kaikai-minimal contract weakens. Tier 1 principle 3 ("fast
compilation … no Haskell-style type-class resolution") strains
because qualified-call resolution is structurally similar to
trait resolution.

**Verdict.** Reject. Costly and against kaikai-minimal
philosophy.

### Option B — Stdlib carries dual names (flat + qualified) as legacy aliases

**Cost.** Mechanical: every `pub fn list_map(...) {...}` gains a
sibling `pub fn map(xs, f) = list_map(xs, f)` in `stdlib/core/list.kai`.
~70 alias declarations across 7 core modules. Plus migration of
`examples/`/`demos/` user code at the user's pace.

`stdlib/core/list.kai` already does this for many of its
operations — `length` + `list_length`, `reverse` + `list_reverse`,
`map` + `list_map`, etc. The pattern works today.

**Risk.** **Medium.** Two systems coexist indefinitely.
Diagnostics get noisier ("did you mean `list_map` or `list.map`?")
and the deprecation cycle stretches. `kai fmt` cannot
mechanically rewrite call sites because both forms are valid
forever.

**Resulting state.** Stable but doubled API surface in stdlib.
The flat-prefix style is downgraded from "deprecated, m14
removes" to "legacy, kept indefinitely". The user-facing form
the documentation wants (`list.map`) becomes available; the
ergonomic UFCS combination from PR #211 (`xs.map(f)`) becomes
available too. But the flat-prefix style stays in the type
table forever, eating import-table size and hurting LSP autoimport
quality.

**Verdict.** Workable as a transitional state, but not the final
state. Could be a **stepping stone** to option C or E (below).

### Option C — Rename only what stage 1 doesn't use (partial)

**Cost.** Negligible at the bootstrap level (since §1 already
showed stage 1 uses zero stdlib calls). Equivalent to a full
rename for everything else.

**Risk.** **Low** for stages, **medium** for the resulting
language story: half-renamed stdlib is harder to teach. But §1
already shows the "what stage 1 doesn't use" set is "everything
in stdlib", so there is no half-state — the rename is total.

**Resulting state.** *In practice equivalent to a full rename.*
The "partial" qualifier evaporates once §1's data is in.

**Verdict.** Effectively merges into option E (below). Not its
own option.

### Option D — Bootstrap split (`stdlib_minimal/` + `stage2-bootstrap`)

This is the issue body's proposal. The premise: stage 1 must
load some stdlib (to compile stage 2's source), and that load is
threatened by the rename, so we split stdlib into a
kaikai-minimal-compatible `stdlib_minimal/` (frozen at flat
names) plus the modern `stdlib/` (qualified names), and we
introduce a `stage2-bootstrap` compiler in kaikai-minimal style
whose typer accepts qualified calls.

**Cost.** **High.** The stage 2 compiler source duplicates: one
copy in flat-name kaikai-minimal style (the bootstrap) and one in
qualified-name full kaikai (the modern source). Both must
selfhost identically. Plus `stdlib_minimal/` (a copy/move of
stdlib/core into a new tree) and a `stage2-bootstrap` Makefile
chain. Estimated 2,000+ LoC of duplicated stage-2 source plus
the `stdlib_minimal/` tree (~500 LoC) plus build orchestration
(~50 LoC).

**Risk.** **Medium-to-high.** The two stage-2 sources must stay
in sync — every compiler change is now a two-source change,
indefinitely (until stage 1 is retired). This is a permanent
maintenance tax. The Tongariki retro on PR #99 already retired
"meta-tracker docs" precisely because parallel-source-of-truth
mechanisms decay.

**Resulting state.** Bootstrap chain expands from 3 stages to 4
(`stage 0 → stage 1 → stage2-bootstrap → stage 2`). The fourth
stage is "stage 2 written in kaikai-minimal that emits qualified
calls". If stage 2 source ever uses a feature that
stage2-bootstrap's typer doesn't support, both sides drift.

**Verdict.** *The premise that motivates this option is empirically
wrong* (§1: stage 1 uses zero stdlib). The cost is large and the
maintenance burden is permanent. **Reject** unless §1's finding is
overturned.

### Option E — Direct rename, no bootstrap split (the option that emerges from §1)

This option falls out of §1's data and is not in the issue body.

**Premise.** Since stage 1 does not load any stdlib, and stage 2's
own compiler source uses zero stdlib functions, the rename is
**not coupled to the bootstrap chain at all**. It is a
single-tree, multi-PR migration of `stdlib/` exports + the
`examples/`/`demos/` consumers.

**Cost.** **Low to medium.** ~545 stdlib-only flat references to
rewrite across ~108 files, plus the `pub fn` declarations
themselves in 7 core stdlib modules. m6.2 qualified-call infra
already supports the post-rename call shape (`list.map(xs, f)`);
PR #211 (UFCS) makes `xs.map(f)` work too.

For the dual-form transition, stdlib keeps both names temporarily
(this is option B's mechanism, used as a per-PR transition tool,
not as a permanent state). Each PR migrates one module: the new
file lands with module-relative names *and* flat aliases; user
code can switch at its own pace; once all consumers in
`examples/`/`demos/`/`stdlib/`-self are migrated, the flat
aliases are removed.

**Risk.** **Low.** Per-PR selfhost is trivially preserved because
stage 1 + stage 2 source don't load stdlib. The risk surface is
exclusively at the user-code level (`examples/`/`demos/`),
gated by `make test-stdlib`, `make test-demos-core`, and the
fixture suites that already exist.

**Resulting state.** Stdlib uses module-relative names. Flat
aliases removed at end of milestone. No bootstrap-chain
expansion. No duplicated stage-2 source. No `stdlib_minimal/`.
No `stage2-bootstrap`.

**Verdict.** **Recommended.** §6 elaborates.

---

## §4 — Selfhost gate strategy

### What the selfhost gate actually is

`make selfhost` compiles `stage1/compiler.kai` through `kaic0`
to produce `kaic1`, then compiles `stage2/compiler.kai` through
`kaic1` to produce `kaic2`, then compiles `stage2/compiler.kai`
through `kaic2` and demands the output match `kaic2`'s own
emission byte-for-byte. **The selfhost gate watches the kaikai
compiler source compile itself.** It does not watch `stdlib/`,
`examples/`, or `demos/`.

Concretely: `make selfhost` does not consume any prelude file.
The byte-equivalence check is between two compilations of
`stage2/compiler.kai`, both with no `--prelude` flags.

### Implication for the m14 milestone under option E

Because `stage2/compiler.kai` does not call any stdlib function,
**every PR that touches `stdlib/*.kai` keeps `make selfhost`
green by construction.** Selfhost is not a meaningful gate for
any of the rename PRs — those PRs cannot affect selfhost output.

The gates that *do* matter for the rename:

- `make test-stdlib` — exercises `stdlib/*.kai` modules through
  fixtures in `examples/stdlib/` (full-kaikai layer end-to-end).
- `make test-demos` / `make test-demos-core` — exercises
  `demos/`.
- `make test-modules-qualified` — exercises the m6.2 qualified
  call surface itself.
- `make test-protocols` — exercises stdlib protocol impls.

Each PR boundary must keep these green, not selfhost.

### Per-PR phasing under option E

The straightforward sequence (no `stdlib_minimal/`,
no `stage2-bootstrap`):

1. **PR 1 — list.kai migration.** `stdlib/core/list.kai`
   already exports both module-relative (`map`, `filter`,
   `length`, `reverse`, …) and flat (`list_map`, `list_filter`,
   …) names. Migrate users in `examples/`/`demos/`/`stdlib/-self`
   to the module-relative form. Remove the flat aliases at the
   end of the PR. Gate: `make tier1` (which subsumes the
   user-fixture suites). Selfhost is unaffected.
2. **PR 2 — string.kai migration.** Add module-relative aliases
   (`length`, `concat`, `slice`, `to_int`, etc.) to
   `stdlib/core/string.kai`. **Note**: many of these are
   stage 0 builtins (`string_concat`, `string_length`,
   `string_slice`). Stage 0's PRELUDE table emits
   `kai_prelude_string_concat` regardless of the source name,
   so `string.concat` calls compile to the same C primitive as
   `string_concat`. The dispatch is via m6.2's resolver; no new
   runtime helper is needed. Migrate consumers; remove flat
   aliases.
3. **PR 3 — option.kai migration.** Same pattern. `option.unwrap_or`,
   `option.map`, etc. Replace `opt_*` aliases.
4. **PR 4 — result.kai migration.** Same pattern. `result.map`,
   `result.and_then`, etc.
5. **PR 5 — char.kai migration.** Same pattern. `char.is_digit`,
   `char.to_lower`, etc. **Note**: stage 1's source defines its
   own `ch_is_digit` etc. inline (line 12). Those are private
   to `stage1/compiler.kai` and are not affected by the rename.
   Stage 2's source likewise. Only stdlib's exported names
   migrate.
6. **PR 6 — tuple.kai + io.kai migration + final cleanup.**
   Trim any remaining flat exports across the core layer. Update
   `docs/stdlib-layout.md` to drop the "deprecated" qualifier and
   lock the canonical form.
7. **(PR 7, optional) — Examples + demos final pass.** A
   sweep-up PR if any consumer was missed in the per-module
   migrations. Likely empty; tracked as a contingency.

Each PR's blast radius is bounded by one stdlib module + its
consumers. Each PR keeps the `tier1` gate green. None of them
threaten selfhost.

### Why the issue body's three-PR phasing is unsafe

The issue body proposed:

> 1. Land `stdlib_minimal` first (stage 1 unchanged, copy/move).
> 2. Land `stdlib/core/` second (stage 2 still uses flat for now).
> 3. Migrate `stage2/compiler.kai` last, atomic with stage2-bootstrap.

§1 shows step 1 is a **no-op** (stage 1 doesn't load stdlib),
step 2 is a **no-op** (stage 2 doesn't either), and step 3 is
addressing a problem that doesn't exist (stage 2 source has no
flat-name calls to migrate). The phasing makes sense only under
option D's premise; once that premise is rejected, the phasing
goes with it.

### Migration window in stdlib itself

A stylistic question: during PR 1, is `stdlib/core/list.kai`
allowed to keep its own internal flat-name calls (e.g. helpers
like `length_loop` calling `length_loop` recursively under flat
names) while the public surface uses the module-relative form?
**Yes.** Internal-to-stdlib helper names are private; the
deprecation only concerns public exports. A future cleanup PR
can rename internals without coordinating with consumers.

---

## §5 — Risk inventory

### Risk 1 — Selfhost regression

**Severity: low.** The argument from §4: stage 1 + stage 2 source
do not load any stdlib, so no stdlib edit can break selfhost.

**Mitigation.** Verified per-PR via `make tier0` (which runs
`make selfhost` + `make demos-no-regression`). The user-fixture
gates (`test-stdlib`, `test-demos-core`, etc.) catch real
breakage.

**Early-warning signal.** Any PR that touches `stage1/compiler.kai`
or `stage2/compiler.kai` is *out of scope* for the m14 milestone
under option E. If the integrator finds themselves editing
those files, the migration has scope-crept.

### Risk 2 — User-code breakage

**Severity: medium.** Downstream user code (kaikai apps outside
this repo) uses the flat-prefix style today and breaks when the
flat aliases are removed.

**Mitigation.** Keep flat aliases for one full minor-version
cycle. Each per-module PR removes flat aliases at end-of-PR
*after* this repo's `examples/` + `demos/` are migrated, but
the broader deprecation window for external users is wider —
flag the deprecation in `CHANGELOG.md` (via the standard cz
process) and document the migration map in
`docs/stdlib-layout.md`.

**Early-warning signal.** A pre-1.0 minor bump (per `.cz.toml`
`major_version_zero = true`) is acceptable to break the flat
aliases; the kaikai project has not promised backward
compatibility pre-MVP (CLAUDE.md "Things to avoid" §"Do not
design against … backward compatibility").

### Risk 3 — Diagnostics quality during transition

**Severity: low.** During the transition (e.g. mid-PR-3, where
`stdlib/core/string.kai` has both `length` and `string_length`
exports), a typo like `string.lenght(s)` will report:

> module `string` does not export `lenght`; available exports:
> length, concat, slice, to_int, to_real, split, contains,
> repeat, trim, starts_with, ends_with, string_length,
> string_concat, …

The "available exports" list contains both forms. Users see twice
the noise.

**Mitigation.** None needed during a single-PR transition. The
flat aliases are gone by end-of-PR.

**Early-warning signal.** If a PR's dual-form window stretches
across multiple PRs (e.g. PR 3 lands `option.unwrap_or` but the
flat alias removal is deferred to PR 6), the diagnostic noise
persists. The phasing in §4 keeps the dual window per-PR-bounded.

### Risk 4 — Documentation drift

**Severity: low-to-medium.** `docs/stdlib-layout.md` lines 80–83
declare flat-prefix "deprecated" with rename "agendada en m14".
The text inside the doc switches Spanish in one phrase
(`agendada en m14`) — the project language rule is
English-only for documentation per CLAUDE.md "Project language".
That's a separate cleanup, out of m14 scope.

The deeper drift risk: if the m14 milestone moves the canonical
form, every cross-reference in tutorial-style docs
(`docs/quickstart.md`, fixture comments, `examples/quickstart/`
narrative) needs review. ~30 files mention flat-prefix names by
audit-time grep.

**Mitigation.** PR 6 (the final cleanup) updates
`docs/stdlib-layout.md` and lints cross-refs. A sweep is
mechanical (grep + replace + manual review of context).

**Early-warning signal.** A changelog entry mentioning "flat
aliases removed" in any pre-PR-6 release is a leak.

### Risk 5 — Stage 0 builtin name overlap

**Severity: medium.** Stage 0's PRELUDE table hardcodes 37 names
that overlap heavily with stdlib's flat-prefix exports
(`string_concat`, `string_length`, `list_length`, `list_append`,
`list_reverse`, `string_to_int`, `char_at`, `char_to_int`, …).
These are not removed by the rename — they are the runtime
primitives.

What this means concretely: after the rename, `string.concat(a, b)`
in user code resolves through m6.2 to `kai_<module>__concat`
emit, but the stdlib `pub fn concat` body still calls the stage 0
builtin `string_concat` underneath. A reader of stdlib source
will see the flat-prefix call and assume it's deprecated.

**Mitigation.** Document in `docs/stdlib-layout.md` that the
stage 0 PRELUDE table names are *runtime primitives*, not
deprecated stdlib functions, and are not affected by the m14
rename. They are flat-prefix because they pre-date m6 module
resolution and live below the user-facing language; they don't
appear on any documented stdlib surface.

If desired, a follow-up milestone (out of m14 scope) could rename
the PRELUDE table entries to module-relative form too —
`kai_prelude_string_length` → `kai_prelude_string__length`, etc.
That is a stage 0 + stage 1 emitter change and a runtime header
change. Not required for m14's user-facing goal.

**Early-warning signal.** A PR description claiming "all flat
names removed from stdlib" without a follow-up note about the
stage 0 PRELUDE is a documentation gap.

### Risk 6 — Bootstrap-retirement complexity (option D specific)

**Severity: N/A under option E.** Listed only because the issue
body's option D introduces it.

Under option D, the milestone gains two new long-lived artifacts
(`stdlib_minimal/` and `stage2-bootstrap`) whose retirement
schedule is unclear. The issue body suggests "retire
`stdlib_minimal/` once stage 1 itself is retired" — but stage 1
retirement has no scheduled milestone today. Under option D, the
artifacts ship with no decommission plan.

Under option E, this risk does not exist: no new artifacts are
introduced.

### Risk 7 — Coverage probe ratchet

**Severity: low.** Each PR adds new tests (the migrated fixture
exercising the qualified call) and removes old tests (the
flat-name fixture). `tools/coverage-baseline.txt` may shift; the
ratchet permits decreases (closed gaps bump baseline down).

**Mitigation.** Per-PR baseline update is part of the standard
landing checklist.

**Early-warning signal.** If `make coverage-probe` (Tier 2) goes
red on any PR, the ratchet was not updated.

---

## §6 — Recommendation

### Verdict

**Adopt option E. Reject options A, B, C, D as named in the
issue body.**

Option E is a direct, single-tree, multi-PR migration of
`stdlib/`'s public surface from flat-prefix to module-relative
names, with per-PR dual-form windows (option B's mechanism used
as a per-PR transition tool, not as a permanent state). No
bootstrap split. No `stdlib_minimal/`. No `stage2-bootstrap`. No
duplication of stage 2 compiler source.

### Why the empirical case overrides the issue body's premise

The issue body's option D is grounded in the claim that
"`stage1/compiler.kai` contains 205+ references to flat-prefix
names" and "stage 2 source must be parseable by stage 1's
parser". §1 confirms the second claim and partially confirms the
first (182 refs by the same methodology) — but **none of those
references are stdlib calls**. They are self-defined helpers
(~6 declarations at the top of each file) plus stage 0 builtin
invocations (mapped through `stage0/emit.c`'s PRELUDE table).
Stage 1's Makefile passes no `--prelude` flags; stage 1 never
sees `stdlib/`.

Once the empirical observation lands, the bootstrap chain is no
longer load-bearing for the rename. The migration becomes a
single-tree, single-namespace cleanup of `stdlib/` and its
consumers in `examples/`/`demos/`.

### Recommended phase order

(Repeats §4 for §6's reader.)

1. PR 1 — `stdlib/core/list.kai`: complete migration, remove
   flat aliases.
2. PR 2 — `stdlib/core/string.kai`: add module-relative
   exports, remove flat aliases.
3. PR 3 — `stdlib/core/option.kai`: same pattern. `opt_*` →
   `option.*`.
4. PR 4 — `stdlib/core/result.kai`: same pattern.
5. PR 5 — `stdlib/core/char.kai`: same pattern. `ch_*` →
   `char.*`.
6. PR 6 — final cleanup: `tuple.kai` + `io.kai` polish, doc
   updates (`docs/stdlib-layout.md` removes the "deprecated"
   qualifier and locks the canonical form).
7. PR 7 (contingency) — orphan-consumer sweep.

Six PRs, each ~ a 10-minute review for the integrator, each
gated by `make tier1`. Selfhost is unaffected by any of them.

### Selfhost gate strategy

`make selfhost` cannot regress on any rename PR (§4). The
relevant gates are `make tier1` (which includes
`test-stdlib`, `test-demos-core`, `test-modules-qualified`,
`test-protocols`) plus a per-PR `make test-stdlib` invocation
for the migrated module. Auto-merge is appropriate.

### What this milestone delivers

- All of `stdlib/`'s user-facing public surface uses
  `module.function(args)`.
- `xs.map(f)` ergonomic form works (UFCS + module-relative
  combination, leveraging PR #211).
- No bootstrap-chain expansion; no parallel-source-of-truth.
- `docs/stdlib-layout.md` becomes the canonical reference; flat
  aliases are gone.

### What this milestone does not deliver

- Stage 0 PRELUDE table rename (separate milestone, out of scope).
- Stage 1 retirement (out of scope; no schedule today).
- Module-system extensions beyond m6.2 (re-exports etc. are
  m6.3+).

### Items the integrator may want to file post-audit

(Not opened by this audit, per lane discipline.)

1. **Stage 0 PRELUDE table rename** — `kai_prelude_string_length`
   → `kai_prelude_string__length`, mirrored in stage 0 emitter +
   runtime header. Tier 3 cleanup; not user-facing.
2. **Spanish phrase in `docs/stdlib-layout.md`** — line 83
   "agendada en m14". Documentation language rule violation.
   Mechanical fix; could be bundled into PR 6.
3. **`stdlib/core/list.kai` flat-aliases-already-shipped fact** —
   noted in §1; the file was partway migrated in earlier work.
   Cross-reference from the m14 milestone's first PR description
   would help reviewers.

### Confidence

Confidence is high on the empirical findings (§1, §2 are
greppable) and high on the recommended option (§3, §6).
Confidence is medium on the per-PR LoC estimates in §4 — they
depend on how much consumer-code churn actually surfaces during
each migration; the per-module surface is small (~50–150 refs
per module) but cross-cutting reviews can expand.

The biggest residual uncertainty is **risk 5** (stage 0 PRELUDE
overlap with stdlib names). It does not block the milestone but
will surface as a "why does stdlib's `string.concat` still call
`string_concat` under the hood?" question from contributors. The
documentation note in §5 mitigates.

---

*End of audit.*
