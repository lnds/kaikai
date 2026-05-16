# Lane experience report — issue #644 ctor overloading (cap 17)

Best-effort retrospective by the implementing agent.

## Goal

Close issue #644 — *typer: constructor overloading at use sites
(cap 17 Response.Ok — book ch17 blocked)*. Canonical repro:

```kai
pub type Response = Ok(String) | NotFound | ServerError(String)

fn build(code: Int, body: String) : Response = {
  if code == 200 { Ok(body) }
  else if code == 404 { NotFound }
  else { ServerError(body) }
}

fn show(r: Response) : String = match r {
  Ok(s)          -> "200: " ++ s
  NotFound       -> "404"
  ServerError(s) -> "500: " ++ s
}

fn main() : Unit / Stdout = println(show(build(200, "hello")))
```

Pre-fix: the m12.8.x diagnostic (PR #507, closing #502) refused the
declaration up front with *"constructor 'Ok' in 'type Response = ...'
collides with the built-in constructor 'Ok' of 'Result'"*. The book
ch17 HTTP server example could not type-check until users renamed
the variant (`OkValue` and friends). Plan B — pinned by the issue
body and validated by the prior #634 attempt — was the favored
route: keep the prelude bare-name table immutable, register user
variants qualified-only when the bare slot is occupied by a prelude
builtin, and disambiguate at use sites via scrutinee / annotation
context.

Plan A (Rust-style dual-key + bidirectional disambiguation in
`synth_call` / `synth_match` / pattern) was ruled out by the prior
#634 attempt: it required expected-type threading to be uniformly
available everywhere a bare ctor surfaces in the prelude, and the
threading is not uniform today. Plan B keeps the prelude bare
lookup pointing at the prelude scheme by construction; the rewrite
to a qualified `EModCall` only fires inside known annotation /
scrutinee contexts.

## Confirmation of Plan B

This lane confirmed Plan B in the first commit's body verbatim per
the brief's load-bearing gate. No Plan A escalation was needed —
the failure modes that bit during implementation (prelude
self-compile divergence, exhaustiveness double-counting, Phase 2
unboxing parity, ambiguity diagnostic firing on prelude code) were
all solvable inside Plan B's scope.

## Sitio del fix — every patch this lane landed

### Typer (`stage2/compiler.kai`)

- `add_variants_loop` (env-build, ~26262): registers user variants
  under `<tname>::<cname>` qualified key always; registers bare
  `<cname>` only when `bare_is_prelude_builtin_variant(env, cname)`
  is false (prelude builtin Ok/Err/Some/None ⇒ bare stays the
  prelude scheme; user variants must go through qualified key).
- `add_builtin_variant_sigs` (~25883): also registers qualified
  `Option::Some`, `Option::None`, `Result::Ok`, `Result::Err`
  alongside the bare entries so `check_expr_against`'s annotation-
  driven disambiguation has a uniform `<parent>::<cname>` lookup
  for builtin types too.
- `validate_union_collisions` (~26515 — `check_variants_for_collision`):
  the m12.8.x cross-module rejection (PR #507) is relaxed. The
  builtin-collision claim (VC with line/col 0 sentinel) is no
  longer a typed error — registration is structurally safe under
  Plan B. The D2 same-file user-vs-user case still fires.
- `check_variant_pattern` (~34673): scrutinee-driven ctor lookup.
  When `expected = TyCon(parent, _)`, the lookup tries
  `<parent>::<cname>` first, then falls back to bare. Match arms on
  `Response` resolve `Ok(s)` to `Response.Ok`.
- `check_expr_against` (~35266): new arms for `ECall`, `EVar`,
  `EBlock`. ECall/EVar route through `ctor_check_or_synth` /
  `nullary_ctor_check_or_synth` which rewrite a bare ctor callee
  to `EModCall(parent, cname)` when a qualified entry exists.
  EBlock threads expected into the block tail so a `{ ...stmts...;
  ctor(args) }` body benefits from the same disambiguation.
- `check_if` + `check_branch_for_ctor` (~35135): branches that are
  ctor-shaped (ECall(EVar)/EVar/EBlock-with-ctor-tail) get routed
  through `check_expr_against` so `if cond { Ok(body) } else { ... }`
  picks `Response::Ok` from the surrounding fn return annotation.
- `check_body_against_ret` (~35261): ECall/EVar bodies get the
  same routing; other shapes fall to `synth` as before.
- `try_bare_call_narrow` (~31446): skips ctor callees (uppercase
  first letter). `pick_by_first_arg` was preferring a concrete-
  first-param ctor scheme (e.g. `Response.Ok : (String) -> Response`)
  over a polymorphic one (`Result.Ok : (a) -> Result[e, a]`) when
  the receiver-narrow chose between two qualified candidates,
  silently re-routing prelude `Ok(x)` to `Response::Ok`. The skip
  is precise: receiver narrowing was designed for fn dispatch
  (`option.map` vs `list.map`), never for ctor resolution.
- `synth_stmt` SLet (~34361): unannotated SLet whose RHS is
  `EVar(c)` or `ECall(EVar(c), _)` runs the bare unannotated ctor
  ambiguity diagnostic via `report_ctor_ambiguity_if_any`. ECall
  and EVar RHS with annotation route through `check_expr_against`
  so the annotation context drives ctor disambiguation.
- New helpers (in topical order):
  `ty_env_strip_qualified_prefix`, `bare_is_prelude_builtin_variant`,
  `ty_env_qualified_parents` + `_loop` + `_excluding_builtin`,
  `report_ctor_ambiguity_if_any`, `filter_builtin_parents`,
  `format_ctor_candidates` + `_loop`, `ctor_needs_qualification`,
  `ctor_check_or_synth`, `nullary_ctor_check_or_synth`,
  `check_branch_for_ctor`.

### Walker / exhaustiveness

- `variants_of_type_loop_arity` (~35716): strips the
  `<target>::<cname>` prefix and emits the bare name so user
  variants registered only under qualified keys still surface as
  bare names in the exhaustiveness walker. Other qualified entries
  (`Eff.op` under `.` separator) remain filtered.
- `variants_of_type` + `variants_of_type_arity` (~35887, ~35906):
  dedup the entry walker's output before merging in the
  `env.unions` summary. Bare and qualified entries both produce
  the same ctor name in the result and must collapse.
- `check_variants_nullary_loop` (~36181 — `synth_variants_of`): same
  strip-prefix discipline as the exhaustiveness walker so
  `variants[Suit]()` returns each tag exactly once (caught the
  `sugars/variants_basic.kai` runtime panic where the typer
  returned `["Suit::Hearts", "Hearts", "Suit::Diamonds", "Diamonds",
  ...]`).
- `synth_variants_of` (~30698) dedup via `union_merge_unique` for
  the same reason — defense in depth against any walker that
  forgets the prefix-strip.

### Backends (C + LLVM)

- `emit_call_expr` (~13180 — C): `EModCall(mod, fname)` routes
  through the typed variant ctor path (`emit_variant_call_typed`
  → `kai_variant_u`) when `evar_payload_tys(variants, fname)`
  returns `Some(tys)`. The pre-existing bare-EVar ctor path
  (~13153) already used `emit_variant_call_typed`; the EModCall
  arm needs the same parity so a `Tree.Node(Int, Tree, Tree)` outer
  ctor (rewritten to `EModCall` by Plan B annotation propagation)
  matches the inner nested ctors' unboxed slot layout. Without
  this, the outer would box slot 0 as `kai_int(...)` while the
  match arm reads `slots[0].i64` raw, returning garbage. Caught
  by the pre-existing `user_redeclares_private_recursive_type`
  fixture under the new `test-shadowing` runner.
- `emit_expr_boxed` value-position EModCall (~16265 — C): when
  `fname` is a registered variant, emit `emit_variant_call(fname, 0, "")`
  for nullary or a stub for non-nullary. Mirrors the bare EVar
  value-position arm.
- `llvm_emit_call_expr` EModCall arm (~47105 — LLVM): short-
  circuits to `llvm_emit_variant_ctor(e, fname, xs)` when `fname`
  is a registered variant, otherwise mints
  `@kai_<mod>__<fname>(...)` as before.
- `llvm_emit_expr` value-position EModCall arm (~46427 — LLVM):
  nullary variant ctor short-circuit to `llvm_emit_variant_ctor`;
  otherwise the existing `llvm_emit_modcall_fn_value` path.

### Fixtures + runner

Seven fixtures under `examples/shadowing/`:

- `user_response_with_ok.kai` (`.out.expected`: `200: hello`) —
  canonical book ch17 repro.
- `user_response_match_arm_disambig.kai` — scrutinee-driven match
  arm resolution; helpers `ok_resp`/`created_resp`/`not_found`
  exercise annotation-driven construction.
- `user_response_annotated_construction.kai` — three `let r: Response = ...`
  bindings (with-payload + nullary + payload again).
- `bare_unannotated_ambiguous.kai` (+ `.err.expected`) — negative
  fixture asserting the new ambiguity diagnostic substring.
- `user_response_qualified_works.kai` — annotation route as the
  stable escape hatch (kaikai does not currently expose
  `<Parent>.<cname>(...)` qualified ctor syntax; the parser routes
  `Foo.bar` through UFCS).
- `prelude_integrity.kai` — `Result[String, Int]` paths the prelude
  itself uses; sanity gate against silent re-routing of prelude
  `Ok(...)` to user variants.
- `export_ambiguous_constructor.kai` (moved here from
  `examples/modules/`) — the original #502 negative fixture is now
  positive, with the prelude rejection removed and use sites
  disambiguating per Plan B.

`test-shadowing` target wired into `stage2/Makefile`'s `test` and
`.PHONY` lists. Same diff / golden discipline as `test-string` and
`test-pipes`; both positive (`<name>.out.expected`) and negative
(`<name>.err.expected`) shapes supported.

### Doc / Makefile delta

- `stage2/Makefile`: new `test-shadowing` target (~3955), added to
  `.PHONY` and `test` chains, and the old
  `modules-collision-neg` hard-coded test block was removed (its
  fixture moved to `examples/shadowing/` as a positive runner).

## Selfhost byte-identical

`make -C stage2 selfhost` reports *"self-hosting fixed point: OK"*
after every gate in this lane. The qualified-key registration and
new helpers do not perturb the emitter's view of compiler.kai
itself (the prelude's bare `Ok` / `Err` / `Some` / `None` lookups
all stayed on the bare entries; no rewrite fired during compiler
self-compile).

## Cross-reference with #643 and the #634 attempt

- **#643 (PR #646)** — privacy leak for `Tree` from `stdlib/
  collections/map.kai`. Lives in the variant-table arity
  discriminator (`variants_of_type_loop_arity` + sentinel) and the
  audit script `tools/audit-prelude-private-types.sh`. Zero overlap
  with this lane: #643 addresses non-`pub` prelude types leaking;
  this lane addresses `pub` prelude ctors colliding with user
  variants. Both fixtures land under `examples/shadowing/`; the
  runner picks them up via the same `test-shadowing` glob.
- **#634** — closed mis-scoped. That lane attempted Plan A
  (Rust-style bidirectional disambiguation) and surfaced the
  prelude-silent-divergence trap. This lane intentionally chose
  Plan B per the issue body, and the #634 retrospective stays as
  bitácora.

## Follow-ups

1. **Qualified ctor syntax (`<Parent>.<cname>(args)`)**. kaikai
   today routes `Foo.bar(x)` through UFCS, so the only stable
   disambiguation surface is annotation (`let r: Response = Ok("x")`).
   A dedicated qualifier form would close the surface gap the
   ambiguity diagnostic admits ("annotate the binding"). Touches
   the parser and `EField` resolution; deferred until a follow-up
   issue prioritises it.
2. **Ambiguity diagnostic for nullary ctors** in non-SLet
   positions. The diagnostic today fires only at `SLet` without
   annotation when the RHS is `EVar(c)` or `ECall(EVar(c), _)`.
   A bare `None` value-position use that silently resolves to
   `Option.None` while a user `type Toggle = None | Yes` is in
   scope does NOT fire. Coverage is intentionally narrow to avoid
   prelude noise — the prelude itself constructs many bare `None`
   / `Some` references inside annotated bodies where Plan B
   already disambiguates correctly. A broader diagnostic would
   need a post-typer walker that consults already-resolved types.
3. **Same-arity sum-type collisions in distinct stdlib modules**
   (e.g. `SplitN` in `stdlib/crypto/hash.kai`, `NB_FragR` in
   `stdlib/regexp.kai`). Inherited from #643's known-limitation
   skip-list. This lane does not touch them.
4. **Phase 2 unboxing parity audit**. The `EModCall` ctor path
   now mirrors the bare EVar ctor path, but the broader unboxing
   discipline (pipe targets, partial application, closure
   conversion) should be audited for any other EModCall paths
   that emit boxed `kai_variant` when the matching arms expect
   unboxed `kai_variant_u` slots. Not in scope here.

## Real cost vs estimate

Brief estimated 300-500 LOC stage2/compiler.kai. Actual:

| Component                                          | LOC |
|----------------------------------------------------|----:|
| `add_variants_loop` + `bare_is_prelude_builtin_variant` | ~30 |
| `add_builtin_variant_sigs` qualified entries       |  ~15 |
| `validate_union_collisions` relaxation             |  ~15 |
| `check_variant_pattern` scrutinee-driven           |  ~20 |
| `check_expr_against` ECall/EVar/EBlock arms        |  ~35 |
| `ctor_check_or_synth` + `nullary_ctor_check_or_synth` + `ctor_needs_qualification` | ~55 |
| `check_if` + `check_branch_for_ctor`               |  ~30 |
| `check_body_against_ret` ECall/EVar arms           |  ~10 |
| `synth_stmt` SLet ambiguity + annotation routing   |  ~40 |
| `try_bare_call_narrow` ctor skip                   |  ~15 |
| `ty_env_strip_qualified_prefix`                    |  ~15 |
| `variants_of_type_loop_arity` strip + dedup        |  ~40 |
| `variants_of_type[_arity]` dedup                   |  ~10 |
| `check_variants_nullary_loop` strip                |  ~15 |
| `synth_variants_of` dedup                          |   ~5 |
| `ty_env_qualified_parents*` + ambiguity diagnostic | ~110 |
| C backend EModCall variant arm + value-position    |  ~30 |
| LLVM backend EModCall variant arm + value-position |  ~25 |

Total ~515 LOC net new in `stage2/compiler.kai`. Within the brief's
~500 LOC estimate by a hair; nothing structurally surprising.

Wall time: ~4 hrs implementation + ~1 hr fixtures/Makefile + ~30 min
retro. Of the implementation time, ~30 min was diagnosing the
`try_bare_call_narrow` first-arg pick (silently picking
`Response::Ok` over `Result::Ok` because the user's variant had a
concrete first param) — surfaced via a temporary `eprint("DBG ...")`
trace in `ctor_check_or_synth` that showed the rewrite firing
correctly while a later synth path drifted. Lesson recorded inline
in the `try_bare_call_narrow` comment.

## Decisions made

- **Bare slot protection is type-shape-keyed, not name-keyed.**
  `bare_is_prelude_builtin_variant` inspects the bare scheme's
  return tycon name (`Result` or `Option`). Hard-coding the four
  ctor names (Ok/Err/Some/None) would have been simpler but would
  bake in the prelude's specific names; the type-shape check
  generalises to any future builtin sum.
- **Ambiguity diagnostic only at SLet without annotation.**
  Earlier drafts emitted the diagnostic from `synth_call`'s
  default arm and `synth EVar`, which fired throughout the
  prelude (every `Some(x)` body where the arm body type was not
  yet resolved). Limiting the fire site to the canonical
  "silently resolves to prelude" shape kept the diagnostic useful
  without prelude noise.
- **`check_expr_against` ECall rewrite is unconditional within
  annotation context.** Earlier draft used a `bare_disagrees`
  guard (only rewrite if the bare scheme's parent differs from
  the expected parent). That guard caused prelude `Ok(s)` inside
  a `Result[...]`-annotated context to not rewrite (bare-parent =
  expected-parent), leaving the ambiguity diagnostic to fire
  later in synth. Unconditional rewrite is safe because the
  qualified scheme IS the correct one when annotation context
  pins the parent — and the rewrite preempts the ambiguity check.
- **Selfhost gate is the load-bearing acceptance probe.** Every
  intermediate edit ran `make -C stage2 selfhost` before the
  next; selfhost-byte-identical caught a Phase 2 unboxing parity
  regression that the cap 17 fixture alone would have missed
  (cap 17 doesn't recurse on `Node`-shaped user types).
- **Move `export_ambiguous_constructor.kai` to `examples/shadowing/`
  instead of deleting it.** The fixture documents a real
  contract change (PR #507's upfront rejection → Plan B
  positive). Keeping it under the new positive runner preserves
  the intent without leaving a confusing `examples/modules/` file
  behind.

## Things this lane intentionally did NOT do

- Did **not** add `<Parent>.<cname>(...)` qualified ctor syntax to
  the parser. UFCS conflict; deferred to a follow-up.
- Did **not** broaden the ambiguity diagnostic to all bare ctor
  positions (only SLet without annotation). Prelude noise is the
  main constraint.
- Did **not** touch `stdlib/core/result.kai`, `stdlib/core/option.kai`,
  or `stdlib/protocols.kai` (per brief constraint). All prelude
  resolution stays on the bare-name path; Plan B's protection is
  in the env-build, not in the prelude sources.
- Did **not** touch `stage0` or `stage1` (per brief). Stage 2 is
  what users hit; stage 1 is the bootstrap stepping stone and is
  allowed to compile cap 17 with the older diagnostic.
- Did **not** modify `tier1-backend-parity` skips (per brief).
- Did **not** touch `CHANGELOG.md` or `VERSION` (per CLAUDE.md).
