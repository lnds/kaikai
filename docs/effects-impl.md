# effects — implementation (Doc C)

Doc A (`docs/effects.md`) pinned the row-and-handler *semantics*;
Doc B (`docs/effects-stdlib.md`) pinned the *stdlib effects* and
their defaults; this document (Doc C) pins the *transform and
runtime* that turn an `effect`-bearing program into machine code
honouring those semantics.

Scope of v1: enough to make m7a and m7b compile to LLVM IR (and
the stage-1 C fallback) that run correctly. The actor mailbox
runtime and the full fiber scheduler are out of scope here — they
live in `docs/fibers-impl.md` (m8+; not yet written). This
document forward-references it only to confirm the boundary.

Reading order: Doc A defines the mental model, Doc B defines what
the stdlib ships, Doc C defines what the compiler emits. If you
are debugging an inference failure, Doc A; if you are writing a
user-visible handler, Doc B; if you are touching the compiler,
Doc C.

## Choice of régime

Two production-quality designs for compiling algebraic effects
sit in the literature, plus a hybrid worth naming explicitly:

- **A — direct capability passing** (Effekt, Brachthäuser 2020):
  handlers are second-class values passed as extra arguments; the
  compiler inlines them aggressively; continuations are live
  stack frames; one-shot is a direct tail call; multi-shot
  requires segmented stacks or a separate `reset/shift` infra.
- **B — generalized evidence passing** (Koka, Xie 2020):
  handlers are reified as *evidence* values; a global CPS
  transform rewrites effectful functions to take an evidence
  parameter; continuations are closures that one-shot specialises
  to a stack frame and multi-shot promotes to the heap; Perceus
  was co-designed with this approach.
- **C — surface-A + runtime-B**: Effekt-style surface (named
  capabilities, lexical scoping, `Eff as log` rebind) over a
  Koka-style evidence-passing runtime. The programmer sees
  capabilities; the compiler emits evidence.

**kaikai picks C.** Rationale, in order of weight:

1. *Fibers (m8) become a corollary, not new runtime.* A
   reified-continuation representation is already needed for
   `Spawn`, `Cancel`, and `Actor[Msg]`. Régime B gives us this for
   free; régime A would require a parallel segmented-stack
   subsystem before m8 can start.
2. *Multi-shot is uniform with one-shot.* `resume` and
   `resume_multishot` share the same closure representation; the
   difference is whether the closure is consumed or RC-bumped.
   This falls out naturally with Perceus (see §*Interaction with
   Perceus*).
3. *Perceus has a published co-design with evidence passing.*
   The Perceus paper (Leijen et al.) assumes Koka's evidence
   runtime. Re-deriving the reuse analysis over direct-style
   capabilities would be new research, which is out of scope for
   stage 2.
4. *Surface-A preserves Doc A's readability.* `Eff.op(x)` stays a
   method call, `Eff as log` stays a lexical rebind, `handle ...
   with ...` stays a statement. Nothing in the source gives away
   that the runtime is evidence-based.
5. *Compile-time cost is bounded.* The CPS transform only rewrites
   functions with a non-empty effect row. Pure code — most of the
   stdlib and most user code — is untouched. Principle #3 (fast
   compilation) survives because the pass's radius is the
   effectful subset, not the whole program.

### Risk of C

No published compiler combines exactly these two choices. The
Effekt paper and the Generalized Evidence Passing paper describe
the two endpoints; the translation between them — how a named
capability in the surface becomes an evidence slot in the runtime
— is design work, not literature lookup. The rest of this
document pins that translation.

### What C rules out

- **Direct inlining of handler clauses as in Effekt's fast path.**
  The generalised CPS transform puts a function call between the
  op call site and the clause body. An inliner can still fold the
  call after monomorphisation, but the default shape is indirect.
  The actual cost is unmeasured at design time; m7a includes a
  micro-benchmark against an Effekt-style direct baseline. If the
  gap exceeds 2× on trivial-clause workloads, the régime decision
  is revisited.
- **Segmented stacks.** Régime B's continuations live in the
  heap, not in a separate stack segment. The scheduler is
  simpler; the stack looks like any other program's stack.
- **First-class named handlers.** Already ruled out by Doc A
  §*Out of scope for v1*; régime C does not revisit that.

### Fallback

If the m7a benchmark exceeds the 2× ceiling, or if implementation
uncovers an unrecoverable gap in the capability↔evidence
translation, the fallback is **régime A pure** (direct capability
passing, à la Effekt):

- The surface stays intact. Doc A's `Eff as log`, named
  capabilities, and `handle ... with` remain unchanged — régime
  A's surface and régime C's surface are identical.
- The runtime swaps: handlers become second-class values passed
  as extra arguments; continuations live as stack frames; one-
  shot is a direct tail call.
- Multi-shot requires segmented stacks. Régime A pays this cost;
  régime C avoided it by reifying continuations.
- Fibers (m8) become a separate runtime investment: segmented
  stacks are a prerequisite. m8 stretches in calendar time but
  not in scope.

Rationale for choosing A pure as the fallback: Doc A's surface
is already approved and shipped. Reverting to régime B pure
would force a Doc A rewrite (named capabilities lose their
meaning under Koka's operation-resolution surface). Reverting to
régime A pure leaves user-visible code untouched and pushes the
cost into the compiler-implementation budget.

The fallback is not the default plan; régime C is. The fallback
exists so that a hypothetical Plan-B execution does not require
re-opening Doc A.

## Types: row slot on `TyFnT`

Doc A §*Interaction with existing HM* specified the change
abstractly: `TyFnT` gains a row slot. Pinning it concretely:

```
TyFnT(args: [Ty], ret: Ty, row: Row)   # was TyFnT(args, ret)

Row = Row {
  labels        : Array[Label],        # invariant: sorted, deduplicated
  tail          : Option[RowVarId],
  display_alias : Option[AliasName],   # for diagnostics only
}
Label = { eff: Symbol, ty_args: [Ty] }
```

- `labels` behaves as a set (order irrelevant; Doc A §*Representation*).
  Concretely it is an `Array[Label]` held under the invariant
  *sorted by `eff` symbol, syntactically deduplicated*. The
  invariant gives canonical iteration for the formatter and for
  error messages without depending on a stdlib `BTreeSet`
  (which stage 2 does not ship). Rows are small (< 10 labels in
  practice), so linear membership checks are not a hot path.
- `ty_args` carries `State[Int]` vs `State[String]` payloads;
  empty for monomorphic effects.
- `tail` is `None` for closed rows and `Some(id)` for rows with a
  variable. Fresh variables are minted during inference as with
  `TyVarT`.
- `display_alias` records the alias name (e.g. `Io`) when the
  row was written through a closed alias at the source. It
  participates in error rendering only; `unify_row` ignores it.
  When an alias-bearing row unifies cleanly, diagnostics print
  the alias; on mismatch the alias is expanded to its full label
  set (Doc A §*Open questions* #5).

Two existing `TyFnT` call sites in the stage-2 checker must
update: the arrow-type constructor (accepts a `row` argument;
pure functions pass the empty row) and the unifier (adds a row
case that calls `unify_row`). `unify_row` implements Doc A §*Row
unification* verbatim.

Public signature reconciliation (Doc A §*Inference*) keeps the
same rule: the inferred row must equal the declared row up to
row-variable unification.

### Row normalisation

Rows are normalised before storage:

1. Sort labels by the effect's resolved symbol (stable across
   compilations; depends only on the module graph).
2. Deduplicate **syntactically**: labels that compare equal in
   both `eff` and `ty_args` collapse (`{Io, Fail, Io}` → `{Fail,
   Io}`). Labels that share the same effect symbol but differ in
   payload — `State[Int]` vs `State[?T]`, where `?T` is an
   unresolved type variable — are *not* merged at normalisation
   time. They may unify later via `unify_row`'s recursive payload
   step. Cost: a row may carry one extra entry per unresolved
   payload during inference, which in realistic programs is one
   or two.
3. If an alias was used at the source, preserve it as a
   *display hint* separate from the canonical labels. The hint
   appears in diagnostics while unification succeeds and expands
   to the label set on failure (Doc A §*Open questions* #5).

The display hint is metadata; it participates in error rendering,
never in unification.

## Surface-to-runtime mapping

The central translation of régime C: a *capability* in the
surface becomes an *evidence value* in the runtime.

### Evidence types

Each `effect` declaration generates two compile-time artifacts:

```kai
effect Io {
  print(s: String) : Unit
  read_line() : String
}
```

produces:

1. A **label symbol** `Io` used by the type checker in rows.
2. An **evidence type** `EvIo` used by the runtime:

```
struct EvIo {
  handler_id : HandlerId
  env        : *Frame                              ; captured enclosing scope
  print      : fn(*Self, s: String, k: Cont[Unit])  -> Answer
  read_line  : fn(*Self,            k: Cont[String]) -> Answer
}
```

- Every op becomes a *plain function-pointer* field (not a
  closure). The calling convention passes `*Self` — a pointer
  to the `Ev`-struct itself — as an implicit first argument.
  Clauses read free variables of the enclosing scope through
  `self.env`, and read per-handler state (for parameterised
  effects) through additional fields (see §*Parameterised
  effects* below).
- Each op's compiled signature gains a final `Cont[Ret]`
  parameter — the reified continuation the clause will either
  call or discard (§*`resume` representation*).
- Each op returns `Answer` — the final result type of the
  enclosing `handle` (the type `S` in Doc A §*`resume`: one-shot,
  explicit*). Per `handle`, `Answer` is instantiated to that
  handle's `S`; clauses produce an `Answer` either by calling
  `resume` (which threads through `body` and the `return` clause
  to an `S`) or by returning a value of `S` directly when
  discarding the continuation.
- `handler_id` is a unique integer identifying the handler
  instance, used only for diagnostics that need to name the
  handler (e.g. *"continuation resumed twice in handler #4
  installed at src/foo.kai:42:5"*). The compiler maintains a
  side table mapping `handler_id` to a record:
  `{ handle_span: SourceSpan, clauses: Map<OpName, SourceSpan> }`.
  Diagnostics select either span depending on what they need to
  point at (the `handle` block as a whole, or a specific
  clause). One-shot violations are detected by the continuation
  closure's `status` byte (§*`resume` representation*), not by
  `handler_id`.

Per-op type generics (Doc B §*Out of scope for v1* amended,
ships in m7b) are handled in §*Per-op type generics*.

### Parameterised effects

A parameterised effect declaration like

```kai
effect State[T] {
  get() : T
  set(v: T) : Unit
}
```

generates a generic evidence type with one extra field per
declared `with`-clause parameter — the *handler state*:

```
struct EvState[T] {
  handler_id : HandlerId
  env        : *Frame
  state      : T                                       ; per-handler state
  get        : fn(*Self,        k: Cont[T])    -> Answer
  set        : fn(*Self, v: T,  k: Cont[Unit]) -> Answer
}
```

The canonical clauses

```
get(resume)    -> resume(state)
set(v, resume) -> resume((), v)
```

lower to:

```
get_clause(self, k):
  tail call k(load self.state)

set_clause(self, v, k):
  store self.state, v
  tail call k(())
```

`resume((), v)` updates state by storing `v` into `self.state`
before tailing into `k`. `resume(value)` (no second argument)
omits the store — state stays as the current `self.state`. The
two-argument form is desugared at the parser level (`resume(value,
new_state)` → store + call), not at the runtime level; the
runtime never receives "the second argument of resume" as a
distinct concept.

Effects with multiple state parameters (none in the v1 stdlib;
hypothetical `effect Reader2[A, B]`) gain one field per
parameter, in declaration order. The CPS transform reads them
positionally.

### Calling a capability

`Io.print("hi")` resolves to:

1. Look up `Io` in the current evidence vector — yields a
   pointer `ev : *EvIo`.
2. Indirect-call the field: `ev.print(ev, "hi", current_continuation)`.
   The first argument is `*Self`, per the calling convention
   pinned in §*Evidence types*.

The evidence vector itself is the per-fiber handler stack
(§*Handler-stack runtime*); the "current" one is the snapshot
visible at this lexical point in the program. §*The CPS
transform* pins how it is threaded through call sites.

### Rebinding

`Eff as log` binds `log` as a local name for the same evidence
value that `Eff` denotes at this point:

```kai
handle { log.print("hi") } with Io as log { ... }
```

compiles identically to

```kai
handle { Io.print("hi") } with Io { ... }
```

except that inside the body the identifier `Io` does not resolve
as a capability (the checker removed it from scope). The runtime
makes no distinction; rebind is purely a name-binding operation.

### Shadowing

Nested handlers of the same effect work by stacking evidence:

```kai
handle {                    # outer
  handle {                  # inner
    Io.print("x")           # uses inner
  } with Io { ... }
  Io.print("y")             # uses outer (inner is gone)
} with Io { ... }
```

"Innermost handler wins" (Doc A §*Out of scope for v1*) splits
across the two phases:

- *Compile-time resolution.* The checker decides which effect
  label `Io.print` refers to and emits a lookup against that
  label. There is no ambiguity at the source level; nesting is
  resolved by lexical scope.
- *Runtime dispatch.* The evidence vector is built dynamically as
  `handle` blocks push and pop their evidence nodes. At
  `Io.print("x")` the lookup walks the vector and finds the
  inner node; at `Io.print("y")` the inner node has already been
  popped, so the same lookup returns the outer node.

The compiler does not pre-resolve which struct the call ends up
talking to — only which label the lookup is parameterised by.

## The CPS transform

### What gets transformed

A function is transformed iff its type row is non-empty. Pure
functions are left in direct style and emit ordinary LLVM IR.
The transform is therefore a pass over a subset of the program,
not a global rewrite.

Boundary rule: a pure function that *calls* an effectful function
is itself effectful by inference — the callee's row contributes
to the caller's row. The subset is therefore closed under
inference: if `f` is pure, none of its callees can be effectful
(otherwise inference would have widened `f`'s row); if any callee
of `f` has a non-empty row, `f` was already inferred effectful
and flagged for transformation in the same pass.

### The transform, per construct

Let `row(e)` denote the inferred row of expression `e` and `Ev(ρ)`
the evidence vector for row `ρ`.

- **Value `v`**: if pure, pass straight through; if reached via a
  continuation, becomes `k(v)`.
- **Let `let x = e1; e2`**: if both pure, unchanged. If `e1` is
  pure and `e2` effectful, pass `e1`'s value into the transformed
  `e2`. If `e1` is effectful, transform it with a continuation
  that binds `x` and runs the transformed `e2`.
- **`if cond { e1 } else { e2 }`**: if `cond` is pure, branch
  inline and transform `e1` and `e2` separately (both arms
  receive the same enclosing continuation). If `cond` is
  effectful, transform `cond` with a continuation that, given
  the boolean, dispatches into the transformed `e1` or `e2`.
- **`match scrut { p1 -> e1 | ... | pn -> en }`**: if `scrut` is
  pure, dispatch inline and transform each arm separately. If
  `scrut` is effectful, transform it with a continuation that
  receives the scrutinee, runs the pattern decision tree, and
  enters the transformed arm. The decision tree itself is pure
  IR — patterns cannot perform.
- **Call `f(args)`** (pure f): direct LLVM call.
- **Call `f(args)`** (effectful f): pass the current evidence
  vector as the extra argument, and the current continuation as
  the last argument. Both are compile-time known at the call
  site.
- **Op call `Eff.op(args)`**: look up `ev : *EvEff` in the
  evidence vector; emit `ev.op(ev, args, current_continuation)`.
  The leading `ev` is the `*Self` argument required by the
  calling convention pinned in §*Surface-to-runtime mapping*.
- **`handle { body } with Eff { ops }`**: allocate an `EvEff`
  whose fields are the compiled op clauses; push it onto the
  evidence vector; transform `body` with the extended vector;
  pop on normal return; apply the `return` clause (if any) or
  identity.
- **`resume(v)` inside a clause**: direct call into the
  continuation passed to the clause. The clause's code path is
  CPS; `resume(v)` is one tail call.
- **`resume_multishot(v)`**: same as `resume(v)` except the
  continuation is RC-bumped before the call (§*Interaction with
  Perceus*).

The transform is syntax-directed and runs per function body
after inference and before monomorphisation (§*Pipeline order*).
It produces a typed-IR node called `EffFn` (effectful function)
that monomorphisation knows how to specialise.

### Continuation representation in source-to-IR

A continuation in IR form is a closure over:
- the frame of local bindings alive at the suspension point,
- the remaining computation reified as a typed IR function,
- the caller's continuation (the tail).

This mirrors Xie 2020 §4. The closure representation is
discussed in §*`resume` representation*.

### Why not transform pure code

Two reasons:

1. Pure code outnumbers effectful code by a wide margin in every
   real codebase. Transforming it would double the IR size for
   no semantic benefit.
2. The CPS representation is not free to read — it obscures data
   flow and complicates Perceus reuse analysis. Keeping pure code
   in direct style means Perceus only has to reason about CPS
   continuation frames, not about every function body.

The cost is that the boundary between pure and effectful code
carries a small conversion — an effectful call from pure code
reifies a trivial identity continuation. The compiler emits a
single shared `id_cont` for this. Post-monomorphisation the
inliner *may* fold it away when the call site is direct; in the
worst case, one extra indirect call is paid per pure→effectful
boundary crossing. The m7a régime-cost benchmark (§*What C rules
out*) measures the actual cost.

## `resume` representation

`resume` is a value of type `(T) -> S / ρ`. Doc A §*`resume`:
one-shot, explicit* pinned its surface semantics; this section
pins its runtime shape.

### One-shot case (default)

The continuation closure is **always allocated on the stack** at
the `perform` site, immediately before the op call. The compiler
does not predict at allocation time which clause will run or
whether it will call `resume_multishot`; pessimism is on the
stack side. If the clause does call `resume_multishot`, the
closure is promoted to the heap at *that* call site
(§*Multi-shot case*). The one-shot path therefore never touches
the heap:

```
frame on stack:
  +-- locals of the caller of perform
  +-- continuation closure:
  |     - environment pointer (up to caller's frame)
  |     - resume fn pointer
  |     - status byte           ; the one-shot check
  |     - handler_id             ; cosmetic, names the handler in panic text
  +-- (clause runs here as a tail call)
```

The closure carries `handler_id` as a copy of the `EvE`'s id at
the time of `perform`. When the runtime panic for "continuation
resumed twice" fires, it names the originating handler even if
the corresponding `handle` block has already exited and its
`EvE` is no longer on the evidence vector.

The clause receives a pointer to this closure. Calling `resume(v)`
is:

1. Check `status == Unresumed`; otherwise panic with
   "continuation resumed twice".
2. Flip `status = Resumed`.
3. Tail-call into the resume fn pointer with `v` and the closed
   environment.

Overhead over a direct call: one load + one branch on `status`.
LLVM *may* fold the check post-monomorphisation when the clause
is trivially one-shot (no branch, no loop) and the inliner can
see the path. In the worst case the check stays as a load +
conditional branch per `resume`. The m7a régime-cost benchmark
(§*What C rules out*) measures the total cost.

### Multi-shot case

`resume_multishot(v)` is the same operation except:

1. Before the call, the stack-allocated closure is *promoted* to
   the heap: `heap_closure = closure_clone(stack_closure)` —
   one allocation, one copy of the closure environment.
2. The `status` byte becomes a *counter* that tracks remaining
   permitted resumptions (unbounded for `resume_multishot`).
3. Subsequent `resume_multishot(v)` calls use the heap copy
   directly; Perceus tracks its RC like any other heap value.

The heap promotion is one allocation + one copy of the closure
environment. It is proportional to the amount of live state at
the suspension point, which in the multi-shot use cases
(backtracking, generators) is small. Doc A §*Out of scope for
v1* already accepts this cost.

### Static detection of illegal one-shot use

The checker detects at type-checking time if a clause clearly
calls `resume` twice on the same control path, or stores `resume`
into a closure that escapes the clause. Such programs are
rejected with "one-shot `resume` escapes its clause — use
`resume_multishot` or rewrite".

Control paths that genuinely cannot be statically decided (a
loop containing a conditional `resume`) fall back to the runtime
check described above.

### Interaction with `Nothing`-returning ops

Doc A §*Discarding the continuation* says a `Nothing`-returning
op cannot resume; Doc A also keeps `resume` in scope in the
clause "for uniformity". The representation honours both:

- The op's signature is generated normally — `fail` has a
  `Cont[Nothing]` parameter just like every other op has a
  `Cont[Ret]`. The `resume` binding is in scope.
- `Cont[Nothing]` is uninhabited (no value of type `Nothing`
  exists), so `resume(v)` cannot be type-checked: there is no
  value `v` to pass.
- The clause's code path therefore returns directly with an
  `Answer` of the handle's `S`, never invoking `resume`. No
  runtime check is needed because no path can reach the call.

The uniformity is purely syntactic: every clause binds `resume`,
so the parser, the desugar pass, and the codegen all treat
`Nothing`-returning ops like any other. The type system rules
out the call without special cases.

## `handle` lowering

`handle { body } with E { ops }` lowers to the following LLVM IR
sketch (pseudocode; actual IR pinned by the emitter):

```
; prologue: build the EvE struct
%ev = alloca EvE
store %ev.handler_id, fresh_id()
store %ev.env,        %enclosing_frame_ptr
store %ev.op_1,       &op_1_clause
store %ev.op_2,       &op_2_clause
; ... one store per op
; ... plus per-handler state fields if E is parameterised
;     (e.g. store %ev.state, %init_value for State[T])

; prologue: push an Evidence node onto the per-fiber stack
%node = alloca Evidence
store %node.eff_label, EffSymbol_E
store %node.handler,   %ev
%old_top = load fiber.evidence_top
store %node.parent,    %old_top
store fiber.evidence_top, %node

; body — transformed per §*CPS transform*; reads the updated
; evidence vector implicitly via fiber.evidence_top, and takes
; id_cont as the final continuation. When body completes the
; normal way it tail-calls id_cont(value), which returns `value`
; as the body's Answer. Early discard from a clause bypasses
; id_cont and produces an Answer directly.
%body_result = call transformed_body(id_cont)

; epilogue
store fiber.evidence_top, %old_top                 ; pop
%final = call return_clause(%body_result)          ; identity if absent
```

- `fiber.evidence_top` is a field of the current `Fiber` struct
  (§*Handler-stack runtime*); it is loaded into a register on
  entry to a function and written back on `handle` push/pop, so
  the cost in the hot path is the same as a thread-local.
- Each op clause is compiled as an ordinary function with
  signature `(self: *EvE, op_args..., k: Cont[Ret]) -> Answer`.
  The `EvE` struct stores the function pointers and any
  per-handler state; closure captures of the enclosing scope go
  through `self.env`.

### Early return / discard path

If a clause does *not* call `resume`, it returns a value of the
handle's result type `S` directly — `Answer` *is* `S` for this
`handle`, so the value flows out as the handle's result and the
remainder of `body` is dropped. No tagging or runtime dispatch
is involved: the static decision (call `resume` vs not) determines
the path. Doc A §*Discarding the continuation* is the semantics;
the mechanism is just CPS return.

### `return` clause

The optional `return(x) -> expr` clause runs on the normal
completion path of `body`. The compiler emits it as a function
that takes the body's result and produces the handle's final
value. If absent, the emitter inserts the identity. LLVM *may*
fold the call post-monomorphisation; in the worst case, one
trivial function call per handle exit.

## Handler-stack runtime

The *evidence vector* is the per-fiber data structure threaded
through the CPS transform. Its concrete layout for v1:

```
struct Fiber {
  ...                           ; other fields (stack, heap, ...)
  evidence_top : *Evidence      ; head of intrusive cons list
}

struct Evidence {
  parent    : *Evidence          ; previous entry (or null)
  eff_label : EffSymbol          ; identifies this effect
  handler   : *void              ; points to the Ev<Eff> struct;
                                 ; cast to *EvEff at the call site,
                                 ; where Eff is statically known
}
```

### Push and pop

`handle`'s prologue pushes an `Evidence` node; the epilogue
pops. Both are O(1) and happen on the caller's stack (the node
is `alloca`'d inside the `handle`'s compiled frame).

### Lookup

`Eff.op(args)` looks up the innermost handler for `Eff`:

```
node = fiber.evidence_top              ; *Evidence
while node != null {
  if node.eff_label == Eff { break }
  node = node.parent
}
ev = node.handler                       ; *EvEff (cast from *void)
; the op call then proceeds as ev.op(ev, args, k)
```

Linear in the depth of the `handle` stack. Depth is typically
small (< 10 in practice; see §*Open questions* for when we would
change this).

### Why intrusive cons, not indexed vector?

Two rejected alternatives:

- **Per-effect slot table**: one slot per declared effect, global
  indexing. O(1) lookup, but clobbers the "innermost wins"
  semantics unless we stack *per slot*; each `handle` then needs
  to push-and-restore one slot. More machinery, no meaningful
  benefit at expected depths.
- **Hash map**: O(1) average lookup, allocation per `handle`.
  Allocation cost outweighs the lookup gain at small depths.

Intrusive cons wins at the common scale. Revisit if
`perf(effects)` shows lookup in the top 5% of any workload.

### Per-fiber isolation

Each fiber owns its own evidence vector. Spawning a fiber
(§*Fibers preview*) copies the parent's `evidence_top` pointer
into the child's `Fiber` struct as the child's *floor*; the
child is free to push its own handlers on top. The parent's
evidence vector is never written from the child.

**Balance invariant.** A fiber's `evidence_top` must equal its
floor at every fiber-yield point and at fiber exit. The compiler
enforces this by emitting `handle` push/pop pairs that are
syntactically balanced — there is no way for source code to push
without a matching pop. A child therefore cannot pop into the
parent's nodes; if `evidence_top` were ever observed below the
floor, that would be a compiler bug, not a user error.

## Interaction with monomorphisation

Generic functions are monomorphised by `(type_args, row)` instead
of just `type_args`. `map[A, B, e]` instantiated at

```kai
map([1,2,3], (x) => x + 1)              # e = {}
map(["a","b"], (s) => Io.print(s); s)    # e = {Io}
map(lines, parse_int)                    # e = {Fail}
```

emits three specialised copies. The copy for `e = {}` runs in
direct style, without the CPS transform — pure code path. The
copies for `e = {Io}` and `e = {Fail}` are CPS-transformed and
read their handlers from `fiber.evidence_top` at op-call sites
(§*Handler-stack runtime*). The row is *not* an extra function
parameter; specialisation distinguishes the *shape of the body*
(direct style vs CPS, which labels the lookup expects), not the
calling convention. Two copies with different rows have the same
ABI on their explicit arguments and differ only in their lowered
IR.

### Code size bound

A function generic over a row variable and N concrete callers
can instantiate at most N rows. In practice most call sites
share rows (the codebase converges on a few well-known rows:
`{Io}`, `{Io, Fail}`, `{Mutable}`, etc.), so the monomorph table
deduplicates aggressively. Stage 2's monomorph pass already hashes
instantiation keys; the row is added to the hash input.

### Row-polymorphic functions not called

A generic function never called does not produce any code; only
called instantiations are emitted. Same rule as type generics:
the instantiation key is `(type_args, row)` symmetrically. Both
contribute to dedup; both block emission when no caller exists.

### Interaction with per-op type generics

A per-op generic like `Mutable.array_make[T](n: Int, init: T)`
does *not* monomorphise the op itself — the op stays type-erased
in the evidence struct. The *caller* is monomorphised: each
distinct `T` at a call site emits a specialised caller that
packages the arguments and calls through to the erased op. The
exact ABI of that erased call (raw-byte layout + element size or
a runtime witness) is pinned in §*Per-op type generics*.

## Interaction with Perceus

Perceus reuse analysis treats the CPS continuation closure as a
first-class allocation. Three cases:

### One-shot, unique (default)

`resume` is called at most once; the continuation closure's RC
is 1. The closure occupies a sub-region of the caller-of-perform's
stack frame (§*`resume` representation*), not the whole frame.
"Reuse" is the tail-call into the resume fn pointer with the
closed environment — no extra allocation, no heap traffic. The
closure is reclaimed implicitly when its enclosing frame returns.

### Multi-shot, unique owner

`resume_multishot` promotes the closure to the heap. First call:
heap allocation, environment copy, RC = 1. Subsequent calls:
if RC stays 1, reuse; otherwise Perceus adds `incref` and a
fresh copy.

### Shared `EvE` across handler clauses

All clauses of the same `handle` share the single `EvE` struct
allocated by the prologue: free variables of the enclosing scope
are reached through `EvE.env: *Frame`, and per-handler state
(`State[T]`'s `state: T`, for example) lives as an additional
field of the same struct. Perceus treats `EvE` like any
stack-allocated struct: heap-allocated payloads inside `env` or
`state` are RC-tracked by their own type's drop sequence; the
struct itself is reclaimed with the `handle`'s frame.

### Drop on handler pop

When the epilogue of `handle` runs, the `EvE` struct allocated
on the stack is reclaimed with the frame. Heap-allocated payloads
reachable through `EvE.env` or `EvE.state` get `decref` calls
inserted by Perceus, ordered by each field's type. The compiler
knows `EvE`'s shape from the effect declaration and emits the
drop sequence once per effect type.

### The "second-class handler" invariant

Doc A §*Out of scope for v1* item 7 rules out first-class named
handlers. Perceus relies on this: an `EvE` value never escapes
its lexical `handle` scope, so its drop is guaranteed to run at
the epilogue.

The continuation closure is a separate matter: it *can* escape
when `resume_multishot` promotes it to the heap. But the
continuation captures values from the *perform site* (locals of
the function that called `Eff.op`, plus the `id_cont` tail), not
the `handle`'s frame and not the `EvE`. While the continuation
is callable, the `handle`'s frame is still alive — the
continuation cannot outlive its handler because the type
`(T) -> S / ρ` only makes sense within the handler's dynamic
extent. No cycle between `EvE` and continuation arises.

Régime C does not weaken either invariant.

## Pipeline order and desugaring

The stage 2 pipeline (see `docs/stage2-design.md` §*Compilation
pipeline*) now reads:

```
.kai source
  → lex
  → parse
  → desugar-pre-resolve   (NEW; purely syntactic sugars)
  → resolve
  → desugar-post-resolve  (NEW; sugars that need capability bindings)
  → infer                 (HM with rows)
  → cps-transform         (NEW; effectful functions only)
  → monomorph             (type args + row)
  → perceus
  → lower                 (to LLVM IR or C)
  → link
```

### Desugar passes

The m7b sugars split into two passes by where the rewrite needs
name information.

**Pre-resolve** (after parse, before resolve). Sugars that are
purely syntactic; the rewrite emits names that resolve will look
up like any other identifier:

- *Trailing lambdas* → ordinary application with the last
  argument as a lambda expression.
- *Local mutable cell* — `var x = init; body` →
  `handle { body } with State[?T](init)` with canonical
  `get`/`set`/`return` clauses. `?T` is a fresh type variable;
  HM inference later unifies it with the type of `init` (or of
  the cell's uses). The variable specialisation pass
  (§*Variable specialisation*) lowers this canonical form to a
  mutable slot when conditions hold.
- *Array indexing* — `a[i]` → `Mutable.array_get(a, i)`;
  `a[i] := v` → `Mutable.array_set(a, i, v)`.

**Post-resolve** (after resolve, before infer). Sugars whose
rewrite depends on which effect a capability binding refers to:

- *Capability read / write* — `@cap` → `cap.ask()` when `cap`
  resolved as a `Reader` capability or `cap.get()` when it
  resolved as `State`; `cap := v` → `cap.set(v)` for `State` or
  `cap.put(v)` for `Writer`.

Resolve records each `Eff as name` rebind (and each default
capability binding produced by `with Eff { ... }`) as a
*capability binding* tagged with its underlying effect. The
post-resolve desugar reads this tag directly, without rerunning
resolution or inference.

Desugar is not user-configurable in either pass; the
transformations are hard-coded and tested.

### Why split

Pre-resolve sugars produce stdlib names (`Mutable.array_get`,
`State[T]` for `var`'s implicit handler) that resolve must look
up uniformly with hand-written calls — so they have to run
*before* resolve. Capability sugars are the opposite: `@cap`
cannot rewrite without knowing whether `cap` denotes a Reader
or a State, and that decision is exactly what resolve produces.
Splitting the pass keeps each transformation local to the
information it needs.

## Variable specialisation

Doc B §`State[T]` *Performance* promised that `var x = 0;
x := @x + 1` compiles to a native mutable int, not a chain of
handler invocations. The mechanism:

### Trigger conditions

All four must hold:

1. The `var` binding desugars to a canonical `State[T]` handler
   (the one the desugar pass emits — user-written handlers do
   not trigger specialisation).
2. No clause calls `resume_multishot`.
3. The captured `state` does not escape into a closure that
   outlives the `handle`'s body.
4. The initial value is a normal expression (not itself the
   result of another op call that could fail).

Conditions 1–3 are compile-time decidable; 4 is a simple
syntactic check.

### The specialisation

A canonical `State[T]` handler that meets the triggers is
replaced by:

- An `alloca` for a slot of type `T`, initialised from the
  `init` expression.
- `@x` (= `State.get()`) → `load` from the slot.
- `x := v` (= `State.set(v)`) → `store` to the slot.
- `return(x) -> expr` → `expr` evaluated after the body,
  dropping the slot at the enclosing frame's end.

No evidence struct, no CPS transform for this particular
`State[T]`. The surrounding body's row still carries `State[T]`
until the canonical handler is replaced — replacement happens
in the CPS transform phase, when the handler's shape is known.

### Why the conditions

1 is for safety: only the shapes we fully understand get
specialised. 2 is because multi-shot requires a heap-promoted
closure, which incompatible with a stack slot. 3 is the same
reason: a closure that outlives the `handle` would have to keep
the slot alive. 4 is sanity: if `init` can `perform`, the slot's
initialisation becomes a CPS suspension point, which defeats the
point.

Specialisation is not a best-effort optimisation — the trigger
conditions are the contract. When the specialisation doesn't fire,
the canonical handler runs normally. The compiler emits a comment
in `--dump=cps` diagnostics noting which specialisations fired
and which didn't, for debugging the m7b ergonomy claim.

## Per-op type generics

Doc B amended Doc A §*Out of scope for v1* item 3: per-op type
generics are in scope for m7b. The implementation:

### At the effect declaration

```kai
effect Mutable {
  array_make[T](n: Int, init: T) : Array[T]
  array_get[T](a: Array[T], i: Int) : T
  array_set[T](a: Array[T], i: Int, v: T) : Unit
}
```

The `[T]` attaches to the *op*, not the effect. The evidence
type stores the op as a type-erased function pointer with the
same `*Self`-leading convention as monomorphic ops:

```
struct EvMutable {
  handler_id : HandlerId
  env        : *Frame
  array_make : fn(*Self, args..., k) -> Answer    ; T-erased
  array_get  : fn(*Self, args..., k) -> Answer
  array_set  : fn(*Self, args..., k) -> Answer
  ...
}
```

Each op's signature uses *raw byte payloads* in place of `T`:
`array_make`'s `init: T` becomes `init: *void` carrying a pointer
to the value, and `array_make` returns `*void` for the resulting
`Array[T]`. The size of `T` is passed alongside as an implicit
runtime argument (§*At the call site*).

### At the call site

A call `Mutable.array_make[Int](10, 0)` compiles to:

1. The CPS transform produces a monomorphic caller for `T = Int`.
2. The caller packages `(10, 0, current_continuation)` using the
   concrete layout for `T = Int` and calls through the erased
   function pointer.

The op implementation must itself handle all `T`s. For stdlib
`Mutable`, the op body is a `Ffi` wrapper whose C counterpart
operates on raw bytes + an element-size parameter — the compiler
passes the element size as an implicit arg after monomorphisation.

Per-op generics do *not* compose with row polymorphism at the op
call site: the op is generic over `T`, not over a row. Doc A
§*Out of scope for v1* item 3's row-polymorphic clause stays
out of scope.

### Evidence-layout stability

The evidence struct's layout is fixed at effect declaration time
and does not depend on the concrete `T` of any call. This is what
lets the op be type-erased. The cost is that the caller pays one
extra argument (element size, or a runtime type witness) for each
generic op — negligible compared to the op's own body.

### Implementation plan (m7b)

This section pins the m7b #2 work plan. The spec above (§*Per-op
type generics*) is the *what*; the breakdown below is the *how*.

#### State at the start of m7b #2

- `effect Foo { op[T](...) : T }` — `parse_effect_ops`
  (`stage2/compiler.kai:3168`) does **not** accept `[T]` after the
  op name. The parser rejects it as malformed.
- `EffectOp = EOp(name, params, ret, line, col)` — no slot for
  per-op tparams. ~10 pattern-match sites across the file.
- `array_make / array_get / array_set / array_length / array_grow`
  are **prelude builtins** (`prelude_table` line ~4714, type
  schemes in `infer_initial_env` line ~7666, listed in
  `prelude_names` line ~4067). Their type schemes already use
  HM polymorphism (`scheme([0], …)`) — what is missing is the
  `Mutable` row.
- `Mutable` is **not** declared and **not** builtin-injected. No
  default handler exists.
- m7b #11 (parametric *effects*, `effect Foo[T] { … }`) is
  orthogonal: it parameterises the effect, not the op. A
  `State[Int]` handler serves only `Int`; per-op `[T]` lets one
  handler instance serve every `T` at every call. Both kinds
  coexist.

#### Partition

The work splits into three sub-PRs, landed in order. **Each
sub-PR is its own branch**, rebased after the previous one
merges. Do **not** combine #2a and #2b: the AST shape changes in
#2a alone are large, and bundling them makes the self-host break
in #2b harder to triage.

##### m7b #2a — Per-op generics mechanism (no consumer)

Goal: a user-declared effect with a per-op generic op compiles,
type-checks, and runs end-to-end. `Mutable` stays untouched.

Touches:

1. **Parser** (`parse_effect_ops`, ~3168) — accept optional
   `[T1, T2, …]` between op name and `(args)`. Match the bracket
   placement of m7b #11's effect-level tparams for visual
   consistency.
2. **AST** — extend `EffectOp` to carry per-op tparams:
   `EOp(name, op_tparams, params, ret, line, col)`. Update **every**
   pattern match on `EOp(...)` in the file: builtin decls
   (`13365–13412`), printer, resolver, inferencer, codegen,
   diagnostics. Existing call sites pass `[]`.
3. **Resolver** — push the op's tparams onto the resolution
   environment when checking the op's signature, popped after.
   Distinct scope from the effect-level tparams (m7b #11): a
   `State[T]` op can also have its own `op[U]`, with `T` bound at
   the effect level and `U` per-op.
4. **Inferencer** — at each call site `Eff.op(args)`, instantiate
   fresh tvars for the op's tparams (the same machinery that
   handles `forall T. …` for top-level functions, applied to the
   op's signature).
5. **Codegen** — pass an implicit element-size hint per op tparam
   as an extra `i64` argument, before user args. The default
   handler ignores it; FFI handlers consume it. This is the
   simplest evidence-layout-stable encoding (Doc C §*Evidence-
   layout stability*); a richer runtime witness can come later.
6. **Diagnostics** — render `Foo.op[T]` faithfully in error
   messages where the op signature is shown.

Test fixtures (new, under `examples/effects/m7b_2a_*.kai`):

- `m7b_2a_op_id_basic` — `effect Box { id[T](x: T) : T }` with a
  trivial `id(x, resume) -> resume(x)` clause; one call with
  `T = Int`, one with `T = String`, in the same handler scope.
- `m7b_2a_op_distinct_types` — same handler, two ops, distinct
  per-op tparams (`get[T] : T` returning state-of-T).
- `m7b_2a_negative_arity` — wrong number of tparams in op signature
  vs declaration (mirroring #11j arity check).
- `m7b_2a_negative_undeclared_tparam` — op body mentions a tparam
  not declared at the op (should be a clear resolver error).

Scope envelope: ~6–8 commits, comparable to m7b #11 a-h. **Do
not** touch `Mutable`, prelude, or stage 2 self-host code paths.

##### m7b #2b — `Mutable` migration

Goal: `Mutable` exists as a real effect declaration; `array_*`
are reached through it; stage 2 self-hosts unchanged at the
top-level row.

Touches:

1. **Builtin decl** — add `builtin_mutable_decl()` mirroring Doc B
   §`Mutable` §*Declaration*: `array_make[T]`, `array_length[T]`,
   `array_get[T]`, `array_set[T]`, `array_grow[T]`,
   `ref_make[T]`, `ref_get[T]`, `ref_set[T]`. All use per-op `[T]`
   (this is the first real consumer of #2a).
2. **Injection** — `inject_builtin_effects` injects `Mutable` per
   `inject_one` (gated on main's row mentioning it) — **not** per
   `inject_unconditional`, because unlike `State`/`Reader`/
   `Writer`, every `Mutable` use comes via the builtin, not via
   user-written `with Mutable { … }`.
3. **Default handler** — emit a default handler for `Mutable`
   whose clauses call directly into the existing
   `kai_prelude_array_*` C entry points and `resume`. The runtime
   piece is small because the C side already exists.
4. **Prelude removal** — drop `array_make / array_length /
   array_get / array_set / array_grow / ref_*` from
   `prelude_names` and `prelude_table`. They no longer exist as
   bare names; the only path is through `Mutable`.
5. **Compiler self-call rewrite** — every `array_make(…)` /
   `array_get(…)` / etc. inside `stage2/compiler.kai` becomes
   `Mutable.array_make(…)` / `Mutable.array_get(…)` / etc.
   Mechanical sed-class change, but the compiler **must keep
   compiling itself** at every commit boundary.
6. **Row propagation check** — once the rewrite is in, every
   compiler function that touches an array acquires `Mutable` in
   its row. Verify `main`'s row picks it up (and only it) and
   that the default handler installs cleanly.

The self-host break is the load-bearing risk. Mitigation: land
this sub-PR as one atomic commit (or a tightly-grouped pair
where #1 = decl + injection + default handler runtime, #2 =
prelude removal + compiler rewrite + Mutable.* migration).
Self-host gate (`make selfhost` + `make -C stage2 selfhost-llvm`)
must be green before the second commit.

Test fixtures: existing `examples/effects/` and any test that
exercises `array_*` is the regression suite. Add at least one
`examples/effects/m7b_2b_mutable_intercept.kai` that installs an
explicit `with Mutable { … }` to log every `array_set`, proving
the observe-mutation use case from Doc B §`Mutable` §*Default
handler*.

##### m7b #2c — Cleanup

After #2b stabilises:

1. Remove any compatibility shim left behind in #2b.
2. Audit the row of every public-facing stage 2 function: if a
   helper that does no real mutation acquired `Mutable` only via
   transitive plumbing, see whether refactoring removes the row.
   This is opportunistic; do not block #2c on it.
3. Update `docs/effects-impl.md` §m7b #2: *Pending* → **Landed**.
4. Update `docs/effects-stdlib.md` §`Mutable` §*Migration plan*:
   strike "currently uses … as unchecked builtins" — that is
   no longer true.

#### Decisions taken

- **Order**: #2a then #2b then #2c. Three sub-PRs, three
  branches, three rebases.
- **When**: after m7b #5b and #7 merge — both touch the desugar
  pipeline and `stdlib/`, neither overlaps #2 textually, but
  parallelising AST-shape changes (#2a's `EOp` extension) with
  in-flight desugar work invites painful textual rebases on the
  mono-file.
- **Not absorbed by #11**: m7b #11 parameterises the *effect*;
  #2 parameterises the *op*. `Mutable` needs the latter — one
  handler must serve every element type — and #11 cannot express
  that.

#### Out of scope for #2

- Row-polymorphic op call sites (Doc A §*Out of scope for v1*
  item 3, row-polymorphic clause). Stays out per Doc B §`Mutable`
  §*Declaration*.
- A richer runtime type witness beyond the implicit element-size
  hint. The hint suffices for `Mutable`'s FFI shape; richer
  witnesses can land if a future op needs them.
- Migration of `Spawn` or `Actor[Msg]` to per-op generics. Doc B
  flags both as candidates but neither is in m7b's scope.

## Diagnostic quality

Three new error classes from effect types. Each has a prescribed
diagnostic shape; the emitter carries the required metadata
through inference and CPS transform.

### Row mismatch

```
error: effect row mismatch
  --> src/foo.kai:42:5
   |
42 |     process(input)
   |     ^^^^^^^^^^^^^^ has row `Io + Fail`, caller expects `Io`
   |
   = help: the caller's declared row does not include `Fail`.
           Either add `Fail` to this function's signature, or
           handle it locally with `handle { ... } with Fail { ... }`.
   = note: row unification failed at label `Fail` (only in
           left-hand side)
```

Required: show both rows side by side in canonical form (aliases
expanded on mismatch, per Doc A §*Open questions* #5); show the
offending label highlighted; point at either the declaration site
or the call site, whichever is the root cause.

Root-cause selection uses the **unification trace**: a stack of
`(step_kind, source_position)` entries that the unifier appends
on each `unify_row` step (label-add, label-remove, payload
recurse, tail-bind). When unification fails, the diagnostic
emitter walks the trace backward from the failure point to the
earliest entry whose `source_position` is in user code (skipping
compiler-internal positions such as inferred row variables) —
that is the span the message points at. The trace is maintained
only during inference; it is dropped once the function's row is
finalised.

### Effect not handled

```
error: effect not handled: Fail
  --> src/foo.kai:7:3
   |
 7 |   Fail.fail("bad input")
   |   ^^^^^^^^^^^^^^^^^^^^^^ requires an enclosing `handle ... with Fail`
   |
 2 | fn main() : Int {
   | ------------------- `main`'s row is empty — no handler in scope
   |
   = help: either declare `main() : Int / Fail`, or wrap this
           call site in `handle { ... } with Fail { ... }`.
```

Required: name the effect, point at the call site, point at the
nearest enclosing function whose row would need to include the
effect, and offer both fixes (extend signature or local handle).
"Nearest enclosing function" is a function that has an explicit
signature — intermediate anonymous lambdas are traversed but not
reported.

### One-shot continuation reused

Runtime diagnostic (static case is caught by the checker with a
separate message):

```
runtime error: continuation resumed twice
  at   src/bar.kai:18:7 (inside `with Search { choose(... , resume) -> ... }`)
  hint: use `resume_multishot` if you intend to branch.
```

The runtime resolves the message via the `handler_id` table
(§*Surface-to-runtime mapping*), which maps each id to both the
`handle` block's span and a per-op clause-span map. The op
in-flight at the panic — the one whose continuation was reused
— picks the clause span. This is the only runtime-specific
diagnostic; the other two are compile-time.

### Budget

Each message includes:

- A one-line summary (the `error:` heading).
- A primary span (where the error is).
- A secondary span (where the root cause is, if different).
- A `help:` fix-it with concrete syntax.
- A `note:` on the underlying rule (for new users).

This document fixes the *shape* (which pieces every effect
diagnostic must contain) and mirrors stage 2's §8 *Diagnostics at
Elm/Rust quality* commitment. The *wording* of each message is
reviewed in m7a's diagnostic-pass task — the same task that
applies the bar to all stage-2 diagnostics, not just effects.

## Interaction with fibers (m8 preview)

Fibers are scheduled computations; each owns its own stack, its
own evidence vector, and its own RC-managed heap. The hook
between effects and fibers is that `Spawn` is an effect handler
whose clauses create new fiber records and install them on the
scheduler.

Brief sketch (full spec in `docs/fibers-impl.md`, TBD):

- `Spawn.spawn(body)` clause: allocate `Fiber`, copy parent's
  evidence pointer, enqueue on scheduler, return a `Fiber[T]`
  handle.
- `Cancel.cancel(fib)` clause: mark the target fiber cancelled;
  the next `perform` on that fiber sees the `Cancel` op
  delivered instead of resuming.
- Cross-fiber message copy (Doc A §*Context*'s fiber isolation
  rule): enforced by `Actor.send` copying via a generic deep-copy
  routine; the sending fiber's Perceus RC is consulted for reuse
  (if the message's RC is 1, donate; else copy).

Nothing in this section changes the runtime shape defined in
earlier sections. Fibers extend it with an additional per-fiber
allocation; effects within a fiber behave as specified above.

## Out of scope for v1

- **Multi-shot `resume` by default.** Opt-in via
  `resume_multishot` (Doc A §*Out of scope for v1*).
- **Handler inlining of the Effekt fast-path shape.** Régime C
  always goes through evidence; the optimiser can fold trivial
  clauses, but the default path is indirect.
- **Segmented stacks.** Régime C reifies continuations as heap
  closures (when promoted) on a normal C stack; the segmented-
  stack approach used by some Effekt backends is not adopted.
- **First-class named handlers.** Doc A rules them out.
- **Cross-fiber evidence sharing.** Each fiber has its own
  vector; there is no global effect registry.
- **Thread-level parallelism that is not fiber-based.** Stage 2
  §*What stage 2 deliberately does not ship*.
- **Dynamic effect declaration.** All `effect` declarations are
  top-level and closed (Doc A §*Open questions* #6 tentative,
  confirmed here).
- **Codemod spec for `kai fmt --upgrade-effects`.** Moved to
  `docs/migrations/m7-effects.md` (TBD). Doc C only specifies
  what the compiler emits; the codemod is a separate tool.
- **Actor mailbox layout, lock-free vs mutex, `DropOldest`/
  Perceus interaction.** Belongs to `docs/fibers-impl.md`.
- **`kai fmt` full formatter spec.** Belongs to
  `docs/formatter.md` (TBD). Doc C specifies only the desugar
  order, which the formatter needs as input.

## Open questions

1. **Evidence lookup representation.** Intrusive cons is the
   pick; the alternative is a per-effect slot table for O(1)
   lookup. The latter becomes attractive if handler depth ever
   exceeds ~50 in realistic workloads.
   *Tentative:* stay on cons; revisit after m7 lands and
   profiling is possible.

2. **Tail-call optimisation inside `State[T]`-recursive
   functions.** When a recursive function reads and writes state
   on every call, the variable-specialisation pass must ensure
   the `load`/`store` do not defeat tail calls. Straightforward
   if the slot is in the caller's frame; needs measurement if
   the recursive frame grows.
   *Open.* Resolve during m7b implementation.

3. **Thread-local vs fiber-struct for `evidence_top`.** A
   thread-local is cheap but ties fibers to kernel threads; a
   fiber-struct field is one more indirection but preserves
   migration across threads.
   *Decided:* fiber-struct field, even in m7a where the runtime
   still has only an implicit single fiber. The cost today is
   one extra indirection at every `handle` push/pop and at every
   `perform` lookup; the benefit is that m8 (the real scheduler)
   needs no refactor of the handler-stack runtime — it only
   adds the spawn/yield logic on top.

4. **Per-op generics and the evidence struct's ABI.** If a
   stdlib effect's op set changes between versions, the evidence
   struct's layout changes with it. Stage 2 has no stable ABI
   promise (Doc A §*Context* and `docs/design.md`), so this is
   deferred. Revisit if/when kaikai gets a package ABI story.
   *Open.* Tracked separately from m7.

5. **Diagnostic rendering for aliased rows.** When `type Io =
   Console + Stdin` is used in a signature, should a mismatch
   show `Io` or `Console + Stdin`? Doc A §*Open questions* #5
   says "expand on mismatch" — this remains the rule, but the
   *kind* of mismatch matters: a missing `Console` should read
   clearer if `Io` is mentioned than if only the expanded set
   appears. Implementation may need both forms.
   *Tentative:* show the alias as a note, the expanded form as
   the primary rendering. Revisit after diagnostic review in
   m7a.

6. **Régime-cost benchmark — concrete shape.** §*What C rules
   out* commits to a 2× ceiling against an Effekt-style direct
   baseline, and m7a #9 makes the measurement a milestone gate.
   *Decided* (m7a #9):
   - **Workload.** Trivial-clause `Counter.tick()` in a tight
     loop. The clause body is `resume(())` — the smallest
     possible `perform`/`resume` round-trip, so the measurement
     isolates dispatch + cont-check + identity-resume cost
     without state, allocation, or branch noise. Composite
     workloads (`State[Int]` counting, nested `Io + State +
     Fail`) are deferred — they bundle costs that the gate is
     not trying to measure.
   - **Metric.** Wall-clock time from `clock_gettime(CLOCK_MONOTONIC)`
     over N = 10⁷ iterations, reported as ns/op (total / N). The
     reciprocal (ops/sec) is included for cross-language sanity
     comparisons but the gate keys off ns/op.
   - **Baseline.** Plain C: a static function with the same
     side-effect (incrementing a global) called directly N times,
     compiled at the same optimisation level. Effekt's own
     backend was the original Doc C target, but installing
     Effekt is a heavyweight prerequisite for a milestone gate
     and the C-direct baseline establishes an absolute lower
     bound that is reproducible on any laptop. Translating the
     2× vs Effekt ceiling: published Effekt overhead vs C-direct
     sits in the 2-3× band, so a 5× ceiling vs C-direct keeps
     the régime under 2× vs Effekt by transitivity. The exact
     coefficient is a design judgement, not a measurement —
     revise if a kaikai↔Effekt direct comparison ever lands.
   - **Threshold.** **5×** ns/op vs C-direct. Above this, the
     régime decision (régime C, surface Effekt over Koka-style
     evidence-passing runtime) is revisited per §*Fallback*.

7. **Per-op generics for non-trivially-copyable `T`.** §*Per-op
   type generics* describes the erased ABI as "raw bytes +
   element size". This works for `Int`, `Bool`, primitive-sized
   types. For `T = Option[Int]` or any sum/record type, the
   ABI needs the *layout*, not only the size — drop sequences,
   alignment, tag positions all depend on `T`'s shape. The
   monomorphic caller already knows `T`'s layout; what crosses
   the erased boundary needs to be enough information for the
   op body to operate without re-deriving it.
   *Open.* Resolve during m7b implementation, before
   `array_make[T]` ships for non-primitive `T`.

8. **Balance invariant under non-lexical fiber boundaries.**
   §*Per-fiber isolation* fixes the invariant `evidence_top ==
   floor` at fiber yield/exit, enforced by lexically balanced
   `handle` push/pop. The invariant holds as long as every
   `handle` block in source corresponds to a single
   compiler-emitted prologue/epilogue pair within the fiber's
   body. If m8's scheduler ever permits constructs that split a
   `handle` across a yield (e.g. `spawn { ... handle ... }`
   where the inner `handle` runs partly in the parent and
   partly in the child), the lexical guarantee weakens.
   *Open.* Revisit when `docs/fibers-impl.md` pins the
   spawn/yield model.

9. **Hole reports inside effect contexts.** Typed holes
   (`docs/typed-holes.md`) report the expected type and the
   visible scope at the hole site, including for ordinary
   bodies. A hole inside a handler clause body should also
   surface the *effect context*: which clause it appears in,
   which op signature constrains its result, what the surrounding
   `handle`'s `S` type is. None of that is reported today; the
   stub at m7a #4e treats clause bodies like ordinary
   expressions. Without it, the LLM-authorability bet (Tier 3,
   `CLAUDE.md`) loses precision exactly where effect-typed
   programs need it most.
   *Open.* Resolve once clause-body type-checking lands (later
   in m7a or alongside m7a #6).

## Milestones m7a and m7b

Doc B §*Next steps* pre-split m7 into m7a (mechanics) and m7b
(ergonomy). Doc C inherits the split and maps implementation
tasks onto it.

### m7a — mechanics

Lands first. End state: `fn main() : Unit / Console {
Console.print("hi") }` compiles and runs.

1. **Row in `TyFnT`** — §*Types: row slot on `TyFnT`*.
2. **Row unification** — `unify_row` per Doc A §*Row unification*.
3. **Evidence type generation** — per effect declaration, per
   §*Evidence types*.
4. **CPS transform** — §*The CPS transform*; effectful functions
   only.
5. **Handler-stack runtime** — §*Handler-stack runtime*.
6. **`perform` / `handle` / `resume` lowering** — §*`handle`
   lowering* + §*`resume` representation*.
7. **Default handlers** — Console, Stdin, Env, File, Mutable,
   Fail (installed by the runtime for `main`, per Doc B §*`main`
   and the runtime*); Ffi as compiler-synthesised.
8. **Diagnostics baseline** — row-mismatch and effect-not-handled
   per §*Diagnostic quality*, with the budget enforced.
9. **Régime-cost micro-benchmark** — measure trivial-clause
   `perform`/`resume` overhead against an Effekt-style direct
   baseline. Gates the régime decision: a 2× regression triggers
   the revisit hook in §*What C rules out*.

### m7b — ergonomy

Lands after m7a. End state: Doc B's code samples read on the
page the way they are written.

1. **Closed effect aliases** — `type Io = Console + Stdin +
   Env + File`; desugared at resolve time to the label set;
   display hint preserved per §*Row normalisation*. **Landed.**
2. **Per-op type generics** — §*Per-op type generics*; Mutable's
   `array_make[T]` / `array_get[T]` / `array_set[T]` migrate to
   this form. *Pending.*
3. **Trailing lambdas** — pre-resolve desugar pass
   (`docs/syntax-sugars.md` §*Trailing lambdas*). Includes
   single trailing, lambda-block as expression, double trailing.
   **Landed.**
4. **`@cap` and `cap := v`** — post-resolve desugar pass
   (depends on capability bindings produced by resolve).
   **Unblocked by #11** — `State[T]` and `Reader[T]` are now
   available as parametric effects; capability bindings of those
   types exist in scope. *Pending.*
5. **`var x = init`** — pre-resolve desugar + variable
   specialisation (§*Variable specialisation*). **Parser-only
   landed; desugar unblocked by #11** — Doc B §3 mandates lowering
   to `handle { rest } with State[T](init) as name { ... }`,
   which now exists end-to-end. A first attempt lowered to a
   1-element `Mutable.array_make` cell-binding instead; that
   shortcut was reverted to stay faithful to Doc B (locality
   guarantee, no Mutable-row contamination). *Desugar pending.*
6. **`a[i]` and `a[i] := v`** — pre-resolve desugar pass.
   **Landed.**
7. **`Reader[T]` / `Writer[W]`** — new stdlib effects (Doc B
   §*Open questions* #2 resolved in their favour). **Unblocked
   by #11** — both effects work end-to-end as #11 fixtures
   (`examples/effects/m7b_11_reader_basic.kai`,
   `m7b_11_writer_basic.kai`); what remains is promoting them
   from test fixtures into `stdlib/`. *Pending.*
8. **Diagnostic review** — every message rewritten against the
   stage 2 §8 bar. *Lands last.*
9. **`|` map pipe** — binary operator wired in the parser,
   pre-resolve desugar to `map(xs, f)` (or `list_map`).
   Originally outside the m7b plan; landed as a sub-task
   surfaced by Linus-style review of #3. **Landed.**
10. *Reserved.*
11. **Parametric effects** — the umbrella sub-milestone that
    unblocks #4, #5 (desugar half), and #7. Scope:
    - parser: `effect Foo[T] { ops }` accepts type parameters;
    - AST: `DEffect` carries `[String]` tparams; `RowExpr`
      carries per-label type args (e.g. `Reader[Int]` distinct
      from `Reader[String]` in the row); `EHandle` carries an
      `Option[Expr]` init for `with Eff[T](init)`;
    - resolver: per-instance substitution of op signatures
      (`Reader[Int].ask : Int` vs `Reader[String].ask : String`);
    - inferencer: type-arg unification per row label;
    - codegen: per-instantiation handler-struct emission, op
      dispatch parameterised by the type instantiation;
    - diagnostics: render `Reader[Int]`, not `Reader`.
    First end-to-end user: `State[T]`. Once that lands,
    `Reader[T]` and `Writer[W]` become the trivial stdlib pair
    of #7. Comparable in scope to m7a #6 + #7 + #8 combined.
    **Landed.** Sub-steps a/b (parser + AST), c-h (`State[T]`,
    `Reader[T]`, `Writer[W]` end-to-end), i (per-instance
    `row.ty_args` propagation), j (arity check), k (diagnostics
    render type args), l (type-check `state` / `log` / `resume`),
    m (followup tests). Promotion of `Reader[T]` / `Writer[W]`
    from test fixtures into `stdlib/` remains as #7.
12. **Open row variables in user-written signatures** —
    `pub fn while(pred: () -> Bool / e, body: () -> Unit / e) :
    Unit / e` etc. Today the resolver (`check_row_expr` in
    `stage2/compiler.kai`) treats every label name as an effect
    declaration and rejects unknown names; row variables only
    appear at inference time via `unify_row` (m7a #2). To make
    row-polymorphic stdlib helpers (`while`, `until`, `repeat`,
    `forever`) writable, the row syntax must accept a row
    variable position — distinct from a label, scoped to the
    function's type parameters. Independent of #11; the two
    can land in either order. Blocker of `stdlib/loop.kai` as
    specified in `docs/stdlib-layout.md` §`loop`.

Within each sub-milestone the ordering is dependency-driven: the
evidence type generation (m7a #3) unblocks everything else in
m7a; the desugar pass (m7b #3–#6) runs as a single addition and
then specialisation (m7b #5) piggybacks on top. m7b #11 and
#12 were each large enough to be milestones in their own right;
both have landed. The remaining m7b items (#2, #4, #5b, #7, #8)
are now all unblocked.

## Next steps

Two documents are unblocked by Doc C:

- **`docs/fibers-impl.md`** — scheduler, per-fiber state,
  `Spawn` / `Cancel` / `Actor` runtime, mailbox layout, deep-copy
  across fibers, Perceus interaction with cross-fiber messages.
  Targets m8.
- **`docs/migrations/m7-effects.md`** — codemod spec for `kai
  fmt --upgrade-effects` (m7a); second-pass codemod for the m7b
  sugars. One-time tools; not part of the compiler proper.

A third document is *not* blocked by Doc C but benefits from it:

- **`docs/formatter.md`** — `kai fmt` canonical output spec.
  Doc C pins the desugar order, which the formatter consumes; the
  rest (line breaks, spacing, import ordering) is independent.

Implementation of Doc C itself lives in m7a / m7b tasks, each a
sub-design-doc when it comes up per stage 2 §*Milestones within
stage 2* convention.
