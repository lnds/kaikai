# Lane experience — `EmitCtx` data-clump elimination (Linus audit Lane A)

Closes Lane A of `docs/stage2-quality-km-audit-2026-05-29.md`: the five-argument
emit-walker data clump in `stage2/compiler/emit_c.kai`.

## Scope as planned

The km audit (Lane A) flagged a data clump: the quintet
`lcs: [String], fns: [EFn], variants: [EVar], lams: [LamInfo], cls: [ClauseInfo]`
threaded verbatim through the C emitter. Token `lcs` appeared **373 times**; the
contiguous typed quintet appeared in **74 signatures**. Any sixth piece of emit
context (a config flag, a target descriptor, a counter) meant editing all 74
signatures plus every call-site — surgery the audit estimated at ~61 signatures.

Plan: introduce one record `EmitCtx` bundling the five, pass a single `cx`
everywhere the quintet travelled together, build it once at the entry point.

## Scope as shipped

- `type EmitCtx = { lcs, fns, variants, lams, cls }` + a helper
  `emit_ctx_with_lcs(cx, l)` that rebuilds the record with a new lexical scope.
- **74 → 0** contiguous typed-quintet signatures; **75** signatures now take
  `cx: EmitCtx` (the +1 is `emit_ctx_with_lcs` itself).
- Token `lcs`: **373 → 60** (−84%). The residual 60 are legitimate: field
  names in record literals (`lcs:`), the helper's param, the seven local-scope
  rebinds (`arm_lcs` / `body_lcs` / `lcs1` / `lcs2`), and the `list_filter_in`
  helper that only ever took `lcs`.
- Net `emit_c.kai` delta: **+18 lines** (the type decl, the helper, and seven
  local `EmitCtx { … }` builders for the lcs/fns-varying descent points).
- Zero changes outside `emit_c.kai`. `EFn`/`EVar`/`LamInfo`/`ClauseInfo` stay
  defined in `fnreg.kai` / `emit_shared.kai`; `EmitCtx` lives in `emit_c.kai`
  next to `BuildMode`, since the LLVM backend has its own walker and does not
  share this clump.

## Design decisions

1. **One record, not a tuple or a typedef alias.** A record with named fields
   reads at the call-sites (`cx.variants`) and lets a sixth field land as one
   edit — the whole point of the audit finding.

2. **`lcs` is the only field that varies during descent.** `fns`/`variants`/
   `lams`/`cls` are invariant for an entire `emit_program` run, so they are
   built once. Each body / arm / clause / lambda extends the *lexical* scope
   only, so the descent points rebuild `cx` with a new `lcs` via
   `emit_ctx_with_lcs`. This mirrors exactly the old
   `…, new_lcs, fns, variants, lams, cls` argument shuffle — same data flow, one
   allocation instead of five argument copies.

3. **kaikai has no record-update syntax** (confirmed via `kai info` — there is
   no `{ ..cx, lcs: l }` form). So `emit_ctx_with_lcs` is a plain constructor
   that re-threads the four invariant fields. For the two sites that vary *both*
   `lcs` and `fns` (`emit_fn_body`, and the module-rotated test/bench/check
   emitters), an explicit `EmitCtx { … }` literal builds the variant locally —
   the helper only covers the lcs-only case.

4. **Byte-identity is the acceptance oracle, not selfhost determinism alone.**
   Selfhost (`kaic2b.c == kaic2c.c`) only proves the new compiler is a fixed
   point. To prove the refactor is *semantically inert* I built kaic2 from
   `main` and from this branch and diffed the emitted C **and** LLVM for a
   403-file corpus (demos + examples/effects + examples/perceus +
   examples/sugars): **0 differences on both backends**. The emitted C for the
   one tier1 fixture that crashes (`issue_318_include`) is also byte-identical
   to main's.

## Structural surprises the brief did not anticipate

- **The clump is not always contiguous, and not always the parameter.** Naïve
  text substitution `lcs → cx.lcs` is wrong in **8 functions**:
  - Seven rebind the lexical scope locally (`let arm_lcs = …`, `let body_lcs`,
    `let lcs1`, and `emit_expr_boxed`'s `EBlock` arm which *shadows* `lcs` with
    a `let lcs = …`). After the rebind, `lcs` is the local, not the param.
  - One (`emit_match_arm_reuse_cons`) receives an *alternative* lexical scope
    (`arm_lcs`) as a distinct parameter and re-passes it alongside the quintet.

  These were excluded from the mechanical pass and hand-edited. The
  `emit_expr_boxed` shadow was the trap: I renamed the inner `let lcs` to
  `let block_lcs` so the descent could build `emit_ctx_with_lcs(cx, block_lcs)`
  without the local colliding with the now-`cx`-typed param.

- **Three call-sites vary both `lcs` and `fns`.** `emit_fn_body` rotates the EFn
  table per-module (`fns_prefer_module`) *and* extends the scope; the test /
  bench / check emitters do the same. `emit_ctx_with_lcs` (lcs-only) can't
  express this, so they build an explicit `EmitCtx { lcs: …, fns: body_fns, … }`.
  My first mechanical pass missed these (their signatures don't contain the
  contiguous quintet — they reach `emit_expr` through locals), and they surfaced
  as `too many arguments to function call, expected 3, have 7` at the C-compile
  step. The clang error was the fast feedback loop here, not selfhost.

- **`emit_lam_helpers` is not a quintet function** but constructs the quintet at
  its single call-site (`emit_lam_helper(src, l, [], fns, variants, lams, cls)`).
  It keeps its loose params and now builds the `EmitCtx` inline.

## Fixtures / coverage

No new fixture: this is a behaviour-preserving refactor confined to one file,
and the acceptance gate is byte-identity of emitted output, which the existing
403-file corpus already exercises end-to-end on both backends. A regression
fixture for "the emitter still emits the same C" would be the corpus diff
itself, which is run as the gate, not checked in. Per CLAUDE.md the
single-file-confined exemption from the fixture rule does **not** apply (this
file is large and load-bearing), but the regression *shape* — "any future edit
that perturbs emitted C" — is caught by the selfhost gate plus the demos
baseline, which already run in tier0/tier1.

## Gates

- `make selfhost`: OK (kaic2b.c == kaic2c.c).
- `make selfhost-llvm`: OK (s1.ll == s2.ll).
- C emitted vs `main` baseline, 403 programs: **0 diff**.
- LLVM emitted vs `main` baseline, 403 programs: **0 diff**.
- `make tier0`: green (selfhost deterministic, demos baseline 34/34).
- `make tier1`: the only failure is `test-issue-318-include` (SIGSEGV / Error
  139). This is **pre-existing**: `main`'s own tier1 CI runs are red on the
  identical crash (run 26612257359, headSha 6c717d8), and the emitted C for the
  fixture is byte-identical between this branch and main. Out of lane — not
  touched here.

## Follow-ups left for next lanes

- **`issue_318_include` SIGSEGV** is unrelated repo debt (main is red on it in
  CI). It needs its own lane / issue; not in scope for a data-clump refactor.
- **A sixth field is now a one-line edit.** When the audit's downstream lanes
  want to thread emit config (e.g. a target descriptor, a diagnostics sink, or
  the `BuildMode` that `emit_program` currently matches on separately), add it
  to `EmitCtx` and update the ~7 build sites, not 75 signatures.
- **The LLVM backend (`emit_llvm.kai`) has the same clump shape** but a separate
  walker; if it grows the same data-clump pressure, a parallel `EmitCtxLlvm`
  (or a shared one sunk to `emit_shared.kai`) is the analogous move. Left for a
  dedicated lane — this one is C-emitter-only per the brief.
