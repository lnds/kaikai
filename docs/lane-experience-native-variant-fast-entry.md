# Lane experience — native variant fast-entry construction (refs #1083)

## Scope as planned vs as shipped

**Planned (brief):** raw-lower scalar Int arithmetic in the native backend to
match the C backend, closing the `kaix_add`/`kaix_mul`/… call-count and the
1.5× native-vs-C wall gap on the rb-tree.

**Shipped:** the arithmetic was already inlined (commits `b13263f2`,
`095680dc`, pre-Fix A) — the hot path had **zero** `kaix_add/mul/sub/div/eq`.
The 54 residual `kaix_*` arith calls lived in `complex____pimpl_*` /
`protocols____pimpl_*` (stdlib, never executed by the rb-tree). The real
codegen gap was **variant construction**: the native backend built every
payload variant through the cold `kai_variant_u` (per-call name/mask register
+ nullary probe + immortal-args hash scan — 34.8% of instructions per the
runtime comment), while the C backend used `kai_variant_u_fast` +
`kai_nullary_fast`. Two blocks shipped, mirroring emit_c op by op:

- **Block 1 — primitive-slot ctors → fast entry.** A ctor with an Int/Real
  slot (`variant_slot_mask != 0`) routes to `kaix_variant_masked`
  (→ `kai_variant_u_fast`), its tag→name/mask stamped once at startup by
  `nproto_register_payload_ctors`. Slots stay boxed (registered mask 0 —
  see the coupling note). Mirror of `emit_variant_call_typed` +
  `kai_register_payload_ctors`.
- **Block 2 — nullary ctors → seeded singleton.** Red/Black/RBLeaf route to
  `kaix_nullary_fast` (an array load from `kai_enum_by_tag`), seeded once by
  `nproto_seed_nullary`. Mirror of `emit_ident_value`'s `kai_nullary_fast` +
  the nullary enum seed.

## What was mirrored from emit_c

- The **mask helpers** (`variant_slot_kind`, `variant_slot_pos`,
  `variant_slot_mask`, `variant_has_primitive_slot`, `evar_payload_tys`) moved
  from `emit_c` to `emit_shared` — the emit_c comment already said they should
  be shared with the LLVM backend. Single source, no divergence.
- The **routing gate** (`has_primitive` = `mask != 0`) matches emit_c's
  `variant_has_primitive_slot`. All-pointer ctors keep `kaix_variant` because
  the immortal-args interning of `kai_variant_u` is a win the fast path drops
  — exactly why emit_c returns `None` (fallback) for non-primitive ctors.
- The **startup registration** mirrors `_kai_register_proto_tables`: one
  `kaix_register_one_payload_ctor` per primitive-slot ctor, one
  `kaix_seed_nullary` per nullary.

## Structural surprise the brief did not anticipate

The KIR carried **no per-slot type information** — `boxed_inits` /
`lower_ctor_slots` stubbed every slot to `SBoxed`. Threading the mask required
adding an `Int` field to `KCon` (`ls_ctor_mask` computes it from the ctor's
declared payload types via `st.vs`, enums=`[]` like emit_c's
`ctor_int_slots_of`). `KConReuse` was left unchanged — see below.

## The coupling that bounded the lane (why mask is registered as 0)

`kaix_variant_masked` writes boxed slots for now, so the **registered mask
must be 0** — the drop walker reads the startup mask, and a non-zero mask over
a boxed slot would skip that child's RC (a leak) or misread it (a UAF). The
C backend registers the *real* mask because it writes raw i64 slots; native
and C are separate binaries with separate startup, so native stays
self-consistent at mask 0. Writing raw i64 slots (the real i64-inline) is a
**coupled reshape** — construction writes raw + `KProj` reads raw + drop mask,
all atomic or the tree corrupts — the same shape as the #1054/#1069/#1074 UAF
family. Deferred to its own lane with a dedicated ASAN cycle (as #1053 was).

## Call-count before → after (rb-tree `insert_loop`, otool)

| entry | baseline | Block 1+2 | C backend |
|---|---|---|---|
| `kai_variant_u` (cold) | 79 | 51 | 6 |
| `kaix_variant_masked` (fast) | 0 | 24 | — |
| `kai_variant_u_fast` (via masked) | 0 | (inlined) | 9 |
| `kai_variant_at` (reuse fast) | 0 | 0 | 4 |

The residual 51 `kai_variant_u` are the **`KConReuse` rebuilds** — the rb-tree
is reuse-dominated, and the reuse path (`kaix_variant_reuse_at`) was left cold.
The C backend routes those through `kai_variant_at`.

## Wall before → after (fresh kaic2, interleaved, warm, 6 reps)

- Baseline: native 0.71s / C 0.47s = **1.51×**
- Block 1+2: native 0.49s / C 0.38s = **1.29×**

Output byte-identical throughout (`size 1000000 height 29`). Pure codegen.

## Gates

selfhost byte-id (`kaic2b.c == kaic2c.c`), tier0, RC ASAN detector (both
backends, all reuse fixtures), inline-gate (extended to assert
`kaix_variant_masked > 0`). The C backend's emitted output is unchanged by the
`emit_c`→`emit_shared` move (verified: rb-tree C binary byte-identical).

## Follow-ups left for next lanes (refs #1083)

1. **`KConReuse` fast-entry** — route the variant reuse rebuild through
   `kaix_variant_at_masked` (mask 0) instead of `kaix_variant_reuse_at`,
   mirroring the C backend's `kai_variant_at`. This is the bulk of the
   remaining wall gap (51 cold `kai_variant_u` in the reuse-dominated rb-tree).
   Moderate risk — touches the reuse-in-place move semantics.
2. **i64-inline slots** — write Int/Real slots as raw words with the real
   mask, and route `KProj` reads to `kaix_variant_arg_i64`/`_f64`. The coupled
   reshape above; its own lane + ASAN cycle.
