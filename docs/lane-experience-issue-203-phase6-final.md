# Lane experience — issue-203-phase6-final

PR 6 (final) of the m14 milestone (#203). Locks `docs/stdlib-layout.md`
as the canonical reference: removes the "deprecated" qualifier on the
flat-prefix style, adds a *Migration status* table summarising what
landed under Phases 1–5, and explains why a residual set of flat
aliases survives (gated on the typer EModCall fix tracked in #219,
plus parser-level keyword reservation for `opt_or` and three
unrelated resolver gaps for the `list_*` survivors). **Closes #203.**

## Objective metrics

- Start (lane init):     2026-05-04T02:23:59-04:00
- End (retro written):   2026-05-04T02:26:29-04:00
- Wall clock:            ~3 min
- Build/test invocations: none — diff is confined to `docs/`.
  Per `CLAUDE.md` Testing discipline, doc-only changes skip every
  tier locally AND in CI via `paths-ignore`.

See appended TSV for the (empty) build log.

## What this PR closed

1. **Canonical layout locked.** `docs/stdlib-layout.md`:
   - Naming-convention paragraph now says **retired** instead of
     **deprecated**, points readers at the Migration status table,
     and pins module-relative names as the canonical user-facing
     form.
   - New *Migration status* subsection (under "Naming convention")
     summarises per-module: bare-name surface size, aliases
     removed, aliases surviving, and the reason for each survivor.
     Cross-references #219 for the typer-side gap, and notes the
     `opt_or` keyword block + the three list-side compiler gaps as
     separate causes.
   - Updated the "Migration from today's `list_*` / `string_*`"
     bullet under *Naming conventions* to past tense, with a
     pointer to the table.
   - Updated the "Next steps" item 2 (the historical note about
     the `core.kai` monolith split) to record m14's closure and
     point to `docs/m14-bootstrap-audit.md`.
2. **Surviving aliases catalogued.** All 16 flat-prefix exports
   that remain alive in `stdlib/core/{list,string,option,result}.kai`
   are listed with cause:
   - 5 `list_*`: prelude-scope resolver gap (4) + string-interpolation
     gap (1) — both filed in Phase 1 retro, distinct from #219.
   - 1 `string_*`: `string_repeat` — #219 collision with
     `list.repeat`.
   - 4 `opt_*`: 3 #219 collisions (`opt_map`/`opt_filter`/`opt_zip`
     vs `list.map`/`filter`/`zip`) + 1 keyword block (`opt_or`).
   - 6 `result_*`: all #219 collisions (5 with `option`'s newly
     bare exports + 1 with `list_collect`).
   - 0 `ch_*`: char-domain names are unique across `stdlib/core/`.
3. **Orphan-consumer sweep complete.** Grep across `stdlib/`,
   `examples/`, `demos/` for the eight migration-target prefixes.
   Findings:
   - `stdlib/` (excluding the alias defs themselves): zero orphan
     references. All Phase 1–5 lanes cleaned their consumers.
   - `examples/` references that look like flat-prefix calls fall
     into three classes, all expected:
     - Calls to surviving aliases (`list_sum` in
       `examples/phase4/euler1.kai`; `opt_zip`/`opt_filter`/`opt_or`
       in `examples/stdlib/option_*_basic.kai`;
       `result_collect`/`result_unwrap_or_else` in
       `examples/stdlib/result_*_basic.kai`; `result_map` cited
       in `examples/ufcs/chain_basic.kai` lines 11–15 as the
       "spelling that compiles today" because UFCS-with-#219 has
       no clean Result-side spelling — explicitly documented in
       Phase 4 retro).
     - Calls to stage 0 PRELUDE builtins (`string_concat`,
       `string_length`, `string_slice`, `string_split`,
       `string_contains`, `list_length`, `list_reverse`, …) —
       below the user-facing language and out of m14 scope per
       audit Risk 5.
     - Comment references in `pipe_*_rejected.kai`, `m12_6_*.kai`,
       and `m4c_flow_through.kai` that quote old names by design.
4. **Tuple + io confirmed N/A.** `stdlib/core/tuple.kai` exports
   only types (`Pair`/`Triple`/`Quad`); no flat-named functions
   exist. `stdlib/core/io.kai` exports a single `pub fn println`
   that is already module-relative shape. The audit's Phase 6
   line about "`tuple.kai` + `io.kai` polish" was a no-op — no
   migration work was required, only the documentation update.

## Surviving aliases summary

| Module                  | Survivors | Cause(s)                                                        |
| ----------------------- | --------: | --------------------------------------------------------------- |
| `stdlib/core/list.kai`  |         5 | 4 prelude-scope resolver gap, 1 string-interpolation gap        |
| `stdlib/core/string.kai`|         1 | typer EModCall name-only lookup (#219)                          |
| `stdlib/core/option.kai`|         4 | 3 × #219 collisions with `list`, 1 × `or` keyword reservation   |
| `stdlib/core/result.kai`|         6 | 6 × #219 collisions with `list` / `option` exports              |
| `stdlib/core/char.kai`  |         0 | (clean — char names unique across `core/*`)                     |
| `stdlib/core/tuple.kai` |       n/a | types-only module                                               |
| `stdlib/core/io.kai`    |       n/a | already module-relative                                         |

Cumulative: **16 flat aliases** remain alive across the five
migrated `core/*` modules. All are documented with inline
comments at their definition sites pointing at the gap that
blocks them.

## Compiler errors I encountered

**None.** The diff is doc-only.

## Friction points

### Orphan sweep — surfaced no surprises

The grep over `examples/`/`demos/`/`stdlib/` returned hits, but
every hit was already accounted for: a surviving alias, a stage 0
PRELUDE builtin, or a comment reference. No code change required
in the consumer sweep. PR 7 (the contingency lane in the audit
plan) did not need to be split out as a separate PR — Phase 6
absorbs it.

### `tuple.kai` and `io.kai` — truly no migration

The lane brief flagged a verification step: "tuple/io: confirm
truly need no migration." Confirmed by inspection — `tuple.kai`
has no `pub fn` exports beyond the three product types, and
`io.kai` exports only `println` (the bare name being also the
canonical name; there is no `io_println` to retire). The audit's
PR 6 description was overly cautious in pairing them with
"polish" work; in practice the work was zero.

### Doc structure — additive edit, not rewrite

`docs/stdlib-layout.md`'s structure already held up well after
Phase 1–5 landed. The change is additive (a new *Migration
status* subsection plus three short edits to existing
paragraphs). No headings reshuffled, no per-module entries in the
*core* section reworded — the table augments rather than replaces
the existing prose. This kept the diff small (~80 lines of
addition + ~20 lines of edit) and reviewable.

### List-side survivor count differed from the lane brief

The brief noted "`list_repeat` (Phase 2 — collision with bare
`repeat`)" as the list module's surviving alias. Verified against
`stdlib/core/list.kai` lines 552–556: the actual survivors are
five (`list_is_empty`, `list_nth`, `list_take`, `list_contains`,
`list_sum`), and `list_repeat` is *not* among them — its
collision was on the `string` side (Phase 2 surfaced
`string_repeat`, not `list_repeat`, as the survivor). The
*Migration status* table reflects what is actually in the file
plus what the Phase 1 retro documented as the cause of each.
This is a brief vs. ground-truth discrepancy, resolved by
trusting the source.

## Spec ambiguities or interpretive choices

### Wording of "deprecated" → "retired"

The lane brief said to "remove the 'deprecated' qualifier" and
to "lock the canonical form". I chose **retired** as the
replacement word: "deprecated" implies "still works, scheduled
for removal", while the post-m14 reality is that the flat-prefix
style is no longer the canonical user-facing form even though a
small set of flat aliases survive on resolver gaps. Saying
**retired** with an immediate cross-reference to the *Migration
status* table reads as "the era ended; here is the cleanup
backlog", which matches the audit's framing.

### Whether to enumerate the 16 surviving aliases as a code block

I considered a long bulleted list of every surviving alias name.
Rejected in favour of the per-module count + cause table. Reasons:
the per-alias names are already grep-able from the source files
(each has an inline comment pointing at #219 / the keyword block /
the resolver gap), and a long list inside the layout doc would
duplicate information that lives correctly with the code.

### Whether to file new evidence on #219

The brief explicitly says "don't fix #219 (that's its own
follow-up issue)". I read this as: do not extend the issue with
new repros either, because each Phase 1–5 retro already
documented its #219 collisions. The doc points at #219 by
number; no new comment was added to the issue.

## Subjective summary

**m14 milestone closing — overall confidence:** HIGH.

- Five of six migration phases (Phases 1, 2, 3, 4, 5) landed cleanly
  with full tier1 + selfhost gates green and per-module retros.
- Phase 6 is doc-only and reduces to: lock the layout doc, document
  what survived, point at the gating issue. Consumer sweep was
  empty.
- The 16 surviving flat aliases all have a documented cause and a
  named gap (#219 plus three list-side compiler gaps plus the `or`
  keyword reservation). None of them are accidental survivors;
  none of them block a future caller from spelling the canonical
  form *as well* (the canonical form works; the alias is
  belt-and-suspenders for cases where the typer can't disambiguate).
- The audit's seven-PR plan landed in six PRs (PR 7 folded into
  PR 6) because there were no orphan consumers to clean.

**Hardest part of this lane:** picking the right wording for the
*Migration status* preamble — it has to convey "the rename ran;
here is what's left and why" without making the reader think the
milestone was incomplete or that the language is broken. The
table form helped: it foregrounds the per-module shape and lets
the prose stay short.

**Easiest:** the orphan sweep. Phases 1–5 had already cleaned
their consumers; the only "hits" the grep produced were calls to
surviving aliases (intentional, retro-documented), stage 0
PRELUDE builtins (out of scope), or comment references quoting
old names (also intentional).

**Compiler help:** none needed — no code touched.
**Compiler hinder:** none.

**Why this was the cleanest phase:** doc-only changes that
codify a state already established by Phases 1–5. The hard work
landed in the prior six PRs; Phase 6 is bookkeeping.

## Limitations of this report

- Did not run any tier locally (doc-only diff skips CI tiers via
  `paths-ignore` per `CLAUDE.md`). If the doc edits accidentally
  touched a code path or build script (they didn't —
  `git diff --stat` confirms `docs/stdlib-layout.md` is the only
  file in the diff), CI would still skip and the gap would land
  unnoticed.
- Did not benchmark anything; this lane changes nothing executable.
- Did not file a new GitHub issue for the three list-side compiler
  gaps mentioned in Phase 1's retro (prelude-scope and
  string-interpolation). The Phase 1 retro is the only place they
  are documented today; tracking them in a dedicated issue would
  be a useful follow-up but is outside m14's scope.
- Did not touch `docs/m14-bootstrap-audit.md` (historical record
  per lane scope). The audit's seven-PR plan is left in its
  original form; this retro is the place to record that it
  landed in six.

## Build/test TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
```

(empty — doc-only diff, no tier runs)
