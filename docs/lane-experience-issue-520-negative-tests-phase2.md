# Lane experience report — issue #520 (negative-space audit phase 2)

## Goal

Close #520. Phase 1 (#511, closed by PR #519) built the
negative-space framework + 31 enforced fixtures + 14
silent-contract entries across 10 contract categories
(`pub`, effect annotation, derive, handle leak, one-shot
resume, Mutable discipline, stage 1 rejection, Ffi, type-name
shadowing, Actor row composition). It closed the
worst-trafficked gaps but left an honest "~30% of the
negative coverage we need for v1.0" gap-statement.

Phase 2 fills the remaining 70%: parser/syntax, patterns,
type-system invariants, protocols, modules, FFI surface,
diagnostic body quality, and effects beyond phase 1. Target
release: v0.55.x patch line.

## Scope as planned vs as shipped

**Planned (per brief):**

- 9 categories × ≥3 fixtures = ≥27 new enforced fixtures.
- Each silent contract → follow-up issue filed (continues
  #511 discipline).
- Existing `silent_contract/` quarantine pattern reused.
- Harness extension for `.diag.expected` multi-line body
  matching in category 7 (diagnostic quality).
- Retro live in the same commit train, ~100-200 lines.

**Shipped:**

- **33 new enforced fixtures** across 8 of the 9 categories
  (Cat 8 / lifetimes-RC: 0 new fixtures, with rationale —
  see "Cat 8: why empty" below).
- **14 new silent-contract fixtures** (counting `import_cycle/`
  as one) clustered into **6 follow-up issues** (#534, #535,
  #536, #537, #538, #539) + one existing-family reference
  (#531 covers `spawn_qualified_no_row`).
- **Harness extended** with `.diag.expected` multi-line body
  matching: every non-empty / non-`#` line in the golden must
  appear (fixed-substring match) somewhere in stderr. Order
  is not enforced because kaikai reorders notes when multiple
  errors fire. 4 fixtures in `diagnostic_quality/` use this
  path.
- 6 new follow-up issues filed (see below).

## Coverage matrix

| # | Category | New enforced (PASS) | New silent_contract (filed) |
| --- | --- | --- | --- |
| 1 | Parser / syntax | 6 (`let_keyword_pattern`, `let_if_pattern`, `fn_named_fn`, `binop_leading`, `unterminated_string`, `let_no_name`) | 0 |
| 2 | Patterns | 4 (`non_exhaustive_match`, `constructor_wrong_arity`, `arm_type_mismatch`, `guard_not_bool`) | 3 → #534 (`pattern_duplicate_binding`, `pattern_duplicate_variant_arm`, `pattern_duplicate_literal_arm`) |
| 3 | Type system invariants | 6 (`return_type_mismatch`, `let_annotation_mismatch`, `arith_string`, `cyclic_type_alias`, `call_arg_type`, `apply_non_function`) | 1 → #534 (`unbound_tyvar_in_signature`) |
| 4 | Protocols / impl | 3 (`impl_double_same_unit`, `call_proto_no_impl`, `derive_then_impl`) | 3 → #535 (`impl_missing_required_method`, `impl_method_signature_mismatch`, `impl_method_arity_mismatch`) |
| 5 | Module system | 4 (`missing_import`, `duplicate_type_decl`, `import_keyword_name`, `qualified_missing_export/`) | 2 → #538 (`import_cycle/`, `duplicate_fn_decl`) |
| 6 | FFI signatures | 3 (`extern_no_return_type`, `extern_pub_qualifier`, `extern_lowercase_abi`) | 2 → #536 (`extern_missing_ffi_capability`, `extern_array_kaikai_type`) |
| 7 | Diagnostic quality (`.diag.expected`) | 4 (`prelude_call_anchors_at_user`, `effect_help_text`, `caret_arith_operator`, `missing_variant_help`) | 0 |
| 8 | Lifetimes / RC | **0** (see rationale below) | 0 |
| 9 | Effects beyond phase 1 | 3 (`handle_with_undeclared_effect`, `cancel_unknown_op`, `pid_escape_from_spawn`) | 3 → #537/#539/#531 (`program_name_no_row` → #537, `exit_no_row` → #537, `handle_missing_return_clause` → #539, `spawn_qualified_no_row` → #531) |
| **Total** | | **33** | **14** (clustered into 6 issues; 1 references existing #531) |

Pre-#520 baseline: 44 PASS in `test-negative.sh`. Post-#520
baseline: **77 PASS** (+33, in line with the brief's ≥27
target). All 14 new silent-contract entries documented with
follow-up issue links.

## Cat 8 (Lifetimes / RC): why empty

The brief is explicit: "Only if kaikai's RC model has
user-observable invariants that fail loudly." kaikai's
memory model is Perceus RC + isolated fibers (BEAM-style,
private heap, messages copied). There is **no borrow
checker** — by design (CLAUDE.md "Things to avoid"). The
contracts that would land in Cat 8 in a Rust-like language
(use-after-move, mutable-aliasing) are not expressible in
the surface language: every binding is shared-RC, mutation
goes through `Mutable` (already covered in `mutable/`), and
inter-fiber communication is copy-only.

The closest user-observable RC contract is "closure captures
a name that doesn't exist in scope" — but that is a
resolver-layer rejection, not an RC-layer one; it falls
naturally under Cat 1 (parser/resolver). The fixture
`/tmp/neg/closure_undef.kai` confirmed the resolver rejects
cleanly; no separate Cat 8 fixture was added to avoid
duplicating Cat 1.

The runtime RC contract that *is* user-observable is the
"continuation resumed twice" panic — covered in Phase 1 by
the three `oneshot/*` fixtures (compile + run + grep stderr
for the runtime panic message).

Result: Cat 8 is intentionally empty. If kaikai ever grows a
linear-types extension or an opt-in borrow checker, this
slot is where its negative fixtures would land.

## Cat 9 (Effects phase 2): #474 prerequisite handling

The brief's note was correct: #474 (Reactor R1) is open with
~7 days of runtime work, and the brief said "do NOT block
on it — the §9 fixtures are TYPING tests, not runtime
concurrency tests." Verified empirically:

- `handle_with_undeclared_effect.kai` — pure typer reject,
  no reactor needed.
- `cancel_unknown_op.kai` — pure typer reject (op not
  declared on the effect).
- `pid_escape_from_spawn.kai` — structured-concurrency
  scope check, runs in the typer pass against
  `docs/structured-concurrency.md`. No reactor needed.

The fixture that *would* need #474 — "fiber cancelled
cleans up open files" — was not written. It is a runtime
post-condition over reactor scheduling, and per the brief
that work defers until #474 closes.

## Effect-set ordering finding (no fixture)

The brief asked: "does `/ Stdout + Stdin` differ from
`/ Stdin + Stdout`?" Empirically, **no** — both orderings
parse to the same row type and type-check identically.
Effect rows in kaikai are unordered sets (per
`docs/effects.md` — rows are commutative). No fixture was
added because there is nothing to reject; the contract is
"order does not matter," and the positive tests already
exercise both orderings.

## Audit findings: where Phase 2 surprised the brief

### Where kaikai held up

- **Parser-level rejections are strong.** Keyword-as-ident,
  binop-leading, unterminated strings, `let` without
  pattern, `pub extern` ordering — all reject cleanly at
  the right column with `expected ...` diagnostics. The
  parser is well-defended.
- **Type-system core is honest.** Return-type mismatch,
  let-annotation mismatch, arithmetic operator type-check,
  cyclic type aliases, call-arg-type mismatch, applying
  non-fn — all reject with anchored diagnostics including
  caret + expected/found notes + (often) a sensible help
  line.
- **Diagnostic body quality is genuinely good.** Cat 7
  fixtures pass with multi-line body matches: anchor at user
  file (not prelude), caret on the offending operand,
  `= note: expected`, `= help: ...` with valid fixes.
- **Structured concurrency boundary holds.** A spawned
  `Pid` cannot escape the spawning fn's scope (the
  "structured" in structured concurrency). One of the
  cleanest rejections in the language.
- **Module system enforces duplicate `type` and missing
  exports.** `type Box = ...; type Box = ...` rejects;
  `lib.two()` when `lib` exports only `one` rejects with
  the export list named. The `module 'lib' does not export
  'two'; available exports: one` shape is excellent UX.

### Where kaikai is silent (the new follow-up issues)

The 14 new silent-contract entries cluster into 6 themes:

1. **Pattern checker gaps (#534)** — duplicate bindings
   `(x, x)`, duplicate variant arms `Yes(x) -> _; Yes(y) -> _`,
   duplicate literal arms `1 -> _; 1 -> _`, and unbound
   type variable in signature `fn f(x: a) : b`. Four
   shapes, one issue: the pattern reachability/exhaustiveness
   pass does not check for redundant arms, and the HM
   sig-checker does not require all tyvars in the body to
   be introduced.

2. **Protocol impl validation gaps (#535)** — `impl P for T`
   is parsed but not validated against `protocol P`'s ops:
   missing required method, wrong return type, wrong arity
   — all accepted. Calls go to a runtime-undefined dispatch.
   `docs/protocols.md` is the spec; the impl-validation pass
   is just absent.

3. **FFI surface validation gaps (#536)** — `extern "C" fn`
   without `/ Ffi` accepts; `extern "C" fn ... : [Int] / Ffi`
   accepts (kaikai-only return type that C cannot consume).
   The `Ffi` capability discipline is half-enforced at the
   call site (issue #531) but not at the declaration.

4. **Prelude effect mapping gaps (#537)** — `program_name()`,
   `exit(n)` are missing from `prelude_effect_for`. Bare
   prelude calls compile from a pure-row `main` without any
   effect demand raised. Distinct from #531 (qualified-form
   propagation) because here the bare-prelude name itself
   has no label mapping.

5. **Module system gaps (#538)** — import cycle silently
   accepted; duplicate `fn` decl silently shadowed (vs.
   `type` which correctly rejects); missing-import emits
   stderr but exits 0 unless downstream references trigger
   a name-resolution failure.

6. **Handle missing-return-clause (#539)** — `handle { ... }
   with E { foo(resume) -> ... }` without an explicit
   `return(x)` clause compiles. Per `docs/effects.md` the
   `return` clause is required; the typer should reject
   (or synthesize a default with a diagnostic note).

## Design decisions and alternatives considered

### `.diag.expected` multi-line body matching

Cat 7 needed a way to assert more than the first line of
the diagnostic. Three options:

1. **Byte-exact golden** — like `examples/refinements/*`.
   Picks: brittle, every diagnostic-text refactor breaks
   every golden.
2. **Regex match** — `.diag.regex.expected`. Picks: more
   power than needed; regex escaping in shell harness is
   annoying.
3. **Fixed-substring lines, all must appear, order not
   enforced.** **Chosen.** Cheap to author, robust to
   diagnostic-text refactors that don't drop a note,
   captures anchor + caret + key notes + help text.

The choice mirrors the existing `.err.expected` shape (first
line only) — we just lift the "first line" restriction to
"every non-empty, non-`#`-comment line."

### Cluster silent contracts into 6 issues, not 14

The brief said "each silent contract → issue." Phase 1
clustered 14 silent contracts into 3 issues; the discipline
was "one issue per *underlying gap*, not per fixture." This
lane follows the same pattern: 14 silent fixtures, 6 issues
(plus 1 reference to the existing #531). Reasons:

- #534 (pattern checker) — all four are the same missing
  reachability/exhaustiveness pass, plus an HM signature
  invariant.
- #535 (protocol impls) — all three are the missing
  impl-validation pass; closing one closes them all.
- #536 (FFI surface) — the two are missing pieces of the
  same `extern "C" fn` declaration validator.
- #537 (prelude effect map) — both are missing rows in the
  same `prelude_effect_for` table.
- #538 (module system) — three distinct sub-issues but in
  the same resolver/loader, naturally one lane.
- #539 (handle missing return) — isolated from #517's lane
  because #517 chose not to scope it.

If a future audit shows one of the 6 should split, the issue
body lists fixtures inline so a sub-issue can take a subset.

### Cat 8 left empty

Documented above. The brief explicitly allowed this
("Only if kaikai's RC model has user-observable
invariants…"). The decision is **not** a coverage gap; it
is "this slot does not apply to kaikai's memory model."
If Cat 8 grows fixtures in the future, it will be because
kaikai adds linear types or borrow-checking — neither
planned for v1.0.

## Structural surprises

- **Patterns checker has the same shape as a 1980s ML
  textbook covers but stops halfway.** Non-exhaustive
  detection works (with great `missing variant + covered`
  help). Reachability does not — three of the four pattern
  silent contracts are "the second arm should be
  unreachable." This is the lowest-hanging fruit in the
  whole audit.

- **Protocol impl validation is genuinely missing, not just
  buggy.** `impl_method_signature_mismatch.kai` returns
  `String` from a `: Int`-declared op and compiles. The
  dispatch resolver clearly never compares the impl
  signature to the protocol op. This was surprising —
  `docs/protocols.md` describes the dispatch table as if
  validation were table-stakes.

- **The bare-prelude vs. qualified-form effect-row
  asymmetry is *systemic*.** Phase 1 found `Mutable.array_set`
  doesn't propagate (#516, closed); Phase 2 finds
  `Spawn.spawn` doesn't propagate (same #531 family). Plus
  the bare-prelude `program_name` / `exit` are missing from
  `prelude_effect_for` entirely. The effect-row checker has
  three orthogonal gaps: (a) qualified-form non-propagation
  (#531, was #516), (b) missing prelude name → label
  mappings (#537), (c) declaration-time FFI signature
  validation (#536). Closing any one of them does not close
  the others.

- **Diagnostic quality is the strongest part of the
  compiler.** Once a contract *is* enforced, the diagnostic
  is exemplary: anchored at user file, caret on the
  offending token, expected/found notes, valid help. Every
  Cat 7 fixture passed first time with the `.diag.expected`
  shape. The compiler's diagnostic infrastructure is
  punching well above its weight.

## Fixtures added

Layout (33 enforced + 14 silent_contract):

```
examples/negative/
├── parser_syntax/                 # NEW — Cat 1, 6 PASS
│   ├── let_keyword_pattern.kai
│   ├── let_if_pattern.kai
│   ├── fn_named_fn.kai
│   ├── binop_leading.kai
│   ├── unterminated_string.kai
│   └── let_no_name.kai
├── patterns/                      # NEW — Cat 2, 4 PASS
│   ├── non_exhaustive_match.kai
│   ├── constructor_wrong_arity.kai
│   ├── arm_type_mismatch.kai
│   └── guard_not_bool.kai
├── type_invariants/               # NEW — Cat 3, 6 PASS
│   ├── return_type_mismatch.kai
│   ├── let_annotation_mismatch.kai
│   ├── arith_string.kai
│   ├── cyclic_type_alias.kai
│   ├── call_arg_type.kai
│   └── apply_non_function.kai
├── protocols/                     # NEW — Cat 4, 3 PASS
│   ├── impl_double_same_unit.kai
│   ├── call_proto_no_impl.kai
│   └── derive_then_impl.kai (+ .flags)
├── modules/                       # NEW — Cat 5, 4 PASS
│   ├── missing_import.kai
│   ├── duplicate_type_decl.kai
│   ├── import_keyword_name.kai
│   └── qualified_missing_export/{main,lib}.kai
├── ffi/                           # Cat 6, +3 new
│   ├── extern_no_return_type.kai
│   ├── extern_pub_qualifier.kai
│   └── extern_lowercase_abi.kai
├── diagnostic_quality/            # NEW — Cat 7, 4 PASS (.diag.expected)
│   ├── prelude_call_anchors_at_user.kai
│   ├── effect_help_text.kai
│   ├── caret_arith_operator.kai
│   └── missing_variant_help.kai
├── effects_phase2/                # NEW — Cat 9, 3 PASS
│   ├── handle_with_undeclared_effect.kai
│   ├── cancel_unknown_op.kai
│   └── pid_escape_from_spawn.kai
└── silent_contract/               # +14 new (clustered into 6 issues)
    ├── pattern_duplicate_binding.kai          → #534
    ├── pattern_duplicate_variant_arm.kai      → #534
    ├── pattern_duplicate_literal_arm.kai      → #534
    ├── unbound_tyvar_in_signature.kai         → #534
    ├── impl_missing_required_method.kai       → #535
    ├── impl_method_signature_mismatch.kai     → #535
    ├── impl_method_arity_mismatch.kai         → #535
    ├── extern_missing_ffi_capability.kai      → #536
    ├── extern_array_kaikai_type.kai           → #536
    ├── program_name_no_row.kai                → #537
    ├── exit_no_row.kai                        → #537
    ├── import_cycle/{main,a,b}.kai            → #538
    ├── duplicate_fn_decl.kai                  → #538
    ├── handle_missing_return_clause.kai       → #539
    └── spawn_qualified_no_row.kai             → #531 (existing)
```

## Harness extension: `.diag.expected`

`tools/test-negative.sh` now recognizes two golden suffixes:

- `<name>.err.expected` — match first non-empty line as
  fixed substring against stderr (legacy behavior).
- `<name>.diag.expected` — every non-empty, non-`#`-comment
  line in the golden must appear as a fixed substring in
  stderr. Order is not enforced (kaikai reorders notes when
  multiple errors fire on the same expression).

Discovery: `find ... \( -name '*.err.expected' -o -name
'*.diag.expected' \)` then dispatch by extension. ~25 LoC
delta in the driver, no churn in existing fixtures.

## Coverage gaps left for next lanes

- **Cat 8 (lifetimes / RC)** — intentionally empty per the
  rationale above. Re-open if kaikai adds linear types or
  borrow checking post-v1.0.
- **Fiber-cancel-runtime fixtures** — deferred until #474
  closes (Reactor R1). The "fiber cancelled cleans up open
  files" shape is a runtime post-condition and the brief
  was explicit about not blocking on #474.
- **Cross-backend parity** (brief category 10) — out of
  scope here; the brief itself flagged it as "borderline
  negative" and suggested a separate `test-backend-parity`
  target. The closing PR for #520 names this as deferred.
- **The 6 new follow-up issues** (#534-#539) — each
  promotes a silent_contract subdirectory to enforced.

## Real cost vs estimate

- Estimate: 2-3 days.
- Actual: one focused session — ~3-4 hours including:
  probing diagnostic shapes (~1 hour), authoring fixtures
  + goldens (~1.5 hours), `.diag.expected` harness
  extension + Cat 7 fixtures (~30 minutes), silent-contract
  triage + issue filing (~30 minutes), retro (~30
  minutes). The pattern from #511 — write fixture, run,
  decide PASS vs silent_contract on the spot — held up at
  3× the surface area.

Velocity benefitted from:
- Re-using the existing `.flags` / multi-file / runtime
  fixture conventions (no new harness shapes except the
  `.diag.expected` extension).
- Clustering silent contracts into 6 issues, not 14 —
  the audit's job is to surface the *gaps*, not the
  fixtures-per-gap count.
- Cat 8 being intentionally empty (decision document, no
  fixture authoring).

## Follow-ups left for next lanes

- **#534** — Pattern checker + unbound tyvar gaps (4
  fixtures move out of `silent_contract/` when closed).
- **#535** — Protocol impl validation (3 fixtures).
- **#536** — FFI surface validation (2 fixtures).
- **#537** — Prelude effect mapping (2 fixtures).
- **#538** — Module system gaps (2 fixtures, one
  multi-file).
- **#539** — Handler missing-return clause (1 fixture).
- **#474** — Reactor R1 prerequisite for runtime-side
  fiber/cancel fixtures (out of scope here).
- **Cross-backend parity** — separate test target if
  desired post-#474.

When any of these closes, the closing PR moves the relevant
silent_contract entries into the enforced sub-dirs, adds
`.err.expected` (or `.diag.expected`) goldens, and updates
`silent_contract/README.md` to remove the row.

## Lane discipline notes

- One lane, one scope: this lane added fixtures + harness
  extension only. No compiler changes. Selfhost byte-identical
  (the kaikai pipeline never saw the new `.kai` files in
  `examples/negative/` during stage1/stage2 self-build).
- tier0 green; tier1 green locally before push.
- Doc + retro live in the same commit train as the fixture
  commits, per the lane-retro discipline pinned in CLAUDE.md
  ("Lane retros are mandatory for non-trivial lanes" — #520
  is non-trivial: 33 new enforced fixtures + harness
  extension + 6 follow-up issues that future lanes will
  reference).
- 6 new follow-up issues filed (#534, #535, #536, #537,
  #538, #539) following the Phase 1 cluster-by-underlying-gap
  convention.
