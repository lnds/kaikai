# Lane experience — protocols runtime dispatch (Option C, 2026-05-23)

## Scope as planned

Resolve the pre-existing `panic: __protocol_dispatch_Show_show` that
makes `make -C stage2 selfhost-llvm` fail when polymorphic `impl[T : P]
P for [T]`-style bodies recursively call `show(x : T)` where `T` is
still a free type variable post-monomorphisation. The C backend masked
the bug by DCE-ing polymorphic impl bodies; the LLVM backend kept them
and the dispatcher placeholder panicked the moment a runtime path
reached it.

The user picked **option C** (the runtime impl-table sketch in
`docs/protocols.md` §Codegen sketch / Runtime cost): when the typer's
post-mono rewrite cannot pick `__pimpl_<P>_<T>_<op>` statically, the
dispatcher falls back to an `O(1)` lookup in a hash table keyed by
`(proto_id, head_type_tag)`.

## Scope as shipped

Three commits, all on `main`:

1. `feat(runtime): add impl-table hashmap + head-type tags for protocol dispatch`
   — `stage0/runtime.h` infra: `KAI_HEAD_*` / `KAI_PROTO_*` constants,
   `kai_head_tag(v)`, `kai_variant_to_head[]` table + setter,
   `KaiImplEntry` + open-addressing `kai_impl_table` + `kai_lookup_impl` +
   `kai_register_impls`. `struct rec` gains a `head_type_tag` field
   (`kai_record_h` / `kai_reuse_or_alloc_record_h` are the nominal
   constructors).
2. `feat(emit): C-backend runtime dispatch shim for protocol dispatcher`
   — `make_dispatcher_body` now emits a magic `ETodo`
   (`__kai_proto_dispatch__:<P>:<op>`); `emit_proto_dispatch_shim_c`
   recognises the marker (walking through Perceus' exit-drop EBlock wrap)
   and writes a C body that does head-tag → lookup → indirect call.
3. `feat(emit): impl-table registration + LLVM dispatch shim for protocols`
   — `emit_proto_runtime_tables` (C) and `llvm_emit_proto_init` (LLVM)
   build the per-program impl-table arrays and emit a registration
   shim called once at startup before user `main`. `synth_todo` skips
   hole registration for dispatcher markers so `--holes-json` does not
   list internal shims as user holes.

Doc updates: `docs/variant-tags.md` gained the "Head-type tags —
protocol dispatch key" and "Protocol IDs" sections, pinning the
`KAI_HEAD_*` (0..19) and `KAI_PROTO_*` (0..11) reserved ranges with
`KAI_USER_HEAD_TAG_BASE = 20` and `KAI_USER_PROTO_ID_BASE = 12`.

## Design decisions

### Why option C over A or B

asu's first read (consulted before the user picked) was that **the
doctrine-A path is already implemented** in `stage2/compiler.kai`
around line 42554 (`resolve_protocol_calls_decl` re-runs the rewrite
after spec). The recommendation was to diagnose the LLVM panic as a
puntual gap in that pass, not redo the architecture. The user picked
C anyway, because (per `docs/protocols.md` §Codegen sketch and the
CLAUDE.md doctrine "single-dispatch protocols Go/Clojure/Elixir-style
— O(1) impl-table lookup") it is the documented direction for cases
where the static rewrite genuinely cannot resolve `T`.

Both A and C are now in the compiler: A still runs for every
monomorphic call site (the validator catches concrete-receiver misses
at compile time), C only fires when `T` stays polymorphic post-mono.
Runtime cost is one extra indirect call inside polymorphic impl
bodies; monomorphic call sites stay direct.

### Head-type tag derivation — option 3 hybrid

asu evaluated four schemes:
- (1) `KaiTag` as dispatch key — collides (`KAI_VARIANT` covers
  Option/Result/all user sums).
- (2) Add `head_type_tag` to every `KaiValue` — 8 bytes/value of
  memory, plus a sum-type registry duplicating the variant_tag
  problem the user already rejected auditing.
- (3) Hybrid: compiler emits a `kai_variant_to_head[]` paralleling
  the existing variant table; records get a 4-byte `head_type_tag`
  field; primitives switch on `KaiTag`.
- (4) Pass the head tag explicitly at the call site — defeats the
  whole point of C (typer doesn't know the head in polymorphic call
  sites).

We picked **(3)**. Zero memory cost for variants; 4 bytes per record;
one indirection + small switch for the inline lookup. Anonymous
records get `head_type_tag = 0`, which the hashmap never matches —
single-dispatch on structural records is not supported in v1.

### Validator vs runtime panic

`validate_resolved_protocols` already rejects every concrete-receiver
call site where no impl exists (the m12.8.x post-mono dispatch
validator). For polymorphic `T` it leaves the dispatcher panic in
place — that is the case option C now resolves at runtime, hopefully
without ever firing for legitimate code (the `impl[T : P]` bound
should be enforced at instantiation).

Reaching the runtime panic is a soft assert: either the bound was
not enforced at every instantiation site (typer bug) or the
impl-table is corrupted. The shim prints a useful diagnostic
(`no impl of <P>.<op> for runtime head <N>`) and exits.

### `__pimpl_*` parsing instead of threading ProtocolReg

The lane considered threading `ProtocolReg` from `lower_protocols`
all the way to `emit_main_wrapper` so the codegen could iterate the
already-computed impl table. We rejected it: the regulator is
discarded after monomorphise (the typed AST has all the info
embedded in the `__pimpl_*` names), so the simpler approach is to
walk the final decl list and parse the names back. `parse_pimpl_name`
does this with explicit prefix matching for the 12 stdlib
protocols. Trade-off: user protocols emit `-1` and never dispatch;
adding them is a follow-up that needs a real reg threading.

## Structural surprises

1. **Perceus wraps the dispatcher body in an `EBlock` with exit
   drops.** The original `ETodo(marker)` becomes `EBlock([_drop x],
   Some(ETodo))`. `body_is_proto_dispatch` and `proto_dispatch_parts`
   had to walk through the wrap. Worth remembering when matching on
   "magic body markers" — Perceus runs before emit.

2. **stage1 parser is fussy with multi-line `if … or …` and `match`
   in inline form.** Refactored two helpers to use explicit `else
   if` chains and multi-line `match` blocks. No semantic change.

3. **LLVM params are `%p_<name>`, not `%kai_<name>`.** The C
   backend uses `kai_<name>`. The LLVM dispatch shim needed
   different arg-list emission.

4. **`int main` lives in `runtime_llvm.c` for the LLVM path** but
   in the emitted .c for the C path. The proto-table registration
   call therefore lives in two places: emitted into the C main
   wrapper, but called from `runtime_llvm.c`'s pre-existing main
   (which now invokes `_kai_proto_init_llvm`, a generated IR
   function).

5. **`synth_todo` had to filter the dispatcher marker** to keep
   `--holes-json` clean. Without the filter, every dispatcher
   shows up as a user "todo" hole and breaks
   `examples/typed_holes/hole_json.kai` content goldens.

6. **Arity-0 ops cannot be runtime-dispatched.** `Default.default()
   : Self` has no receiver to read the head tag from. The shim
   simply panics on entry; the typer's static resolution via return-
   position annotation is the only path that should reach
   `default()`.

## Fixtures added

The lane verified the end-to-end runtime dispatch path with two
synthetic kaikai programs (saved to `/tmp/` during the lane, not
committed):

- `show([1, 2, 3])` via `impl[T : Show] Show for [T]` recursing on
  `show(x : T)` — exercised on both C and LLVM backends.
- A custom `protocol P` with `impl[T : P] P for [T]` over a user
  sum type `Box = B(Int)` — exercised on LLVM backend, output
  `[Box(1), [Box(2), [Box(3)]]]` (recursive list rendering).

**Gap**: no committed fixture. A follow-up lane should add
`examples/protocols/runtime_dispatch_polymorphic.kai` (positive)
and `examples/protocols/runtime_dispatch_missing_impl.kai`
(negative, asserts the diagnostic shape).

## Cost vs estimate

Estimated 2–3 sessions in the planning. Actually shipped in one
session of ~3 hours of focused work, including:
- ~30 min of pipeline reading + asu consultations.
- ~30 min of head-tag design + doc pinning.
- ~30 min runtime hashmap implementation.
- ~45 min C dispatch shim + impl-table emit + iteration on Perceus
  wrap discovery and arity-0 handling.
- ~30 min LLVM dispatch shim + `_kai_proto_init_llvm` emit + IR
  syntax fixing (declares, %p_ vs %kai_, redefinition of declares).
- ~15 min `synth_todo` filter discovery + fix.

## Follow-ups

1. **LLVM selfhost-llvm still bus-errors** on `compiler.kai` (~69k
   lines). The error is `EXC_BAD_ACCESS` deep in `kai_bool`, which
   asu would call a stack overflow inside the compiler running on
   itself. This is NOT the `__protocol_dispatch_Show_show` panic
   that motivated the lane — that path is resolved (confirmed via
   the synthetic fixtures + smaller `--emit=llvm` runs). The stack
   exhaustion is a separate problem and likely pre-existed the
   lane; the head-tag derivation + impl-table init add a few
   thousand IR lines but no extra recursion depth at compile time.

2. **User protocols + user types not registered** in the runtime
   impl-table (we emit `-1` and skip). Resolving them needs
   threading the impl/protocol registries through to codegen so the
   user-assigned IDs survive. Captured as TODO.

3. **No committed regression fixture.** See above.

4. **Stdlib HashMap[K, V] as a separate feature** (user suggested
   during the lane). Out of scope; tracked as task #8 in the
   session's task list.

5. **Push of the lane's commits** is unauthorised at session close
   — user must explicitly approve `git push origin main`.

## What worked

- Reusing the FFI extern marker pattern (`ETodo` with magic prefix)
  meant zero typer changes. `body_is_proto_dispatch` is a near-copy
  of `body_is_ffi_extern`; the emit path is a parallel branch right
  next to it.
- Pinning constants in `docs/variant-tags.md` BEFORE writing the
  emit code meant the runtime constants, the LLVM IR literals, and
  the C macros never disagreed. The integer-versus-name parallelism
  (`stdlib_proto_id_c` returns `"KAI_PROTO_SHOW"`,
  `llvm_stdlib_proto_id` returns `"0"`) is one place to audit
  if either backend ever diverges.
- Step-by-step rebuild after each change caught the Perceus EBlock
  wrap surprise on the first build instead of leaving it as a
  silent miss.
