# Lane experience — native-parity burn-down 6

**Scope:** sixth burn-down lane on the in-process libLLVM native backend's
parity with the C-direct oracle (KIR Lane 1.5). From a clean `main` (burn-down
5 merged at 81 gaps). Target: chip the `tools/native-parity-baseline.txt`
ratchet toward zero — the antechamber to the native-default flip (a separate
lane). 1M-context lane: diagnose whole families at once, causal order first.

**Outcome:** FOUR root causes closed, each diagnosed to a clean cause with the
C-direct oracle as ground truth. Closed cross-platform (byte-id vs C on the
closed fixtures): date_basic + date_iso (Clock dead-code), reuse_nested_subpattern
(nested-variant Form B), and the EBang transversal close (any `expr!` user). The
json_real ×4 advanced from `unbound-register` (build abort) to a SEPARATE
json-array decode divergence (a pre-existing parser bug this lane unmasked).
Several large families turned out DESIGN-bearing on inspection and were
documented, not forced (the brief's "a gap that needs a design decision is not
improvised").

## The four root causes closed

### 1. EBang (postfix `!`) — unhandled in KIR lowering (transversal)

`EBang(inner)` (the `expr!` Option/Result propagation operator) had NO arm in
`lower_expr` (kir_lower_walk.kai) — it fell through to `lower_unhandled`, which
lowers to unit, so every `let x = lookup()!` silently produced the wrong value
(or, via date's `today : Date / Clock`, a build abort downstream). The C-direct
oracle (`emit_bang`, emit_c.kai) lowers it to a statement-expr: test the success
tag, unwrap slot 0 on match, `return _b` (early-return the `None`/`Err`) on miss.

Fix: `lower_bang` (kir_lower_walk.kai) mirrors `emit_bang` as a control-flow
fork — a join register, `condbr (kai_tag_eq bv <Some/Ok-tag>) ? ok : fail`, the
fail block `KRet`s `bv` (the short-circuit), the ok block stores `KProj(bv, 0)`.
The success ctor is read from `inner.ty` via the SHARED `bang_success_variant`
(emit_shared.kai), so all three backends agree on the discriminant. Perceus
already treats `EBang` as consuming (`pcs_consumes_kind`), and the KIR lowers
from the SAME post-perceus AST as emit_c, so the RC discipline is identical to
the oracle by construction — no special-casing.

A trap caught here (the "kaic1 is lax" lesson): `KStore` takes a `KVal`, but I
first passed it `KProj(bv, 0)` (a `KOp`). kaic1 (the bootstrap compiler) does
NOT type-check this strictly, so the kaic2 built fine but the dump panicked
`non-exhaustive match` at runtime (a `KProj` reaching a `KVal` match). Fix:
materialise the `KProj` into a register with `bind_op` first. **Every sub-value
in a `KVal` position must be `bind_op`'d first** — kaic1 won't catch the KOp/KVal
confusion.

### 2. Clock dead-code synth-handler (no-effect-handler, Clock slice)

The native backend ABORTED THE BUILD on a `KPerform <eff>` for a default-block
effect performed in a DEAD function main never installs. Minimal repro:
`fn get_time() : Instant / Clock = time.monotonic()` with a Clock-free `main` —
the C oracle compiles + runs (the dead fn's perform is emitted but never reached,
dispatched dynamically via the evidence stack); the native walk's
`nfx_find_handler_by_eff` miss → `nemit_unsupported` → module refused.

Root cause: `kir_default_handlers` filters `install_order ∩ main_row`, but the
native emits a body for EVERY fn (including `today`), whose `KPerform Clock`
needs a synth `KHandlerDecl` for its op field index. Fix (asu-reviewed):
`kir_synth_handler_effects` — a DELIBERATE SUPERSET of `defaults` (every
default-block effect in `install_order`, NO main_row filter), exposed as a new
`KProgram.synth_defaults` field. The DISPATCH set (`ndef_synth_handlers`) uses
the superset; the INSTALL/teardown set (`kai_main_install_defaults` push/pop)
stays on `defaults` (LIFO-balanced). Semantics = the C oracle exactly: the dead
fn's perform compiles, and would null-deref → SIGSEGV only if executed (it is
not) — the C oracle's `kai_evidence_lookup_node` is also unguarded, so this is
byte-id failure-mode parity. Spawn/File/NetTcp have NO default block
(`find_effect_default_block` → None), so they get no synth handler and keep
aborting loud — a separate family (they need a real runtime, not a dispatch
shape). Closed date_basic + date_iso.

### 3. nested-variant payload-bearing — Form B (variant-slot)

`Node(_, Node(Red, a, ak, b), k, r)` — a nested variant WITH payload in a slot.
The decision tree tested only NULLARY/literal sub-patterns (burn-down 3's
`kai_tag_eq`); a payload-bearing nested ctor trapped `bind_unsupported_nested`,
leaving `a`/`ak`/`b` unbound. Fix (atomicity, asu's load-bearing warning):
THREE changes land together —
- `psub_is_discriminating` (kir_lower_variant) now returns `true` for ANY
  `PVariant` (a ctor in a slot always discriminates by tag);
- `var_seal_variant` tests the slot's tag THEN recurses into the payload's own
  discriminating sub-slots (`var_emit_slot_tests` against the projected value),
  so arbitrary depth (`Some(Node(Red, ..))`) falls out by recursion, all routing
  a miss to the SAME arm fail label;
- `bind_slot_pattern` (kir_lower_bind) recurses a payload-bearing variant via
  `bind_nested_slot` instead of trapping — sound because the tag test ran first.

Closed `reuse_nested_subpattern` + a minimal Form-B fixture, byte-id. The other
rb-tree fixtures (nested_pattern_reuse_balance, llvm_rc_nested_match,
llvm_arm_top_reuse_shared, int_field_inline) advanced from `unbound-register`
to a SECOND, design-bearing gap (see below).

### 4. nested-variant payload-bearing — Form A (list-head record-field-variant)

`[Pair { fst: _, snd: JReal(r) }]` — a list arm whose head is a record whose
`snd` field is a nested variant. `lm_bind_head` handled only a `PBind` head; a
record/variant head bound NOTHING and tested NOTHING (so a `JNum` would have
matched the `JReal` arm — a silent miscompile the unbound-register masked). Fix:
`lm_emit_head` (kir_lower_match) projects a nested head once, then `lm_emit_head_test`
(test the discriminating record fields / variant tag, mismatch → arm fail) +
`lm_bind_head_value` (recurse the bind via the shared `bind_pattern_fields_any`).
`bind_field_pattern` (kir_lower_bind) now recurses a field's nested variant
(gated by the field test). A SIMPLE binder head keeps the direct slot-0 bind (no
extra register, so the list_match golden is unchanged). Imports: kir_lower_match
now imports kir_lower_variant + kir_lower_bind (no cycle — neither imports
match), with the bundle order moved (variant before match).

This closed the unbound-register for json_real ×4 — they now BUILD + RUN but
diverge on a SEPARATE pre-existing json-array decode bug (see below). The bind
is verified correct (the decode fails upstream before the bind is reached).

## Structural surprises the brief did not anticipate

- **selfhost caught a `PHole` arity bug kaic1 tolerated.** `lm_emit_head` matched
  `PHole -> st`, but `PHole(Option[String])` needs a sub-pattern. kaic1 (lax)
  built the kaic2 fine; no validation fixture used a list-head `?`-hole, so it
  never executed; `make selfhost` (kaic2 compiling its own source, which DOES use
  that shape) caught it as `variant pattern has too few sub-patterns`. The exact
  "bundle-concat hides what selfhost catches" lesson — selfhost is a HARD gate
  before "done", not optional.
- **json_real's divergence is NOT my bind — it's a pre-existing json-array bug.**
  Closing the unbound-register unmasked it: `json_decode("[1]")` returns `None`
  on native (`[]` and `{}` empty work; non-empty array/object fail). The parser
  is a stack-based iterative loop (`json_loop` with `JMode`/`[JFrame]`); three
  minimal structural repros (char-read, recursive list-build, frame re-push loop)
  all PASS native, so the bug is in some interaction the minimal repros don't
  capture. Reclassified to the regex/json family (burn-down 3's diagnosed-but-open
  char/decode cluster), not closed. The recurring "close a build error, unmask a
  runtime bug" shape.
- **rb-tree reuse needs the token model the native emit lacks.** The Form-B fix
  made the rb-tree fixtures BUILD, but they diverge (`size: 0` / SIGSEGV): the
  simple `KConReuse` → `kaix_reuse_or_alloc_variant` eager-decrefs slots 1:1,
  which DOUBLE-FREES on a non-bijective rebuild (a rotation moving a subtree
  between slots). The C oracle uses the Koka drop-reuse-token protocol
  (`kaix_drop_reuse_token`/`kaix_variant_at`/`kaix_reuse_free` — runtime
  forwarders EXIST), but the native EMIT traps `KDropReuse`/`KFreeToken`
  (`nemit_unsupported`, emit_native_term.kai). Wiring the native emit to produce
  the token model is the next lane's design-bearing work; the runtime is ready.

## Fixtures added and coverage

No new positive fixtures: the closed baseline fixtures ARE the regression
coverage, locked by `tools/native-parity-baseline.txt`. All 7
`examples/kir/*.kir.expected` goldens re-verified GREEN by `make test-kir` (the
head-path optimization kept the common-case dump unchanged — the simple binder
head keeps the direct `KProj(cur, 0)` bind, so no golden churned, avoiding the
burn-down-4 stale-golden failure mode).

## Design-bearing residue (documented, not forced)

Per the brief — a gap needing a design decision is documented, not improvised:
- **json/regex array-decode** (json_real ×4 + regex ×8 + jwt) — a pre-existing
  parser bug, array/object non-empty fail, not isolable with minimal repros.
- **rb-tree reuse-token model** (4 perceus fixtures) — native emit lacks
  `KDropReuse`/`KFreeToken`; runtime ready. See
  `project_kaikai_native_reuse_token_model_gap`.
- **no-effect-handler Spawn/File/NetTcp** (9) — need a real scheduler/syscall
  runtime, not a dispatch shape.
- **binserialize `Some([x, y])`** — a list-pattern inside a variant slot (the
  Form-B/A machinery binds variant/record sub-patterns, not a nested list).
- **pipe-lowering** (collatz/euler1/fizzbuzz/imc/capture) — `EPipe` → unit; the
  multi-candidate combinator arg shape reverted twice (upstream `args` corruption).
- **clause-param-origin** (7, alias-dispatch subset-2b).

## RC + soundness validation

- Parity byte-id vs C-direct on every closed fixture (a leak/double-free diverges
  or crashes).
- `make test-kir` GREEN (goldens unchanged) + the `.kir.fns` filter trims to
  user-fns (the burn-down-4 lesson honored).
- selfhost byte-id (after the PHole fix).
- No changes to emit_c.kai or stdlib/ (the oracle does not adjust to itself).

## Real cost vs estimate

Diagnosis dominated, as always. `KAI_NATIVE_DUMP_IR` and the `--emit=kir` dump
collapsed each family to one cause. Two asu consults paid for themselves: the
nested-variant test/bind split (atomicity warning that prevented the
`var_arm_is_tag_catchall` false-green) and the synth-handler superset (install
vs dispatch set separation). The PHole bug cost one extra bootstrap — the cheap
reminder that kaic1 is lax and selfhost is the real gate.
