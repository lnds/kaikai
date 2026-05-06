# Lane experience — issue #210 (Perceus reuse-in-place: typer-aware shape predicate)

Date: 2026-05-05 (single working session, ~25 min wall).
Lane branch: `issue-210-reuse-in-place-typer-shape`.
Issue spec: `gh issue view 210`.
Predecessor: PR #208 (issue #118) wired runtime helpers + emit
dispatch but disabled the variant + record recogniser arms.

## Objective metrics

- Lane start: `2026-05-05T21:58:48-04:00`.
- Lane end:   `2026-05-05T22:24:00-04:00`.
- Wall: ~25 min total (well under the 3–4 h budget).
- Build / tier invocations: 7 total (2× selfhost, 2× selfhost-llvm,
  1× tier0, 1× tier1, 1× tier1-asan). All green.

## What shipped

Three deltas:

1. **Typer-aware shape predicate** in `stage2/compiler.kai`
   (`pcs_shape_matches`, `pcs_ty_concrete`): replaces the lexical
   `pcs_callee_is_variant_ctor` heuristic. The predicate compares
   `mangle_ty(scr.ty)` to `mangle_ty(body.ty)` after confirming both
   sides resolve to a concrete `Ty` (rejecting `TyAny` and `TyVarT`).
   `mangle_ty` already produces a canonical name + tparam encoding,
   so distinct `Maybe[Int]` vs `Maybe[Bool]` rebuilds get different
   strings and the predicate rejects the misfire.
2. **Variant + record recogniser arms re-enabled** in
   `pcs_try_reuse_arm` (stage 2): formerly a `_ -> body`
   fall-through, now dispatches to the existing `pcs_try_reuse_variant`
   / `pcs_try_reuse_record` helpers when `pcs_shape_matches` admits
   the rewrite. `pcs_recognise_reuse_arms` was extended with a
   `scr_ty: Option[Ty]` parameter so the per-arm predicate can read
   the scrutinee's resolved type without a second AST walk.
3. **LLVM-side dispatch + runtime wrappers** for the variant +
   record cases. PR #208 only wired the cons branch on the LLVM
   path. Added `kaix_reuse_or_alloc_record` /
   `kaix_reuse_or_alloc_variant` shims in `stage0/runtime_llvm.c`,
   the matching `declare`s in the IR prelude, and
   `llvm_emit_reuse_record` / `llvm_emit_reuse_variant` lowering
   functions that mirror `llvm_emit_record_lit` /
   `llvm_emit_variant_ctor`'s argument-buffer alloca pattern.

The 1 fixture added is the negative-test (`reuse_variant_no_misfire`).
The 2 positive fixtures (`reuse_variant_basic`, `reuse_record_basic`)
already existed as placeholders from PR #208 — their comments were
updated to reflect the now-active recogniser path.

## The typer-aware predicate (before / after)

Before (lexical, pre-#210, disabled):

```kaikai
fn pcs_callee_is_variant_ctor(e: Expr) : Bool {
  match e.kind {
    EVar(nm) -> match char_at(nm, 0) {
      Some(c) -> c >= 'A' and c <= 'Z'
      None    -> false
    }
    _ -> false
  }
}
```

After (typer-aware, #210, active):

```kaikai
fn pcs_shape_matches(t1: Option[Ty], t2: Option[Ty]) : Bool {
  match t1 {
    None -> false
    Some(a) -> match t2 {
      None -> false
      Some(b) -> {
        if pcs_ty_concrete(a) and pcs_ty_concrete(b) {
          mangle_ty(a) == mangle_ty(b)
        } else { false }
      }
    }
  }
}
```

The lexical check survives in `pcs_try_reuse_variant` solely as a
*disambiguator between ctor and fn callees of the same return type*
(e.g. `fn make_some(x: Int) : Maybe[Int]` would otherwise pass the
shape match). Typer-aware shape + lexical-ctor combined = sound.

## The negative test (`reuse_variant_no_misfire.kai`)

Two functions over `Option[Int]`:

- `bump`: `Some(d) -> Some(d + 1)` — same-TyCon rebuild, same
  arity, expect reuse.
- `classify`: `Some(d) -> if d > 0 { Some(true) } else { Some(false) }`
  — type changes from `Option[Int]` to `Option[Bool]`, expect NO
  reuse.

Empirical verification (post-mono C emit):

```c
static KaiValue *kai_classify(KaiValue *kai_m) {
  /* ... */ kai_variant(0, "Some", 1, ...) /* fallback alloc */
}
static KaiValue *kai_bump(KaiValue *kai_m) {
  /* ... */ kai_reuse_or_alloc_variant(_scr, 0, "Some", 1, ...) /* reuse */
}
```

Confirmed: `bump` fires the reuse helper, `classify` does not.

## Friction points

- **`args` shadow trap (~10 min)**: I named a local binding `args`
  in `pcs_try_reuse_variant` to capture the ECall's argument list.
  The stage 1 compiler's known `args`-vs-prelude shadow bug
  (memory `feedback_kaikai_param_args_shadow.md`, 2026-05-01) made
  `list_length(args)` evaluate to `0` instead of the bound list
  length. The whole reuse branch silently returned `body` unchanged.
  Caught with eprint trace inside `pcs_try_reuse_variant`. Renamed
  to `cargs` and the recogniser fired on the first re-run.
  Lesson: this trap is real even for fresh code; the global-`args()`
  prelude name shadows aggressively. Worth flagging in a doc.
- **LLVM backend missed in PR #208 retro**: the issue spec called
  the runtime helpers + emit dispatch "already shipped via PR #208",
  but only the C-side dispatch + `kaix_reuse_or_alloc_cons` shim
  were wired. `kaix_reuse_or_alloc_record` and
  `kaix_reuse_or_alloc_variant` did not exist; the IR prelude was
  missing the corresponding `declare`s; `llvm_emit_call` had no
  branches for `__perceus_reuse_record` / `__perceus_reuse_variant`.
  Discovered when `make selfhost-llvm` failed with `use of undefined
  value '@kai___perceus_reuse_variant'`. Added all three on the
  same lane (the issue scope estimate of 30–50 LoC turned into
  ~100 once LLVM was complete). Selfhost LLVM byte-identical
  immediately after.

## Empirical verification

- 7 perceus-issue118 fixtures green (6 pre-existing + 1 new
  negative-test).
- 7 perceus-issue118-asan fixtures green under ASAN+UBSan.
- `make tier0` green (selfhost C byte-identical, demos baseline 26).
- `make tier1` green (~5'51", full test suite + fmt + bench + check).
- `make tier1-asan` green (issue #118 + #91 + #139 + #140 etc.).
- `make selfhost` byte-identical on the C path.
- `make selfhost-llvm` byte-identical on the LLVM path.

## Subjective summary

- **Confidence**: high. The shape predicate is mechanically simple
  (`mangle_ty == mangle_ty`), the disambiguator is one line, and
  the negative-test pins the exact regression that disabled the
  arms in PR #208. Both selfhost paths converge byte-identical
  on a single iteration; the corruption mode the original lane
  hit (variant misfire writing wrong-typed payloads) is structurally
  prevented.
- **Hardest**: the `args` shadow detour. Without the trace I would
  not have suspected stage1's known prelude trap inside the
  perceus pass. The eprint approach was direct and worked, but
  the bug class is a recurring time-sink — adding to memory.
- **Easiest**: the C-side recogniser activation (~30 LoC). The
  `pcs_try_reuse_variant` / `pcs_try_reuse_record` helpers were
  already in place from PR #208, just unused.
- **Compiler help vs hinder**: the typer's `Expr.ty` annotations
  on post-mono nodes were exactly what the predicate needed. No
  re-walk, no second pass. The shape-mismatch case never reached
  the recogniser because mono had already diverged the
  `mangle_ty`s.

## Limitations

- **Cross-instantiation reuse not yet enabled.** A `Maybe[Int]` cell
  consumed and a `Maybe[Box[Int]]` cell rebuilt would be rejected
  by `mangle_ty == mangle_ty` even though the runtime layout is
  compatible. Out of scope for this lane.
- **Polymorphic / row-variable cases stay rejected** (any `TyVarT`
  on either side falls through to no reuse). Once the typer pins
  a concrete type post-mono, those cases auto-enable.
- **Per-fn alloc-count delta NOT measured.** Issue #210 notes the
  perf delta is bounded by issue #209 (RC discipline gaps). Until
  #209 lands, the recogniser fires but most call sites still pre-
  dup the binding for downstream readers and the reuse helper
  decides at runtime to fall back. `KAI_TRACE_RC` will report
  reuse-vs-alloc once both lanes are in.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T22:14:04-04:00	tier0	DONE	-
2026-05-05T22:21:15-04:00	tier1	DONE	-
2026-05-05T22:22:15-04:00	tier1-asan	DONE	-
```
