# Variant tags — atom-style global convention

This document pins the convention every kaikai stage (0, 1, 2) and the
C runtime in `stage0/runtime.h` must follow when assigning the
`int32_t variant_tag` field of `KaiVariant`. The convention is the
single source of truth; stages 0/1/2 must agree on it byte-for-byte
in their tables, and the runtime hardcoded sites must match.

## Why a global convention

Today the runtime's `kai_variant` / `kai_variant_u` constructors take a
`tag` parameter that the strcmp-based matcher ignores; emit sites mostly
pass `0` (padding). This is wasteful and latently buggy — the nullary
and immortal variant caches in the runtime key off `(tag, name)`, so two
sites that build `kai_variant(0, "Err", …)` and `kai_variant(1, "Err",
…)` (both real today, see audit on 2026-05-22) fragment the cache and
break sharing.

Once the consumer flip (strcmp → `variant_tag == N`) lands, agreement on
the tag value becomes load-bearing: a mismatch is a runtime miscompare.
This convention exists so the flip is safe.

The convention is Erlang-atom-style: tags are **global** across the
program, not per-sum-type. Two constructors with different names always
have different tags. Two constructors with the same name are forbidden
(kaikai does not allow constructor-name shadowing across sum types
inside the same module).

## Reserved range — builtins (tags 0..10)

The reserved range is fixed and must agree across stage0/stage1/stage2
and `stage0/runtime.h`. User variants start at `KAI_USER_VARIANT_TAG_BASE
= 11` and increment in declaration order during resolve.

| Tag | Constructor  | Sum type             | Arity |
| --- | ------------ | -------------------- | ----- |
| 0   | `Some`       | `Option[T]`          | 1     |
| 1   | `None`       | `Option[T]`          | 0     |
| 2   | `Ok`         | `Result[T, E]`       | 1     |
| 3   | `Err`        | `Result[T, E]`       | 1     |
| 4   | `SigInt`     | `Signal`             | 0     |
| 5   | `SigTerm`    | `Signal`             | 0     |
| 6   | `SigHup`     | `Signal`             | 0     |
| 7   | `SigUsr1`    | `Signal`             | 0     |
| 8   | `SigUsr2`    | `Signal`             | 0     |
| 9   | `Exited`     | `ProcessExit`        | 1     |
| 10  | `Signaled`   | `ProcessExit`        | 1     |

`KAI_USER_VARIANT_TAG_BASE = 11`.

The reserved range is intentionally small and ordered by builtin sum
type, with each sum type's constructors contiguous. This keeps the
convention auditable by hand and avoids the temptation to inject
"convenience" tags later (e.g. literal Booleans — `True`/`False` are
not variants in kaikai; they are primitive `Bool`).

## User-variant assignment

When the compiler resolves a `type Foo = A | B(Int) | C` declaration,
it appends `A`, `B`, `C` to the global variants table in that order.
Each one receives the next free tag starting from
`KAI_USER_VARIANT_TAG_BASE` (11 today). Tags monotonically increase as
declarations are processed; tags are stable within a single compilation
of a given source.

Tags are **not** stable across compilations of different programs. A
change in the order of `type` declarations or the addition of a new
constructor shifts subsequent tags. The compiler must therefore emit
the tag literal from the table at the call site, not assume a fixed
value for any user constructor.

Each stage keeps its own variants table; the assignment must be
deterministic in declaration order so that stage1's table and stage2's
table agree on tags for any source they both compile. This is the same
discipline `make selfhost` already protects (per-compiler determinism;
see `docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md`).

## Where the convention is encoded

- **`stage0/runtime.h`**: hardcoded `kai_variant(N, "Name", …)` calls
  must use the reserved tag for builtin names. Audit on 2026-05-22:
  65 sites need migration (38× `Err=0`, 2× `Err=1`, 19× `Ok=0`, 7×
  `None=0`, 6× `Some=0`, plus `Exited`, `Signaled`, signal helpers).
- **`stage0/emit.c`**: `register_builtin_variants` seeds the reserved
  table; `register_user_variants` increments from
  `KAI_USER_VARIANT_TAG_BASE`. The 3 `kai_variant` emit sites consult
  `find_variant_tag` instead of emitting a literal.
- **`stage1/compiler.kai`**: `builtin_variants()` returns the 11
  reserved `EV` entries with their tags baked in;
  `collect_user_variants_loop` threads the next-free counter starting at
  11. Emit sites use `evar_find_tag(variants, name)`.
- **`stage2/compiler.kai`**: same shape as stage1; the EVar AST node
  carries a `tag` field populated at resolve time. `emit_variant_call_*`
  consult `evar_find_tag(variants, name)` instead of emitting `0`.

## Invariants

1. **No two builtin names share a tag.** Specifically `Ok ≠ Err`,
   `Some ≠ None`, `Exited ≠ Signaled`. (Today violated by `Ok=0`
   alongside `Err=0` in runtime.h — that is the proximate bug this
   convention fixes.)
2. **No two user variants share a tag** within a single compilation.
3. **Builtin tag values are stable across kaikai versions within an
   edition.** A change to the table is a breaking change for compiled
   artefacts; treat it as an edition-bump decision.
4. **`KAI_USER_VARIANT_TAG_BASE` is `11`.** If the reserved range is
   extended (e.g. a new builtin), bump this constant and document it
   here.

## Consumer flip — separate lane

This document only pins the **producer side**: every site that
constructs a variant emits the correct tag. The matcher (consumer
side) continues to use strcmp until a follow-up lane flips it to
`variant_tag == N`. Splitting the lanes lets us land the producer fix
under the existing strcmp regime (no semantic change yet) and then
flip consumers on a separate change with its own perf measurement.

## Head-type tags — protocol dispatch key

`variant_tag` identifies a *constructor* (`Some`, `None`, `Ok`, …). Single-
dispatch protocols (`docs/protocols.md`) need a *head type* identifier
(`Option`, `Result`, `Foo`, `Int`, …) so the runtime impl-table lookup
`(proto_id, head_type_tag) → fn_ptr` collapses sibling constructors of the
same sum type to one entry. This section pins the head-type tag
convention used by the runtime impl-table.

### Primitive head tags (reserved 0..15)

| Tag | Head           | KaiTag mapping          |
| --- | -------------- | ----------------------- |
| 0   | (anonymous)    | structural / opaque     |
| 1   | `Unit`         | `KAI_UNIT`              |
| 2   | `Bool`         | `KAI_BOOL`              |
| 3   | `Int`          | `KAI_INT`               |
| 4   | `Real`         | `KAI_REAL`              |
| 5   | `Char`         | `KAI_CHAR`              |
| 6   | `String`       | `KAI_STR`               |
| 7   | `List`         | `KAI_NIL` ∪ `KAI_CONS`  |
| 8   | `Array`        | `KAI_ARRAY`             |
| 9   | `Byte`         | `KAI_BYTE`              |
| 10  | `Closure`      | `KAI_CLOSURE`           |
| 11  | `Fiber`        | `KAI_FIBER`             |
| 12  | `Pid`          | `KAI_PID`               |
| 13  | `Bytes`        | reserved                |
| 14  | reserved       |                         |
| 15  | reserved       |                         |

`HEAD_LIST` covers both `KAI_NIL` and `KAI_CONS` — single-dispatch on
`List` does not distinguish the empty from the cons case (the body
pattern-matches on the value as usual).

`head_type_tag = 0` is reserved for anonymous / structural records.
`impl P for { ... }` is not allowed in v1; the post-mono validator
rejects any impl whose target is anonymous.

### Stdlib nominal heads (reserved 16..19)

The stdlib sum types are pinned alongside the primitives so the runtime
and all three stages agree byte-for-byte:

| Tag | Head          | Constructors          |
| --- | ------------- | --------------------- |
| 16  | `Option`      | `Some`, `None`        |
| 17  | `Result`      | `Ok`, `Err`           |
| 18  | `Signal`      | `SigInt`…`SigUsr2`    |
| 19  | `ProcessExit` | `Exited`, `Signaled`  |

`KAI_USER_HEAD_TAG_BASE = 20`. User-declared sum types and records
receive head tags from 20 upward, assigned in declaration order during
resolve. Like `variant_tag`, user head tags are stable within a single
compilation but not across compilations (declaration-order
dependent).

### Runtime derivation — `kai_head_tag(KaiValue *v)`

The runtime helper maps any value to its head tag:

- **Immediates / boxed primitives** — switch on `v->tag` (the existing
  `KaiTag`) to the constant from the table above.
- **Variants** (`KAI_VARIANT`) — `kai_variant_to_head[v->as.var.variant_tag]`.
  One indirection into a static array emitted by the compiler. Zero
  memory cost in the variant payload itself.
- **Records** (`KAI_RECORD`) — read the new `int32_t head_type_tag`
  field on `as.rec`. Initialised by `kai_record(…)` from the value
  passed at construction.
- **Lists** — `KAI_NIL` and `KAI_CONS` both map to `HEAD_LIST = 7`.

`kai_head_tag` is `static inline` in `runtime.h`; the cost is one
load + small switch, predictable on hot paths.

### Compile-time tables emitted by the compiler

For each compilation unit, the codegen emits:

```c
/* head tag of each variant constructor, indexed by variant_tag */
static const int32_t kai_variant_to_head[] = {
  [0]  = 16,  /* Some -> Option   */
  [1]  = 16,  /* None -> Option   */
  [2]  = 17,  /* Ok   -> Result   */
  [3]  = 17,  /* Err  -> Result   */
  ...
};

/* impl table entries collected during codegen */
static const KaiImplEntry kai_impl_table_entries[] = {
  { .proto_id = 0, .head_tag = 3,  .fn = &__pimpl_Show_Int_show },
  { .proto_id = 0, .head_tag = 16, .fn = &__pimpl_Show_Option_show },
  ...
};
```

`kai_runtime_init` walks `kai_impl_table_entries` and populates the
open-addressing hashmap before user `main` runs.

### Invariants

1. **One head tag per nominal type** within a compilation. Two
   distinct `type Foo = …` declarations never share a head tag.
2. **`kai_variant_to_head[t]` is defined for every `t` in the
   variants table.** Missing entries are a codegen bug.
3. **Primitive head tags are stable across kaikai versions within an
   edition.** Same discipline as variant tags (see §Invariants
   above). User-assigned tags ≥16 are *not* stable across recompilation
   (declaration order shifts them); the impl-table is rebuilt per
   compilation, so this is fine.
4. **`KAI_USER_HEAD_TAG_BASE = 20`.** If a new primitive or stdlib
   head is added (e.g. `Map` becomes a runtime primitive), bump this
   constant and document it here.

### Protocol IDs

`(proto_id, head_type_tag)` is the dispatch key. The stdlib protocols
pin the first 12 IDs in declaration order in `stdlib/protocols.kai`:

| ID  | Protocol       |
| --- | -------------- |
| 0   | `Show`         |
| 1   | `Eq`           |
| 2   | `Ord`          |
| 3   | `Hash`         |
| 4   | `Serialize`    |
| 5   | `BinSerialize` |
| 6   | `Default`      |
| 7   | `Add`          |
| 8   | `Sub`          |
| 9   | `Mul`          |
| 10  | `Div`          |
| 11  | `Rem`          |

`KAI_USER_PROTO_ID_BASE = 12`. User-declared protocols receive IDs
from 12 upward in declaration order. Like head tags, user proto IDs
are stable within a compilation but not across compilations.

### Where the convention is encoded

- **`stage0/runtime.h`** — `kai_head_tag` inline helper; `struct rec`
  carries `int32_t head_type_tag`; `KaiImplEntry` and impl-table
  hashmap.
- **`stage0/emit.c`**, **`stage1/compiler.kai`**, **`stage2/compiler.kai`**
  — each maintains its own head-tag table, threaded through resolve in
  declaration order from `KAI_USER_HEAD_TAG_BASE`. Codegen emits both
  the `kai_variant_to_head[]` array and the impl-table entries at the
  end of the translation unit.

Cross-stage table duplication is accepted as deferred consolidation, on
the same precedent as `variant_tag` (see audit decision 2026-05-22).
