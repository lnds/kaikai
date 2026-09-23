/-
  Drop-once for arm binders and block-let binders.

  Diagnostic model. Not a gate, not built, not run by CI. See README.md.

  Written against: the unified payer (`perceus_payer.kai`), plus
  stage2/compiler/perceus.kai and stage2/compiler/emit_shared.kai.

  `PerceusFour.lean` covers owned PARAMS. Params cannot collide with
  binders — they release different references — but the binder emitters
  can collide with each other, and they are the shape behind #1784,
  #1786, #1791 and #1758. This file models both binder families.

  A binder's birth ref comes from the bind-time incref: `emit_pat_binds`
  increfs every alias extraction for an arm binder, and an `SLet` binds
  the rhs value. Exactly one payer must close it.

  ARM BINDERS — `pcs_collect_arm_drops` (perceus.kai:5517), planted by
  `pcs_arm_drop_arms` (5379). Four exclusions before the rule:

    in outer_scope             owned by an enclosing scope
    pre_tail ∧ read in tail    drop site precedes the tail; skipped here
    guard has a bare read      the guard consumed it on both edges
    elided to `_`              no bind-incref was emitted at all

  then, on arm-LOCAL use counts (a rescan, NOT the fn-wide table — the
  global table over-counts when sibling arms bind the same name):

    read inside a lambda       → drop
    ≥2 arm-local reads         → drop (every read dup-wrapped)
    0 reads, not `_`-named     → drop (nothing else pays the incref)
    exactly 1 read             → no drop (the last read transfers raw)

  BLOCK-LET BINDERS — `pcs_collect_block_let_exit_drops`
  (perceus.kai:4935). Three exclusions, then a rule mirroring the param
  exit drop:

    not bound in this block    outer scope owns it
    read in the block tail     `ptd_tail_exit_drops` pays it instead
    in move_last_set           the move already paid at the last use

    LUBlocked                  → drop
    LUAt, ≥2 uses or forced    → drop
    LUAt, 1 use                → no drop (last read transfers raw)
    LUUnused                   → whichever side `pcs_let_paid_inline`
                                 names: the emitter's inline decref if
                                 it holds, perceus if it does not

  That last line is the cross-module coupling this file exists to check.
  The payer for an unused let may live in the EMITTER rather than in
  perceus, and both sides must ask the SAME predicate — when they asked
  two different ones (`is_fresh_alloc` vs a syntactic shape test), every
  rhs satisfying neither fell between them and leaked.

  Verify with `lean PerceusBinders.lean` (exit 0, no output). Lean 4, no
  Mathlib.
-/

namespace PerceusBinders

/-! ## Shared vocabulary -/

/-- `LU`, infer.kai:1615, for block-lets (which consult the fn-wide
    table). Arm binders use arm-local counts instead. -/
inductive LU where
  | at (pos : Nat)
  | blocked
  | unused
  deriving DecidableEq, Repr

/-- Where a binder's birth ref is released. `inline` is the emitter's
    decref for an unused fresh let; the others are perceus drops. -/
inductive Site where
  /-- `pcs_collect_arm_drops` / `pcs_collect_block_let_exit_drops`. -/
  | drop
  /-- `ptd_tail_exit_drops`, after the tail. -/
  | tailDrop
  /-- `block_unused_lets`, inline right after the SLet. -/
  | inlineDecref
  /-- The last read transferring the ref raw to its consumer. -/
  | consume
  /-- The guard consuming the binder on both edges. -/
  | guardConsume
  deriving DecidableEq, Repr

/-- Drop-once, relative to whether a birth ref exists at all.

    A binder that took a bind-incref must be released exactly once. A
    binder the pass elided to `_` never took one, so releasing it zero
    times is correct and releasing it once would over-free. Every site
    here fires on every path through the arm/block that owns the binder
    — the per-arm path split is `PerceusFour.lean`'s subject. -/
def soundFor (hasRef : Bool) (sites : List Site) : Prop :=
  sites.length = (if hasRef then 1 else 0)

instance (hasRef : Bool) (sites : List Site) : Decidable (soundFor hasRef sites) := by
  unfold soundFor; infer_instance

/-! ## Arm binders -/

/-- An arm binder's state. Counts are ARM-LOCAL (`pcs_collect_uses_expr`
    rescan at perceus.kai:5538), not the fn-wide table. -/
structure ArmCfg where
  /-- Shadows a name from `outer_scope`. -/
  inOuter     : Bool
  /-- The arm body is an `EBlock` with a tail (`pre_tail` at 5397). -/
  preTail     : Bool
  /-- The binder is read in that tail. -/
  readInTail  : Bool
  /-- The guard has a bare read of the binder (5528). -/
  guardRead   : Bool
  /-- Named `_` / `_foo`: no bind-incref is emitted. -/
  underscore  : Bool
  /-- Arm-local non-lambda read count. -/
  armReads    : Nat
  /-- Some read sits inside a lambda (5540). -/
  inLambda    : Bool
  deriving DecidableEq, Repr

/-- `pcs_arm_elide_names` (5410): a plain `PBind` never read anywhere in
    the arm is rewritten to `_`, so no bind-incref happens. Modelled as
    "no birth ref to pay". -/
def armElided (c : ArmCfg) : Bool :=
  c.armReads == 0 && !c.inLambda && !c.guardRead && !c.readInTail

/-- `pcs_collect_arm_drops`, perceus.kai:5517. -/
def armDrops (c : ArmCfg) : List Site :=
  if c.inOuter then []
  else if c.preTail && c.readInTail then []
  else if c.guardRead then []
  else if c.inLambda then [.drop]
  else if c.armReads ≥ 2 then [.drop]
  else if c.armReads == 0 && !c.underscore then [.drop]
  else []

/-- `pcs_arm_tail_drop_body` (5424): a binder the pre-tail site skipped
    because its read is in the tail gets paid after the tail instead. -/
def armTailDrops (c : ArmCfg) : List Site :=
  if c.inOuter then []
  else if c.preTail && c.readInTail && !c.guardRead then
    -- The tail read transfers the ref when it is the only read; with
    -- ≥2 reads the dups leave the birth ref for this payer.
    (if c.armReads ≥ 2 || c.inLambda then [.tailDrop] else [])
  else []

/-- The consuming read. A single arm-local read, not in a lambda,
    transfers the birth ref raw. A guard read consumes on both edges
    (5399-5400) and is counted as the payer instead. -/
def armConsume (c : ArmCfg) : List Site :=
  if c.guardRead then [.guardConsume]
  else if c.inLambda then []
  else if c.armReads == 1 then [.consume]
  else []

/-- An elided binder takes no bind-incref, so it has no birth ref. -/
def armHasRef (c : ArmCfg) : Bool := !armElided c

def armReleases (c : ArmCfg) : List Site :=
  if armElided c then []
  else armDrops c ++ armTailDrops c ++ armConsume c

/-! ### Arm reachability -/

/-- A binder in `outer_scope` is not this arm's to release; its releases
    belong to the enclosing scope, so the model excludes it rather than
    reporting a leak. -/
def armOuterIsOther (c : ArmCfg) : Bool := !c.inOuter

/-- `readInTail` describes where a read is, so it needs a read, and it
    only means anything when the body HAS a tail. -/
def armTailNeedsRead (c : ArmCfg) : Bool :=
  (!c.readInTail || (c.armReads ≥ 1 && c.preTail))

/-- A guard read is a read: it cannot coexist with a zero count. -/
def armGuardNeedsRead (c : ArmCfg) : Bool :=
  !c.guardRead || c.armReads ≥ 1

/-- `name_is_unused_binder` (5545) tests the NAME, so an `_`-named
    binder that is nonetheless read is not a state the surface produces:
    `_` is not referenceable. -/
def armUnderscoreIsUnread (c : ArmCfg) : Bool :=
  !c.underscore || (c.armReads == 0 && !c.inLambda && !c.guardRead)

/-- A lambda read is a read. -/
def armLambdaNeedsRead (c : ArmCfg) : Bool :=
  !c.inLambda || c.armReads ≥ 1

def armReachable (c : ArmCfg) : Bool :=
  armOuterIsOther c && armTailNeedsRead c && armGuardNeedsRead c
    && armUnderscoreIsUnread c && armLambdaNeedsRead c

def armSpace (maxReads : Nat) : List ArmCfg :=
  [false, true].flatMap fun io =>
    [false, true].flatMap fun pt =>
      [false, true].flatMap fun rt =>
        [false, true].flatMap fun gr =>
          [false, true].flatMap fun us =>
            (List.range maxReads).flatMap fun n =>
              [false, true].map fun il =>
                { inOuter := io, preTail := pt, readInTail := rt,
                  guardRead := gr, underscore := us, armReads := n,
                  inLambda := il }

def armCounterexamples (maxReads : Nat) : List ArmCfg :=
  (armSpace maxReads).filter fun c =>
    armReachable c && !(decide (soundFor (armHasRef c) (armReleases c)))

/-! ## Block-let binders

The shipped rule (`perceus_payer.kai`, `pcs_let_payer`) names exactly ONE
payer per binder. The two sides consult the SAME predicate,
`pcs_let_paid_inline`, which replaced `is_fresh_alloc`: the emitter pays
iff it holds, perceus iff it does not. Complementary by construction.

The earlier state was two predicates that were not complements —
`is_fresh_alloc` on the emitter side and `pcs_rhs_is_bare_var` on the
perceus side — and every rhs that satisfied neither fell between them.
Measured then: `if`, `match`, a block, a field access and a pipe rhs all
leaked 30 over 10 calls. -/

/-- `PcsPayer` (perceus_payer.kai:27). A collector plants a drop only
    when the payer names it, so two collectors cannot both pay.

    The shipped type has six variants; this file models the three a
    block-let can take, plus `none`. `entry` and `branch` belong to
    params (`PerceusFour.lean`).

    Note what is NOT a variant: there is no `tail` payer. A binder whose
    read is in the tail is still `exit` — `ptd_needs_drop`
    (perceus_tail_drop.kai:122) asks the same `pcs_fate_of` and guards
    with `ptd_has_drop` so the drop is planted once. The tail is a
    different SITE for the same payer, not a different payer. An earlier
    draft of this model invented a `tail` payer; the shipped type has
    none. -/
inductive Payer where
  /-- `PyExit` — the block-exit collector, or the post-tail pass when
      the read is in the tail. -/
  | exit
  /-- `PyInline` — `block_unused_lets`, inline in the emitter. -/
  | inline
  /-- `PyRead` — the last read transfers the ref on. -/
  | read
  /-- `PyNone` — nothing to pay: no birth ref exists. -/
  | none
  deriving DecidableEq, Repr

structure LetCfg where
  lu          : LU
  /-- Bound by THIS block, not visible in `outer`. -/
  inBlock     : Bool
  /-- Read in the block's tail expression. -/
  inTail      : Bool
  /-- In `move_last_set`: already paid at the last use. -/
  moves       : Bool
  /-- In `force_set`: dups on every read, so the birth ref survives. -/
  forced      : Bool
  /-- `pcs_count_non_lam_uses` over the fn-wide table. -/
  nonLamUses  : Nat
  /-- `pcs_let_paid_inline` — the ONE predicate both sides ask. True
      means the emitter's inline decref pays it; false means perceus
      does. There is no third case, which is the point. -/
  paidInline  : Bool
  /-- The tail hosts a self-tail-call, which `ptd_tail_exit_drops`
      skips so TCO is not broken. -/
  tailSelfCall : Bool
  deriving DecidableEq, Repr

/-- `pcs_let_payer` (perceus_payer.kai): the single site that pays.

    Exclusions first — a binder owned by an enclosing scope, or already
    paid by a move at its last use, is not this block's to release. Then
    the classification decides, and for `.unused` the ONE predicate
    routes it to whichever side owns it. -/
def letPayer (c : LetCfg) : Payer :=
  if !c.inBlock || c.moves then .none
  -- A tail whose self-call the goto lowering rewrites is paid per goto
  -- path by a ledger this file does not carry.
  else if c.inTail && c.tailSelfCall then .none
  else match c.lu with
    | .blocked => .exit
    | .at _    => if c.nonLamUses ≥ 2 || c.forced then .exit else .read
    | .unused  => if c.paidInline then .inline else .exit

/-- One payer, one release. A `.none` payer means either the move already
    paid (`moves`), the enclosing scope owns it, or the goto ledger pays
    per path — none of which this model carries. -/
def letReleases (c : LetCfg) : List Site :=
  match letPayer c with
  | .exit   => [if c.inTail then .tailDrop else .drop]
  | .inline => [.inlineDecref]
  | .read   => [.consume]
  | .none   => if c.moves && c.inBlock then [.consume] else []

/-! ### Block-let reachability -/

/-- A binding owned by an enclosing scope is not this block's to pay. -/
def letInBlockOnly (c : LetCfg) : Bool := c.inBlock

/-- `.unused` means no read anywhere, so it is in no read-shaped set and
    has no tail read and no use count. -/
def letUnusedIsInert (c : LetCfg) : Bool :=
  !(c.lu == .unused)
    || (!c.inTail && !c.moves && !c.forced && c.nonLamUses == 0)

/-- `.at` has at least one read; `.blocked` is counted through the
    closure, not as a plain non-lambda read. -/
def letAtHasARead (c : LetCfg) : Bool :=
  match c.lu with
  | .at _    => c.nonLamUses ≥ 1
  | .blocked => c.nonLamUses == 0
  | .unused  => c.nonLamUses == 0

/-- `move_last_set` holds binders whose LAST read lands in a consuming
    slot — it needs a read, and a forced binder dups instead of moving. -/
def letMoveNeedsRead (c : LetCfg) : Bool :=
  !c.moves || (c.nonLamUses ≥ 1 && !c.forced)

/-- `.blocked` is a closure capture: the move and the force both need a
    known plain last read. -/
def letBlockedIsInert (c : LetCfg) : Bool :=
  !(c.lu == .blocked) || (!c.moves && !c.forced)

/-- A self-call in the tail only matters when the binder is read there. -/
def letSelfCallNeedsTail (c : LetCfg) : Bool :=
  !c.tailSelfCall || c.inTail

/-- `ptd_tail_exit_drops` deliberately skips a tail holding a self-call:
    the drop would hide it from `tcrec_rewrite_decls` and break TCO
    (perceus.kai:2760-2764). The payer is then the goto/TRMC ledger,
    which distributes the release per goto path and is NOT modelled
    here. Excluded, since otherwise every such binder reports as a leak
    against a payer this file does not carry. -/
def letNotTcoHandoff (c : LetCfg) : Bool := !c.tailSelfCall

def letReachable (c : LetCfg) : Bool :=
  letInBlockOnly c && letUnusedIsInert c && letAtHasARead c
    && letMoveNeedsRead c && letBlockedIsInert c && letSelfCallNeedsTail c
    && letNotTcoHandoff c

def luSpace (maxPos : Nat) : List LU :=
  .unused :: .blocked :: (List.range maxPos).map LU.at

def letSpace (maxPos maxUses : Nat) : List LetCfg :=
  (luSpace maxPos).flatMap fun lu =>
    [false, true].flatMap fun ib =>
      [false, true].flatMap fun it =>
        [false, true].flatMap fun mv =>
          [false, true].flatMap fun fo =>
            (List.range maxUses).flatMap fun n =>
              [false, true].flatMap fun fr =>
                [false, true].map fun sc =>
                  { lu := lu, inBlock := ib, inTail := it, moves := mv,
                    forced := fo, nonLamUses := n, paidInline := fr,
                    tailSelfCall := sc }

def letCounterexamples (maxPos maxUses : Nat) : List LetCfg :=
  (letSpace maxPos maxUses).filter fun c =>
    letReachable c && !(decide (soundFor true (letReleases c)))

/-! ## Results -/

/-- The searches have teeth. -/
theorem arm_space_is_populated :
    ((armSpace 4).filter armReachable).length = 40 := by native_decide

theorem let_space_is_populated :
    ((letSpace 4 4).filter letReachable).length = 150 := by native_decide

/-- **Arm binders: no double release and no leak** across every reachable
    configuration, for arm-local read counts below 4. -/
theorem arm_binders_sound : armCounterexamples 4 = [] := by native_decide

/-- **Block-let binders: clean.** No double release and no leak across
    every reachable configuration. -/
theorem let_binders_sound : letCounterexamples 4 4 = [] := by native_decide

/-- The shape that leaked before the payer was unified, kept as the
    regression it closes: a never-read `let` the emitter did not pay.

    The defect was that the two sides asked DIFFERENT predicates —
    `is_fresh_alloc` in the emitter, `pcs_rhs_is_bare_var` in perceus —
    which are not complements, so every other rhs fell between them.
    Measured then, 10 calls each, on C: an `if`, a `match`, a block, a
    field access and a pipe rhs each leaked 30. The corpus held 8
    programs in this class that no gate saw.

    `pcs_let_paid_inline` is now the single predicate: the emitter pays
    iff it holds, perceus iff it does not. -/
def letLeak : LetCfg :=
  { lu := .unused, inBlock := true, inTail := false, moves := false,
    forced := false, nonLamUses := 0, paidInline := false,
    tailSelfCall := false }

theorem letLeak_is_paid_once : letReleases letLeak = [.drop] := by decide

theorem inline_side_still_pays :
    letReleases { letLeak with paidInline := true } = [.inlineDecref] := by
  decide

/-- The property that makes the class closed rather than enumerated:
    for a never-read binder the payer is total and single-valued — never
    `.none`, never both sides. A boolean predicate cannot leave a gap. -/
theorem unread_always_has_exactly_one_payer :
    ∀ paid : Bool,
      (letReleases { letLeak with paidInline := paid }).length = 1 := by
  decide

/-- And no reachable configuration takes two payers. -/
theorem never_both_payers :
    ((letSpace 4 4).filter fun c =>
      letReachable c && (letReleases c).length > 1) = [] := by native_decide

/-- The tail-drop handoff is load-bearing: a binder whose read is in the
    tail is declined by the block-exit collector and paid after the tail
    instead. Removing that payer leaks it. -/
theorem tail_site_is_reachable :
    ((letSpace 4 4).filter fun c =>
      letReachable c && letPayer c == .exit && c.inTail) ≠ [] := by
  native_decide

/-! ## What this does not cover

- The arm model treats the arm as one path. Which ARM a release lands in
  is `PerceusFour.lean`'s question; here every site fires on every path
  through the arm that binds the name.
- Destructuring `SLet` (PRecord, PVariant, PTuple) — `emit_let_stmt`
  decrefs the composite temp and perceus ignores non-`PBind` patterns
  (perceus.kai:4930-4932).
- Rest / `@` / narrowing binders, which keep the drop path even when a
  plain `PBind` would be elided (perceus.kai:5408-5409).
- The tcrec goto ledger, which reads an arm drop as a consume
  (perceus.kai:5548-5549). Its interaction with `armTailDrops` under
  TRMC is not modelled.
- Handler clause binders (`EHandle` / `HR`), walked by the same pass with
  their own scope threading.
- Raw locals excluded by `prc_pat_bindings_skip_raw`, which carry no rc.
-/

end PerceusBinders
