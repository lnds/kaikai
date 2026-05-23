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
