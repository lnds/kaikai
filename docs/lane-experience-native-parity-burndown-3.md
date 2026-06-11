# Lane experience — native-parity burn-down 3

**Scope:** third burn-down lane on the in-process libLLVM native backend's
parity with the C-direct oracle (KIR Lane 1.5). Target: chip the
`tools/native-parity-baseline.txt` ratchet down toward zero, the antechamber
to the native-default flip.

**Outcome:** ratchet 91 → 82 (closed 9). ONE root cause — **shared-tag
sub-discrimination** in the variant `match` lowering — erased the entire
`Duplicate integer as switch case` family (18 fixtures, the largest single
family). 9 close outright; the other 9 advanced to a pre-existing bug the
build error had masked (reclassified, see below). New module
`stage2/compiler/kir_lower_variant.kai` (A++, 77 LOC). No flip — that stays
a separate lane with its own gates.

## Scope as planned vs as shipped

**Planned (brief):** work the families in order — build-failed-other (24,
"decompose by real error signature first"), then value-divergent,
no-effect-handler, SIGSEGV/SIGABRT, timeout, clause-param.

**Shipped:** the brief's "build-failed-other grab-bag" decomposed, on real
signature, into TWO clean root causes: `Duplicate integer as switch case`
(18) and `Call parameter type does not match function signature` (6) — plus
`Undefined symbols` (3, bits/crypto) and `KPerform: no handler` (already a
named family). The lane took the 18-fixture `Duplicate` family — the highest
single-cause ROI — to completion and stopped there: a coherent mid-size
green PR, the rest diagnosed for burn-down 4. This is the brief's "cut at a
coherent point, mid-size green PRs > one eternal PR" path, chosen because
the next families (regex/json char-decode, Call-param-mismatch) are
DIFFERENT root causes in different zones, not a continuation of this one.

## The root cause and the fix

The variant path of `lower_match` (kir_lower_walk.kai) switches on the
scrutinee's top-level tag (`KTagOf` → `KSwitch`) — the O(1) jump table for
the common case where every arm is a distinct constructor. It broke when two
arms shared a top-level tag but discriminated on a payload sub-pattern
(`Exited(0)`/`Exited(_)`, `Branch(Red,_)`/`Branch(Black,_)`): `plan_arms`
emitted two `KC(tag, ..)` cases → a duplicate i32 switch case LLVM rejects,
AND the second arm was dead (no payload test).

The fix (asu-reviewed Option B — keep the switch, sub-discriminate only on
collision; the Maranget good-decision-tree shape GHC/OCaml/Rust use):

1. `plan_arms` dedups: ONE switch case per distinct tag, first-appearance
   order.
2. A shared tag routes its case to a GROUP block holding a sub-fail-chain
   (`lower_variant_groups`/`lower_group_chain` in kir_lower_walk.kai): each
   arm tests its payload, branches to its body on match / the next arm on
   mismatch; the tag's catch-all arm ends the chain; the final fail routes
   to the switch default (`ldef`).
3. The per-slot test (kir_lower_variant.kai): literal slot → `kai_eq_raw`,
   nullary ctor / enum slot → `kai_tag_eq` (additive runtime shim
   `kaix_tag_eq`, runtime_llvm.c — compares ONLY the immediate variant_tag,
   never recursing into fields nor a custom `Eq` impl that would
   proto-dispatch-panic).
4. `bind_slot_pattern` (kir_lower_bind.kai): a nullary nested ctor (`Red`)
   becomes a bind no-op (the sub-chain tested its tag; it binds no inner
   name) instead of trapping `bind_unsupported_nested`. A payload-bearing
   nested ctor (`Some(Node(x))`) still traps loud — the remaining
   nested-variant-test gap.

Why Option B over a fourth full fail-chain (Option A): the switch is the
Tier-2 "runtime-efficient" jump table; turning a 12-constructor match with
unique tags into 12 sequential tag-tests is a codegen regression across the
WHOLE corpus (incl. the self-host) to fix 18 fixtures. The tie-breaker
"runtime-efficiency beats expressive novelty" decides it. B reuses the
record path's field-test machinery inside a switch case — no new conceptual
mechanism.

## Structural surprises the brief did not anticipate

- **The grab-bag was two clean causes, not a grab-bag.** The brief framed
  build-failed-other as "no diagnostic, decompose first." One `diag.sh`
  pass over the baseline split it cleanly: `Duplicate integer as switch
  case` (18) and `Call parameter type does not match` (6). The "grab-bag"
  framing overstated the disorder — the native build errors ARE diagnostic,
  they were just never tallied.
- **Closing the build error unmasks a runtime bug (burn-down 1's lesson,
  again).** 9 of the 18 `Duplicate` fixtures now BUILD + RUN but diverge:
  regex panics `unterminated character class` (the regex parser's
  `Some(']')` char-match never fires — yet a MINIMAL `Some(']')` match
  passes parity, so the bug is upstream in how the parser reads the char,
  not in the match lowering this lane fixed), json decodes `A` to `'1'`
  not `'A'` (a hex-nibble read). These are reclassified, not closed.
- **The enum-slot case is BOTH `Duplicate-switch` AND `nested-variant-test`.**
  `Branch(Red,_)` shares the `Branch` tag (→ Duplicate-switch) AND its `Red`
  sub-pattern was hitting `bind_unsupported_nested` (→ nested-variant-test).
  The fix closes both for the NULLARY case: the `kai_tag_eq` test supplies
  the discriminant the bind needed. The payload-bearing case stays the gap.

## Fixtures added and coverage

- `examples/perceus/match_shared_tag_subdiscrimination.kai` + `.out.expected`
  — one program covering all three load-bearing shapes: literal slot
  (`Exited(0)`/`Exited(1)`/`Exited(_)`), enum slot
  (`Branch(Red,_)`/`Branch(Black,_)`/`Leaf`), and a shared-tag catch-all
  that still BINDS its payload (`Some(0)`/`Some(n)`/`None`). Lives in
  `examples/perceus/` so the parity harness (DIRS includes perceus) and the
  test-suite golden both exercise it. Golden generated from the C oracle;
  native byte-identical.

Coverage gap deliberately left: the payload-bearing nested-variant-test case
(`Some(Node(x))`) — still traps loud, a burn-down-4 follow-up.

## Cross-platform note (burn-down 1/#813 lesson honored)

`list_helpers` / `list_zip3_scan` PASS native on macOS but SIGSEGV on Linux.
The local ratchet prompts to remove them ("improved"); they STAY listed —
the ratchet is validated on Linux/CI, and burn-down 3 does not touch their
root cause. Not removing them is correct, not an oversight.

## RC validation (issue #812 — KAI_TRACE_RC is broken)

Per #812, the incref/decref balance counter reports 0 despite emitted RC
ops, so it is NOT a usable gate. RC soundness validated instead by: parity
byte-id vs C oracle (a leak/double-free diverges output or crashes), the
`__pcs_scr` borrow discipline (tests borrow via `KProj`/`kai_eq_raw`, never
consume the shared scrutinee), and the additive `kaix_tag_eq` being
RC-free (immediate tag compare, immortal Bool result).

## Real cost vs estimate

The diagnosis (one `diag.sh` pass) + asu design consult + the fix itself
were fast; the bulk was verification (selfhost byte-id, full-corpus parity
zero-regression, tier0/tier1). The asu consult paid for itself — it caught
the `ldef`-routing exhaustiveness subtlety and the borrow-vs-own RC split
before they became debugging.

## Follow-ups for burn-down 4

1. **regex/json char-decode** (9 reclassified) — likely ONE root cause: a
   char/string-index read on the native path (`Some(']')` real-parser miss,
   `\uXXXX` hex-nibble wrong). Diagnose the native string-index read first.
2. **Call-param-mismatch** (6: map/jwt/fx) — one cause: a generated call
   passes an operand whose LLVM type disagrees with the callee (a
   monomorph/boxing shape, likely Map's comparator closure).
3. **no-effect-handler** (11: Spawn 7 / Clock 2 / NetTcp 1 / File 1) — extend
   the #793 default-handler install pattern.
4. **nested-variant-test payload-bearing** (12) — the `Some(Node(x))` case
   this lane left as a documented trap; needs the recursive tag-test +
   sub-bind the sub-chain machinery can now host.
5. **pipe-lowering** (5) + **Real box/unbox** (4) — diagnosed in
   docs/native-parity-gaps.md.
