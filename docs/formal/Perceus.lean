/-
  Perceus drop-once, for the owned-in-scope move.

  Diagnostic model. Not a gate, not built, not run by CI. See README.md.

  Written against: 20dcde3d (fix(perceus): release a never-read owned param
  once, not again per arm), stage2/compiler/perceus.kai.

  The invariant: an owned reference is released exactly once on every
  execution path. Two emitters can release the same param and must not
  both fire:

    pcs_collect_entry_drops        perceus.kai:4468
    pcs_inject_param_branch_drops  perceus.kai:5108

  selected by

    pcs_owned_scope_move_params    perceus.kai:5051

  Verify with `lean Perceus.lean` (exit 0, no output). Requires Lean 4,
  no Mathlib.
-/

namespace Perceus

/-- `LU`, infer.kai:1615. The last-use classification of one param. -/
inductive LU where
  /-- Last use at a position; the read transfers the ref. -/
  | at (pos : Nat)
  /-- A closure capture blocks last-use reasoning. -/
  | blocked
  /-- The body never references the param. -/
  | unused
  deriving DecidableEq, Repr

/-- Where a reference is released. Arms are indexed; the model fixes
    the arm count of one top-level match.

    A drop is not the only way a ref is released: the move consumes it
    at its single use, and that transfer is a release on that path.
    Counting drops alone would report the consuming arm as leaking. -/
inductive Site where
  /-- The entry drop, before the match. On every path. -/
  | entry
  /-- A branch-local drop inside arm `i`. Only on paths through `i`. -/
  | arm (i : Nat)
  /-- The move's consuming use inside arm `i`; the ref transfers there. -/
  | consume (i : Nat)
  deriving DecidableEq, Repr

/-- A release site fires on the path through arm `i`. -/
def firesOn (i : Nat) : Site → Bool
  | .entry     => true
  | .arm j     => j == i
  | .consume j => j == i

/-- Releases on the path through arm `i`. -/
def onPath (sites : List Site) (i : Nat) : List Site :=
  sites.filter (firesOn i)

/-- Drop-once: exactly one release on the path through each of the
    `n` arms. Both over- and under-release violate it. -/
def soundOn (sites : List Site) (n : Nat) : Prop :=
  ∀ i, i < n → (onPath sites i).length = 1

instance (sites : List Site) (n : Nat) : Decidable (soundOn sites n) := by
  unfold soundOn
  exact Nat.decidableBallLT n _

/-- `pcs_collect_entry_drops`: a never-read param takes an entry drop.
    Any other classification takes none here. -/
def entryDrops : LU → List Site
  | .unused => [.entry]
  | _       => []

/-- Arms that never read the param, for a param classified `lu` in a
    match of `n` arms.

    `.unused` means no arm reads it, so every arm is dead for it.
    `.at p` is the shape the move targets: the param is consumed at its
    single use, modelled as arm `p % n`; the others are dead. -/
def deadArms (lu : LU) (n : Nat) : List Nat :=
  match lu with
  | .unused  => (List.range n)
  | .at p    => (List.range n).filter (· != p % n)
  | .blocked => []

/-- The move's consuming use. For `.at p` the rewriter moved the param,
    so the single read in arm `p % n` takes the ref and no drop is owed
    there. `.unused` has no read to consume at; `.blocked` is not moved. -/
def consumeSites (lu : LU) (n : Nat) : List Site :=
  match lu with
  | .at p    => if n == 0 then [] else [.consume (p % n)]
  | .unused  => []
  | .blocked => []

/-- `pcs_inject_param_branch_drops` as it was before #2084.

    `pcs_owned_scope_move_params` gated on "no path reads p twice"
    (`mp <= 1`) and on every arm reading p zero or one times. A param the
    body never reads passes both vacuously: zero reads is ≤ 1, in every
    arm. So `.unused` was selected and got a branch drop in every arm —
    on top of the entry drop it already takes. -/
def armDropsBuggy (lu : LU) (n : Nat) : List Site :=
  (deadArms lu n).map Site.arm

/-- The same emitter after #2084: the selection gate skips never-read
    params (`pcs_never_read`, the same `LUUnused` test that decides the
    entry drop), so no branch drops are planted for `.unused`. -/
def armDropsFixed (lu : LU) (n : Nat) : List Site :=
  match lu with
  | .unused => []
  | _       => (deadArms lu n).map Site.arm

/-- Every release the pass emits for one param. -/
def releases (armDrops : LU → Nat → List Site) (lu : LU) (n : Nat) : List Site :=
  entryDrops lu ++ armDrops lu n ++ consumeSites lu n

/-- The bug. A never-read owned param in a two-arm top-level match is
    released twice on every path: once at entry, once in the arm.

    Nothing below names the double release; `decide` finds it. -/
theorem buggy_is_unsound :
    ¬ soundOn (releases armDropsBuggy .unused 2) 2 := by
  decide

/-- The fix, on the shape that broke: entry drop only, one release. -/
theorem fixed_is_sound_unused :
    soundOn (releases armDropsFixed .unused 2) 2 := by
  decide

/-- The fix does not regress the case the move exists for: a param read
    once takes no entry drop, a branch drop in each arm that does not
    read it, and the consuming use in the arm that does — one release
    either way. -/
theorem fixed_is_sound_read :
    soundOn (releases armDropsFixed (.at 7) 2) 2 := by
  decide

/-- The fix holds wherever the consuming use lands, not just at the one
    position checked above. `LU` is infinite (`.at` carries a `Nat`), so
    this is bounded: every consuming position below 16, at 2 and at 3
    arms. -/
theorem fixed_is_sound_read_bounded :
    ∀ p, p < 16 → soundOn (releases armDropsFixed (.at p) 2) 2 := by
  decide

theorem fixed_is_sound_read_bounded_3 :
    ∀ p, p < 16 → soundOn (releases armDropsFixed (.at p) 3) 3 := by
  decide

/-- The bug is not an artefact of the two-arm case: the never-read param
    is over-released at every arm count the model checks. -/
theorem buggy_is_unsound_bounded :
    ∀ n, n < 8 → 2 ≤ n → ¬ soundOn (releases armDropsBuggy .unused n) n := by
  decide

theorem fixed_is_sound_unused_bounded :
    ∀ n, n < 8 → soundOn (releases armDropsFixed .unused n) n := by
  decide

/-- `.blocked` is released by neither emitter here: the exit-drop path
    (`pcs_collect_exit_drops`) covers it, and that path is not modelled.
    Stated so the gap is explicit rather than silently absent. -/
theorem blocked_is_not_modelled :
    releases armDropsFixed .blocked 2 = [] := by
  decide

end Perceus
