# Lane experience — issue #260 + #261 (extern "C" fn + name override)

## Objective metrics

- Lane span: 2026-05-05T02:48:49−04:00 → 2026-05-05T03:12:42−04:00
  (~24 minutes, well under the 3.5 h budget — the change shape was
  much smaller than estimated because the lowering tag mechanism
  already existed; only the parser front and an AST-field shuffle
  were new).
- Build TSV (each green on first try):

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T02:51:39-04:00	tier0_baseline	OK	-
2026-05-05T03:05:39-04:00	tier0_post_impl	OK	-
2026-05-05T03:10:50-04:00	tier1	OK	-
2026-05-05T03:11:50-04:00	tier1_asan	OK	-
```

- Selfhost convergence: byte-identical fixpoint on **first** kaic2
  rebuild after the lexer + parser changes. Both C and LLVM
  backends converged.
- Files touched: `stage2/compiler.kai` (~150 lines net), `docs/effects-stdlib.md`
  (FFI section rewritten), `stage2/Makefile` (one new test target +
  two `test` aggregate edits), 2 migrated fixtures
  (`examples/effects/ffi_extern_c_basic.kai`, `…/myffi.kai`),
  3 new fixtures + goldens under `examples/ffi/`.

## Diagnosis

Three sites had to change to switch surface syntax without
re-architecting the lowering:

- **Lexer** (`keyword_kind`, `tk_name`, `tk_is`, `TokKind`):
  add `extern` keyword. Strict additive — no other site reads
  the keyword table. ~5 lines.
- **Parser** (`parse_decl`): replace the legacy
  `parse_optional_extern_c_attribute` peek with two new branches:
  (a) reject the legacy `[<extern_c>]` attribute outright with a
  migration error (issue #260 hard rename); (b) recognise the
  new `extern "C" [( "..." )] [pub] fn ...` shape via a new
  `parse_extern_decl` that routes back into the existing
  `parse_axiom_decl` (with `extern_c=true` and an optional
  C-symbol override). ~140 lines added, 50 lines deleted (the
  legacy attribute helper).
- **Codegen** (`emit_fn_body`, `llvm_emit_fn`): one extra call
  to `ffi_extern_symbol_of(body, name)` so the call target inside
  the FFI shim picks up the override when present. ~6 lines
  changed across both backends.

The single AST field added (`c_symbol_override: String` on
`DAxiom`) cascaded to 18 match sites — purely mechanical
underscore-padding except the two destructuring spots that
actually consume the field (`dump_decl`, `lower_axiom_one`).

## Algorithm

### Parse rule

```
extern_decl := "extern" STRING_LIT [ "(" STRING_LIT ")" ] [ "pub" ] "fn"
               ident "(" params ")" ":" type [ "/" effect_row ]
```

The ABI literal is mandatory and must equal `"C"`. Other ABIs
trigger a parse error pointing the user at the only supported
ABI in MVP. The optional override symbol literal is validated
against `[A-Za-z_][A-Za-z0-9_]*` so a typo'd quote does not
silently link a symbol the user did not intend.

### Override resolution

Encoded into the existing magic ETodo body tag instead of a
parallel codegen channel. The lowering writes:

- `__kai_ffi__` when there is no override (symbol = kaikai
  identifier);
- `__kai_ffi__:<override>` when the parser saw
  `extern "C"("override")` (symbol = `<override>`).

The C and LLVM emitters call `ffi_extern_symbol_of(body, name)`
to pick the right symbol when synthesising the shim's `extern`
forward declaration and call site. The kaikai-side wrapper symbol
(the function name visible to other kaikai code) stays the
identifier — only the inner C-side call target changes.

This avoids splitting the `body_is_ffi_extern` discipline across
two pieces of state (a tag plus a string), which was the temptation
when first writing the parser.

### Migration mechanics

The legacy-attribute detection (`parse_at_legacy_extern_c_attribute`)
peeks at exactly the 3-token shape `[ < extern_c` so a bare `[` —
e.g. a list literal at the top of a const — does not trigger.
Anything inside brackets that starts with `<extern_c` produces the
hard migration error pointing at the new syntax. There is no
deprecated alias.

## Migration count

- In-tree FFI fixture sites migrated: 2
  (`examples/effects/ffi_extern_c_basic.kai`,
  `examples/effects/ffi_pub_axiom_cross_module/myffi.kai`).
- New fixtures: 3 under `examples/ffi/` (basic, name override,
  legacy syntax rejection).
- Out-of-tree consumers known to use the legacy syntax: `uira/`
  (kaikai-demo-raylib). The migration is mechanical and not part
  of this lane.

## Empirical verification

The headline `puts` + `strlen` example from the lane brief:

```kaikai
extern "C" fn puts(s: String) : Int / Ffi
extern "C"("strlen") fn c_strlen(s: String) : Int / Ffi

fn main() : Unit / Ffi + Stdout = {
  let _ = puts("hello")
  let n = c_strlen("hi")
  print(int_to_string(n))   # expect 2
}
```

Output (via `bin/kai run`):

```
hello
2
```

Both calls work: `puts` resolves to the kaikai identifier
verbatim; `c_strlen` links via the override symbol `strlen`. The
fixtures `examples/ffi/extern_c_basic.kai` and
`examples/ffi/extern_c_name_override.kai` lock this in for both
backends (C and LLVM) under tier1.

The legacy rejection sanity-checks via
`examples/ffi/extern_c_old_syntax.err.kai`:

```
error: `[<extern_c>] axiom` is removed (issue #260) — use `extern "C" fn name(...) : T / Ffi` instead
  --> examples/ffi/extern_c_old_syntax.err.kai:6:1
    |
  6 | [<extern_c>]
    | ^
```

## Friction points

- **Selfhost convergence on first attempt.** Lexer additions
  often ripple, but the new keyword name (`extern`) does not
  collide with any kaikai identifier in the compiler source, so
  the first kaic2 → C → kaic2-prime → C round was byte-identical.
  No iteration needed.
- **Old-syntax rejection ergonomics.** The 3-token peek
  (`[`, `<`, `extern_c`) is precise — list literals at the top
  of a const declaration (which always start with `[` followed
  by something that is *not* `<`) do not get false-positive
  diagnostics. Tested by running tier1, which exercises hundreds
  of fixtures with bracketed expressions.
- **AST field positioning.** Considered placing
  `c_symbol_override` between `extern_c` and `name` (positions
  3-4) versus between `name` and `tparams` (positions 4-5). Chose
  the latter: it reads as "the kaikai name first, then optionally
  what we link it to" which mirrors the surface syntax order.
- **Encoding the override in the magic tag.** Tempting to add a
  parallel field; doing it via the tag prefix kept the codegen
  side as a one-line `ffi_extern_symbol_of` call and avoided
  threading an extra state piece through `emit_fn_body`. The cost
  is one parse of the tag string at codegen time per FFI shim
  (negligible).

## Subjective summary

A lane where the design lever from PR #242 (the magic
`__kai_ffi__` tag) paid off: the override slotted into the same
mechanism with one extra parse-the-tag helper, no parallel
threading. The bulk of the work was mechanical — adding one
field to a sum-type variant and updating 18 wildcard match
sites. The new parser path is ~80 lines and the legacy attribute
helper deletion balanced most of it out.

The estimated 3.5 h was generous — actual was ~24 minutes
because (a) selfhost converged first try and (b) no typer or
lowering changes were needed on top of the parser front. Worth
recalibrating: lanes that only touch the parser surface and
reuse existing lowering machinery should budget closer to 1 h
than 3 h.

## Limitations

- Out-of-tree consumers (notably `uira/` for raylib bindings) need
  a mechanical migration: `[<extern_c>] axiom` → `extern "C" fn`,
  with the option of collapsing C-shim layers via the new
  `extern "C"("real_symbol") fn nice_name` override. Not part of
  this lane.
- ABIs other than `"C"` remain rejected. Adding `"C++"` or
  `"system"` would require both a parse-side allowlist and a
  codegen-side mangling story; out of MVP scope.
- Override validation is syntactic only (`[A-Za-z_][A-Za-z0-9_]*`).
  Whether the symbol actually links is still a build-time concern,
  not a parse-time one.
- The supported FFI type set is unchanged from PR #242: `Int`,
  `Real`, `Bool`, `String`, `Unit`. Pointers and compound structs
  are out of scope until a dedicated FFI doc lands.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T02:51:39-04:00	tier0_baseline	OK	-
2026-05-05T03:05:39-04:00	tier0_post_impl	OK	-
2026-05-05T03:10:50-04:00	tier1	OK	-
2026-05-05T03:11:50-04:00	tier1_asan	OK	-
```
