/-
  Nominal identity: when two same-named types must not unify.

  Diagnostic model. Not a gate, not built, not run by CI. See README.md.

  Written against: d214406d, stage2/compiler/{infer,tycon_home,driver,
  tycon_scheme_home}.kai.

  #2013 states its acceptance as a COUNT — string comparisons per pass —
  and says why: "the failures here are silent. The compiler keeps
  building, the self-host stays byte-identical and the corpus stays green
  while identity is being thrown away, because a bare name still resolves
  to something. Only a count fails."

  A count says identity eroded. It does not say which pair of
  declarations got confused, or which rule let them. This file states the
  rule and asks whether it can separate two homonyms at all.

  The rule, `module_slot_compat` (infer.kai:7586):

      None    ~ _       compatible
      _       ~ None    compatible
      Some a  ~ Some b  a == b

  used by unify (7206), by two structural equalities (1156, 8226), by
  dischargeability (10060) and by monomorph (1648).

  The ordering, `tag_tycon_modules` (tycon_home.kai:34), called from
  driver.kai:5752 on `typed_prog.decls` — AFTER the typer. Its own header
  says `resolve_ty_with_binds` builds every `TyCon` before the home-aware
  tables exist, "so it leaves `module_origin` empty".

  So during inference the slot that distinguishes two homonyms is empty,
  and an empty slot is compatible with everything.

  Second, and this is what the #2013 lane measured (PR #2094): making
  the propagation TOTAL does not fix it either. `tsh_slot` discards the
  reading module and `tsh_scan` stamps only a name the decl stream
  declares exactly once, so a contested name is left bare by
  construction. Propagation is necessary and not sufficient; what is
  missing is a home chosen per reading module.

  Verify with `lean Naming.lean` (exit 0, no output). Lean 4, no Mathlib.
-/

namespace Naming

/-- A module, as the home slot names one. Two are enough to state the
    collision; the names are opaque. -/
inductive Home where
  | a
  | b
  deriving DecidableEq, Repr

/-- `TyCon(Option[String], String, …)` — ast.kai. The model carries the
    home slot and the spelling; type arguments are out of scope. -/
structure TyCon where
  /-- `module_origin`. `none` is the state `resolve_ty_with_binds`
      leaves and `tag_tycon_modules` later fills. -/
  home  : Option Home
  /-- The bare name. Two homonyms share it; that is the whole problem. -/
  name  : String
  deriving DecidableEq, Repr

/-- `module_slot_compat`, infer.kai:7586. -/
def slotCompat : Option Home → Option Home → Bool
  | none,    _       => true
  | _,       none    => true
  | some x,  some y  => x == y

/-- The `TyCon` arm of unification, infer.kai:7206:
    `module_slot_compat(am, bm) and an == bn`. -/
def tyConUnifies (x y : TyCon) : Bool :=
  slotCompat x.home y.home && x.name == y.name

/-! ## The two declarations

Two distinct types that share a spelling — the namespace-collision
corpus's whole subject. They are DIFFERENT declarations; a sound typer
must not unify them. -/

def declInA : TyCon := { home := some .a, name := "Node" }
def declInB : TyCon := { home := some .b, name := "Node" }

/-- Stamped, the rule separates them. This is what `tag_tycon_modules`
    buys — and it is why the pass exists. -/
theorem stamped_homonyms_do_not_unify :
    tyConUnifies declInA declInB = false := by decide

/-! ## The ordering defect

`resolve_ty_with_binds` leaves `module_origin` empty, so during
inference the same two declarations are these instead. -/

def duringInferA : TyCon := { home := none, name := "Node" }
def duringInferB : TyCon := { home := none, name := "Node" }

/-- Unstamped, the rule cannot tell them apart. Two different
    declarations unify. -/
theorem unstamped_homonyms_unify :
    tyConUnifies duringInferA duringInferB = true := by decide

/-- Worse, and this is the part a count cannot report: one stamped side
    does not help. A partially-stamped program still unifies the pair,
    because `none` is compatible with everything.

    So stamping earlier is not enough on its own — stamping must be
    TOTAL before the typer runs. That is NECESSARY; the section below
    shows it is not sufficient. -/
theorem half_stamped_still_unifies :
    tyConUnifies declInA duringInferB = true := by decide

/-- The rule is not merely weak on `none`; it is unable to express
    "these are different" whenever either side is unstamped. Stated as
    a general fact rather than three examples. -/
theorem none_unifies_with_everything :
    ∀ t : TyCon, tyConUnifies { home := none, name := t.name } t = true := by
  intro t
  simp [tyConUnifies, slotCompat]

/-! ## Total propagation is necessary but NOT sufficient

Measured by the #2013 lane (PR #2094) and confirmed against the source:
propagating a view to every construction site does not make the slot
separate two homonyms, because of how the slot is chosen.

`tsh_slot` (tycon_scheme_home.kai:77) discards the reading module:

    pub fn tsh_slot(v: TyHomeView, nm: String) : Option[String] =
      match v { THV(hs, _) -> tsh_home_of(hs, nm) }

`THV` carries it — `THV([TyHomeEntry], Option[String])`, the second slot
is `cur`, "the view of `hs` from the module `cur`" — and the `_` throws
it away. The home then comes from `tsh_scan`, which stamps a name only
when the whole decl stream declares it EXACTLY ONCE (line 95,
`if n == 1 { found } else { None }`). Its own comment states the policy:
"a name with two declarations leaves the reader nothing to pick between,
and a wrong home is worse than none."

So on a contested name — the only case that matters — the scan returns
`none` by construction, however complete the propagation. -/

/-- The decl stream as the scan sees it: the homes declaring one name. -/
def tshScan : List Home → Option Home
  | [h] => some h
  | _   => none

/-- One declaration: stamped. -/
theorem tshScan_stamps_the_uncontested :
    tshScan [Home.a] = some Home.a := by decide

/-- Two declarations — the collision — yields `none`, which is the
    universal donor. The contested case is exactly the one left bare. -/
theorem tshScan_leaves_the_contested_bare :
    tshScan [Home.a, Home.b] = none := by decide

/-- Therefore: with the slot chosen this way, two homonyms unify no
    matter how total the propagation is. The view reaching every
    construction site does not change what the scan returns.

    This is the lane's refutation, as a theorem: propagation is
    necessary (the section above) and not sufficient (here). What is
    missing is a home chosen per READING module — the `cur` that `THV`
    already carries and `tsh_slot` discards. -/
theorem total_propagation_still_unifies_homonyms :
    tyConUnifies
      { home := tshScan [Home.a, Home.b], name := "Node" }
      { home := tshScan [Home.a, Home.b], name := "Node" } = true := by
  decide

/-- The scan that would work: on a contested name, pick the reading
    module's own declaration when it has one. The `cur` slot already
    exists on `THV`; this is the rule that would consult it.

    Stated to show the fix is a rule change, not more propagation. -/
def tshScanByReader (decls : List Home) (cur : Option Home) : Option Home :=
  match decls with
  | [h] => some h
  | _   => match cur with
           | some c => if decls.contains c then some c else none
           | none   => none

/-- Under it, the two readers disagree — which is what separates them. -/
theorem reader_scan_separates_homonyms :
    tyConUnifies
      { home := tshScanByReader [Home.a, Home.b] (some Home.a), name := "Node" }
      { home := tshScanByReader [Home.a, Home.b] (some Home.b), name := "Node" }
      = false := by decide

/-- And it does not regress the uncontested name: one declaration still
    stamps the same home for every reader. -/
theorem reader_scan_keeps_the_uncontested :
    ∀ cur, tshScanByReader [Home.a] cur = some Home.a := by
  intro cur; rfl

/-! ## What a sound rule would look like

Identity by `SymId` — #2013's direction — is equality of a resolved
id, with no compatible-with-anything case. Modelled as a total home:
every `TyCon` carries one by construction. -/

structure TyConId where
  home : Home
  name : String
  deriving DecidableEq, Repr

def tyConIdUnifies (x y : TyConId) : Bool :=
  x.home == y.home && x.name == y.name

def idDeclInA : TyConId := { home := .a, name := "Node" }
def idDeclInB : TyConId := { home := .b, name := "Node" }

theorem id_separates_homonyms :
    tyConIdUnifies idDeclInA idDeclInB = false := by decide

/-- And it still unifies a declaration with itself — the property the
    weak rule was protecting, which a total home does not lose. -/
theorem id_unifies_reflexively :
    ∀ t : TyConId, tyConIdUnifies t t = true := by
  intro t; simp [tyConIdUnifies]

/-- No unstamped state exists to exploit: there is no `TyConId` that
    unifies with two declarations that do not unify with each other.
    That is exactly the property `slotCompat` fails. -/
theorem id_has_no_universal_donor :
    ¬ ∃ u : TyConId, tyConIdUnifies u idDeclInA ∧ tyConIdUnifies u idDeclInB := by
  rintro ⟨u, h1, h2⟩
  simp [tyConIdUnifies, idDeclInA, idDeclInB] at h1 h2
  rw [h1.1] at h2
  exact absurd h2.1 (by decide)

/-- The weak rule DOES have one, and it is the state the typer runs in. -/
theorem slot_has_a_universal_donor :
    ∃ u : TyCon, tyConUnifies u declInA ∧ tyConUnifies u declInB := by
  exact ⟨{ home := none, name := "Node" }, by decide, by decide⟩

/-! ## Why the corpus stays green: the respelling carries the weight

The slot is not what separates two homonyms today. `home_spell.kai` gives
each contested declaration the name `<name>__<home>`, so the two sides
differ in the SPELLING and `x.name == y.name` already fails — the slot is
never consulted. Measured against the compiler at `d214406d`: two modules
each declaring `Node`, one passed to the other, is rejected with
`expected: (mb.Node) -> Int, found: (ma.Node)`.

So the rule below models the slot honestly, and the corpus is green
because a SECOND mechanism — the ~742 LOC #2013 wants to retire — is
doing the separating. That is the finding: the respelling is not
redundant with the slot, it is load-bearing IN PLACE of it. Retiring it
before the slot is total would remove the only working mechanism.

The model states the order the two halves must go in, which #2013 lists
as unverified ("the ordering between them is unverified", item 4). -/

/-- Transitivity would be the thing to fail. It does not: the weak rule
    is not transitive, and the counterexample is the unstamped pair. -/
theorem slotCompat_is_not_transitive :
    ∃ x y z : TyCon,
      tyConUnifies x y ∧ tyConUnifies y z ∧ ¬ tyConUnifies x z := by
  exact ⟨declInA, { home := none, name := "Node" }, declInB,
         by decide, by decide, by decide⟩

/-- The id rule IS transitive on a fixed name — an equivalence, which is
    what a typer needs from nominal identity. -/
theorem tyConIdUnifies_transitive :
    ∀ x y z : TyConId,
      tyConIdUnifies x y → tyConIdUnifies y z → tyConIdUnifies x z := by
  intro x y z hxy hyz
  simp [tyConIdUnifies] at *
  exact ⟨hxy.1.trans hyz.1, hxy.2.trans hyz.2⟩

/-! ## What this does not cover

- Type arguments. `TyCon` carries `[Ty]`; unification recurses through
  them and the model does not.
- `ERecordLit`, `PVariant`, `PVariantRecord`, `EVar` — the other bare-name
  nodes in #2013's table. They identify by string with no slot at all,
  which is the `none` case permanently, not a state to be fixed by
  ordering.
- Whether `tag_tycon_modules` can run before inference. The model shows
  the ordering must change or the slot must be total; it says nothing
  about whether the tables that pass needs exist that early. Its own
  header says they do not, which is the real obstacle and is a
  compiler-architecture question, not a logical one.
- The respelling (`type_scope.kai` and friends, ~742 LOC). It makes two
  homonyms produce different STRINGS, which is a second mechanism for
  the same goal; this model covers the slot, not the spelling.
-/

end Naming
