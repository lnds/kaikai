# Lane experience — issue-203-phase2-string

PR 2 of the 7-PR m14 milestone (audit `docs/m14-bootstrap-audit.md`,
PR #214). Migrates `stdlib/core/string.kai` user-facing exports
from flat `string_*` to module-relative `string.*` form, and
sweeps every consumer in `examples/`, `demos/`, and `stdlib/`.

## Objective metrics

- Start (lane init):     2026-05-04T00:04:11-04:00
- End (retro written):   2026-05-04T00:20:58-04:00
- Wall clock:            ~17 min
- Build/test invocations:
  - `tier0`: 3 (baseline, post-migration, post-revert) — all OK
  - `tier1`: 2 (one FAIL with the typer collision, one OK after revert)
  - `tier1-asan`: 1 OK
  - `selfhost`: 1 OK (no-op gate; `tier0` already exercises it)

See appended TSV at the bottom of this file for the full log.

## Migration inventory

`stdlib/core/string.kai` exports today (flat-name surface, before lane):

```
pub fn string_starts_with    (stdlib export)
pub fn string_ends_with      (stdlib export)
pub fn string_trim_left_loop (stdlib export — was pub, treated as helper)
pub fn string_trim_right_loop(stdlib export — was pub, treated as helper)
pub fn string_trim           (stdlib export)
pub fn string_repeat         (stdlib export)
pub fn string_join           (stdlib export — name-overlaps PRELUDE builtin of same arity)
```

Stage 0 PRELUDE table (`stage0/emit.c` lines 425–467) defines
ten `string_*` runtime primitives that are NOT stdlib exports
and are out of scope (audit Risk 5):

```
string_length, string_concat, string_concat_all, string_join,
string_to_int, string_to_real, char_at, string_split,
string_contains, string_slice
```

`string_join` is the lone overlap — it appears in BOTH stage 0
PRELUDE and `stdlib/core/string.kai` (same arity, same effective
behaviour). Migrating the stdlib `pub fn string_join` to bare
`pub fn join` is fine: stdlib's `join` and PRELUDE's
`string_join` keep distinct dispatch keys.

### Counts

- Flat exports retired:        4 of 5 (`starts_with`, `ends_with`,
                                       `trim`, `join`)
- Flat aliases surviving:      1 (`string_repeat` — typer gap, see
                                  Compiler errors / issue #219)
- Helpers demoted to private:  2 (`string_trim_left_loop`,
                                  `string_trim_right_loop` — never
                                  intended as a public surface;
                                  dropped `pub` keyword)
- Consumer files modified:    11 + 2 (12 code, 2 doc-comment-only)
- Consumer call sites changed: 14 (qualified `string.foo`) + 1
                                  (left as `string_repeat` because
                                  the alias survives)
- Stage 0 PRELUDE call sites left alone: ~140 (every consumer
  that calls `string_concat`, `string_length`, `string_slice`,
  `string_to_int`, etc. — these are runtime primitives, not
  stdlib exports)

### Files modified

```
stdlib/core/string.kai           (definitions + tests)
stdlib/decimal.kai               (1 site — kept string_repeat)
stdlib/path.kai                  (2 sites + 1 doc comment)
stdlib/encoding/json.kai         (2 sites)
stdlib/net/http.kai              (1 doc-comment-only)
examples/stdlib/map_tree_basic.kai (1 site)
examples/stdlib/jwt_encoder.kai  (1 site)
examples/stdlib/list_sort.kai    (2 sites)
examples/stdlib/list_helpers.kai (2 sites)
examples/stdlib/map_basic.kai    (3 sites)
examples/stdlib/list_flat_zip.kai (1 site)
examples/stdlib/list_while_uniq.kai (1 site)
demos/poker/main.kai             (1 site)
```

## Compiler errors I encountered

### Class 1 — Typer EModCall ambiguity (BLOCKING; filed as #219)

After adding `pub fn repeat(s: String, n: Int)` to
`stdlib/core/string.kai`, every existing `list.repeat(...)` call
(e.g. `examples/stdlib/list_basic.kai:36`) failed to type-check:

```
error: type mismatch in function call
  --> ../examples/stdlib/list_basic.kai:36:48
     |
  36 |   println(int_to_string(list_length(list.repeat(0, 0))))
     |                                                ^
  = note: expected: (String, Int) -> String
  = note: found:    (Int, Int) -> ?t32 / ?e18
warning: bare name 'repeat' is exported by multiple modules with no
         root-file shadow: list, string; use a qualified call
         (e.g. list.repeat(...)) to disambiguate
```

The qualified call `list.repeat(0, 0)` is being typed against
`string.repeat`'s scheme. Root cause: stage 2's `EModCall(mod,
fname)` typer branch (`stage2/compiler.kai:22984`) does a flat
`ty_env_lookup(st.env, fname)` regardless of `mod`. The codegen
side already handles the same-name-different-module case via
`efn_resolve` + `fns_prefer_module` (`stage2/compiler.kai:9290`,
explicitly noting the `loop.repeat` vs `list.repeat` precedent),
but the typer side has not been universalised. The inline
comment at the EModCall branch acknowledges the gap:

> v1 shares the symbol with legacy unqualified callers; v2 will
> universalise the prefixed `kai_<module>__<name>` minting and
> use this variant to disambiguate two modules that export the
> same name. Until then, the lookup is the same as `EVar(fname)`.

**Where:** `stage2/compiler.kai:22984` (EModCall branch in `synth_expr`).
**Fix attempts in this lane:** none — out of scope for a stdlib
migration PR.
**Resolution:** Filed issue #219 with repro + fix sketch. Kept
`string_repeat` as a flat alias with an inline comment in
`stdlib/core/string.kai` pointing to #219.
**Detection:** `make tier1` after the migration; failed in the
`stdlib FAIL list_basic` step. Took one tier1 cycle (~133s) to
surface — would not have shown up in `tier0` because
`examples/stdlib/list_basic.kai` is not on tier0's path.

### Class 2 — Doc-comment fallout from naive sed

A perl regex sweep (`s/\bstring_starts_with\b/string.starts_with/g`)
also rewrote a comment in `stdlib/path.kai:4` that listed
"the string builtins" with examples. The comment now mixed
`string_concat` (PRELUDE primitive) with `string.starts_with`
(stdlib helper) under the same heading. Cleaned up by hand to
disambiguate the two layers in the comment text.

## Friction points

### Did the resolver-gap fixes from PR #218 actually allow zero surviving aliases?

Yes for the gaps PR #218 fixed (prelude scope and string
interpolation): no consumer files in `stdlib/random.kai`,
`stdlib/regexp.kai`, `stdlib/collections/{queue,set,stack}.kai`
or in `#{...}` interpolation segments referenced any of the
five `string_*` exports, so we did not exercise PR #218
end-to-end on this lane. PR #218's fixes were necessary
preconditions but not load-bearing for THIS PR's migration set.

What blocked zero-alias was a DIFFERENT, previously unknown
resolver gap on the typer side (#219). PR #218 closed two
resolver gaps; this lane uncovered a third.

### Did the stdlib-export vs stage 0 builtin distinction need any tooling?

No special tooling — but the disambiguation was non-trivial and
warrants a note for future lanes. The mental model:

1. `stage0/emit.c` PRELUDE table = runtime primitives. Calls
   compile to `kai_prelude_<name>` regardless of what user
   code spells (so a hypothetical `pub fn string_concat`
   wrapper in stdlib would resolve to the stdlib version
   while a bare `string_concat(...)` call still works).
2. `stdlib/core/string.kai` `pub fn` = stdlib exports. Migrate
   these.
3. The two layers can use the same flat name (`string_join` is
   in both); the dispatcher disambiguates by lookup order.

A `grep '^pub fn ' stdlib/core/string.kai` plus a side-by-side
read of `PRELUDE[]` in `stage0/emit.c` was sufficient to
classify all 15+ `string_*` names.

### Did UFCS opportunism create readability wins or losses?

Did not exercise UFCS in this lane. Every consumer call site
already had explicit arguments (e.g. `string_join(map(...), ",")`)
that translated 1:1 to `string.join(...)`. UFCS-style
`xs.string.join(",")` would be syntactically invalid (no method
call on an unimported module); UFCS-style `xs.join(",")` would
require `import string` in scope and would lose the explicit
module qualifier the audit prefers. Phase 1 (list) had the same
finding.

## Spec ambiguities or interpretive choices

### How did you treat `string_trim_left_loop` / `string_trim_right_loop`?

**Demoted to private (`fn` instead of `pub fn`).** The audit's
PR 2 description names them as `pub` exports today but they are
loop helpers used only by `string_trim` itself; the file's own
header comment described them as "Cross-file dependency: ...
call `ch_is_space` from `core/char.kai`" — a concession to the
prelude-collection order, not a documented user surface.

Searched for any external caller:

```
grep -rn '\bstring_trim_left_loop\b\|\bstring_trim_right_loop\b' \
  --include="*.kai" \
  | grep -v stdlib/core/string.kai
```

No hits outside the file itself. Demoting them to `fn` removes
two no-longer-public flat names from the surface. They are NOT
counted as "migrated" in the inventory (they retired, did not
move).

### How did you handle stage 0 builtin call sites in mixed-form consumer files?

`stdlib/encoding/json.kai`, `stdlib/decimal.kai`, and
`examples/stdlib/list_*.kai` all mix:

- Stage 0 PRELUDE calls (`string_concat`, `int_to_string`,
  `string_length`) — KEPT as flat names.
- Stdlib export calls (`string_join`, `string_repeat`) —
  migrated to `string.join` / kept as `string_repeat` per the
  surviving-alias rule.

Result is files like `stdlib/encoding/json.kai:96`:

```
JArr(xs) -> string_concat(string_concat("[", string.join(map(xs, json_encode), ",")), "]")
```

Which mixes flat (`string_concat`) and qualified (`string.join`)
calls in the same expression. This is not a regression — the
audit Risk 5 explicitly accepts it as the post-m14 steady state.
Future cleanup of PRELUDE-table names is a separate milestone.

### `string_join` collision with PRELUDE

The stdlib `pub fn join` has the same name and arity as the
PRELUDE `string_join` builtin. After the migration, the stdlib
`join` is what `string.join(...)` resolves to (via
`EModCall("string", "join")`); bare `string_join(...)` calls
still resolve to PRELUDE. Both implementations have the same
observable behaviour for `[String], String -> String`. Verified
by running tier1 (which exercises `examples/stdlib/jwt_encoder.kai`
and similar consumers) — all tests passed.

## Subjective summary

**Confidence:** HIGH for the 4 migrated symbols. The surviving
`string_repeat` alias is unavoidable given today's typer; the
inline comment + #219 hand-off make the rationale explicit.

**Hardest:** Diagnosing the `list.repeat` failure. The error
message gave a concrete location and concrete signature mismatch
(string vs int args), but it took reading
`stage2/compiler.kai:22984` and the comment about "v1 shares the
symbol with legacy unqualified callers" to understand it was the
typer side, not codegen, that was at fault. Once the gap was
identified, the fix decision (keep `string_repeat` as alias)
was straightforward.

**Easiest:** The consumer migration itself. A perl one-liner
across 11 files, then verified by `grep -rn` that no flat names
remained outside the surviving-alias case. Sed/perl on
`\bstring_foo\b` boundaries is safe for this surface because
none of the names are substrings of other identifiers in
practice.

**Compiler help:** The
"warning: bare name 'repeat' is exported by multiple modules"
warning was useful in pinpointing the collision; the subsequent
type-mismatch error was where the actual story lived.

**Compiler hinder:** None notable. The pre-existing typer
limitation is a known shortcoming, not a surprise.

## Limitations of this report

- Did not benchmark whether the type-mismatch error message
  could be improved (e.g. naming both `string.repeat` and
  `list.repeat` schemes when both are in scope). That polish
  belongs to #219, not to this PR.
- Wall clock figures are dominated by `tier1` (264s) and tier1
  re-run after revert; no attempt was made to subset
  `make test-stdlib` to shorten the cycle, even though the
  Phase 1 retro flagged that as a target for future PRs.
- The 4-of-5 outcome is a partial win. A follow-up PR after
  #219 lands would need to remove the surviving alias plus
  migrate `stdlib/decimal.kai:183`. Estimated effort: <10 min.

## Build/test TSV (appended)

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T00:06:49-04:00	tier0-baseline	OK	34
2026-05-04T00:08:53-04:00	tier0-postmig	OK	34
2026-05-04T00:11:36-04:00	tier1	FAIL	133
2026-05-04T00:14:07-04:00	tier0-revert	OK	35
2026-05-04T00:18:36-04:00	tier1	OK	264
2026-05-04T00:19:31-04:00	tier1-asan	OK	49
2026-05-04T00:19:56-04:00	selfhost	OK	19
```
