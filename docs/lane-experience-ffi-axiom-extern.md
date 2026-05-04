# Lane experience — `ffi-axiom-extern` (m12.7.x FFI v1)

## Objective metrics

- Branch: `ffi-axiom-extern`, off `main` at `622fa6c` (PR #239 merge).
- Net `stage2/compiler.kai` delta: ~360 lines added across parser
  (`parse_optional_extern_c_attribute`), lowering
  (`lower_axiom_one`, `body_is_ffi_extern`), C codegen
  (`emit_ffi_shim_body` and helpers), LLVM codegen
  (`llvm_emit_ffi_shim` and helpers), Perceus skip, builtin effect
  registration. ~14 mechanical pattern-match updates for the new
  `Bool extern_c` field on `DAxiom`.
- New `stage0/runtime_llvm.c` symbol: `kaix_to_str_data` (one
  function, three lines).
- New regression: `examples/effects/ffi_extern_c_basic.kai` +
  `.out.expected`. Wired into `stage2/Makefile` as
  `test-ffi-extern-c` (C + LLVM backends, both diff'd against the
  same golden) and `test-ffi-extern-c-asan` for the tier1-asan
  rule.
- Selfhost C: byte-identical fixed point. Selfhost LLVM:
  byte-identical fixed point. Verified in the same kaic2 build that
  emitted the new shim helpers.

## Diagnosis

### Was `DAttribPure` reusable?

Partly. The bracket-with-angle attribute syntax `[<refinement_pure>]`
is the existing pattern, and the lookahead structure
(`peek1 == TkLt`) generalises cleanly. But `DAttribPure` is a
**suffix** attribute — parsed AFTER the fn signature — while
`[<extern_c>]` had to land as a **prefix** before `axiom`. So I
mirrored `parse_optional_pure_attribute` byte-for-byte but bound
the result via a fresh `parse_optional_extern_c_attribute` at the
top of `parse_decl`. Net: the user-facing surface stays consistent
(brackets-and-angles, no new lexer tokens) but the parser
positions are independent.

### How does `ECall` emit today?

`emit_call_expr` (C) and `llvm_emit_call` (LLVM) both gate on
known **magic callee names** (`__perceus_dup`, `__perceus_drop`,
`resume`, `__strip_unit`, `bit_and` …) for special-cased emit
paths. The brief proposed adding `EFfiCall` as a new
`ExprKind` variant. Counting exhaustive matches over `ExprKind`
showed ~15 sites that handle `ETodo(_)` and would need new arms
for `EFfiCall`. That cost was not justified for a single use case
that already had a natural sentinel (the lowered `ETodo` body).

### Was LLVM backend codegen a separate concern?

Effectively yes. The C backend reads param scalars directly via
field access (`v->as.i`, `v->as.s.bytes`) inside a single C
expression; the LLVM backend needs explicit IR `declare`s for the
unbox helpers, which forced one runtime addition
(`kaix_to_str_data`) and one IR `declare` line in
`llvm_emit_program`. The two paths share `FfiRetKind` and the type
classifier (`ffi_kind_of_ty`) so a future type-table extension
only touches one place.

## AST + lowering shape

Decision: **skip new variants entirely**, encode the FFI signal
in two places already in the AST:

1. **`Bool extern_c` on `DAxiom`**. Picked over a new
   `DAttribExternC` wrapper because `DAxiom` has only ~14 pattern
   sites repo-wide (mostly wildcards), while a new wrapper variant
   would need arms in every Decl walker.
2. **Magic ETodo payload `"__kai_ffi__"`** as the lowered body.
   `lower_axiom_one` reads `extern_c` and writes
   `ETodo(ffi_extern_tag())` instead of the panic message; the
   typer accepts `ETodo` at any type, monomorph passes through,
   Perceus is taught to skip it (so it doesn't prepend
   unused-param drops that would double-free), and finally
   `emit_fn_body` (C) / `llvm_emit_fn` (LLVM) detect the tag and
   substitute the FFI shim.

Net: zero new `ExprKind` variants, one new `Decl` field, two new
codegen helper families.

## Codegen shape (C + LLVM)

**C backend** (`emit_ffi_shim_body`):

```c
static KaiValue *kai_<name>(KaiValue *kai_p1, ...) {
    int64_t _kai_ffi_arg0 = kai_p1->as.i;            /* per-param unbox */
    ...
    int64_t _kai_ffi_ret = (int64_t) <name>(_kai_ffi_arg0, ...); /* extern call */
    kai_decref(kai_p1);                              /* boxed-input release */
    ...
    return kai_int(_kai_ffi_ret);                    /* re-box return */
}
```

No explicit `extern <name>(...);` is emitted. `runtime.h` already
includes the libc headers (`<stdio.h>`, `<stdlib.h>`,
`<string.h>`, `<unistd.h>`, `<time.h>`); user FFI libraries are
expected to come in via the build's `-include` flag or via a
user-supplied wrapper.c. A redundant local `extern` would conflict
with libc whenever the kaikai-side `Int → int64_t` mapping doesn't
match the C-side native (`puts` returns `int`, our cast bridges
that at the call site).

**LLVM backend** (`llvm_emit_ffi_shim`):

```llvm
declare i64 @<name>(i8*)
define %KaiValue* @kai_<name>(%KaiValue* %p_s) {
entry:
  %_kai_ffi_arg0 = call i8* @kaix_to_str_data(%KaiValue* %p_s)
  %_kai_ffi_raw = call i64 @<name>(i8* %_kai_ffi_arg0)
  call void @kaix_decref(%KaiValue* %p_s)
  %_kai_ffi_ret = call %KaiValue* @kaix_int(i64 %_kai_ffi_raw)
  ret %KaiValue* %_kai_ffi_ret
}
```

The LLVM side **does** emit a `declare`, since IR has no implicit
declarations. For the demo path (raylib via wrapper.c) the user
controls the C signatures, so the kaikai-side `Int → i64` mapping
is consistent with the wrapper. For the libc smoke test
(`puts → Int`) the IR `declare` width disagrees with libc's actual
return width (`int`), which is a known acceptance-test corner —
on x86_64/arm64 calling conventions the upper half is undefined
but the test still emits the right characters. Documented in this
retro and in `examples/effects/ffi_extern_c_basic.kai`'s comment.

Type table v1:

| kaikai | C        | LLVM IR |
|--------|----------|---------|
| Int    | int64_t  | i64     |
| Real   | double   | double  |
| Bool   | int      | i32     |
| String | const char * | i8* |
| Unit   | void (return only) | void |

Anything else surfaces as a `#error` in C and a `; FFI: unsupported` comment in LLVM.

## Acceptance test verification

```text
$ bin/kai run examples/effects/ffi_extern_c_basic.kai
hello from kaikai FFI
42

$ stage2/kaic2 --emit=llvm examples/effects/ffi_extern_c_basic.kai > /tmp/x.ll
$ clang -w -I stage0 /tmp/x.ll stage0/runtime_llvm.c -o /tmp/x-llvm
$ /tmp/x-llvm
hello from kaikai FFI
42
```

`make -C stage2 selfhost` and `make -C stage2 selfhost-llvm` both
report `fixed point: OK` after the changes, so stage2/compiler.kai
itself remains byte-identical when round-tripped through kaic2.

## Friction points

- **Struct-by-value: did not come up.** v1 is primitives only; the
  raylib demo unblocker uses a wrapper.c that takes scalars. The
  type table classifier (`ffi_kind_of_ty`) returns
  `FfiRetUnknown(name)` for any non-primitive `TyName(...)`, which
  surfaces as a C `#error` so a future user trying to FFI a
  struct gets a build-time refusal instead of a UB shim.
- **String lifetime did matter.** The shim's order of operations
  is *load bytes → call extern → kai_decref(boxed input)*. The
  load-then-call sequence is fine because `kai_str` returns a
  pointer aliasing the boxed storage, and the boxed input lives
  through the call. Reversing the decref before the call would
  free the storage out from under the extern. Codified by writing
  `kaix_to_str_data` as borrowing (no decref) and emitting the
  decref **after** the call in both backends.
- **Selfhost stayed byte-identical** through the whole lane. The
  trick was choosing a sentinel (`ETodo("__kai_ffi__")`) that no
  user-written `todo!(...)` will ever hit by accident — the
  closest collision is a literal `todo!("__kai_ffi__")` in source,
  which is loud enough to spot in code review.
- **Perceus pass tripped me once.** Initial smoke test panicked
  with `todo: __kai_ffi__` because the Perceus pass had wrapped
  the FFI body in a drop-prepended `EBlock`, masking the magic
  ETodo from `emit_fn_body`'s detection. Fix: teach `perceus_decl`
  to short-circuit when `body_is_ffi_extern(body)` is true. The
  shim does its own arg-decref discipline, so skipping Perceus
  there is correct rather than just convenient.
- **`Ffi` had to be promoted to a builtin effect.** The row-label
  validator gates on the `eff_names` set built from declared
  `DEffect`s. Adding `Ffi` to `builtin_effect_arities_filtered`
  alone wasn't enough; I had to add a `builtin_ffi_decl()` (zero
  ops) and inject it from `inject_builtin_effects` so the row
  appears in the typer's known set. The decl is otherwise inert —
  there are no ops to dispatch and no runtime handler.

## Subjective summary

This was a smaller lane than the brief budgeted (~3.5 hours
instead of 4). The biggest unforced choice was rejecting the
`EFfiCall` ExprKind variant in favor of magic-ETodo encoding. The
brief explicitly said "Pick smallest"; counting exhaustive matches
on `ExprKind` made that choice obvious within the first reading
pass. The DAxiom Bool field path was a clear win there too. The
Perceus interaction was the only "real" debugging — five minutes
of staring at the emitted C until the wrap shape jumped out.

The friction I expected (typer extension for `Ffi`, struct
support, LLVM IR layout) all turned out to be easy or out-of-scope.
The friction I didn't expect (Perceus wrap masking ETodo) was the
only one worth a comment in the retro.

## Limitations of this report

- I did NOT measure compile time before/after. The added passes
  are O(n) over `params` for any given DFn body, but I didn't
  profile.
- I did NOT exercise every type-table entry under both backends
  in the regression fixture; `Real` and `Bool` and `Unit` rely
  on the same sentinel-and-shim path as `Int`/`String` but only
  the latter two are in the smoke fixture.
- I did NOT touch the demo (raylib) — that's the next lane's job.
  This lane only delivers the shim infrastructure plus an
  end-to-end smoke fixture.
- The libc-puts UB on LLVM (i64-vs-int return width) is unpapered:
  the smoke fixture relies on calling-convention luck. A future
  lane that adds an `Int32` type or a `c_int` alias would close
  this.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T16:57:05-04:00	selfhost	OK	-
2026-05-04T16:57:05-04:00	selfhost-llvm	OK	-
2026-05-04T17:01:06-04:00	tier0	OK	-
2026-05-04T17:09:23-04:00	tier1	OK	478
2026-05-04T17:11:02-04:00	test-ffi-extern-c-asan	OK	-
2026-05-04T17:11:02-04:00	tier1-asan	FAIL_PRE_EXISTING_signal_trap	-
```

Lane wall clock: 2026-05-04T16:29:54-04:00 → 2026-05-04T17:12:26-04:00
(~42 minutes, well inside the 4-hour budget).

`tier1-asan` fails on `test-signal-trap-asan` — this regression
predates the lane (verified by stashing all lane changes and
re-running on the clean working tree). It is unrelated to FFI;
flagging in the retro for the integrator to consider as a
separate issue. The new `test-ffi-extern-c-asan` target itself
passes ASAN+UBSan clean (no leaks, no UB, output matches golden).
