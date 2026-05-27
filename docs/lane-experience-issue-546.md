# Lane experience — issue #546: dispatcher dedup keyed on op-name alone

## Scope as planned vs as shipped

**Planned (brief):** the protocol-dispatcher generator dedupes the
synthesised `__proto_<op>` dispatcher by op NAME only, so two protocols
declaring an op with the same name collide — the second protocol's dispatcher
is never generated. Proposed fix: rename the dispatcher to `__proto_<P>_<op>`
(or key the dedup on `(protocol, op)`), updating every call site that resolves
`X.op(...)` to the dispatcher name.

**Shipped:** a narrower, lower-risk fix. The collision is real and
constructable, but its load-bearing cause is NOT the generator dedup — it is
the **static protocol-call rewrite** (`resolve_protocol_calls` →
`try_rewrite_proto_call`) fixing the protocol to whichever was declared first
via the single-result `proto_op_lookup`. The fix makes the rewrite iterate
every protocol declaring the op and pick the one with an impl for the receiver
type. The dispatcher name stays `__proto_<op>`; the generator dedup is
untouched.

## Was the collision constructable?

Yes — real bug, not latent. Repro (confirmed failing before the fix):

```kai
protocol Speak { sound(x: Self) : String }
protocol Move  { sound(x: Self) : String }
type Animal = Dog | Cat
type Vehicle = Car | Bike
impl Speak for Animal  { fn sound(x: Animal)  : String = "woof" }
impl Move  for Vehicle { fn sound(x: Vehicle) : String = "vroom" }
fn main() : Unit / Stdout = { Stdout.print(sound(Dog)); Stdout.print(sound(Car)) }
```

Before: `error: no impl of 'Move' for type 'Animal' (operation 'sound')`. The
rewrite looked up `sound` → first POR (`Move`), searched impls only in `Move`,
found none for `Animal`, declined to rewrite, and left a residual `__proto_sound`
dispatcher tagged `Move` — which `validate_resolved_protocols` then rejected.
After: `woof` / `vroom`, independent of protocol declaration order (verified
both orders).

## Why rewrite-loop, not the dispatcher rename

kaikai dispatches protocol ops by **bare name** — `sound(x)`, never
`Speak.sound(x)`. There is no surface syntax to qualify the protocol at the
call site; only the receiver's type disambiguates. So the call site genuinely
needs a *type-directed* choice among the protocols declaring the op — which is
exactly what the rewrite loop does.

The dispatcher rename would only matter for the **polymorphic-residual** path:
a body that stays polymorphic past monomorphisation and falls to the runtime
`__proto_<op>` shim (which embeds a single `proto_id`). That path is only
producible from a protocol-bounded generic. kaikai **rejects protocol bounds on
user functions** — verified: `fn f[T : Speak](a: T)` →
`error: type-parameter kind must be 'Type' or 'Measure'`. Bounds exist only on
stdlib `impl[T : P] …` declarations, where the bound *fixes the protocol*, so
the shared-name dispatcher is never ambiguous in any reachable residual.

Conclusion: the rewrite-loop closes every constructable #546 case. The rename
would ripple into `emit_c` / `emit_shared` / the impl-table mangling and
threaten selfhost byte-identity, for zero additional constructable coverage. It
was deliberately left out of scope.

## Structural surprises

- The brief framed this as a generator-dedup bug. The dedup IS keyed on op-name
  alone, but tracing the actual failure showed the residual misdispatch is
  produced by the rewrite picking the wrong protocol, long before the runtime
  dispatcher matters. The generator dedup is a latent symptom only.
- `proto_op_lookup` (single-result, first match) had two callers in the rewrite
  path; both needed the multi-candidate variant. A third caller (`find_impl`,
  used elsewhere) was left on the single-result lookup since it is not on the
  bare-name rewrite path.

## Fixtures added

- `examples/protocols/shared_op_name_546.kai` + `.out.expected` (`woof\nvroom`).
  Picked up automatically by `test-protocols`, which validates both the C and
  LLVM backends against the golden. Two protocols sharing op `sound`, impls on
  distinct types.

Coverage gap: no fixture exercises the return-typed-Self path
(`try_rewrite_ret_candidates`) across two protocols, because constructing a
`from(x) : Self`-shaped op shared by two protocols with disjoint impls is
awkward and the single-protocol path is already covered by `proto_pa_*`. The
code path mirrors the args-borne one exactly; the risk is low.

## Cost vs estimate

Small, single-file change (`stage2/compiler/protos.kai`, +96/−41, mostly two
new helper fns + comments). Most of the lane time went to *proving the scope
reduction* — confirming the polymorphic-residual path is unconstructable —
rather than to the edit itself.

## Follow-ups for next lanes

- If kaikai ever gains protocol bounds on user functions (it does not today,
  and that would be an edition-level surface change), the runtime dispatcher
  WOULD become ambiguous for a shared op name, and the `__proto_<P>_<op>` rename
  would then be load-bearing. Revisit #546's scope at that point.
- The generator dedup keyed on op-name alone is now harmless but still
  technically imprecise; a future cleanup could key it on `(protocol, op)` for
  clarity, gated behind a selfhost check.
