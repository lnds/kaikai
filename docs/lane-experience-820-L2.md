# Lane experience — #820 L2 (typer: evidence injection + monomorphisation + collision diagnostic)

## Scope as planned vs as shipped

**Planned (lane-plan §B-L2):** inject the L1 evidence obligations as hidden
params (A.1 representation), thread them through monomorphisation
specialising per evidence-tuple alongside types, and ship the #789 op-name
collision diagnostic at the row-discharge hole. Codegen still ignores the
injected params (that is L3). Record the §A.3 arity spike.

**Shipped:** all three, with one design correction the brief's literal
wording did not anticipate (the monomorph threading), and one soundness
refinement the L1 predicate forced (the collision false positive). The
frontend half landed; codegen provably ignores everything (byte-id green).

New module `stage2/compiler/evidence_inject.kai` (A−, 205 LOC code, cogcom
avg 2.4 / max 8). Additions to `evidence.kai` (expose `obl_is_collision` /
`obl_join` / `obl_walk`, refine the collision predicate) and
`evidence_scan.kai` (the distinct-instance counter). Driver wiring: the
collision diagnostic in the error chain before `monomorphise`, the
injection + reindex exercised post-mono, and the `--dump=evidence-arity`
mode.

## A.1 representation — raw pointer as `TyHandle`, materialised as metadata

The evidence is a raw evidence-node pointer, typed as the opaque non-RC
`TyHandle` scalar (already in `ast.kai` for LLVM handles), so Perceus never
dup/drops it. The honest L2 form of "inject as hidden params" is a LATERAL
metadata structure `[FnEvidenceParams]` (keyed by pre-mono symbol, one
`FnEvidenceParam` per distinct effect instance the body demands, with its
L1 supplier), NOT real `Param`s in the `DFn`. The emitted AST is untouched,
so codegen consumes nothing and byte-id stays trivially green — which is
exactly what proves codegen ignores it (the coupled-reshape contract).
L3 materialises the params + rewrites call sites; that is the half that
breaks byte-id, and it is L3's by design.

One injected param per DISTINCT effect INSTANCE, not per op call: a body
performing `State.get` and `State.set` on one instance gets a single
`State` evidence param. Collisions inject nothing (they are errors).

## Threading through monomorphisation — the brief's verb vs what is honest

The brief says "specialise functions per evidence-tuple alongside types —
a row-polymorphic `fn f()` receives the caller's evidence tuple." Read
literally that means extending the `MonoTuple` specialisation key with an
evidence slot. It cannot be done honestly in L2, and the reason is in the
data flow: monomorphisation keys off `[ResolvedCS]` (resolved call sites),
whose `RCS(name, _, _, tys, units, _)` carries only the call's type/unit
args — NOT which evidence instance the caller passes to that callee. That
binding is the L3 call-site rewrite; until it exists, every call site to a
generic `f` collapses to one `MonoTuple` because the only thing that could
distinguish them (the evidence) is not represented in `ResolvedCS`. An
evidence slot derived from the callee would be constant per function — a
dead key that either breaks byte-id (with a mangle suffix) or discriminates
nothing (without one). Both are theatre.

The honest half of "thread through monomorphisation" L2 can do is make the
metadata SURVIVE the mono fan-out, re-keyed by mangled symbol. Mono maps
one pre-mono `f` to N post-mono `f__mono__Int`, `f__mono__String`; each
inherits `f`'s obligations (evidence demand is a row property, invariant
under type substitution). `reindex_evidence_params` demangles each spec
back to its base and re-keys the metadata onto the mangled name, so L3
finds each specialisation's obligations after the fan-out. Mono is the
transporter, not the consumer; L3 is the consumer. The `MonoTuple` key,
the mangle, and `ResolvedCS` are untouched — zero byte-id risk.

## The #789 collision diagnostic — and the false positive it surfaced

L1 already detects, per op resolution, whether the by-name walk mis-binds
(`obl_is_collision` over `tp.op_res`). L2 turns that detection into a real
`diag_error` at the call site, closing the #789 soundness hole the by-name
dispatch left open (the diagnostic that #789's tangential native fix never
shipped). The three L0 collision fixtures move out of quarantine into
`.err.expected` goldens and now reject at compile time.

The trap: the L1 predicate was built for a DUMP (analysis, exit 0), not for
gating. Turning it into a hard error exposed a FALSE POSITIVE on
`demos/stack`, a legitimate program using the issue-#148 `var xs = []`
clause-scope sugar. That `var` desugars to a `with State as xs` handler;
reading `@xs` inside the unrelated `Stack` clause suppresses the alias tag
crossing the clause boundary, producing an `OpResolution` STRUCTURALLY
IDENTICAL to the real #789 cross-instance fixtures (same op, same cands
`[State, Env]`, same chosen `State`, same `alias=Some(name)`, same
`tagged=false`). The `OpResolution` alone cannot separate the false
positive from the true positives.

The distinguisher is handler topology, not the resolution record: a
cross-instance collision is real only if ≥2 distinct live instances of the
chosen effect exist — the by-name walk binds the wrong one only when
another instance shadows the intended one. `demos/stack` has ONE `State`
(the lone `var xs`), so there is nothing to mis-bind to. The #789 fixtures
have TWO (the `var remaining` plus a separate `with State[[Int]]` runner in
a called function). `obl_eff_instance_count` (new, in `evidence_scan.kai`)
counts distinct instances by handler alias (bare = one anonymous instance);
the cross-instance branch of `obl_is_collision` now requires `>= 2`. After
the refinement, the three #789 fixtures still reject, `demos/stack`
compiles, and a whole-corpus scan (demos + effects/stdlib/actors/spawn
examples) shows ZERO spurious collisions.

Lesson: a predicate tuned for a non-failing dump is not automatically safe
as a compile gate. The accidental-equivalence the redesign tolerates
(≤1 live handler) is exactly the case the dump could flag harmlessly but a
gate must not — the gate has to look past the single resolution to the
program's instance topology.

## A.3 arity spike — the prior is refuted on the static axis

`--dump=evidence-arity` over the whole compiler self-compile + stdlib
(`kaic2 --dump=evidence-arity stage2/main.kai`, which pulls the full
compiler via `import compiler.driver`):

PRIMARY metric — distinct effect labels in each function's row (the hidden-
param arity its signature carries, forwarders included):

- functions:  5568
- max:        5
- p99:        5
- mean:       0.85
- > 3 params: 503

SECONDARY metric — distinct instances PERFORMED locally (op-call
obligations): max 0, mean 0.00 over the compiler.

The first cut measured the secondary metric and reported mean 0.00 — a
red flag, not a datum: it counts only effects performed by a direct op-call
in the body, so a function that forwards `Stdout` to a callee (its row has
`Stdout`, its body has no `Stdout.print`) reads as 0. The compiler forwards
almost everything through helpers, so the op-call metric is blind to it.
The metric L3 actually consumes is the ROW cardinality — the param count a
signature carries — which is the upper bound, not the op-call floor.

By the row metric, the lane-plan prior ("kaikai rows are narrow, individual
params wins") is REFUTED on the static axis: **p99 = 5 > 3**, and 503
functions (~9%) carry a 5-effect row. They are all the typer's `synth_*` /
`try_*` / `unify_*` helpers, whose inferred row is uniformly
`Stdout + Stderr + Stdin + File + Mutable`. The binary criterion (p99 ≤ 3 →
individual params) is not satisfied.

Caveat for L3: all five are builtin-default effects sharing startup
evidence nodes (design §A.2), so an L3 ABI could collapse them to a single
ambient "default evidence" param rather than five individual params,
pulling the effective arity back under the threshold. That collapse is an
L3 design lever (the build-both runtime measurement of §A.3 parts 2/3), not
an L2 call. L2 reports the static distribution; L3 measures both ABIs and
decides. The honest L2 finding: the static distribution does NOT confirm
the narrow-rows prior — L3 must measure, and the builtin-default-collapse
question is now the sharp one.

## RC tripwire (A.1 corruption gate)

`KAI_TRACE_RC` over an effectful program shows only the standard `str` /
`cons` / `variant` tags in dup/drop — no `evidence` / `handle`-tagged
value. This is trivially clean because L2 materialises NO evidence value in
the emitted AST: the metadata is lateral, codegen reads none of it, so
nothing of `TyHandle` lineage ever reaches Perceus. The tripwire becomes
non-trivial in L3, when the evidence node becomes a real flowing value.

## Gates

- byte-id GREEN (selfhost `kaic2b.c == kaic2c.c`) — proves codegen ignores
  the injected metadata, the only thing it proves and exactly what the
  coupled-reshape contract requires.
- Three #789 fixtures out of quarantine, reject with the collision
  diagnostic as `.err.expected`. `demos/stack` and the whole corpus stay
  clean (zero spurious collisions).
- `--dump=evidence-arity` histogram recorded (above + PR body).
- RC tripwire: zero evidence-tagged values in dup/drop.
- tier0 green; serial native-vs-C parity clean.

## Cost vs estimate

The diagnostic was the cheap, well-specified half (L1 did the detection;
L2 wired the error + goldens). The expensive surprises were two:
1. The monomorph-threading verb required refuting the literal reading and
   settling on metadata-survival (asu consult): the brief described the
   L2+L3 end state, not the L2-alone honest scope.
2. The collision false positive on `var`-clause-scope sugar: an
   OpResolution-identical legitimate program forced a topology-aware
   refinement (distinct-instance count) the L1 dump never needed.

## Follow-ups for L3

- Materialise the evidence params + rewrite call sites (the byte-id-
  breaking half). Consume `reindex_evidence_params`' output per spec.
- Resolve the §A.3 ABI: build-both individual-params vs packed-tuple, and
  measure whether the builtin-default effects collapse to one ambient param
  (which would restore the narrow-rows picture) or each materialises
  separately (which would push toward the tuple, given p99 = 5).
- The op-call obligation metric undercounts forwarders; if L3 needs a
  per-call-site evidence routing table, it must derive from the row +
  supplier, not the local op-call walk.
