# Lane retro — issue #565: privacy check leaks across module boundary

## Scope

Regression follow-up to #510 (the lane that turned `pub` from
parse-only into a load-bearing visibility check). The privacy
validator landed in #510 worked for direct imports but blew up
under a transitive shape:

```
consumer (tiny.kai)
  └─ import ahu.cell
       └─ import actor
```

Calling `cell.keep(...)` from `tiny.kai` produced

```
error: `overflow_code` is private to module `cell`
  --> tiny.kai:44:55
error: `alloc_for_policy` is private to module `cell`
  --> tiny.kai:68:12
```

Two oddities:

1. The line/column coordinates were not from `tiny.kai` (9 lines)
   — they were from `stdlib/actor.kai`. The validator pointed at
   `actor.kai`'s own intra-module call sites (`with_mailbox_policy`
   calling `alloc_for_policy`, both in the same file) but printed
   `tiny.kai` as the source.
2. The error blamed module `cell`, but `overflow_code` /
   `alloc_for_policy` are authored in `actor.kai`.

Both pointed at the same root cause: the `PubAccessEntry` table
attributed every decl of `actor.kai` to `cell` instead of `actor`.

## Root cause

`collect_pub_access_table` builds the home-attribution list for
the flat imports decl stream via `imports_home_anchors`. The
stream order — from depth-first `process_imports` — is:

```
[ actor.decls ..., cell.decls ... ]
```

`actor.kai` starts with non-DFn decls (`type MailboxPolicy`,
constructors) before its first explicit DFn anchor. The algorithm
seeds `cur_mod = target_mod` and walks left-to-right; when an
anchored DFn (with `module_origin = Some("actor")`) lands, it
calls `propagate_home_back` to retroactively assign `"actor"` to
the preceding non-anchored decls that still read `cur_mod`. That
works correctly inside one module.

The bug surfaced at the **module boundary** in the merged stream.
After the last `actor` DFn, `cur_mod` stayed `"actor"`. Then
`cell.kai`'s first decl (`pub type StepResult`, a DType with no
`module_origin`) inherited `cur_mod = "actor"`. When `cell`'s
first explicit DFn (`pub fn keep` with `Some("cell")`) finally
arrived, `propagate_home_back` walked the accumulator backward
replacing every entry tagged `"actor"` with `"cell"` — and the
walk did not stop at `actor`'s own anchored DFns because all the
previous entries were `"actor"`, indistinguishable from the
DType DFn's transient `cur_mod` heritage. The propagation
overran the whole `actor.kai` block.

In short: the `prev == new` stop-condition assumed `cur_mod` only
held its value because nothing had anchored it yet. Once a real
anchor wrote `"actor"`, the propagation had no way to tell the
"anchored `"actor"`" apart from a "derived `"actor"`" entry.

## Fix shape (option B from the brief)

Decision was to keep the per-decl home list (no major restructure
to span-driven lookup, since `vpa_decl` already gets the right
`home` via the same algorithm for everything inside a module —
the only break was the cross-module boundary in the *table*).

Concretely:

- New type `HomeAnchor = HA(String, Bool)` — home + anchored flag.
  Replaces `[String]` as `imports_home_anchors`'s accumulator and
  `cpa_with_homes`'s home list.
- `imports_home_anchors` tags an entry `HA(m, true)` when the
  decl carried `module_origin = Some(m)`, and `HA(hint, false)`
  when the decl inherited the running `cur_mod`.
- `propagate_home_back` stops at the first anchored entry it
  encounters (in addition to the existing "home differs" stop).
  Derived entries with the matching home still flip; anchored
  entries with the same home are now treated as a hard boundary.
- Two trivial accessors `ha_home` / `ha_anchored` keep
  `cpa_with_homes` and the running-home update site readable.

No span-driven rewrite was needed — the `home` argument that
`vpa_decl` already threads through `vpa_decls_loop` was always
correct per-decl; only the *table lookup target* was wrong.

`vpa_diag` already prints `decl_home` from the matched entry, so
the "private to module `actor`" message corrects itself once
the table attributes every `actor.kai` decl to `"actor"`.

## Why option A wasn't necessary

The brief suggested resolving each call site's "current module"
from its span as the most robust option. That would have been a
deeper rewrite (touching `vpa_decl` / `vpa_expr` plumbing and
the span-on-Decl invariant) and the symptom only fired because
the *table* lied — every call site already saw the right `home`
in `vpa_decl`. Tightening the table-build was strictly smaller
and preserved the same boundary the existing comment in
`propagate_home_back` already attempted to enforce.

## Error reporter

No separate fix needed. `vpa_diag` reads the `decl_home` from
`pae_decide_loop`'s first matching `PubAccessEntry`. With the
table now correctly attributing `actor.kai` decls to `"actor"`,
the negative fixture `violation_transitive` now reports
"private to module `leaf`" — the right module, not the
intermediate `mid`.

## Fixtures shipped

Positive — `examples/modules/transitive_import_privacy/`:

- `leaf.kai` — `pub fn leaf_public_step` calls non-`pub`
  `leaf_private_helper`.
- `mid.kai` — `import leaf`; `pub fn mid_keep` wraps it.
- `main.kai` — `import mid`; calls `mid.mid_keep`.
- `main.out.expected` — golden `41`.

Wired through `test-modules-transitive-privacy` in
`stage2/Makefile` (added to `.PHONY`, `test`, and `test-fast`).
Pattern mirrors `test-modules-derive-export`.

Negative —
`examples/negative/pub_enforcement/violation_transitive/`:

- `leaf.kai` / `mid.kai` / `main.kai` — `main` bare-names
  `leaf`'s private helper through a transitive chain.
- `main.err.expected` — `\`leaf_private_helper\` is private to
  module \`leaf\`` (asserts the right module name).

Picked up automatically by `tools/test-negative.sh`'s
`examples/negative/**/main.err.expected` glob.

## Selfhost / tier behavior

- `make tier0` — selfhost byte-identical, demos baseline holds
  (28 passing, baseline 27).
- The fix sits entirely inside the pub-access validator's table
  builder; the only stream it changes is the per-decl `home`
  attribution, which `vpa_decl` consumes through the same
  `decl_home_hint_reset` path. No downstream pass observes the
  difference.

## Real cost vs estimate

Brief estimated 1–2 days. Actual: ~1.5 hours. The bug was
narrow once the boundary semantics were spelled out; the fix is
~30 lines of compiler.kai including the new type and accessors.

The longest stretch was repro setup — `kai-pkg paths` segfaults
in this branch (orthogonal bug, blocked the `kai.toml`-driven
repro from the issue body), so the lane fell back to
`stage2/kaic2 --path /tmp/local-ahu-min --path stdlib --check
tiny.kai` to reproduce.

## Follow-ups left for next lanes

- `kai-pkg paths` segfault — independent regression, separate
  lane. Not blocking for #565 because direct `--path` invocation
  reproduces the bug shape (and the issue body's repro can be
  validated once `kai-pkg` is fixed).
- Audit other lanes that cited "private to module X" diagnostics:
  the cross-module attribution was wrong in this specific chain
  shape — chains where the leaf has only `pub` DFns and no
  preceding non-DFn decls would have been correctly attributed
  even before this fix, so the impact radius is "transitive
  imports of modules with type/effect/protocol decls preceding
  their first DFn". `stdlib/actor.kai` was the only stdlib hit
  in the issue body, but the fix covers the class.

## Cross-references

- #510 — original privacy-enforcement lane that introduced
  `PubAccessEntry` + `imports_home_anchors` + the
  `propagate_home_back` boundary algorithm.
- `docs/lane-experience-issue-510-pub-enforcement.md` (if
  present) — context for the original design choices.
