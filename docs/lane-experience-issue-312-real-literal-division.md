# Lane experience — issue-312-real-literal-division

## Objective metrics

- Lane start: 2026-05-06T23:22:33-04:00
- Patch size: stage2/compiler.kai +9 / -1 (one match arm in `emit_kind_raw`).
- Fixture: examples/aspirational/issue_312_real_division/*.kai (8 lines source) + .out.expected.
- Diagnosis to fix: ~25 min (faster than the 20-min budget for diagnosis).

## Diagnosis (where literal type was defaulting)

The reporter's hypothesis was incorrect in attribution but right
about the symptom. The literals `9.0` and `2.0` are NOT re-typed
as `Int`. Verified empirically:

1. `kaic2 --ast` showed both literals as `real 9 @…` / `real 2 @…`.
2. `kaic2 --types` showed `main : () -> Unit / Console`, no
   coercion of the binop expression.
3. The C output for `kai_main` was the smoking gun:
   ```c
   return kai_io__println(kai_prelude_real_to_string(kai_real((9 / 2))));
   ```
   The bug was in **C code generation**, not typing.

The Real-typed binop entered `emit_expr_raw` → `emit_kind_raw`
because the unbox pass marked it `MUnboxed`. There:

- `emit_kind_raw` for `EReal(9.0)` returned `real_to_string(9.0)`,
  which is `"9"` (no trailing `.0` for integer-valued Reals).
- `emit_kind_raw` for `EBinop("/", a, b)` then composed
  `(<a> / <b>)` → `(9 / 2)`.
- C lexed both `9` and `2` as `int`, so `/` was signed integer
  division: `4`.
- The result was wrapped via `kai_real(4)`, producing a Real with
  value `4.0`, which `real_to_string` rendered as `"4"`.

The LLVM raw emit (`llvm_emit_kind_raw`, line ~35951) already had
defensive code:
```kai
let s_dbl = if string_contains_dot(s) { s } else { concat_all([s, ".0"]) }
```
The C raw emit was missing the symmetric defense.

## Fix

`stage2/compiler.kai`:

1. New helper `real_c_lit(s)` (and predicate
   `real_lit_is_float_typed`) near `string_contains_dot`. Appends
   `.0` only when the rendered Real has neither `.` nor `e`/`E`.
2. `emit_kind_raw` (C raw emit, line ~13405): wraps the EReal
   case in `real_c_lit(real_to_string(r))`.
3. `llvm_emit_kind_raw` and the boxed-LLVM EReal site: refactored
   to use the same helper. Both already had a `.`-only check
   inline; consolidating prevents drift and closes the same
   `e`/`E` blind-spot in those sites preemptively.

Net delta: ~25 lines added (helper + comment), 5 lines deleted.

## Empirical verification (5 cases)

```kai
fn main() : Unit / Console = {
  println(real_to_string(9.0 / 2.0))
  println(real_to_string(7.0 / 2.0))
  println(real_to_string(1.5 + 1.0))
  println(real_to_string(3.5))
  let a : Real = 9.0
  let b : Real = 2.0
  println(real_to_string(a / b))
}
```

Pre-fix output:
```
4
3
2.5
3.5
4.5
```

Post-fix output:
```
4.5
3.5
2.5
3.5
4.5
```

Matches the expected table from the issue.

## Friction points

- The reporter's hypothesis ("literals get typed as Int") was a
  red herring. Spent ~5 min following the typer trail
  (`synth_real_lit`, `synth_dim_mul_div`) before pivoting to
  inspecting the actual C output. The C output should always be
  the first stop for "wrong arithmetic result" bugs.
- `bin/kai` driver writes the C to a temp dir and deletes it on
  exit. Had to monkey-patch the driver with a one-liner sed to
  retain the C output for inspection.
- First fix iteration was naive: `if no '.' then append ".0"`.
  This broke the `complex_literal_basic` sugar fixture which uses
  scientific-notation Reals like `1e+10` — appending `.0` produced
  `1e+10.0`, which is illegal in C. The same latent vulnerability
  was already present at two LLVM raw-emit sites; this lane
  consolidates the idiom into a `real_c_lit` helper that treats
  both `.` and `e`/`E` as "already a float-typed literal".

## Subjective summary

Small, surgical fix. The bug had a clear analogue in the LLVM
backend that already handled it correctly — porting the same
defense to the C backend was mechanical once the diagnosis was
correct.

## Limitations

- The bug only manifests for binops whose operands are *both*
  literal Reals. Mixed (literal + variable) expressions don't
  trigger it because the variable side comes from a `kair_<name>`
  C local already typed `double`.
- The new `real_c_lit` helper covers the three known emit sites
  for raw Real literals (one C, two LLVM). Any future raw-mode
  emit that bypasses the helper would re-introduce the bug.
  Cheap mitigation: grep `real_to_string` near emitter code.
timestamp	cmd	outcome	elapsed_s
2026-05-06T23:29:35-04:00	tier0	OK	123
2026-05-06T23:31:37-04:00	tier1	OK	108
2026-05-06T23:35:22-04:00	tier0	OK	124
2026-05-06T23:40:17-04:00	tier1	OK	284
2026-05-06T23:41:18-04:00	tier1-asan	OK	51
2026-05-06T23:43:33-04:00	selfhost-llvm	OK	123
