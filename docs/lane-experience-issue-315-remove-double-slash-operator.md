# Lane experience — issue #315 (remove `//` integer-division operator)

## Objective metrics

- Lane wall time: 23:25:15 → 23:45:00 (~20 min, vs ~2h calibrated budget).
- Compiler delta: stage2/compiler.kai −41 / +18, stage1/compiler.kai −5 / +5
  (lexer error path swap), stage2/Makefile +18 / −7 (new test target +
  unbox harness rewording).
- Fixtures: 1 added (`examples/aspirational/issue_315_no_double_slash.{err.kai,err.expected}`),
  2 migrated (`examples/perceus/unbox_phase2_pow_idiv.kai` retargeted from
  `//` to `/`; `examples/effects/issue_78_runtime_name_shadow.kai`).
- Tier gates: tier0 OK, tier1 OK, tier1-asan OK, selfhost (C) byte-identical,
  selfhost-llvm byte-identical, test-issue-315-no-double-slash OK,
  test-unbox-phase2 (pow_idiv) OK.
- Build TSV appended at the bottom.

## Audit (count of operator usages found)

`grep -rn ' // \| //$\|^// ' stdlib/ examples/ demos/` excluding URLs and
comments produced 3 real operator sites and 1 string-literal site:

| Site | Kind | Action |
|------|------|--------|
| examples/perceus/unbox_phase2_pow_idiv.kai:27 | `b // 7`  | migrated to `/` |
| examples/perceus/unbox_phase2_pow_idiv.kai:29 | `d // a`  | migrated to `/` |
| examples/effects/issue_78_runtime_name_shadow.kai:19 | `x // y` | migrated to `/` |
| examples/stdlib/path_basic.kai:18 | `"a//"`, `"//b"` (string literal) | left alone |
| stdlib/ | (no operator usage)         | — |
| demos/ | (only `// ...` inside `#`-comment lines) | left alone |
| stage1/compiler.kai, stage2/compiler.kai | (no operator usage in source) | — |

The structural assertions in `stage2/Makefile` for the unbox phase 2
pow_idiv fixture were updated to grep for `kai_op_div(` (the post-#315
runtime helper name) instead of `kai_op_idiv(` — when both operands are
raw `Int`, the unbox pass now folds `/` to native C `/` via the existing
`op_is_raw_arith` arm, so neither `kai_op_div` nor `kai_op_idiv` should
appear inside the inner-loop body.

## Compiler sites removed (9 enumerated)

All in `stage2/compiler.kai` (line numbers refer to pre-edit positions):

1. **Token enum** (line 71) — `TkSlashSlash` removed from the operator
   discriminator family.
2. **Token name string repr** (line 126) — `TkSlashSlash -> "//"` removed
   from `tok_kind_str`.
3. **Lexer recognition** (lines 723–730) — `'/' '/'` no longer produces
   `TkSlashSlash`; instead emits `TkError` with the migration message
   `` `//` operator is removed (issue #315) — use `/` instead (Int / Int truncates) ``.
4. **`tk_is` comparison** (line 1743) — `TkSlashSlash` arm removed.
5. **`op_of_tok`** (line 2001) — `TkSlashSlash -> "//"` arm removed.
6. **Parser `parse_mul_rest`** (line 2303) — `TkSlashSlash` removed from
   the multiplicative-operator predicate.
7. **Typer `synth_binop`** (lines 26461–26465) — the `op == "//"` branch
   that pinned both operands to `Int` and returned `TyInt` was removed.
8. **C-emit `binop_cname`** (line 10520) — `else if op == "//" { Some("kai_op_idiv") }`
   removed (the helper itself stays in `stage0/runtime.h` — it remains
   reachable via the runtime-name-shadow fixture's `idiv` user fn).
9. **C-emit `emit_kind_raw`** (lines 13416–13423) — the `op == "//"`
   branch that emitted native C `/` was removed; the raw-Int `/` case
   now lands in the existing `op_is_raw_arith` arm above it.

Plus the LLVM mirrors (lines 35260, 35729, 35979, 36031) — the
`@kaix_idiv` helper declaration, the `llvm_binop_helper` lookup, the
`or op == "//"` mode test, and the dedicated `sdiv i64` branch in
`llvm_emit_binop_raw`. `llvm_raw_arith_op` already maps `/` to
`sdiv i64` for the Int case, so no new code was needed.

Plus the unbox-pass `decide_mode` branch (line 32141) and three comment
sites (around line 31985 + the synth_binop preamble + the unbox-pass
preamble) that referenced `//` / `kai_op_idiv` — rephrased to mention
issue #315 instead.

`stage1/compiler.kai` mirrored the surface-level removals (token enum,
token name, lexer, `tk_is`, `op_of_tok`, `parse_mul_rest`, `binop_cname`).
Stage 1 has no typer / unbox / llvm passes, so the inventory there is
narrower.

## Migration count (stdlib + examples + demos)

- stdlib: 0 operator usages.
- examples: 3 migrated, 1 string-literal left alone, 1 comment-only
  occurrence in `examples/stdlib/path_basic.kai` left alone.
- demos: 0 operator usages (2 hits inside `#`-comments in
  `demos/vs/java/main.kai`, `demos/vs/kotlin/main.kai` — explanatory
  text, not source code).

## Error fixture shape

`examples/aspirational/issue_315_no_double_slash.err.kai`:

```
fn main() : Unit / Stdout = {
  let q = 20 // 6
  print(int_to_string(q))
}
```

`examples/aspirational/issue_315_no_double_slash.err.expected`:

```
`//` operator is removed (issue #315) — use `/` instead (Int / Int truncates)
```

The fixture is wired through a fresh `test-issue-315-no-double-slash`
target in `stage2/Makefile`, modelled on the pre-existing
`extern_c_old_syntax` (issue #260) one-off pattern: the target shells out
to `kaic2`, asserts the binary exits non-zero, and `grep -F`s the first
line of the expected file out of the captured stderr. Wired into the
`.PHONY`, `test`, and `test-fast` umbrella targets so it runs under the
default `make tier1` cadence.

## Empirical verification (selfhost byte-identical)

- `make tier0` → tier0 OK; selfhost (C) reached fixed point on the first
  iteration; demos baseline 26/26 holds (`forth` and `mini_ledger` are
  pre-existing baseline failures that were already counted).
- `make tier1` → full `make test` + demos no-regression + fmt fixtures +
  bench smoke + check smoke: all green.
- `make tier1-asan` → all ASAN gates pass (m12.7.x FFI, issue #118, #91,
  http client, crypto, regex, env mutate, securerandom).
- `make -C stage2 selfhost-llvm` → `llvm self-hosting fixed point: OK`
  on the first iteration.
- `make -C stage2 test-unbox-phase2` → all unbox phase 2 fixtures green,
  including the rewritten `unbox_phase2_pow_idiv` (output `2027`, same as
  before the migration).
- `make -C stage2 test-issue-315-no-double-slash` → `issue_315 OK
  (rejection diagnostic matches)`.

No selfhost iteration was needed — the lex/parse/typer surface change
is mechanical and produces the same downstream IR for all surviving
operator sites.

## Friction points

- The `unbox_phase2_pow_idiv` fixture's whole *purpose* is asserting
  that `//` lowers to native C `/`; after #315 the test still
  exists but the operator under test is `/`. The structural grep in
  the Makefile had to be retargeted from `kai_op_idiv(` to
  `kai_op_div(` (the post-#315 surviving helper name) so that the
  assertion still has teeth — when both operands are raw `Int`, the
  unbox pass folds `/` to native C `/` via the existing
  `op_is_raw_arith` arm, so neither helper should appear in the inner
  loop. Decided to keep the fixture rather than retire it: it still
  catches a regression where raw-Int `/` accidentally falls back to
  the boxed `kai_op_div` helper.
- `stage0/runtime.h` and `stage0/lexer.c`/`emit.c`/`parser.c` still
  recognise `TK_SLASH_SLASH` and define `kai_op_idiv`. Per the lane
  brief, stage 0 is out of scope (it must compile from any system
  with bare `cc`, and the bootstrap path no longer feeds `//`-carrying
  source into it once stage 1 is rebuilt). Left alone — the dead
  branches will be cleaned up when stage 0 is next refreshed.
- The pre-existing `extern_c_old_syntax` target was a great template
  for the new error-fixture wiring; copying its shape kept the new
  Makefile rule small.

## Subjective summary

Calibrated 2h budget, actual ~20 min — the fixture audit revealed
only 3 operator sites in tree, and the compiler removal was tightly
inventoried in the issue itself (9 sites, all syntactically narrow).
The risky-looking `unbox_phase2_pow_idiv` retarget went smoothly
because `/` and `//` already produced byte-identical IR on raw `Int`
operands; the structural assertion just needed a one-token rewrite.
Selfhost converged on iteration 1 on both backends.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T23:36:36-04:00	tier0	OK	-
2026-05-06T23:41:27-04:00	tier1	OK	-
2026-05-06T23:42:21-04:00	tier1-asan	OK	-
2026-05-06T23:44:45-04:00	selfhost-llvm	OK	-
2026-05-06T23:44:45-04:00	test-issue-315	OK	-
```
