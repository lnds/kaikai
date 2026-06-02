# Lane retro — enum-as-int slot representation (Koka enum, C backend)

Base: `8b7585b` (post-#742, rb-tree 5.84×C wall / 2.30×C RSS).

## Scope as planned vs. as shipped

**Planned (user ask):** represent sum types whose every constructor is
nullary (`type Color = Red | Black`) as an immediate integer, Koka-style,
rather than a heap `KaiValue*`. A general language improvement — the user
explicitly accepted "even 1% on rb-tree, because it's more general."

**Shipped:** the quirurgical cut asu recommended — **enum-as-int in a
constructor SLOT**, not as a standalone variable. A `Color` stored as a
field of another variant (e.g. `RBNode`'s `slots[0]`) is packed as its
immediate `variant_tag` (new `KAI_VAR_SLOT_ENUM` = kind 3) instead of a
pointer to its interned singleton. Standalone values (`let c = Red`) stay
boxed singletons. This removes one heap dereference per field read — the
`is_red`-shaped guard goes from `slot[0].ptr->variant_tag == 11` (two
derefs) to `slot[0].i64 == 11` (one immediate compare).

## Result (rb-tree N=1M, M4 Pro)

| metric | baseline | enum-as-int |
|---|---|---|
| wall | ~1.36s (5.84×C) | ~1.32s (5.68×C) — **~2.7% faster** |
| `is_red` self-time (sample) | 232 | 202 (−13%) |
| `insert_loop` self-time | 324 | 279 |
| alloc_total / incref / RSS | unchanged | **unchanged** |

The win is pure CPU (one fewer pointer chase per color read), NOT memory:
`Color` was already interned to 0 real allocs (#304 nullary singleton
cache), so enum-as-int changes neither alloc count nor RC traffic. It only
deletes dereferences. That is why the wall moved ~2.7% and the RC trace is
byte-identical.

Gates: selfhost byte-identical (kaic2b.c == kaic2c.c), ASAN clean (smoke +
enum_slot_repr fixture + rb-tree), tier0 green (34/34 demos), new fixture
`enum_slot_repr` wired into tier1 (plain) + tier1-asan.

## The two mechanisms it touches (and why both backends stay sound)

The change is a **codegen decision**, not a `KaiValue` layout change. The
runtime already had the per-slot 2-bit `slot_mask` machinery from the
tagged-Int lane (#741); ENUM is the previously-`reserved` kind 3. Every
slot read/write site that decodes the mask was audited for the new kind:

- **construct** (`emit_variant_slot_init`, `emit_trmc_slot_inits`,
  `rv_new_temps`): `{.i64 = kai_take_enum(<boxed>)}` — extract the tag,
  release the (immortal) box.
- **inline test** (`emit_enum_slot_test`, new): `slot[i].i64 == <tag>` —
  the load-bearing fast path, no deref, no re-box.
- **extract/bind** (`emit_pat_binds_variant`, `emit_arm_borrow_binds`,
  `rv_slot_binds`): `kai_enum_slot_box(slot[i].i64)` re-interns the tag
  into its singleton `KaiValue*`. This is the soundness-critical path asu
  flagged: a forgotten re-intern hands a raw i64 where a `KaiValue*` is
  expected → UAF. The `enum_slot_repr` fixture exercises it (extract →
  pass to another fn → equality → rebuild) under ASAN.
- **RC** (`kai_variant` free, `rv_old_slot_drops`): an enum slot has kind
  != PTR, so the existing "decref only PTR slots" guards skip it for free
  — exactly like Int/Real. The singleton is rc==INT32_MAX anyway.
- **runtime read-back** (`kai_variant_slot_box`, `kai_op_eq`): ENUM reads
  re-intern (box) or compare i64; both added.

**LLVM backend:** unchanged. It always lays variants out with mask==0
(all `KaiValue**`) — it supports neither tagged-Int nor enum-as-int. The
one probe that called `variant_has_primitive_slot` is passed `[]` as the
enum set so it keeps detecting only Int/Real. Each backend is internally
self-consistent (constructs and reads Color uniformly within itself), so
observable output is identical; only the C backend's internal repr changes.
This follows the C-only-optimization precedent tagged-Int (#741) set.

## Structural surprises

- **The classifier lives in emit, not the typer.** `EVar` (the emit-side
  ctor registry) is per-CONSTRUCTOR and does not know which TYPE owns it,
  so "all ctors of this type are nullary" can't be answered from `EVar`
  alone. The decision is `type_is_enum_like(tname, decls)` over the DType
  table, precomputed once into `EmitCtx.enums` (a `[String]` of enum-like
  type names) and threaded to `variant_slot_kind`.
- **Threading `enums` was the bulk of the diff.** `variant_slot_kind` has
  ~18 call-sites in emit_c. Most had `cx` in scope (→ `cx.enums`); 5 pure
  helpers (`slot_read_for_test`, `arm_ptr_binders`, `emit_arm_borrow_binds`,
  `rv_overwrites`, `rv_old_slot_drops`) needed an explicit `enums` param.
  Done surgically per-call, never a global text replace (the #742 retro's
  warning held).
- **The TRMC path has its OWN slot emitter** (`emit_trmc_slot_inits`),
  separate from `emit_variant_slot_init`. The first rb-tree run paniced
  `non-exhaustive match` because TRMC wrote Color as `{.ptr=...}` while the
  test read `slot[0].i64` — a construct/read mismatch. Every slot emitter
  must learn the new kind together, or one path diverges. This is the same
  class of trap as a missing reuse-arm mirror.

## Fixtures added

- `examples/perceus/enum_slot_repr.{kai,out.expected}` — soundness gate:
  two enum types (`Color`, `Dir`), inline guard read, extract-into-binding
  + cross-fn pass (the re-intern path), equality over extracted enums, and
  node rebuild from a flipped enum. Wired into `test-perceus-enum-slot`
  (tier1) and `test-perceus-enum-slot-asan` (tier1-asan).

## Cost vs. estimate

Estimated quirurgical; actual ~matched. The runtime + classifier were
small; the slot-emitter threading and finding the TRMC-specific emitter
were the time sinks. One full prototype cycle (the balance reuse-token
that did NOT move the wall) preceded this lane and informed the
"prototype-or-measure-first" instinct — but the user chose to implement
this one directly on the general-correctness argument, accepting the
modest rb-tree number.

## Follow-ups

1. **LLVM backend enum-as-int** — currently C-only. The LLVM backend would
   need typed-slot support generally (it lacks tagged-Int too). Out of
   scope; tracked with the broader "LLVM typed slots" gap.
2. **Standalone enum unboxing** — `let c = Red` is still a boxed singleton.
   Making enum-typed VARIABLES `int64_t` reopens the mixed-sig net-negative
   (#741 revert territory); deliberately not attempted.
3. **Mixed-type pointer tagging** (`RBTree = RBLeaf | RBNode(...)` → tag the
   nullary `RBLeaf` as an immediate, payload ctors as real pointers) is the
   full-Koka representation. It's a runtime-ABI change touching every
   match/decref/FFI site + stage0/1 rewrite — a separate, much larger lane.
