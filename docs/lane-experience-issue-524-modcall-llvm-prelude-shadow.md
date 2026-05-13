# Lane retro — issue #524 (LLVM EModCall vs. prelude shadowing)

## Scope

LLVM backend bug: `llvm_emit_call`'s `EModCall(mod, fname)` arm
checked `llvm_is_prelude_fn(fname)` before honouring the qualifier.
When `fname` happened to match a prelude entry (e.g. `read_bytes`,
which the prelude exports for stdin), the call was routed to the
prelude symbol (`@kaix_prelude_read_bytes`) instead of the module's
own export (`@kai_file__read_bytes`). The C backend never had this
bug because its EModCall lowering goes through `c_sym(fname,
Some(mod))` first.

## Bug confirmation

```kai
import fs.file
fn main() : Int / Stdout + File = match file.read_bytes("missing.txt") {
  Err(_) -> { print("err"); 1 }
  Ok(_)  -> { print("ok");  0 }
}
```

- `KAI_BACKEND=c` → `err` (correct, file does not exist).
- `KAI_BACKEND=llvm` → `panic: non-exhaustive match at 2:35`,
  exit 0 with no stdout. The match scrutinee was an `Array[Byte]`
  (Stdin's return type) instead of `Result[String, Array[Byte]]`,
  so neither arm fit and the typer-trusted match fell through.

## Fix shipped

Single-site change in `stage2/compiler.kai` (~44648):

```diff
 EModCall(mod, fname) -> {
   let ra = llvm_emit_args(e, xs, [])
-  let sym = if llvm_is_prelude_fn(fname) { llvm_prelude_symbol(fname) }
-            else                         { string_concat("@kai_", c_sym(fname, Some(mod))) }
+  let sym = string_concat("@kai_", c_sym(fname, Some(mod)))
   ...
 }
```

The qualifier is authoritative: if the user wrote `mod.fname`, the
resolver already proved `mod` exports `fname`, so the prelude
shortcut must not fire. The shortcut stays valid for the unqualified
`EVar(fname)` path (a different arm of `llvm_emit_call`).

## Other names potentially affected

Audit of `llvm_prelude_table` against `pub fn` exports under
`stdlib/`:

- `stdlib/fs/file.kai` exports `read_bytes` / `write_bytes` —
  exact case from the issue. Now fixed.
- `stdlib/core/list.kai` exports `map` / `filter` / `reduce` /
  `each` / `flat_map`. All five are in the prelude table. Pre-fix,
  qualified `list.map(xs, f)` under LLVM would have routed to
  `@kaix_prelude_map`. This was masked in practice because callers
  who wanted the polymorphic prelude shape used the bare name; the
  qualified surface was rarer. The fix covers all five
  automatically.
- `stdlib/core/option.kai` and `stdlib/core/result.kai` export
  `map` / `filter` — same shape, same fix.
- `stdlib/os/process.kai` exports `exit` (qualified `process.exit`)
  vs. prelude `exit`. Same fix.

No other `pub fn` in `stdlib/` collides with a prelude entry by
name. `Env.args()` looks like a candidate but `Env` is an effect,
not a module — its op-call goes through the per-instance dispatch
path, not `EModCall`.

The fix is uniform: removing the prelude shortcut from the
EModCall arm covers every collision in one go.

## Selfhost behaviour

Selfhost stays byte-identical: stage 0 → stage 1 → stage 2 still
produces the same stage-2 C source post-fix. The compiler itself
does not call `file.read_bytes` (or any other module-qualified
prelude-shadowed name) qualified, so the emitted symbols don't
change for the bootstrap path.

`make tier0` green: 25 demo OK, 3 no-golden, baseline 28 (was 27
pre-lane — the fixture added in this lane bumps the baseline by 1).

## Fixtures added

- `examples/stdlib/fs_file_bytes_qualified.kai` — positive: missing
  file under `file.read_bytes(_)` returns `Err(_)`, prints `err`,
  exits 0. Picked up automatically by the wildcard
  `test-stdlib` runner under the C backend.
- `stage2/Makefile` `test-issue-524` target — explicitly compiles
  the same fixture through the LLVM backend (`--emit=llvm`) and
  diffs stdout against the same `.out.expected`. Wired into
  `test` and `test-fast` so tier1 runs it on every PR. Without
  this target the LLVM-side regression would not be exercised in
  CI (the wildcard `test-stdlib` runner is C-only, and the
  hardcoded `test-llvm` list does not include `examples/stdlib`).

## Doc updates

- `examples/stdlib/fs_file_bytes_llvm.kai`'s preamble references
  this collision as "filed as a follow-up". The follow-up is now
  closed; left the comment as historical context (the lane that
  shipped #513 wrote it). A pointer to #524 in the same comment
  could go in a doc-only follow-up, but it is not required by the
  doc discipline (the comment is true: that lane's scope did not
  include this fix).

## Cost

- Estimate: 0.5–1 day.
- Real: < 1 hour. The issue body specified the fix shape exactly;
  the only judgement calls were the audit (5 minutes — `llvm_prelude_table`
  has ~60 entries, very few collide with stdlib exports) and adding
  the LLVM-side regression target (the wildcard `test-stdlib`
  runner is C-only, so the new fixture would otherwise pass tier1
  without ever touching the LLVM emit path the bug lived in).

## Follow-ups

None. The bug shape is closed and the regression is wired into CI
under both backends.
