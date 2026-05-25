# Lane experience — issue #677 phase 1p: extract compiler/protos.kai

The m12.8 single-dispatch protocol system, lifted out of `main.kai` into
a new module. Promoted **ahead** of the N2 monomorph lane: the abandoned
monomorph-extract lane discovered ~537 LOC / 31 fns of protocol-resolve
coupling (`monomorphise` calls `resolve_protocol_calls_decl`) that the
original section analysis #692 had missed. Mirroring that surface into a
monomorph module would have duplicated the whole protocol system;
pre-sinking only the `resolve_*` fns would have split one cohesive
domain across two modules. Integrator call: extract protocols whole
first, then re-run N2 importing it downward.

This is the largest single-block lane in phase 1 (4080 LOC of code,
~4334 with the header + mirrors), and the one with the sharpest
structural surprise: the obvious module name `protocols` is **already
taken** by the stdlib prelude, and the flat bundle hides the clash
completely.

## Scope as planned (the brief)

Move the three contiguous m12.8 sub-sections — (A) definition + lowering,
(B) post-inference protocol-call rewrite, (C) Eric level-2 rep-invariant
verifiers — from `main.kai:18687–22809` (banner-to-banner) into a new
`compiler/protocols.kai`. Imports expected: `ast`, `diag`, `util`,
`modules`, `infer`. Mirror the AST-functor family + a couple of helpers
downward with a `proto_` prefix. Public surface estimate 10–25.

## Scope as shipped

Delivered as one source block, one new module, with three deviations the
grep-level analysis could not see — one of them load-bearing.

### 1. The module is named `protos`, not `protocols` (load-bearing)

`import compiler.protocols` does not resolve to `compiler/protocols.kai`.
The module binding is the **last dotted segment** (`protocols`), and that
name is already registered by the implicit stdlib prelude
(`stdlib/protocols.kai` — the user-facing Show / Eq / Ord / Hash impls,
loaded from `KAI_STDLIB_PATH` on every compile). The importer resolves
`protocols` to the already-registered stdlib module instead of this file,
so the AST variant namespace this module pattern-matches on
(`TyName`, `UOne`, `POR`, `DFn`, `DType`, `TBSum`, …) is invisible, and
the compile dies with ~330 "unknown variant" errors.

The trap: **the flat bundle masks it entirely.** `awk 1 $(BUNDLE_SRCS)`
concatenates everything into one global namespace, so `kaic1
build/bundle.kai` builds `kaic2` clean (exit 0), and `kaic2` compiling
the *bundle* is fine. The clash only appears when `kaic2` compiles the
**modular** `main.kai` and processes the `import compiler.protocols`
line against the prelude-populated module table. This is the N1/N3
"modular kaic2 exposes what the flat bundle hides" lesson — but landing
on the *module name itself*, not on an upward symbol reference.

Diagnosis path (recorded so the next agent skips it): the errors point
at misattributed `main.kai:96` line numbers, and `infer.kai` does the
identical `TyName`/`UOne` matching with the same imports and works — so
imports were ruled out. A byte-identical copy under a *different* name
(`protocols_trunc.kai`) imported clean while `compiler.protocols` failed
at 330 errors. That isolated the cause to the name in one step. Renaming
to `protos` fixed it: 0 errors, selfhost byte-identical.

The brief fixed the name as `compiler/protocols.kai`; correctness
overrode it (the name was simply unusable). The lane took the call
locally rather than escalating — it is an implementation fact, not a
language-surface or authorization decision.

### 2. `LoweredProtocols` had to join the public surface

The brief's pub estimate covered the fns; it missed that
`lower_protocols` *returns* `LoweredProtocols`, a module-local record.
The modular compiler rejected the pub fn exposing a non-pub type:
`error: pub fn lower_protocols exposes non-pub type LoweredProtocols`.
Marked it `pub`. Final public surface: **14** — 2 types
(`OpAr`, `LoweredProtocols`) + 12 fns. `LowerImplsRes`, `StmtsAcc`,
`StmtTyped`, `InvBlockRes` stay private (no external signature touches
them). This is the same "pub fn drags its return type pub" shape the
bundle hides and the modular build exposes — worth pre-checking every
pub fn's return + param types against the module-local type list before
the first selfhost.

### 3. Five mirrors, not the "a couple" estimate

The AST-functor family `map_expr_kind` pulls its whole transitive family
(`map_arm_exprs` / `map_field_exprs` / `map_elem_exprs` /
`map_stmt_exprs` / `map_clause_exprs` / `map_return_exprs` /
`map_opt_return_exprs` / `map_opt_expr`), and `type_expr_display`
transitively needs `unit_expr_display`, and `make_dispatcher_body` needs
the dispatcher-marker builder. All copied verbatim from the canonical
`inf_*` versions in `infer.kai` with a `proto_` prefix (single consumer
each here ⇒ mirror, not sink — the phase-1m `dsg_` discipline). The
`"__kai_proto_dispatch__:"` marker prefix is byte-pinned in the mirror:
the desugar + emitter sides (still in main) read it back.

## Cross-section findings for downstream lanes

- **N2 monomorph (relaunches next).** `resolve_protocol_calls_decl` is
  now `pub fn` in `compiler.protos`. Monomorph imports protos downward
  and calls it per-decl — no duplication, no mirror. This is the whole
  reason this lane went first. `validate_resolved_decls` is also pub for
  the post-mono coherence check the driver runs at `mono_decls`.
- **emit_c / emit_llvm.** The emitter (still in main) consumes
  `proto_dispatch_tag_prefix` / `body_is_proto_dispatch` /
  `proto_dispatch_parts` — these live near desugar in main (~8544), NOT
  in this block, and were left there. The emitter also calls
  `string_to_lower_ascii` (now pub here) to fold effect names.
- **driver.** Calls `lower_protocols`, `op_arities_from_ops`,
  `collect_local_fn_arities`, `filter_shadowed_ops`,
  `rename_proto_calls_decls`, `resolve_protocol_calls`,
  `validate_resolved_protocols`, `validate_typer_invariants` — all pub.
- **desugar (still in main).** Calls `proto_dispatcher_name` to stringify
  `#{...}` interpolations into dispatcher calls — pub.
- **The `protos` name is a precedent.** Any future `compiler.<X>` module
  whose last segment collides with a stdlib module name (`array`,
  `effects`, `core/*`, `protocols`) will hit the same wall. Check
  `stdlib/*.kai` + `stdlib/core/*.kai` filenames before naming a module.

## Fixtures + coverage

`stage2/tests/test_protos.kai`: **16 unit tests + 2 property checks**,
all green (`kai test` 16/16; `kai check` 2/2, 100 iter each). Unit tests
cover the pure helpers (`proto_dispatcher_name`, `string_to_lower_ascii`,
`op_arities_from_ops`, `filter_shadowed_ops`, `collect_local_fn_arities`)
and `lower_protocols` end-to-end through the real parser (plain module
untouched; `protocol` decl registers its op + emits a dispatcher + drops
the protocol node). Property checks use `Int` generators — the
pre-existing `--prop-check` String-generator fragility still reproduces
on main, so both checks drive `Int`.

Gap: the post-inference rewrite (B) and the rep-invariant verifiers (C)
are exercised only transitively via selfhost, not by a direct unit test
in this file — they need a fully-typed `TypedProgram`, which the test
harness can build via `infer_program` (as `test_perceus.kai` does) but
the lane did not, to keep the fixture focused on the lowering path. A
follow-up could add a typed-program rewrite assertion.

## Cost vs estimate

Bigger than the brief implied. The mechanical relocation + pub/doc pass
was ~1 unit as estimated. The module-name collision was the unbudgeted
cost: ~half the lane went into diagnosing 330 misattributed-line errors
on a byte-identical module that compiled fine as a root file and as a
flat bundle but failed only as a modular import. The "rename a copy and
re-import" probe is the fast path; it should be the first move whenever a
new module produces a wall of `unknown variant` errors that no other
module with the same imports produces.

## Public surface (14)

Types: `OpAr`, `LoweredProtocols`.
Fns: `lower_protocols`, `op_arities_from_ops`, `collect_local_fn_arities`,
`filter_shadowed_ops`, `rename_proto_calls_decls`, `proto_dispatcher_name`,
`resolve_protocol_calls`, `resolve_protocol_calls_decl`,
`validate_resolved_protocols`, `validate_resolved_decls`,
`validate_typer_invariants`, `string_to_lower_ascii`.
