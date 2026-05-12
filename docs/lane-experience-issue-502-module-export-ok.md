# Lane experience report — issue #502 (constructor name collision breaks pub type export)

## Goal

Fix #502: a module declaring `pub type Respuesta = Ok(String) | Other`
breaks export of OTHER `pub type`s in the same module. The compiler
reports the unrelated type as not exported AND points errors at
nonexistent line numbers in the user file.

## Scope as planned vs as shipped

**Planned:** qualify constructor entries by their containing type
(`Respuesta.Ok` vs `Result.Ok`) at module-export resolution; restore
correct source positions in diagnostics.

**Shipped:** detect the collision up front and refuse the declaration
with a precise diagnostic. The full overloading story (allow
`Respuesta.Ok` and `Result.Ok` to coexist in the same env) is **not**
shipped — see *Why not the planned approach* below.

The user-visible effect matches the workaround the issue ships: rename
the variant. The compiler now tells you *why* and *where* instead of
producing six unrelated type-mismatch errors at fictitious line
numbers in the user file.

## What the bug actually was

`add_builtin_variant_sigs` registers four entries in the TyEnv at
program start:

```
Some : ∀a.    a -> Option[a]
None : ∀a.       Option[a]
Ok   : ∀e,a.  a -> Result[e, a]
Err  : ∀e,a.  e -> Result[e, a]
```

These are bare names — `ty_env_lookup("Ok")` returns the
`Result.Ok` scheme. Lookups by bare name are how every prelude
construction site (`stdlib/protocols.kai`, `stdlib/core/result.kai`,
…) resolves `Ok(...)` and `Err(...)`.

When a user declares `pub type Respuesta = Ok(String) | Other`,
`add_variants_loop` (stage2/compiler.kai:24662) calls
`ty_env_add(env, "Ok", scheme_user)` which **prepends** a new entry
under the bare `"Ok"` key. `ty_env_lookup` walks newest-first, so from
this point on every reference to `Ok` — *including the prelude's own
calls to `Ok(s)` inside `BinSerialize for String::from_bytes` etc.* —
resolves to `(String) -> Respuesta` instead of `(a) -> Result[e, a]`.

The cascade of errors on the user file with line numbers in the 600+
range (`/tmp/repro/main.kai:678:28`) is `report_one_error` reporting
the diagnostic against the *root file* path while the failing AST node
is actually a prelude line — the file/line accounting is not
provenance-aware once the prelude and user decls are concatenated.
That's a second bug, but it's a *symptom* of the first: only when the
prelude type-checks against the wrong scheme does the cascade emit
those misattributed locations.

`Nota` "stops being exported" is also a symptom: `Nota` is fine, but
because the typer aborts after the cascade, the `dominio.Nota`
reference in `main.kai` never reaches a successful resolution, and
later passes that look at the failed-typing AST report `Nota` as
unknown (or, in the `let t : dominio.Nota = ...` form, the
module-export message that the issue captured).

## Why not the planned approach (qualify constructor entries)

The brief proposed registering user variants under
`Respuesta.Ok` and disambiguating call sites by expected type. That
is the *right* long-term fix and lines up with how `DFn` already
registers under both bare and `<mod>::<fname>` keys (issue #219). Two
reasons it's not in this lane:

1. **It needs constructor overloading at use sites.** When a prelude
   line writes `Ok(s)` with no type annotation, the typer has to pick
   between `Result.Ok` and `Respuesta.Ok` from the call's expected
   type. That's a bigger change than an env-build fix — it touches
   `synth_call`, `synth_match` arm typing, and the disambiguation of
   pattern-position constructors. The current `EModCall` mechanism
   only fires for explicitly qualified `mod.name(...)` syntax;
   variant constructors don't go through `EModCall`.

2. **#502, #503, and #504 all need the *visible* failure mode to
   change first.** Today these bugs produce wrong-file/wrong-line
   diagnostics that mislead users about which file is broken. A
   targeted rejection at the offending DType eliminates that
   misdirection at no cost to the rest of the language. If/when the
   constructor-overloading work lands, the rejection becomes redundant
   and can come out, but until then it's the difference between
   `dominio.kai:3:5` and `main.kai:678:28`.

The fix is intentionally **resolver-only** per the brief constraint.
No syntax change, no module-export semantics change.

## The implementation

stage2/compiler.kai:24858 onward:

- `validate_union_collisions_decls` already existed for D2 (two user
  unions claiming the same component name). Pre-seed its claim list
  with synthetic `VariantClaim`s for the four builtin variants
  (line/col 0/0 sentinel).
- `validate_union_collisions_loop` now threads the module table so it
  can resolve a colliding DType's source file. Without this, the
  diagnostic anchors on the root file path even when the offending
  decl came from an imported module — which would *recreate the
  wrong-file complaint from the original bug report.*
- `report_builtin_collision` is the new diagnostic helper; the
  collision result branches on `prev.line == 0` to choose between the
  user-vs-user (D2) and user-vs-builtin messages.

The change is ~50 lines of stage2 source and one new test target.
Stage 1 (`stage1/compiler.kai`) is unchanged — the validator is a
stage-2-only pass and stage 1 is allowed to compile collision-bearing
sources without diagnosis (it just produces the same buggy output the
old stage 2 produced). Tightening stage 1 here is unnecessary: stage 1
is a stepping stone to stage 2, and stage 2 is what users hit.

## Coverage / fixtures

Three new fixtures under `examples/modules/`:

1. `export_constructor_collision/` — multi-file: `dominio.kai`
   declares `Nota` first, then `Respuesta` with renamed constructors
   (`OkResp` / `OtherResp`). `main.kai` constructs both. Verifies
   that the **un-collided** version exports cleanly — the lane brief's
   "after the rename, both types export" expectation. Golden:
   `id=1 s=done`.
2. `export_constructor_collision_reverse/` — same shape but `Respuesta`
   declared first, `Nota` second, exercising the order-doesn't-matter
   property the bisect surfaced.
3. `export_ambiguous_constructor.kai` + `.err.expected` — single-file
   negative fixture asserting the diagnostic substring. The `.err.expected`
   file holds the first line of the expected diagnostic; the test
   verifies kaic2 exits non-zero AND emits that substring on stderr.

Wired into `stage2/Makefile`'s `test-modules-collision` target,
included in the default `test` and `test-fast` targets, listed in
`.PHONY`.

## Bisect findings (worth recording)

- Renaming `Ok` → `OkOther` makes the entire program compile cleanly.
  Confirms the bug is *only* the constructor-name collision, nothing
  about the `pub type Nota` shape.
- Dropping `Nota` entirely from `dominio.kai` does **not** fix the
  cascade. The `Respuesta = Ok(String) | Other` decl alone, on its
  own, breaks the prelude. So #502's "Nota disappears" framing is a
  symptom of the cascade aborting later passes; the real fault is
  upstream of any `pub type Nota` consideration.
- Reversing the order (`Respuesta` declared *before* `Nota` in
  `dominio.kai`) is also broken. Order doesn't matter; what matters
  is that any `Ok(_)` user variant exists.

## Overlap with #503 and #504 — partial only

I checked whether the same fix closes the sibling issues:

- **#503** (`#derive(Show)` blocks `pub type` export). Does **not**
  share a root cause. `#derive` is a separate annotation-driven path
  through `lower_protocols`; the failure mode is different (the
  exported `pub type` simply isn't in the module's export table after
  `lower_protocols` rewrites the decl). #503 should be diagnosed
  separately by tracing how `DDerive` interacts with
  `collect_pub_exports`.

- **#504** (`Tree` user type collides with stdlib's `Tree[k, v]`).
  Same shape as #502 but at the **type** level, not the constructor
  level. The user's `type Tree = Leaf | Node(...)` and the stdlib's
  `pub type Tree[k, v] = TEmpty | TNode(...)` both register under
  bare `Tree`, and the typer picks one by insertion order. The fix
  pattern would be analogous to this lane's: detect the collision in
  a pre-typer validation pass, emit a precise diagnostic at the
  user's DType, and tell the user to rename. The `top_names`
  collection mechanism in `validate_union_collisions_decls` already
  has the data needed; the #504 lane could either extend this
  validator or write a sibling validator for type-level collisions.

So: #502 is closed by this PR, #504 has a clear template to follow,
#503 is independent.

## Real cost vs estimate

Brief estimated 4-8 hrs. Actual: ~3 hrs.

Most of the time was reading enough of the typer to be sure that
"reject at validate_union_collisions" wasn't going to break a valid
program. The stage2 source is large (53k lines); `add_variants_loop`,
`add_builtin_variant_sigs`, and `validate_union_collisions_decls` were
all already in the right neighborhoods. The hard part was deciding
*not* to do the qualified-key fix — which is the right long-term
answer but a much bigger lane.

## Follow-ups

1. **Constructor overloading at use sites.** When two `pub type` decls
   want to share a constructor name (`Respuesta.Ok` and `Result.Ok`),
   the typer should pick the right scheme from the expected type.
   Required for the issue's natural `pub type Response = Ok(String) |
   ServerError | NotFound` pattern to work. Touches `synth_call`,
   `synth_match`, and pattern resolution. Estimate: 2-3 days.

2. **Provenance for diagnostic file paths.** The
   `report_one_error(path, ...)` calls in the cascade attribute every
   error to the *root file path* even when the failing AST node came
   from an imported module. This lane works around the symptom by
   refusing the collision before the cascade fires; the underlying
   provenance gap would still surface in any other cascade originating
   from a prelude line. Worth its own audit lane.

3. **#503 and #504 in their own lanes.** Don't bundle.

## Things this lane intentionally did NOT do

- Did **not** change `DType`'s constructor (no `Option[String]` for
  module origin like `DFn` has). The 53 pattern matches on `DType`
  across the source make that a multi-day refactor; the
  `resolve_decl_file` helper using the module table covers the same
  need for this lane's diagnostic.
- Did **not** touch stage 1. Stage 1 is fine compiling these sources
  (it produces the same buggy output stage 2 used to); stage 2 is
  where the user-facing diagnostic lives.
- Did **not** touch `CHANGELOG.md` or `VERSION` (per CLAUDE.md).
