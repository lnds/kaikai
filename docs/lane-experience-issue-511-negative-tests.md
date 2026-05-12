# Lane experience report — issue #511 (negative-space test suite)

## Goal

Close #511. Pre-lane, the test discipline was almost entirely
positive: we asserted that valid programs compile and run, but
NOT that invalid programs are rejected with the expected
diagnostic. That blind spot was the predicate for #510: `pub`
was advertised in `docs/design.md` for the full life of the
language and silently unenforced, because no negative test
asserted the contract. The fix in #510 closed the bug; this
lane closes the *meta*-bug — the test process that allowed
the bug to live so long.

Target release: v0.55.0.

## Scope as planned vs as shipped

**Planned (per brief):**

- `tools/test-negative.sh` runner that iterates
  `examples/negative/**/*.kai`, runs kaic2, asserts non-zero
  exit + diff-clean stderr against `.err.expected` golden.
- Wire `test-negative` into `stage2/Makefile`'s `tier1` chain.
- ≥3 fixtures across 10 contract categories (pub, effect
  annotation, derive, handle leak, one-shot resume, Mutable,
  stage 1 rejection, Ffi, type-name shadowing, Actor row
  composition).
- For each contract: write the fixture FIRST, run it, and file
  a follow-up issue if the language silently accepts.

**Shipped:**

- `tools/test-negative.sh` — POSIX shell driver, ~140 lines.
  Skeleton mirrors `tools/test-llvm-driver.sh` and the
  in-Makefile `test-modules-qualified-neg` pattern. Three modes:
  - compile-time negative (`<name>.err.expected` golden,
    substring-grep against stderr — same convention as the
    existing modules-qualified-neg target);
  - stage 1 rejection (`<name>.kaic1.err.expected` routes to
    `stage1/kaic1` instead of `kaic2`);
  - runtime-time negative (`<name>.run.err.expected` — used for
    contracts like one-shot resume that are enforced at runtime
    per `docs/effects-impl.md`, compiled C runs and the binary
    panic message is the golden).
  Three optional siblings:
  - `<name>.prelude.kai` — implicit `--prelude` invocation for
    the fixture (used for the `pub_enforced_negative_prelude`
    shape);
  - `<name>.flags` — extra CLI args (used to pin
    `--prelude stdlib/protocols.kai` for derive fixtures and
    `--path stdlib` for the actor row fixtures);
  - multi-file fixture mode: `<dir>/main.kai` plus
    `<dir>/lib.kai` for the import-resolution shape, golden
    file is `<dir>/main.err.expected`.
- `test-negative` target in `Makefile` (root), wired into
  `tier1` after `test-diagnostics-collected`.
- 31 enforced fixtures across 10 categories (28 compile-time +
  3 runtime). 14 silent-contract fixtures in
  `examples/negative/silent_contract/` documenting gaps that
  the audit surfaced.
- 3 follow-up issues filed for the silent contracts
  ([#516](https://github.com/lnds/kaikai/issues/516),
  [#517](https://github.com/lnds/kaikai/issues/517),
  [#518](https://github.com/lnds/kaikai/issues/518)).

## Coverage matrix

| Category | Enforced fixtures (PASS) | Silent-contract fixtures (filed) |
| --- | --- | --- |
| 1. pub enforcement (smoke for #510) | 3: `violation_import`, `violation_prelude`, `violation_constructor` | 0 |
| 2. Effect annotation on `pub` signatures | 3: `pub_fn_unannotated_effect`, `pub_fn_unit_leak_stdout`, `pub_fn_match_arm_effect` | 2: `pub_fn_mutable_unannotated`, `pub_fn_transitive_effect` ([#516](https://github.com/lnds/kaikai/issues/516)) |
| 3. `#derive` on incompatible types | 3: `derive_show_no_field_show`, `derive_eq_no_field_eq`, `derive_binserialize_sum_payload` | 0 |
| 4. Effect-set leakage at handle boundary | 3: `handle_unknown_op`, `op_arg_type_mismatch`, `op_in_pure_fn` | 5: `handle_residual_effect`, `handle_partial_with_other_effect`, `handle_clause_missing_resume`, `handle_clause_wrong_arity`, `main_row_user_effect` ([#517](https://github.com/lnds/kaikai/issues/517)) |
| 5. One-shot continuation reuse | 3 (runtime): `resume_twice`, `resume_with_values_twice`, `resume_collect_pair` | 0 |
| 6. `Mutable` discipline on Array writes | 4: `param_array_write_no_row`, `param_array_write_other_row`, `raw_array_set_no_row`, `field_array_write_no_row` | 3: `mutable_op_no_handler_or_row`, `mutable_param_write_pure_row`, `mutable_through_field` ([#516](https://github.com/lnds/kaikai/issues/516)) |
| 7. Stage 1 protocol/impl/effect rejection | 3: `protocol_decl.kaic1`, `impl_decl.kaic1`, `effect_decl.kaic1` | 0 |
| 8. Ffi capability boundary | 3: `legacy_extern_c_axiom`, `non_c_abi`, `extern_with_body` | 1: `ffi_call_no_capability` ([#516](https://github.com/lnds/kaikai/issues/516)) |
| 9. Type-name shadowing (post-#510, #504 closure) | 3: `user_ok_collides_with_result`, `user_err_collides_with_result`, `user_some_collides_with_option` | 2: `tree_collision_via_import`, `duplicate_type_decl` ([#518](https://github.com/lnds/kaikai/issues/518)) |
| 10. Effect-row composition (Actor — issue #472) | 3: `missing_actor_reply`, `send_no_actor_row`, `receive_no_actor_row` | 0 |
| **Total** | **31** | **13** (clustered into 3 issues) |

## Audit findings

The point of negative fixtures is to make every contract
**observable**. A fixture that PASSes confirms enforcement; a
fixture that FAILs (silent contract) is a successful audit
finding, not a lane failure.

### Validated (PASS):

- **`pub` enforcement** (#510 holds) — all three smoke fixtures
  reject cleanly, both for `import` and `--prelude`, both for
  bare names and constructors.
- **Effect propagation for direct op calls** — `pub fn f() : Int = { print("x"); 0 }`, match-arm effects, unit-return leak all reject.
- **`#derive(Show/Eq/BinSerialize)` for incompatible field types** — the validator names the offending type before runtime dispatch.
- **`a[i] := v` lowering raises `Mutable` demand** when the target Array is a parameter, captured field, or used through the bare prelude `array_set`. This was the #251/#252 work and it holds.
- **Stage 1 parser rejects `protocol`/`impl`/`effect`** — clean
  parser-level error at line/col, no segfault.
- **FFI parse-time invariants** — `[<extern_c>] axiom` legacy
  syntax, non-C ABI, extern with body all reject up front.
- **Constructor name collisions with built-in `Result`/`Option`** — `user_ok_collides_with_result` (#502 fix) and friends reject up front.
- **Actor parametric effect labels** (#472) — `Actor[Request]` and `Actor[Reply]` are distinct; missing labels reported with the parametric instantiation visible.
- **One-shot resume** — runtime path emits the documented `continuation resumed twice` panic message (the contract is "compile-time OR runtime"; runtime is what stage 2 ships).

### Silent contracts surfaced (FAIL):

Three clusters, three issues:

1. **[#516](https://github.com/lnds/kaikai/issues/516) —
   qualified-effect calls don't propagate.** Calling
   `Mutable.array_set(...)` (qualified) does NOT raise
   `Mutable` demand in the enclosing fn's row, whereas the
   bare prelude `array_set(...)` does. Same shape for `Ffi`
   (any `extern "C" fn` call) and for transitive effects
   (calling `callee : Int / Stdout` from `caller : Int`).
   Together this means the effect-row contract advertised in
   `docs/design.md` §"effects visible in row types" is
   partially enforced: it catches direct op invocation but not
   transitive accumulation. 6 silent fixtures collapse to one
   underlying gap.

2. **[#517](https://github.com/lnds/kaikai/issues/517) —
   handle block: residual effects + clause shape not
   validated.** `handle { body } with Eff { ... }` does NOT
   subtract `Eff` from `body`'s row and require the residual
   to surface in the enclosing fn — instead the residual is
   silently dropped, and the runtime segfaults when the
   un-handled op fires. Handler-clause shape (missing
   `resume`, wrong arity, unknown effect after `with`) is
   accepted by the kaikai typer; the downstream C compiler
   trips on the resulting function-pointer mismatch. 5 silent
   fixtures collapse to one underlying gap.

3. **[#518](https://github.com/lnds/kaikai/issues/518) — type
   collision via import + duplicate decl.** #504 closed the
   prelude-loaded collision shape (`type Tree` user vs.
   stdlib `pub type Tree[k, v]` when stdlib is `--prelude`
   loaded), but the same shape via `import` still produces
   misleading "missing TreeNode, TreeLeaf" errors that name
   constructors the user never wrote. Also: declaring the
   same type name twice in one module is silently accepted.
   2 silent fixtures.

## Design decisions and alternatives considered

### `silent_contract/` quarantine

The brief asked for FAIL fixtures to surface in the harness
output for the audit, but tier1 also has to stay green. Three
options:

1. Mark FAIL fixtures with `xfail` and continue. Picks: needs
   harness state, complicates the "PASS == green" rule.
2. Skip FAIL fixtures entirely. Picks: loses the audit signal.
3. Quarantine FAIL fixtures under `silent_contract/` with a
   README that lists them, file follow-up issues that link
   back, exclude the subtree from the harness via
   `-not -path '*/silent_contract/*'`. **Chosen.**

Reason: the audit lives in the tree (fixtures + README + issue
links). When an issue closes, the migration is mechanical: move
the fixture out of `silent_contract/`, add the
`.err.expected` golden, harness picks it up. No xfail state
machine, no harness output to interpret beyond "all green".

### Runtime-time negative path (`.run.err.expected`)

`docs/effects-impl.md` is explicit that continuations are
one-shot and the second resume is a *runtime* panic — the
typer can't always prove single-resume. So the harness gained
a second find-pass for `.run.err.expected` files: compile via
kaic2 → cc → run the binary, grep stderr for the documented
panic message. Three fixtures use this path. The same
mechanism would work for refinement panics (`docs/refinements`)
and other runtime-enforced contracts, but those are out of
scope for #511.

### `<name>.flags` sibling for invocation customization

Some fixtures need `--prelude stdlib/protocols.kai` (derive
needs the Show/Eq protocol decls), some need `--path stdlib`
(actor needs `import actor` to find `stdlib/actor.kai`). Two
options:

1. Hard-code per-category invocations into the shell driver.
   Picks: hard to extend, brittle when a new category arrives.
2. Per-fixture `<name>.flags` sibling that the driver cats and
   word-splits into the kaic2 command line. **Chosen** —
   future fixtures only need to drop a `.flags` file.

The `.prelude.kai` sibling convention is independent and
predates this: it lets a fixture explicitly load a stand-in
prelude (the existing `pub_enforced_negative_prelude/` shape
from #510).

## Structural surprises

- **Effect-row inference is much less complete than the
  positive tests suggested.** The audit found that effect
  propagation works for "user writes `print(x)` in a body that
  declares pure row", but breaks down for "user writes
  `Effect.op(x)` in a body that declares pure row, OR calls a
  helper whose row contains the effect". 6 of the 14 silent
  fixtures are facets of this one gap. The positive tests
  passed because they were structured around direct op call
  shapes, which IS the enforced path.

- **`handle` block typing is one of the weakest spots.** Five
  silent fixtures live in this area. The compile-time
  enforcement is mostly cosmetic — handler clause syntax is
  parsed and named, but residual effects, clause shape, and
  unknown-effect-after-`with` are all unchecked. The runtime
  consequence is segfaults that look like RC bugs but are
  actually missing effect handlers.

- **The `pub` audit from #510 was the right exemplar but
  underweighted the magnitude.** The retro for #510 named *one*
  silent contract closed. The lane closing #511 surfaced 3
  more clusters of silent contracts (#516, #517, #518) before
  it finished. The pattern — "advertised in docs, never
  asserted in tests, silently broken for the full life of the
  language" — is more pervasive than the #510 retro implied.

## Fixtures added

Layout (32 enforced + 14 silent_contract, 31 fixtures hit the
harness; one (the existing `pub_enforced_negative_*` from #510
under `examples/modules/`) is not re-counted here):

```
examples/negative/
├── actor_row/                  # category 10 — 3 PASS
│   ├── missing_actor_reply.kai
│   ├── receive_no_actor_row.kai
│   └── send_no_actor_row.kai
├── derive/                     # category 3 — 3 PASS
│   ├── derive_binserialize_sum_payload.kai
│   ├── derive_eq_no_field_eq.kai
│   └── derive_show_no_field_show.kai
├── ffi/                        # category 8 — 3 PASS
│   ├── extern_with_body.kai
│   ├── legacy_extern_c_axiom.kai
│   └── non_c_abi.kai
├── handle_leak/                # category 4 — 3 PASS
│   ├── handle_unknown_op.kai
│   ├── op_arg_type_mismatch.kai
│   └── op_in_pure_fn.kai
├── mutable/                    # category 6 — 4 PASS
│   ├── field_array_write_no_row.kai
│   ├── param_array_write_no_row.kai
│   ├── param_array_write_other_row.kai
│   └── raw_array_set_no_row.kai
├── oneshot/                    # category 5 — 3 PASS (runtime)
│   ├── resume_collect_pair.kai
│   ├── resume_twice.kai
│   └── resume_with_values_twice.kai
├── pub_effect/                 # category 2 — 3 PASS
│   ├── pub_fn_match_arm_effect.kai
│   ├── pub_fn_unannotated_effect.kai
│   └── pub_fn_unit_leak_stdout.kai
├── pub_enforcement/            # category 1 — 3 PASS
│   ├── violation_constructor/{lib,main}.kai
│   ├── violation_import/{lib,main}.kai
│   └── violation_prelude/{secret_lib,secret_lib.prelude}.kai
├── shadowing/                  # category 9 — 3 PASS
│   ├── user_err_collides_with_result.kai
│   ├── user_ok_collides_with_result.kai
│   └── user_some_collides_with_option.kai
├── stage1_rejections/          # category 7 — 3 PASS (via kaic1)
│   ├── effect_decl.kai
│   ├── impl_decl.kai
│   └── protocol_decl.kai
└── silent_contract/            # 14 silent — 3 issues filed
    ├── README.md
    ├── duplicate_type_decl.kai
    ├── ffi_call_no_capability.kai
    ├── handle_clause_missing_resume.kai
    ├── handle_clause_wrong_arity.kai
    ├── handle_partial_with_other_effect.kai
    ├── handle_residual_effect.kai
    ├── main_row_user_effect.kai
    ├── mutable_op_no_handler_or_row.kai
    ├── mutable_param_write_pure_row.kai
    ├── mutable_through_field.kai
    ├── pub_fn_mutable_unannotated.kai
    ├── pub_fn_transitive_effect.kai
    └── tree_collision_via_import/{lib,main}.kai
```

Goldens use the substring-grep convention (first line of
`.err.expected` is grep -F'd against stderr) — same shape as
`test-modules-qualified-neg`. This keeps the goldens insensitive
to minor diagnostic-text refactors while still asserting the
load-bearing message.

## Coverage gaps left for next lanes

- **Refinement-violation runtime panics** — `docs/refinements`
  describes `assert` and contract-violation messages with
  structured context. The existing `test-violations` target in
  `stage2/Makefile` covers them with byte-exact goldens; not
  duplicated here.
- **Stage 0 parser negatives** — stage 0 is even more
  restricted than stage 1. Negative-test the rejection of
  `protocol`/`impl`/`effect`/`pub`/`use`/etc. at stage 0
  level. Out of scope here because stage 0 is C source that
  the tier1 build path doesn't exercise on `.kai` inputs (it
  builds stage 1 from `stage1/compiler.kai`).
- **Holes** (`#hole`) — `docs/holes.md` says the typer should
  produce structured JSON when a hole is reached. The
  `library_mode` diagnostics fixtures cover the JSON shape;
  not duplicated here.
- **LSP diagnostics** (issue #447) — once the LSP ships,
  every fixture in `examples/negative/` becomes a natural
  cross-validation source for the JSON shape. No work
  required until the LSP lands.

## Real cost vs estimate

- Estimate: 2–3 days.
- Actual: one focused session — ~3-4 hours of probing,
  authoring, gating, retro. The harness is small (~140 LoC);
  the fixture work was repetitive (write + run + golden);
  the audit (filing 3 issues for silent contracts) consumed
  another ~30 minutes.

Velocity benefitted from doing the audit work in line with
fixture authoring: every silent contract was caught the moment
the fixture didn't reject, then turned into either a follow-up
issue or a `silent_contract/` quarantine entry. No retros got
batched, no findings got lost.

## Follow-ups left for next lanes

- **#516** — Effect-row inference: qualified-effect calls do
  not propagate. Largest of the three: affects `Mutable`,
  `Ffi`, transitive `pub fn` rows.
- **#517** — handle block residual effects + clause shape.
  Touches the same row-inference machinery but also adds new
  validation passes (clause arity, with-effect existence).
- **#518** — Type-collision via import + duplicate decl.
  Resolver / module layer, not typer.

Each of those is a candidate lane. When any of them closes,
the closing PR should move the relevant fixtures out of
`silent_contract/`, add their `.err.expected` goldens, and
update `silent_contract/README.md` to remove the row.

## Lane discipline notes

- One fix, one lane: the lane added fixtures + a harness, no
  compiler changes. Selfhost is byte-identical (the kaikai
  pipeline never saw the new `.kai` files in `examples/negative/`
  during stage1/stage2 self-build).
- tier0 + tier1 green locally before push.
- Doc + retro live in the same commit train as the framework
  and fixture commits, per the lane-retro discipline pinned in
  CLAUDE.md (issue #511 is a non-trivial new test surface —
  every future lane will reference `test-negative` and
  `silent_contract/`, so the retro is load-bearing).
