# Lane experience — issue #1021: single alias-binder collector (the structural close)

The closing lane for #1021. Its remit was not "fix bug #N" but the collapse
Linus made the close condition: replace the N ad-hoc alias-binder collectors
of the native lowering with ONE, a mirror of the C oracle's `emit_pat_binds`,
so the N+1 bug of the chain (#1027 → #1028 → #1035 → #1036) cannot appear.

## Scope as planned vs shipped

Planned: collapse the collectors, close the latent top-level-variant gap, and
verify the native self-compile circle closes end-to-end.

Shipped: exactly that. New module `stage2/compiler/kir_pat_alias.kai` (km A+,
101 LOC) holds the single recursive collector `pat_alias_binders` plus two
pure position predicates that partition it. All five ad-hoc collectors in
`kir_lower_match.kai` are gone (that file shrank 626 → 417 LOC, A 90.4), and
every call-site in `kir_lower_walk.kai` now reads the one collector or a named
partition of it. The native-vs-C parity is 24/24; the self-compile circle
closes byte-for-byte except a single residual of a DIFFERENT family (native
Real-literal encoding), reported below and left to a follow-up.

## The collapse: one source of truth, partitioned

The oracle decides alias-ness in one textual walk: a binder reached with
`is_alias=true` shares storage with the scrutinee (a variant pointer slot, a
cons head/tail/rest) and gets a structural `kai_incref`; a binder reached with
`is_alias=false` already owns its ref (a `kai_op_field` result, a boxed Real
slot). The native backend re-derived that discipline with FIVE collectors,
each keyed by arm shape:

- `list_arm_alias_binders` — flat cons head/tail.
- `owned_arm_alias_binders`/`oab_pat` — recursive (the only faithful mirror).
- `decon_arm_alias_binders` — recursive minus the direct cons slots.
- `variant_arm_alias_binders` — FLAT top-level variant pointer slots.
- `variant_reuse_scrutinee_binders` — variant reuse, one nested level.

Bugs #1028 and #1035-bug#4 were both literally "used the flat collector where
the recursive one was needed" — the same one-line fix twice. Each new arm
shape inherited the hole.

The collapse keeps ONE definition of "which binders are alias" —
`pat_alias_binders`, the recursive mirror of `emit_pat_binds` (descends PList
heads+rest, PVariant pointer slots by kind, PAs; excludes record fields
because `kai_op_field` already increfs; excludes Int/Real slots by kind). The
other roles are PARTITIONS of it, by pure position predicate, never a re-walk:

- ROL A — dup #858 (owned non-reuse arm): `pat_alias_binders` (the full set).
- ROL B — decon dual-branch: `pat_decon_alias_binders` = A minus direct slots.
- ROL C — reuse-gc / arm-top token: `pat_direct_alias_slots` (the direct
  cons/variant top-level slots) for the cons fork and the arm-top-reuse token;
  the FULL `pat_alias_binders` for a variant rotation's reuse-gc set.

The invariant that makes this correct is not "same static incref count" but
"same binder, same regime" (unconditional / shared-branch-only), guaranteed by
deriving all four sets from the same collector.

## The latent gap the collapse closed by construction

asu's review found the chain's next bug before it fired: `variant_reuse_-
scrutinee_binders` descended exactly ONE nested level, so a rotation that
re-embeds children TWO pointer levels deep (`Node(_, Node(_, Node(R,a,..,b),
..),..)` lifting `a`/`b`) left those grand-grand-children un-dup'd. Under a
shared donor the match-exit `decref(donor)` would free them mid-rebuild. The
oracle's `emit_pat_binds_variant` DOES incref them (two levels of
`is_alias=true`), so the recursive collector covers them by construction. The
reuse-gc variant call-site now uses the full `pat_alias_binders`; the arm-top
token stays on `pat_direct_alias_slots` because the oracle's `arm_ptr_binders`
is deliberately top-level (the token rebuilds the top-level ctor) — verified,
not assumed.

## The circle closes

`make KAI_LLVM=1 kaic2` self-compiles `stage2/main.kai` (~58s), the object
links via `tools/native-selfhost-link.sh`, the native-built binary runs
(`--version`), compiles a sample program byte-identically to the oracle, and
compiles the WHOLE compiler (128360 lines) with three lines of difference —
all three in Real literals (see residual). The gate
`tools/test-native-selfhost-gate.sh` now escalates from LINK+RUN to full
SELF-COMPILE (emits C for a sample and diffs against the oracle).

CWD matters: the self-compile must run from the repo ROOT
(`--path stdlib stage2/main.kai`), not from `stage2/`, because a transitive
`import` of a `stdlib/core/*.kai` module resolves relative to the CWD — the
oracle behaves identically, so both must run in the same directory.

## Residual — a DIFFERENT family, not this lane

The three lines that differ are all Real literals: native emits
`kai_real(3.95253e-323)` where the oracle emits `kai_real(3.14159)` /
`kai_real(0.0)`. `3.95253e-323` is the bit pattern of the integer 8;
`4.44659e-323` is 9. The native-built compiler reinterprets a small integer's
raw bits as a double when re-emitting a Real literal. This lives in the native
Real-literal codegen (`kir_lower_lit` / `emit_native_lit`, untouched by this
lane, git diff = 0), is orthogonal to alias-binder dup discipline, and is a
candidate for a new issue. Per the close criterion it is "one minor residual
of another family" — the structural collapse is the close regardless.

## Fixture

`examples/native/two_level_alias_binder.kai` exercises the recursive collector
at depth two. It uses the OWNED path (reads the whole `l` again so perceus
emits fresh-alloc + owned dups, NOT the reuse recogniser) because a two-level
rotation THROUGH the reuse recogniser trips a pre-existing FRONTEND bug: a
three-level nested discriminating pattern with reuse makes the oracle's
`emit_pat_binds` skip the deepest binders (`kai_a`/`kair_vx` come out
undeclared in the emitted C). That is family B (perceus↔emit desync), not this
lane; the owned path both backends handle, and native-vs-C parity is green.

## What made this lane tractable

The diagnosis was already closed by the chain retros and asu's family split
(A = native alias-binder collectors; B = frontend perceus↔emit pairs). This
lane went straight to the collapse. asu's one correction — that the three
roles are not a clean partition, C2 had its own one-level descent — was the
difference between closing the symptom and closing the cause.

## Shim lockstep, caught by escalating the gate

Escalating the gate to require LINK + SELF-COMPILE immediately caught a gap
the previous LINK+RUN gate could not: PR #1030's struct-by-value FFI added two
in-process libLLVM prims (`kai_llvm_struct_type`, `kai_native_target_abi`)
without the matching non-static thunks in
`stage0/runtime_llvm_native_shim.c`, so the self-compiled object referenced
`kaix_prelude_llvm_struct_type` / `kaix_prelude_native_target_abi` and did not
link. It shipped because `tier1-native` is not a required check. This lane
adds the two thunks (the shim stays in lockstep with `native_prims.kai`). The
lesson: every PR adding an in-process libLLVM prim must add its shim thunk;
the escalated gate now enforces it, and it becomes merge-blocking the day
`tier1-native` is promoted to required.

## Follow-ups

- New issue: native Real-literal encoding emits a small int's raw bits as a
  double when the native-built compiler re-emits a Real literal.
- New issue (family B): three-level nested discriminating pattern + reuse makes
  the C oracle's `emit_pat_binds` leave the deepest binders undeclared.
- Once the Real-literal residual is fixed, the self-compile diff is empty and
  `tier1-native` can be promoted to a required check (integrator's call).
