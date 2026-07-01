# Lane experience — issue #1025 bug#2: native self-tail arm alias-binder UAF (+ escalated bug#3)

## Scope as planned vs as shipped

**Planned (the #1025 bug#2 contract):** with the #1027 cons-reuse SIGSEGV
closed, the native-built compiler LINKs + RUNs (`--version`) but aborts with
`field access on non-record` in `validate_pub_access` → `vpa_sig_te` when it
compiles ANY program. The brief's starting diagnosis (from the #1025 retro):
a shared-substructure UAF surfacing during vpa, `TypeExpr` records physically
shared between `prelude_segs` and `qualified_prelude`. Close it → recursive
self-host unblocks → escalate the #1021 gate from LINK+RUN to full self-compile.

**Shipped:** a real, distinct native miscompile found and fixed — but it is NOT
the whole of bug#2. Reduction from the real compiler split the residual into two
independent native codegen bugs:

- **bug#2a (FIXED):** an OWNED, non-reuse self-tail `match` arm whose pattern
  binds a POINTER-kind sub-binder over a BORROW path (a nested variant slot in a
  list head, or deeper) never got the structural `KDup` the C oracle's
  `emit_pat_binds` emits under `is_alias`. The arm's pre-injected scrutinee drop
  (`match_selftail_scr_drop`, the #860 leak fix) then freed the borrowed binder
  before the arm body read it. This is the #858 fix generalised past the flat
  cons head/tail to nested variant slots at any depth.

- **bug#3 (ESCALATED, not fixed):** the compiler's ACTUAL vpa crash is a
  DIFFERENT mechanism — a live, healthy variant node's payload slots get
  overwritten as-if-cons during the prelude walk (a slot-overwrite, NOT a
  free-recycle). Characterised below; could not be reduced to a standalone
  fixture despite ~15 attempts, so it is escalated rather than force-fixed.

Because bug#3 still blocks end-to-end compile, the circle does NOT close: this
lane does **not** `closes #1025` / `closes #1021`, and the #1021 gate stays at
LINK+RUN.

## bug#2a — root cause and fix

The minimal repro (native crashes / diverges at -O2 AND -O0; C oracle correct):

```kaikai
type Ty = { tn: Int }
type Dec = D(Ty)
fn walk(ds: [Dec], acc: Int) : Int = match ds {
  [] -> acc
  [D(tg), ...r] -> walk(r, acc + tg.tn)   # self-tail; tg borrowed from head
}
```

KIR of the self-tail arm (both backends share this lowering):

```
L4:
    dup r                 # #858: list ...rest binder dup'd
    drop .t0$sd           # match_selftail_scr_drop — pre-injected scrutinee drop
    .t8 = kai_op_field_at(tg, 0)   # reads tg, BORROWED from the just-dropped scrutinee
    tcrec-goto ...
```

`tg` is bound `tg: box = proj .t4.0` — a `KProj` BORROW of the head variant's
slot, no incref. The list `...rest` binder `r` was dup'd by the #858 fix
(`list_arm_alias_binders`), but `tg` — a binder NESTED inside the list head's
`D(...)` variant sub-pattern — was not in that set. When the pre-injected
`drop .t0$sd` cascades through the head cons and the `D` node, it frees `tg`'s
`Ty` record; `kai_op_field_at(tg, 0)` then reads recycled memory (a `type
mismatch in +` or a wrong value). The C oracle's `emit_pat_binds` increfs EVERY
alias binder recursively (list → variant slot → …), so C never sees it.

The `kir_lower_match.kai` comment already flagged this exact gap as a follow-up:
the #858 fix restored the incref for the flat cons head/tail set only and left
"the variant / record arm paths … the analogous gap exists there" for later.

**Fix (two files):**

1. `kir_lower_match.kai` — `owned_arm_alias_binders(p, vs)`: a recursive
   collector mirroring the oracle's `emit_pat_binds` alias set. Descends
   `PList` (heads + `...rest`), `PVariant` POINTER slots (Int/Real/enum excluded
   by `ls_slot_kind`), and `PAs` (name + inner). It does NOT descend into record
   fields — `kai_op_field` already increfs those (their subtree is owned,
   `is_alias=false` in the oracle), so an extra dup would double-incref.

2. `kir_lower_walk.kai` — `lower_list_arms` now dups
   `owned_arm_alias_binders(p, st_body.vs)` (was: `list_arm_alias_binders(p)`
   only) for the `owned ∧ ¬reuse` arm.

Verified fixed (native == C oracle) at both -O0 and -O2:
- `[D(tg), ...r] -> walk(r, acc + tg.tn)` (nested variant slot)
- `[DType(_, TBRec(fields)), ...] -> lookup(rest, …)` (variant-in-variant,
  the `lookup_record_field` shape the residual UAF also hit via infer)
- two consecutive impls in a reconstructed list (the `impl Numeric` shape)

## bug#3 — characterisation (ESCALATED)

With bug#2a fixed, the native-built compiler still aborts compiling any program:
`vpa_sig_te` reads the `impl Numeric for Real` `target_te` as a non-record.

Repro handle (native, -O2): build the native compiler, compile any `.kai`:

```
make KAI_LLVM=1 kaic2
cd stage2 && KAI_NATIVE_RUNTIME_BC=../stage0/runtime_llvm.bc \
  ./kaic2 --emit=native --path ../stdlib main.kai       # -> main.o
cd .. && tools/native-selfhost-link.sh stage2/main.o /tmp/kaic2-native
echo 'fn main() : Unit / Console = print("hi")' > /tmp/hi.kai
/tmp/kaic2-native --emit=c --path stdlib /tmp/hi.kai    # ABORTS: field access on non-record
```

Evidence gathered (instrumented, since reverted):

- The crash is on the SECOND `impl Numeric` (Real). The first (Int) walks fine.
- Instrumenting the runtime `kai_op_field_at` abort: the offending value has
  `tag = KAI_CONS (7)`, `rc = 6` — a LIVE, HEALTHY cons cell where a `TypeExpr`
  RECORD is expected. So it is a TYPE CONFUSION / slot overwrite, NOT a
  free-after-use (a freed cell would have rc≈0 / garbage tag). Confirmed with
  `KAI_TRACE_RC=1` (free-lists off): the crash is identical, ruling out
  free-list recycling.
- Reading the 2nd impl's `sub_decls` reports `list_length = 257` (an impl has 8
  methods). 257 ≈ the number of decls remaining in `qualified_prelude` from that
  point on — i.e. the DImpl node's field-3 slot holds the OUTER list tail, and
  its field-2 (`target_te`) holds a cons. The node's payload slots have been
  overwritten AS-IF it were a cons cell (head/tail), while its variant TAG is
  still `DImpl`.
- Timeline: at `qualified_prelude` build (`rqc_decls`) and immediately after
  `collect_pub_access_table`, BOTH impls read `subs = 8` (clean). The 257
  appears only once `vpa_decls_loop` starts WALKING the list. A read-only walk
  of `qualified_prelude` performed TWICE back-to-back makes the SECOND walk loop
  forever — the first read has turned the spine into a CYCLE. So the read walk
  itself mutates the list: a native-codegen defect in an OWNED self-tail list
  walk over a spine whose element nodes are SHARED (`target_te` aliased with the
  still-live `prelude_segs` via the `qualtype_te` bare-name passthrough).
- Present on `main` (pre-fix) too — bug#2a's fix neither caused nor cured it;
  the two are independent.

Why not fixed here: exhaustive reduction (nested-variant heads, two-impl
reconstructed lists, shared element nodes across two lists, consume-one-then-
read-other, a 300-decl prelude mirror with `Console + File`, TRMC-shaped
rebuilds) all reproduce the RC/borrow shapes but NONE reproduce the
slot-overwrite. The trigger depends on a perceus ownership decision that only
emerges from the real compiler's AST graph. Forcing a fix without a reproducing
fixture would violate the RC-change discipline (serial parity, a regression
fixture that crashes-before / passes-after). Escalated to the owner: this is a
chain, and the owner decides whether the next lane pushes on bug#3.

## Verification (this lane)

- bug#2a repros: native == C oracle at -O0 and -O2.
- `make selfhost` byte-id: green (C oracle unchanged — the extra `KDup` matches
  the oracle's existing alias-incref, so the C emit is byte-identical).
- Serial backend parity (`BACKEND_PARITY_JOBS=1`, mandatory for an RC change).
- Regression fixture: `examples/perceus/selftail_nested_variant_binder_1025.kai`
  (+ `.out.expected`) — crashes native pre-fix, matches the C oracle post-fix.

## Follow-ups left for next lanes

- **bug#3** blocks full recursive self-host. The #1021 gate stays at LINK+RUN.
  Next step: a hardware watchpoint on the 2nd `impl Numeric` DImpl node's
  payload slot (address captured at the post-table walk) to catch the writer,
  or bisect the perceus RC decisions over the real `qualified_prelude`. The
  slot-overwrite-as-cons signature points at a variant node donated to a cons
  reuse (`KConReuse`) as if unique while still shared — a cross-shape reuse the
  #1027 same-shape cons fix did not cover.
- The top-level variant path (`dup_variant_arm_binders` /
  `variant_arm_alias_binders`) still uses the SHALLOW binder set; a nested
  variant binder in a top-level (non-list) owned variant arm has the same
  latent gap bug#2a fixed for list arms. Not exercised by the crash path, and
  the #858 retro warns a blunt variant-wide dup regressed 12 fixtures, so it
  needs the recursive collector gated exactly like the list path — a separate,
  fixture-driven lane.
