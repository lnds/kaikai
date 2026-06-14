# Lane experience — native-hash-proto-dispatch

**Scope as planned:** close the native-backend hash proto-dispatch cluster —
`hashmap_basic`, `hashmap_collision`, `hashset_basic`, `hashset_ops`,
`crypto_hash_basic` (5 fixtures the in-process libLLVM backend diverged on).
The brief's premise: all 5 fail because the native backend never populates
the protocol impl-table, so `kai_lookup_impl` returns NULL at runtime.

**Scope as shipped:** 4 of 5 fixtures closed via fixes in THREE distinct,
orthogonal subsystems — none of which the brief anticipated as separate. The
5th (`crypto_hash_basic`) is a fourth subsystem (native-backend RC ownership)
left as a precisely-diagnosed gap for a dedicated lane.

## The brief's premise was wrong for 4 of 5 fixtures

Only the *dispatch path in general* depended on the impl-table. Isolating
each remaining fixture (reproduced WITHOUT protocols where possible) surfaced
four unrelated pre-existing native bugs:

| Fixture | Root cause | Subsystem |
|---|---|---|
| `hashmap_basic` | impl-table never populated + dispatcher lowered to a panic stub | proto-dispatch |
| `hashmap_collision` | record-match over a tuple-of-sums treats every variant-field arm as catch-all → first arm matches everything | record-match discrimination |
| `hashset_basic/ops` | bare `empty()` in `hashset.from_list` resolved to the imported `hashmap.empty` instead of the local `hashset.empty` | KIR module-preference |
| `crypto_hash_basic` | native RC does not transfer `KDup` ownership into its consuming op; a TRMC loop reusing a multi-read heap param corrupts it | native-backend RC ownership |

## Fixes shipped

### 1. Proto-dispatch (the brief's actual target)
TWO gaps, not one:
- **Dispatch site**: the `__proto_<op>` dispatcher's body is an `ETodo`
  marker. C / LLVM-text re-emit it from the AST (`body_is_proto_dispatch`);
  the native path consumes KIR, where the shared lowering reduced the marker
  to `kai_prelude_panic` (`todo: __kai_proto_dispatch__:...`). Fix: lower the
  marker to `kaix_proto_dispatch<N>(proto_id, proto_name, op_name, args...)`
  in `lower_fn` (`kir_lower_fns.kai`) — a boxed call to a new runtime shim
  (`runtime_llvm.c`) that derives the head tag, resolves `kai_lookup_impl`,
  and indirect-calls. asu (logged) chose the runtime-shim over a KIR
  `KProtoDispatch` node: dispatch is a runtime detail, the cast-per-arity
  lives in C (one ABI source of truth), the IR stays ordinary calls.
- **Impl-table registration**: `_kai_proto_init_llvm` was an empty `void()`
  no-op. New `emit_native_proto.kai` (A+, 120 LOC) fills it with
  `kaix_register_one_variant_head` + `kaix_register_one_impl` per row,
  reusing the pure collectors from `emit_shared`. NO boxed shims — UNLIKE
  C / LLVM. The KIR is all-boxed, so every impl is already emitted
  `ptr(ptr,...)` (= the dispatch ABI); a `_boxed` shim would mis-call a
  `ptr(ptr)` impl as `i64(ptr)` (the `hashmap_collision` corruption before
  the shim was removed). Registers the bare `c_sym` (`nfn_sym` adds no
  `kai_` prefix — that is C's convention, not native's).

`stdlib_head_tag_int` + a new `stdlib_proto_id_int` moved to `emit_shared`
(shared runtime-table identity).

### 2. Record-match variant-field discrimination (`kir_lower_rec.kai`)
A tuple `(K1, K2)` desugars to `PRecord([fst: PVariant(K1), snd: PVariant(K1)])`.
The record-match lowering tested only LITERAL fields; a variant field counted
as no test, so `rec_arm_is_catchall` read every all-variant arm as a catch-all
and the first arm matched everything (`K1 == K2` returned `true`). Fix: reuse
the shared `lm_field_tests` (made `pub`) — which already dispatches `kai_eq_raw`
for a literal field and `kai_tag_eq` for a variant field — plus
`bind_pattern_fields_any` for the binds. Removed the now-dead literal-only
test/bind helpers. C / LLVM unaffected (they emit from the AST).

### 3. KIR module-preference (`kir_lower.kai` + `kir_lower_fns.kai`)
`hashset.from_list`'s bare `empty()` resolved to the imported `hashmap.empty`
(head of the EFn table) instead of the local `hashset.empty`, so the returned
value was a HashMap with no `inner` field → `no such field inner`. The C / LLVM
emitters call `fns_prefer_module(fns, mo)` before lowering each body; the KIR
`lower_fn` did not. Fix: new `ls_enter_fn_in(st, sym, locals, mo)` rotates the
EFn table to prefer `mo` before lowering, mirroring the oracle.

## The 4th bug — crypto, left as a documented gap

`crypto_hash_basic` does NOT use proto-dispatch (it calls `sha256`/`sha512`
directly). It hangs in `sha512`. Bisected to a minimal 65-line repro
(`/tmp/crypto_min.kai`): a TCO modulo-cons loop (`extend_loop`) that reads a
`[Int]` param multiple times and reuses it in `append1(drop1(window), wt)` on
the back-edge. Native hangs (the list param corrupts → `append1` recurses
forever); C-direct is correct.

Root cause (verified against the real native-walk IR via `KAI_NATIVE_DUMP_IR`,
NOT the `--emit=llvm` TEXT backend which is a different emitter and misled the
initial dropmask hypothesis): the native walk does **not thread a `KDup`
result into its consuming op**. For `[wt, ...acc]` the KIR is
`dup acc; .t = kai_cons(wt, acc)`; the native emits an orphaned
`kaix_internal_dup(acc)` (result discarded) then re-loads `acc` for the cons,
so ownership never transfers — the opposite of emit_c
(`kai_cons(wt, kai_internal_dup(acc))`). Coupled with the back-edge dropmask
being ignored by `nemit_tcrec`/`nemit_trmc_step`, the refcount is wrong, and
adding drops without first fixing the dup transfer double-frees.

**Critical gate correction:** a length-only check (`extend len == 64`) is
FALSE-GREEN — the corruption is in list *values*, not length. A fix here must
gate on value parity vs C-direct.

This is a native-backend RC-architecture lane (threading every `KDup` result
into its consumer across `nemit_op`/`nemit_stmt`/`nemit_atom`, then applying
the dropmask), high regression risk over the whole native corpus. Out of scope
for an impl-table lane; it has its own gate (value parity over
`examples/perceus/*` + `examples/effects/*` + a shared-list TRMC fixture +
selfhost byte-id + ASAN). `crypto_hash_basic` stays in the parity baseline.

## Fixtures + ratchet

`tools/native-parity-baseline.txt`: removed the 4 closed fixtures; kept
`crypto_hash_basic`. Each closed fixture is validated by FULL stdout diff
(value parity, not length) native == C-direct.

## Structural surprises

- The brief's single-cause premise hid 3 unrelated bugs. Isolating each
  (reproducing without protocols) was the load-bearing step, not the fix.
- All-edits-land-in-main trap: the first pass edited the MAIN checkout, not
  the worktree (cwd resets to the worktree but `file_path` used the bare repo
  path). Caught when the rebuild had no effect; transferred via patch, main
  reverted clean.
- `--emit=llvm` (TEXT) vs the in-process native walk are DIFFERENT backends.
  The crypto diagnosis only converged once read against `KAI_NATIVE_DUMP_IR`.

## Follow-ups for next lanes

- Native RC ownership transfer (the crypto root cause) — a dedicated lane.
- Module-preference for lambda/clause bodies: `lower_fn` is fixed;
  `lower_lam_fn`/`lower_clause_fn` still use the unrotated table (no observed
  bug yet, but the same class).
