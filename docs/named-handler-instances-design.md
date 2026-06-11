# Named handler instances — design

**Status: SUPERSEDED (2026-06-11) by
`docs/effects-capability-passing-design.md`.** The design review (asu)
found this doc's premise false: the runtime is dynamic-scoping over the
fiber evidence stack, not capability-passing — the `handler_id` is
lexical and dies at function boundaries (`alias_map_disable_tag`), so
the flagship example cannot work on today's model. The owner chose the
structural fix (capability-passing evidence transport, "decision A");
this doc's surface design survives as §6 of the successor. Kept as
lineage.

**Original status:** proposal (2026-06-08). Promotes the "Named handlers
as first-class values" item from `docs/effects.md` §*Out of scope for
v1* (lines 718–721) to a concrete, scoped feature.

**Depends on:** #789 (effect/`State` evidence resolved by op-name, not
handler instance) as a soundness prerequisite — see §4.

---

## 1. The problem

Today kaikai resolves an effect op to a handler **by effect name**, with
the rule that *the innermost handler for an effect always wins*
(`docs/effects.md`:722–725). The doc's own workaround for "I need two of
the same effect" is "declare two effects."

That rule has two costs:

1. **It cannot express two live instances of one effect addressed
   independently.** The Effekt `named` example —
   `add {c1: Cell} {c2: Cell} {dst: Cell}`, three independent cells
   passed to one function — has no faithful kaikai form. You must invent
   three distinct effects, which does not scale and loses the point.

2. **By-name resolution is unsound when op names collide** (#789).
   Reading a `@var` inside an unrelated handler clause, or calling a
   user effect's `get` while a `State` runner is installed, binds to the
   wrong handler's evidence — producing silent corruption, a runtime
   `type mismatch`, or a segfault. The repros are in #789.

These are the same gap seen from two sides: there is no surface way to
*name* an instance, because resolution does not key on instance identity.

## 2. The infrastructure already exists

This proposal is **not** a new runtime mechanism. Per-instance dispatch
is already implemented; it is simply not the default path and not
surfaced.

- The evidence struct already carries a unique instance id:
  `EvIo { handler_id : HandlerId, … }` (`docs/effects-impl.md`:216).
- The runtime already has by-id lookup:
  `kai_evidence_lookup_node_by_id` (`stage2/runtime.h`:11053) — *"find
  the evidence node whose handler_id matches `id`, no name match
  required."* Added under m7b #15 precisely so an outer alias's op stays
  reachable after an inner `with Eff as other` shadows the effect name.
- The surface already binds an instance name: `with Eff as alias`
  (`docs/effects.md` open question #1, decided). Codegen already mints
  the id at the `as` site:
  `KaiHandlerId kai_alias_<a>_id = _ev.handler_id;`
  (`stage2/compiler/emit_c.kai`:6307).

So a `with Eff as a` binding *already* dispatches `a.op(...)` by
`handler_id`. What is missing is (a) making `a` a value that can leave
the `with` block as a function argument, and (b) routing the **default**
(non-aliased) op call through the same by-id lookup so name collisions
stop being unsound.

## 3. Surface design (D1)

**Reuse `as`. No new form.** The rebound name becomes a first-class
*capability value*. Its type in a signature is the **capability type
itself** — `State[Int]`, `Cell`, `Logger` — never a wrapper like
`Handler[State[Int]]`.

```kaikai
effect Cell { get() : Int  set(n: Int) : Unit }

# A function that consumes three Cell instances. The cells are PROVIDED
# (passed in), so they do NOT appear in the row — the row is empty.
fn add(c1: Cell, c2: Cell, dst: Cell) : Unit / {} =
  dst.set(c1.get() + c2.get())

fn main() : Int / Stdout = {
  handle { handle { handle {
    c1.set(1)
    c2.set(2)
    add(c1, c2, target)
    Stdout.print(int_to_string(target.get()))
    0
  } with State[Int](0) as target {
      get(resume) -> resume(state)
      set(v, resume) -> resume((), v)
      return(x) -> x
    }
  } with State[Int](0) as c2 { get(resume) -> resume(state)
                               set(v, resume) -> resume((), v)
                               return(x) -> x }
  } with State[Int](0) as c1 { get(resume) -> resume(state)
                               set(v, resume) -> resume((), v)
                               return(x) -> x }
}
```

Why the capability type and not a `Handler[E]` wrapper: a wrapper is a
type constructor *over an effect*, i.e. effect-kinded generics / HKT —
**Tier 1.3 forbids HKT.** With the capability type, one uniform rule
covers everything already in the language:

- **capability in a row** (`fn f() : T / Cell`) = the effect is
  *demanded*; a `handle … with Cell` must satisfy it.
- **capability as a parameter** (`fn f(c: Cell) : T / {}`) = the effect
  is *provided* by the caller; it does not enter the row.

A `with Eff as a` binding produces a value of type `Eff` usable in
either position. `a.op(...)` is an ordinary method call (open question
#1, already decided).

## 4. Soundness: the dependency on #789 (D3)

The default op call (`Eff.op(...)`, no alias) still routes through
`kai_evidence_lookup_node` — the **by-name** path that is the root of
#789. Named instances must route every call through the by-id path.

**Order, not fusion:**

1. **Close #789 first** by moving the default resolution from by-name to
   by-id. This is mechanically the generalization of the already-shipped
   aliased path (§2) to the non-aliased case: every `handle` already
   mints a `handler_id`; the op call captures the id of its lexically
   resolved handler at the call site, exactly as the `as` path does, and
   dispatches with `kai_evidence_lookup_node_by_id`. The by-name lookup
   is retired.
   - Separately, #789 Repro 3 exposes an **orthogonal typer hole**: a
     row variable `e` absorbs an effect that has *no* real handler when
     an op name collides with an installed effect, so a type error is
     missed and codegen segfaults. That gets its own fixture and fix; it
     is not solved by the resolution change.

2. **Then named instances** is a thin surface layer on top: it exposes
   the `handler_id` the by-id path already keys on.

Keeping two live resolution paths (by-name *and* by-id) is the
anti-pattern that produced #789 in the first place — so the sequence is
deliberate: unify resolution, *then* surface it. Do not ship named
instances on top of a still-by-name default.

## 5. Escape (D2)

A first-class capability value must not outlive the `handle` that
installed it — its evidence frame is popped at the closing brace. kaikai
forbids both a borrow checker and escape-analysis passes (Tier 1), so
the restriction is **positional and second-class-lite**, checked locally
in `resolve`/`infer` with no new pass and no flow analysis:

> A capability value (a name bound by `as`) may appear **only in
> argument position of a call**. It may not be the RHS of a `let`, a
> field of a record literal, an element of a collection, or a free
> variable captured by a lambda or closure that can outlive the
> `handle`.

This is a syntactic check at the binding's use sites, in the spirit of
Effekt's second-class block parameters: a capability flows *down* into
functions you call, never *up* into values that survive. It is strictly
weaker than Effekt's `box`/`at {r}` second-class system (no region
types, no escape analysis) and strictly enough to keep the evidence
frame from escaping.

A row-variable escape encoding (the instance carries the handle's row
variable; the typer rejects escape because that variable cannot appear
in the handle's result type, à la `runST`) was considered and **not
chosen for v1**: it depends on the `handle` skolemizing a region
variable, and kaikai's decision to *mask* `Mutable`/`State` at scope
boundaries (`docs/effects.md`:620) rather than skolemize a region is
evidence that quantifier is not present. Assuming it would be hidden
typer work. The positional rule is what falls out of the existing
mechanism; revisit row-escape only if skolemization is verified to
exist.

### Why the no-escape restriction costs nothing

Koka uses named handlers + rank-2 types to build a *dynamic heap of
first-class mutable references* (`samples/handlers/named/heap.kk`:
`new-ref`, `dynamic-ref` returning a ref out of its creating scope).
That machinery exists **because Koka has no primitive mutable reference
— it reconstructs references on top of the effect system**, and an
escaping reference then needs rank-2 region typing (and `div` + `ctl`
multi-shot, both excluded in kaikai) to stay sound.

kaikai does not carry that weight on its effects, because the cases that
*need* a value to outlive its creating scope already have first-class,
primitive carriers:

- **A reference that must escape its scope** → `Ref[T]` under `Mutable`
  (`ref_make` / `ref_get` / `ref_set`, `docs/effects-stdlib.md`:1479).
  `Ref[T]` is a first-class, storable value — `stdlib/collections/hashmap.kai`
  keeps `buckets : Ref[Array[..]]`, `count : Ref[Int]`, `cap : Ref[Int]`
  in a record that outlives every method, and swaps the whole bucket
  array through the `Ref` on resize. That is exactly Koka's
  `dynamic-ref`, done with a data type instead of a named effect.
- **A dynamic, addressable population of stateful entities** → actors
  (`Actor[Msg]`, a first-class storable `Pid`) or fibers (`Spawn`).
  "Many independent things with private state, created at runtime,
  addressed individually" is a *process* shape in kaikai, not a handler
  shape.

So the no-escape rule does not remove an expressible program; it only
declines to make *handler instances* a second mechanism for something
`Ref[T]` and actors already cover. Named instances is then a genuinely
lexical feature — N static instances of one effect passed *down* into a
function (`samples/handlers/named/ask.kk`, the Effekt `named` example) —
and forbidding escape removes nothing from that shape, because anything
that needed to escape would be a `Ref`, not a capability.

## 6. Scope and stability (D4)

- **Purely additive.** Today you *cannot* name an instance; granting the
  capability breaks no existing program. Ships as `feat:` within the
  current **hanga-roa** edition. No edition bump.
- **The #789 fix is `fix:` / PATCH and is not breaking.** For any
  program whose resolution is unambiguous (one handler per effect on the
  stack), by-name and by-id are identical. Only programs that today
  exploit the unsound by-name collision change behaviour — and they
  change from *undefined* (silent corruption / segfault) to a defined
  result or a compile error. The stability guarantee
  (`docs/decisions/editions-stability-without-stagnation-2026-05-15.md`)
  protects *correct* code, not undefined behaviour; UB → compile-error
  is explicitly permitted within an edition.
- **Migration note (one line):** "If a program relied on op-name
  collision between two effects sharing an op name, it was unsound
  (#789) and now errors at compile time; rename the colliding op or
  handle the effect explicitly."

## 7. What this does NOT add

To keep the feature inside the tiers, the following are explicitly out:

- **No `Handler[E]` type, no effect-kinded generics, no HKT** (Tier 1.3).
- **No escape analysis / region types / borrow checker** (Tier 1).
- **No multi-shot.** A captured capability is still one-shot; naming an
  instance does not let you resume it twice.
- **No first-class storage of capabilities.** §5's positional rule
  forbids stashing a capability in a long-lived value — that would
  require escape analysis to make sound. This is not a gap: a value that
  must outlive its scope uses `Ref[T]` (`Mutable`); a dynamic population
  of stateful entities uses actors/`Spawn` (§5 *"Why the no-escape
  restriction costs nothing"*). Capabilities stay lexical on purpose.

## 8. Acceptance

- The Effekt `named` example (`add {c1}{c2}{dst}`) has a faithful
  kaikai port that compiles and runs.
- #789's three repros are fixed by the by-id resolution change (§4.1).
- A positive fixture under `examples/effects/` exercises two live
  instances of one effect addressed independently.
- A negative fixture rejects a capability value escaping its `handle`
  (stored in a `let` that outlives the block).
- selfhost byte-id intact; the resolution change is behaviour-preserving
  for all unambiguous programs (the existing 388 fixtures stay green).

## 9. Open questions

1. **Parameterised instances in signatures.** `fn f(c: State[Int])`
   names the cell's payload type. Is `fn f(c: State[T])` (row-poly over
   the payload) allowed, or only monomorphic instances? Leaning
   monomorphic for v1 — matches "one `handle … with State[Int]` is
   monomorphic per instance" (`docs/effects.md`:262).
2. **`mask` as a sibling operator.** Koka's `mask` makes an inner op
   call skip the innermost handler and reach an outer one. With named
   instances the same reach is achievable by naming the outer instance,
   so `mask` may be redundant here — but it is the ergonomic form when
   you do not want to thread a name. Decide alongside, not before.
