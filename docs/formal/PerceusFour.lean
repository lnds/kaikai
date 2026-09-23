/-
  Drop-once across all four param release emitters.

  Diagnostic model. Not a gate, not built, not run by CI. See README.md.

  Written against: 0de97943 (bump 0.123.0), stage2/compiler/perceus.kai.

  `Perceus.lean` models one emitter pair on one shape. This one asks the
  wider question: across every emitter that can release an owned PARAM,
  and every classification the pass can assign it, does some combination
  release twice (or leak)?

  The four emitters, and where the driver wires them
  (`pcs_rewrite_fn_body`, perceus.kai:1874-1984):

    pcs_collect_entry_drops        4468   entry, unconditional
    pcs_collect_exit_drops         4481   exit,  gated by skip/force/count
    pcs_inject_param_branch_drops  5108   per-arm, for owned_moves
    the move's consuming use              per-path, for skip_set params

  The coupling that makes them interact (perceus.kai:1885):

      skip_set = skip_set0 ∪ owned_moves

  so a param selected for the owned-in-scope move is in `skip_set`, which
  is exactly the test `pcs_collect_exit_drops` uses to withhold the exit
  drop. Branch drops replace the exit drop; they do not add to it. That
  substitution is the invariant this file checks.

  The intended partition is stated in the driver's own comment
  (perceus.kai:4409-4421):

    LUUnused        → entry drop
    LUBlocked       → exit drop
    LUAt, count ≥ 2 → exit drop   (every read dup-wrapped)
    LUAt, count = 1 → no drop     (the read transfers the ref)

  Verify with `lean PerceusFour.lean` (exit 0, no output). Lean 4, no
  Mathlib.
-/

namespace PerceusFour

/-- `LU`, infer.kai:1615. -/
inductive LU where
  | at (pos : Nat)
  | blocked
  | unused
  deriving DecidableEq, Repr

/-- The param's state going into the drop pass. `skip` and `forced` are
    the two sets the driver threads into `pcs_collect_exit_drops`;
    `owned` is membership in `owned_moves`. `nonLamUses` is
    `pcs_count_non_lam_uses`. -/
structure Cfg where
  lu         : LU
  /-- In `skip_set`: reads transfer raw, no dup. Implied by `owned`. -/
  skip       : Bool
  /-- In `force_all`: read dup'd, exit drop forced to pay the birth ref. -/
  forced     : Bool
  /-- Selected by `pcs_owned_scope_move_params`. -/
  owned      : Bool
  /-- `pcs_count_non_lam_uses`; only ≥2 vs <2 matters. -/
  nonLamUses : Nat
  /-- `pcs_consumed_on_every_path`. False means the single read sits in
      some-but-not-all arms, so the paths that skip it own the birth ref
      with no consumer. -/
  everyPath  : Bool
  deriving DecidableEq, Repr

/-- Where a reference is released on a path through arm `i`. -/
inductive Site where
  | entry
  | exit
  | arm (i : Nat)
  | consume (i : Nat)
  deriving DecidableEq, Repr

def firesOn (i : Nat) : Site → Bool
  | .entry     => true
  | .exit      => true
  | .arm j     => j == i
  | .consume j => j == i

def onPath (sites : List Site) (i : Nat) : List Site :=
  sites.filter (firesOn i)

/-- Exactly one release on the path through each of the `n` arms. -/
def soundOn (sites : List Site) (n : Nat) : Prop :=
  ∀ i, i < n → (onPath sites i).length = 1

instance (sites : List Site) (n : Nat) : Decidable (soundOn sites n) := by
  unfold soundOn
  exact Nat.decidableBallLT n _

/-- `pcs_collect_entry_drops`, perceus.kai:4468. -/
def entryDrops (c : Cfg) : List Site :=
  match c.lu with
  | .unused => [.entry]
  | _       => []

/-- `pcs_collect_exit_drops`, perceus.kai:4481. `LUBlocked` always;
    `LUAt` only when not skipped AND (forced OR ≥2 non-lambda uses). -/
def exitDrops (c : Cfg) : List Site :=
  match c.lu with
  | .blocked => [.exit]
  | .at _    => if c.skip then []
                else if c.forced || c.nonLamUses ≥ 2 then [.exit]
                else []
  | .unused  => []

/-- The consuming arm for `.at p` in an `n`-arm match. -/
def consumingArm (p n : Nat) : Nat := p % n

/-- Arms that never read the param. -/
def deadArms (c : Cfg) (n : Nat) : List Nat :=
  match c.lu with
  | .unused => List.range n
  | .at p   => (List.range n).filter (· != consumingArm p n)
  | .blocked => []

/-- `pcs_inject_param_branch_drops`, perceus.kai:5108: a drop in every
    arm that does not read the param, for each param in `owned_moves`.

    Post-#2084, `pcs_owned_scope_move_params` will not select a never-read
    param, so `owned ∧ .unused` is unreachable in the real pass. The model
    does NOT encode that here — it is the fence being tested, and
    hard-coding it would assume the answer. `gateIsReachable` below states
    it as a precondition instead. -/
def branchDrops (c : Cfg) (n : Nat) : List Site :=
  if c.owned then (deadArms c n).map Site.arm else []

/-- The consuming read. `pcs_is_non_last` (perceus.kai:3060) wraps a read
    in `__perceus_dup` only when it is NOT the last use, so an `LUAt`
    param transfers its birth ref raw at the last read — `skip` only
    removes the dup on the earlier reads.

    Two exceptions, both of which make the last read consume a DUP
    rather than the birth ref, leaving the exit drop as the payer:

    - `force_set` dups on every read, last included (perceus.kai:3072).
    - `≥2` non-lambda uses: the driver's own categorisation
      (perceus.kai:4418) — every read is dup-wrapped, so the original is
      unconsumed. Not skipped, since `skip` is what removes those dups.

    This models the BIRTH ref only; the dups a consumer receives are
    balanced by that consumer and are out of scope. -/
def consumeSites (c : Cfg) (n : Nat) : List Site :=
  match c.lu with
  | .at p   => if n == 0 then []
               else if c.forced then []
               else if c.nonLamUses ≥ 2 && !c.skip then []
               else if c.everyPath then (List.range n).map Site.consume
               else [.consume (consumingArm p n)]
  | _       => []

/-- Every release the pass emits for one param. -/
def releases (c : Cfg) (n : Nat) : List Site :=
  entryDrops c ++ exitDrops c ++ branchDrops c n ++ consumeSites c n

/-! ## Reachability

The driver does not produce every `Cfg`. A configuration must satisfy
these to describe a real param; asking about the others reports bugs in
states the compiler never builds. -/

/-- `owned_moves ⊆ skip_set` — perceus.kai:1885. -/
def ownedImpliesSkip (c : Cfg) : Bool := !c.owned || c.skip

/-- `pcs_owned_scope_move_params` fence 6 (perceus.kai:5045-5047, the
    #2084 fix): a never-read param is not selected for the move. -/
def gateIsReachable (c : Cfg) : Bool := !(c.owned && c.lu == .unused)

/-- The move and the branchy-single-use force are mutually exclusive:
    `force_all` exists to make a read dup, `skip_set` to make it transfer
    raw. `pcs_branchy_single_use_params` takes `skip` as an exclusion. -/
def notBothSkipAndForced (c : Cfg) : Bool := !(c.skip && c.forced)

/-- A never-read param has no reads, so nothing forces a dup on it and
    its use count is zero.

    `skip` is NOT excluded: `skip_set` also receives `owned_moves`
    (perceus.kai:1885), so a never-read param wrongly selected for the
    move lands in it. Excluding it here would hide exactly the #2084
    state from the search. -/
def unusedIsInert (c : Cfg) : Bool :=
  !(c.lu == .unused) || (!c.forced && c.nonLamUses == 0 && (!c.skip || c.owned))

/-- `LUBlocked` means a closure capture blocks last-use reasoning; the
    move and the force both require a known single read. -/
def blockedIsInert (c : Cfg) : Bool :=
  !(c.lu == .blocked) || (!c.skip && !c.forced && !c.owned)

/-- A skipped `.at` param transfers at a single read, so it cannot have
    ≥2 non-lambda uses (`pcs_branch_aware_skip_params` gates on
    `max_paths ≤ 1`). -/
def skipImpliesSingleUse (c : Cfg) : Bool :=
  !c.skip || c.nonLamUses < 2

/-- `LUAt` means the param HAS a last-use position, so it has at least
    one non-lambda read. A zero count with `.at` is not a state the
    classifier produces. -/
def atHasARead (c : Cfg) : Bool :=
  match c.lu with
  | .at _ => c.nonLamUses ≥ 1
  | _     => c.nonLamUses == 0

/-- `pcs_branch_aware_skip_params_b` (perceus.kai:3293-3297) admits a
    param only when `pcs_consumed_b_expr` holds — consumed on EVERY path.
    So `skip_set0` membership implies `everyPath`.

    `owned_moves` is the deliberate exception: it is unioned into
    `skip_set` afterwards (perceus.kai:1885) and is branchy by
    construction, with the branch drops paying the arms that do not
    read. That is the substitution this file exists to check, so `owned`
    is exempt here rather than excluded. -/
def skipImpliesEveryPath (c : Cfg) : Bool :=
  !c.skip || c.owned || c.everyPath

/-- `pcs_branchy_single_use_params` (perceus.kai:4565): a single-use
    `LUAt` param NOT consumed on every path and not already skipped is
    put in `force_all` — its read is dup'd and the exit drop pays the
    birth ref. The paths that skip the read would otherwise leak.

    So a branchy single-use param is never left bare: it is skipped or
    forced. This is a fence, not a fact about the emitters, and removing
    it is what `counterexamplesNoBranchyForce` below tests. -/
def branchyIsTreated (c : Cfg) : Bool :=
  match c.lu with
  | .at _ => if c.nonLamUses == 1 && !c.everyPath then c.skip || c.forced
             else true
  | _     => true

/-- `everyPath` describes where the read is, so it only means anything
    for a param that HAS a read. -/
def everyPathNeedsRead (c : Cfg) : Bool :=
  match c.lu with
  | .at _ => true
  | _     => !c.everyPath

/-- The owned-in-scope move targets a param read in one arm and not the
    others — if every arm consumed it there would be no dead arm to plant
    a branch drop in, and nothing to gain. -/
def ownedIsBranchy (c : Cfg) : Bool := !c.owned || !c.everyPath

def reachable (c : Cfg) : Bool :=
  ownedImpliesSkip c && gateIsReachable c && notBothSkipAndForced c
    && unusedIsInert c && blockedIsInert c && skipImpliesSingleUse c
    && branchyIsTreated c && everyPathNeedsRead c && ownedIsBranchy c
    && atHasARead c && skipImpliesEveryPath c

/-! ## The search space -/

def luSpace (maxPos : Nat) : List LU :=
  .unused :: .blocked :: (List.range maxPos).map LU.at

def cfgSpace (maxPos maxUses : Nat) : List Cfg :=
  (luSpace maxPos).flatMap fun lu =>
    [false, true].flatMap fun skip =>
      [false, true].flatMap fun forced =>
        [false, true].flatMap fun owned =>
          (List.range maxUses).flatMap fun uses =>
            [false, true].map fun ep =>
              { lu := lu, skip := skip, forced := forced, owned := owned,
                nonLamUses := uses, everyPath := ep }

/-- Reachable configurations that violate drop-once at `n` arms. -/
def counterexamples (n maxPos maxUses : Nat) : List Cfg :=
  (cfgSpace maxPos maxUses).filter fun c =>
    reachable c && !(decide (soundOn (releases c n) n))

/-! ## Results -/

/-- The search has teeth: of 512 configurations in the space, 80 are
    reachable, spanning all three `LU` classes and both the `owned` and
    `forced` treatments. A green result below is not an empty quantifier. -/
theorem search_space_is_populated :
    ((cfgSpace 6 4).filter reachable).length = 80 := by native_decide

theorem search_covers_owned :
    ((cfgSpace 6 4).filter (fun c => reachable c && c.owned)).length = 6 := by
  native_decide

/-- **No double release and no leak.** Every reachable configuration
    releases each owned param exactly once on every path, at 2, 3 and 4
    arms, over all consuming positions below 6 and use counts below 4.

    This is the question asked: the four emitters do not collide. -/
theorem no_violation_2 : counterexamples 2 6 4 = [] := by native_decide
theorem no_violation_3 : counterexamples 3 6 4 = [] := by native_decide
theorem no_violation_4 : counterexamples 4 6 4 = [] := by native_decide

/-- The #2084 shape, with fence 6 removed. Without
    `gateIsReachable` the search finds the historical bug: a never-read
    param selected for the move takes the entry drop AND a branch drop in
    every arm.

    This is what makes the green result above meaningful — the search
    does find a collision when one exists. -/
def counterexamplesNoFence6 (n maxPos maxUses : Nat) : List Cfg :=
  (cfgSpace maxPos maxUses).filter fun c =>
    (ownedImpliesSkip c && notBothSkipAndForced c && unusedIsInert c
      && blockedIsInert c && skipImpliesSingleUse c && branchyIsTreated c
      && everyPathNeedsRead c && ownedIsBranchy c && atHasARead c
      && skipImpliesEveryPath c)
    && !(decide (soundOn (releases c n) n))

theorem fence6_is_load_bearing : counterexamplesNoFence6 2 6 4 ≠ [] := by
  native_decide

/-- And the violation it finds is precisely the never-read owned param. -/
theorem fence6_counterexample_is_2084 :
    counterexamplesNoFence6 2 6 4
      = [{ lu := .unused, skip := true, forced := false, owned := true,
           nonLamUses := 0, everyPath := false }] := by
  native_decide

/-- The skip/exit coupling is load-bearing too: if a moved param kept its
    exit drop (drop the `skip` test in `pcs_collect_exit_drops`), the
    consuming path releases twice. This is the #599 comment's claim,
    checked rather than trusted. -/
def exitDropsIgnoringSkip (c : Cfg) : List Site :=
  match c.lu with
  | .blocked => [.exit]
  | .at _    => if c.forced || c.nonLamUses ≥ 2 then [.exit] else []
  | .unused  => []

def releasesIgnoringSkip (c : Cfg) (n : Nat) : List Site :=
  entryDrops c ++ exitDropsIgnoringSkip c ++ branchDrops c n ++ consumeSites c n

/-- With the skip test gone, a forced-exit param that is also skipped
    would double-release — which is why `notBothSkipAndForced` holds in
    the real pass. Stated over configurations that are otherwise
    reachable. -/
theorem skip_test_is_load_bearing :
    ((cfgSpace 6 4).filter fun c =>
      (ownedImpliesSkip c && gateIsReachable c && unusedIsInert c
        && blockedIsInert c && skipImpliesSingleUse c && branchyIsTreated c
        && everyPathNeedsRead c && ownedIsBranchy c && atHasARead c)
      && !(decide (soundOn (releasesIgnoringSkip c 2) 2))) ≠ [] := by
  native_decide

/-! ## What this does not cover

Only owned params of a fn whose body is a top-level match. Not modelled:

- Arm binders and block-let binders (`pcs_arm_drop_pass`,
  `ptd_tail_exit_drops`), which have their own emitters and their own
  skip/force sets. They release DIFFERENT references than the param
  emitters here, so they cannot collide with these four — but they can
  collide with each other, which is a separate model.
- Reuse (`pcs_recognise_reuse_expr_b`), TRMC/goto lowering, and the
  self-recursion fences that `pcs_branchy_single_use_params` applies.
  Goto lowering bypasses the exit wrap per iteration, which the release
  sets here assume runs once.
- Borrowed and raw params, which the driver removes from `drop_params`
  before the entry/exit collectors see them (perceus.kai:1967).
- Guards. `pcs_param_each_arm_move_safe` rejects a param read in a guard;
  the model has no guard positions, so it cannot check that fence.
-/

end PerceusFour
