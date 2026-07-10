# Lane experience — issue #1156: write-side raw ctor-slot permit, re-landed

Closes #1156. The lane re-lands the write-side raw ctor-slot permit that
#1136 withdrew with discipline, after root-causing the native self-compile
SIGSEGV the withdrawal documented; fixes the compare+store binder
mode-loss (with the existence-scan, not a codegen change); and rides the
mechanical `alwaysinline` on `kai_variant_reuse_at`.

The permit is now SOUND — the #1136 escape is closed, self-host is green
— but the projected ~8-10% rb-tree win does NOT materialise: the honest
measurement is ~2% (inside the noise floor). The remaining lever is the
call-arg increment (follow-up 2), not the permit. This lane's value is
unblocking that increment with a sound write-side permit, not the bench
number. See "Measured".

## The root cause, exactly

The #1136 retro carried the crash (`EXC_BAD_ACCESS` in
`perceus__perceus_decl_b`, a raw i64 of value 4 dereferenced as a pointer
at slot offset 8) and the two-probe bisect isolating the permit as the
trigger, but not the mechanism. The mechanism is a **name-collision
register widening whose store side had no box border**:

1. The native emitter keeps ONE register (alloca) per binder NAME per
   function. Two sibling match arms binding the same name at different
   KSlots — one arm's binder seeded raw by the permit (compared + stored
   into a rebuild), the other arm's same-named binder boxed (an
   unmodelled use) — merge in `rspec_add`, which widens the register to
   `SBoxed`, "the universal slot every value fits".
2. But the `KLet` emission materialised its op at the KIR-PLANNED slot
   (`nemit_op_fx_slot` — for the raw arm, a raw `.i64` slot read) and then
   stored **blind** (`nemit_store_reg`), never consulting the table. The
   raw word landed in the ptr-wide alloca; every boxed-border read of the
   register (`kaix_int_field`, direct ptr loads) then dereferenced a raw
   integer as a `KaiValue *`. Value 4 was a LINE NUMBER: `perceus_decl_b`
   binds `ln`/`cl` on kind-1 Int slots in several arms; the permit made
   some of those arms raw (stored into the `DFn(...)` rebuild) while
   sibling arms stayed boxed — the first same-name mixed-verdict
   collisions at scale.
3. The load side and `KStore` already had total borders
   (`nemit_load_reg_boxed` / `nemit_store_slot_aware`); the `KLet` store
   was the one missing seam. The permit did not create the hazard — it
   created the first program (the compiler itself) that exercised it.

The mechanism follows directly from reading `nemit_stmt`'s `KLet` arm
against `rspec_add`: the op is materialised at the KIR-planned slot
(`nemit_op_fx_slot`) and stored with `nemit_store_reg`, which never
consults the register's actual (possibly widened) slot. Every other seam
in `emit_native_fn` (the load side, `KStore`) already has a total border;
the `KLet` store was the one that stored blind. `perceus_decl_b` binds
`ln`/`cl` on kind-1 Int slots across several arms — exactly the shape the
crash frame named — so the permit's first same-name mixed-verdict
collision at compiler scale reproduced the escape.

**The fix** (`nemit_store_reg_boxing`): the `KLet` store consults the
register's actual slot; a raw-planned value stored into a widened `SBoxed`
register boxes at the border (`kaix_int`/`kaix_real`/`kaix_bool`),
mirroring `nemit_load_reg_boxed` in the other direction. A planned-boxed
value can never meet a raw register slot (widening only promotes TO
`SBoxed`), so the border is one-directional. Matching slots — the
overwhelmingly common case — store untouched, zero cost. Confirmed by the
native self-host gate: the native-built kaic2 now compiles `main.kai`
end-to-end (where it SIGSEGV'd before) and self-compiles byte-identical to
the oracle.

Why this fix and not a KIR-side one-verdict-per-name rule: the
name-keyed register table is an EMITTER artefact; `bind_slot_kslot` sees
one arm's body and cannot know a sibling arm rebinds the name. The
emitter alone sees the merge (`rspec_add` is where the widening lives),
so the emitter border is the aligned seam — the same philosophy as every
other border in `emit_native_fn`.

## The compare+store mode-loss — fixed by the existence-scan alone

The #1136 evidence: a compare+store binder (`kx` in the
`variant_raw_rebuild_1136` shape) reached `bind_slot_kslot` with the
FIRST `EVar(kx)` read carrying no `MUnboxed`, because perceus's dup-wrap
(`pcs_rewrite_kind`) rebuilds a non-last read as `__perceus_dup(...)` with
the default mode. A compare+store binder always has a non-last read (the
compare), so a first-node scan missed the seed at precisely the binders
the permit exists for.

The fix is the **existence-scan** `bind_slot_kslot` discriminant (any
`MUnboxed` read decides, not the first) — re-landed from the #1136 probe
state. A compare+store binder's STORE read (the last use, into the
rebuild) is not dup-wrapped, so it keeps its seed, and the existence-scan
finds it however many earlier reads perceus rebuilt with the default
mode. Verified: `kx` in the rebuild lowers `int64_t kair_kx = …[1].i64`,
compares raw (`kair_k > kair_kx`), and stores raw (`{.i64 = kair_kx}`).

A first attempt also preserved the mode on the dup-wrapped read itself
(threading `emode` into `pcs_rewrite_kind`). That was BOTH unnecessary —
the existence-scan already sees the store read's seed — AND a regression:
the emitter resolves a raw variant-slot binder by `kind1_raw` membership,
never by a read's mode, so preserving `MUnboxed` on a non-variant binder
whose bind is boxed (`n` in fizzbuzz's `match i`) made the emitter emit an
undeclared raw `kair_n` at the dup'd read (`use of undeclared identifier
'kair_n'`). The full-corpus backend-parity run caught it — three demos
(`fizzbuzz`/`collatz`/`attr_unstable_refine_narrow`) C-failed. perceus is
left untouched; the existence-scan carries the whole fix.

## Scope as planned vs as shipped

- **Write-side permit** (shipped): `native_arm_binder_raw_safe` accepts a
  binder as a DIRECT arg of a variant-ctor call in a kind-1 Int slot
  position, resolved through the same `CtorIntSlots` registry the seeding
  reads. The reuse-in-place ctor init lowers a kind-1 Int slot through
  `nemit_atom_raw`, which loads the binder at its own register slot (raw
  i64 direct, boxed unbox-borrowed) — total in both directions, so a raw
  binder lands in the raw slot and a boxed one is borrowed down, never a
  deref hazard.
- **`CtorIntSlots` builtin-tag guard** (shipped): `build_ctor_int_slots`
  now applies the SAME builtin-tag guard `typed_inits` carries — a ctor
  with `tag < user_variant_tag_base()` has all-boxed slots in the table.
  Without it the permit's `cis` and the actual write layout could skew: a
  mixed-writer builtin-tag ctor writes boxed (`typed_inits` guard) while
  an unguarded `cis` would still mark its Int slot raw, so the permit
  could seed a binder raw for a slot the ctor writes boxed. One table, one
  guard — read-side seeding and write-side permit share the source of
  truth, matching `slot_kind_of`'s sentinel. (The #1136 doctrine: a static
  kind is sound only when the tag has exactly ONE writer layout.)
- **KLet store border** (shipped): the root-cause fix above.
- **Existence-scan KSlot discriminant** (shipped): re-landed from the
  #1136 probe state — the whole compare+store mode-loss fix (perceus
  untouched, see above).
- **`alwaysinline` on `kai_variant_reuse_at`** (shipped): `runtime.h`,
  mirroring the `runtime_llvm.c` shims.
- **perceus dup-wrap mode preservation** (NOT shipped): tried, reverted —
  unnecessary and a cross-backend regression (see the mode-loss section).

## Measured

rb-tree bench, 1M random inserts, native backend, macOS arm64, 10
interleaved samples base-vs-lane to cancel thermal drift:

- baseline (permit reverted): median **0.500s**, min 0.484s
- this lane (permit live):     median **0.491s**, min 0.484s

**The permit does NOT move the rb-tree needle measurably** (~2%, inside
the noise floor — both share a 0.484s min). The ~8-10% the #1136 retro
projected does not materialise, and the honest reason is follow-up 2: the
hot binder is `insert_loop`'s Black-arm `kx`, which flows into
`balance_left`/`balance_right` as a plain CALL argument — a shape the
permit does not model, so it stays boxed. The permit models the
compare+store-into-rebuild, which the bench's descent does hit, but the
rebuild is not the bottleneck; the balance calls are. Native vs C-direct
on the same bench is ~1.28x (0.491s vs 0.409s median), unchanged by this
lane. The lane's load-bearing win is CORRECTNESS: the write-side permit
re-lands sound (the #1136 escape closed, self-host green), unblocking the
call-arg increment (follow-up 2) that should actually move the bench.

Counters (#1104 parity, unchanged — the border is a store-time repr
coercion, not an RC operation, so it touches no incref/decref/alloc):

- `test-perceus-1104-balance-token-donation`: `alloc_total=100015 <
  150000`, no `reuse_freed` — arm token still donated across balance.
- `test-perceus-1104-sibling-param-move`: `incref+decref=200000 < 600000`,
  `alloc_total=100015` — sibling-param move + reuse RC elision intact.

## Fixtures added (wired)

- `examples/perceus/variant_raw_widen_1156.kai` — the escape shape
  reduced: two sibling arms bind the SAME names (`kx`/`vx`) on kind-1 Int
  slots with opposite verdicts (raw compare+store vs boxed call use); the
  keys are EVEN so a missing store border is a deterministic
  tagged-immediate-check failure + deref, not a silent misread. Green on
  both backends with the border (C oracle, native, ASAN-clean); the same
  widening shape at compiler scale was the #1136 self-compile SIGSEGV.
  Targets `test-perceus-1156-raw-widen` (TEST_LIGHT_TARGETS) +
  `test-perceus-1156-raw-widen-native` (tier1-native.yml).
- `variant_raw_rebuild_1136` now exercises the permit for real: its
  binders lower raw end to end (pre-lane they stayed boxed).

## Structural surprises

1. **The escape was never about the ctor slot.** The #1136 withdrawal
   reasoned about the WRITE path (raw word into the raw slot — which was
   always sound); the actual escape was the REGISTER, a per-name
   resource two arms share. The permit's only causal role was creating
   same-name mixed verdicts.
2. **The border closes the class, not the instance.** The crash frame
   (`perceus_decl_b`) was one same-name mixed-verdict collision; the
   compiler's own source has many kind-1 Int binders across sibling arms,
   any of which the permit could turn into the same widening. A KIR-side
   per-name rule cannot see the merge (the register table is an emitter
   artefact), so the emitter border is the only seam that closes every
   instance at once.
3. **The mode-loss needed no codegen fix — just a better scan.** The
   #1136 retro suspected tcrec/TRMC/guard-walk, then this lane's first
   attempt patched the perceus dup-wrap. Both were wrong: the compare+
   store binder's STORE read (last use, not dup-wrapped) always keeps its
   seed, so the existence-scan discriminant finds it with perceus
   untouched. The perceus patch was not just redundant — it regressed the
   C backend (undeclared `kair_n` on non-variant binders). The lesson: a
   pass that "fixes" a mode the DOWNSTREAM consumer resolves by another
   channel (here `kind1_raw` membership) is fixing the wrong layer.
4. **Full-corpus parity is the only gate that caught the perceus
   regression.** The perceus mode-preservation passed every #1136/#1156
   fixture AND the native self-host gate; it broke three demos
   (`fizzbuzz`/`collatz`/`attr_unstable_refine_narrow`) that live in
   `demos/` — a tree NO tier compiles. Only the manual full-corpus
   `test-backend-parity.sh` (run serially per this lane's brief) reached
   them. A codegen change touching a cross-backend seam must run it.
5. **The permit's `cis` table lacked `typed_inits`' builtin-tag guard.**
   A soundness review of the write-side permit surfaced that
   `build_ctor_int_slots` classified an Int slot purely by `variant_slot_
   kind`, with no builtin-tag guard, while the actual write layout
   (`typed_inits`) boxes every slot of a builtin-tag ctor. The permit and
   the write layout consulted the same registry but applied DIFFERENT
   guards — the exact skew the #1136 one-writer-layout doctrine warns
   about. The store border made it non-fatal (a raw arg re-borrows at the
   ctor init), but it wasted the permit and would become a live bug the
   day a ctor init fast-path trusted `SIInt64 == raw-store`. Fixed at the
   table's source, so both consumers inherit the guard.

## Cost vs estimate

Root cause and mechanism were already isolated in the #1136 retro (the
register-widening escape + the compare+store mode-loss); the store border
and the existence-scan were mechanical once the mechanism was clear. The
unplanned cost was two-fold: the soundness review of the write-side permit
(which found the `cis` builtin-tag skew, surprise 5), and unwinding the
prior attempt's perceus mode-preservation after full-corpus parity caught
it regressing three demos. Net, the shipped change is SMALLER than the
withdrawn #1136 attempt: perceus untouched, the fix concentrated in the
emitter border + the seeding table's guard.

## Follow-ups (documented, not filed)

1. `demos/vs/python/main.kai` fails the C oracle on a PRE-EXISTING typer
   error (`type annotation needed: the receiver of .amount is not
   resolved`), independent of this lane — reproduced on the clean base
   with all changes stashed. It lives in `demos/`, which no tier compiles,
   so it never gated CI. Out of this lane's scope; not fixed inline.
2. The Black-arm `kx` in `insert_loop` still takes the boxed verdict:
   it flows into `balance_left`/`balance_right` as a plain call arg,
   which the permit does not model. Accepting a binder as a direct arg
   of a call whose CALLEE param is raw (the `prc_raw_param_names`
   registry perceus already consults) is the next increment of this
   lever — same one-verdict discipline, one more modelled consumer.
3. Native full-mask parity (#1136 follow-up 2) unchanged.
