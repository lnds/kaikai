# Lane experience — issue #514 lane A: fixed-width integers

Numeric lane A for the Orongo (1.0) numeric set: the fixed-width integer
primitives `Int32`, `UInt32`, `UInt64`, and `Int128`. This is the base
of the #514 chain — lanes B (BigInt) and C (Decimal-Int128) build on the
types added here. `#514` stays open; lane D (last in A->B/C->D) closes it.

## Scope as planned vs as shipped

Planned (brief): four kind-distinct unboxed scalar primitives modeled on
`Byte`, full arc each — lexer suffixes, typer/unification, mono, Perceus,
both backends (C `__int128` + native libLLVM i128), Show/Eq/Ord/Hash,
honest FFI marshalling.

Shipped: all four types with the full arc, both backends at parity,
selfhost byte-id green, fixtures wired, `kai info` documented. Two
deliberate scope calls diverged from the brief's framing:

- **Boxed + RC, not raw-unboxed.** The brief described the types as
  "unboxed, no RC — scalars". They ship boxed with RC, like `Byte`
  (see Surprises). Arithmetic correctness is preserved (wrapping in the
  width); the zero-cost-raw representation is a deferred optimisation,
  not a correctness gap.
- **Int128 is NOT an FFI boundary type.** The brief named
  `int32_t/uint32_t/uint64_t` for honest FFI; `__int128` has no standard
  C FFI width, so `Int128` across `extern "C"` is rejected with a clear
  message. Int32/UInt32/UInt64 marshal honestly.

## Design decisions (and alternatives)

Four 1.0-surface decisions were taken with the language architect
(`asu`) up front, verified against the code, not the brief:

1. **Operators `+ - * < ==`, not named prims.** `synth_binop` already
   dispatches on the concrete operand type post-inference (Go-style, not
   type-class resolution), so fixed-width operands stay on the unify-and-
   emit fast path. The `Byte` precedent (named `byte_add`/`byte_eq`) is
   historical, not a design rule; copying it would have failed the money-
   math ergonomics goal.
2. **`EInt` for the value, `EFixed(String, Ty)` for the literal.** The
   first design put the width in `EInt.ty`, but intermediate passes drop
   `.ty`, so `40i32` lost its type by the typer. The fix: a dedicated
   `EFixed` node carrying the digit span + width type, so the width rides
   the node itself and an `Int128` literal above 2^63 survives the
   compiler's own 64-bit `decode_int` (decoded per-backend, `kai_i128_parse`).
3. **`Int32` IS the honest type; legacy `I32`/`U32` FFI annotations keep
   their `Int` mapping.** Rather than break existing FFI code, the new
   value types are a separate `FfiRetFixedVal` / `KFfiFixedVal` kind whose
   shim reads the box's width field; `I32`/`U32` stay the Int-boxed
   annotation. The honest path is to write `Int32` directly.
4. **Suffix `42i32`, peek-1 disambiguation.** No underscore; one char of
   lookahead after `i` distinguishes the complex literal `42i` (non-digit
   follows) from `42i32` (digit follows). LL(1)-pure.

## Structural surprises the brief did not anticipate

- **"Unboxed scalar" desynced Perceus RC.** Marking the types unboxable
  (`ty_is_unboxable_t`) while the native backend still boxed them meant
  Perceus inserted no dup/drop, so `show(x)` consumed the box and `x + 1`
  read freed memory ("type mismatch in +" on native, segfault on `<`).
  `Byte` is NOT unboxable for exactly this reason. The fix was to follow
  `Byte`: box with a tag, ride the RC regime, dispatch arithmetic in the
  shared `kai_op_*` runtime — which also unified the two backends.
- **Bundle hides what selfhost catches.** The concatenated `BUNDLE_SRCS`
  compiles cross-module calls to private helpers fine; `make selfhost`
  (real imports) rejected `is_fixed_width_name` / `decode_int` as private.
  Three helpers needed `pub`. Selfhost also caught a `KVal` exhaustiveness
  gap (`kir_dump_val` missing `KFixedV`) the bundle masked.
- **`lldb` is the right tool for `panic: non-exhaustive match`.** The
  panic names no source location; a breakpoint on `kai_prelude_panic`
  (and the fprintf path) gave the exact walker frame
  (`kai_proto_map_expr_kind`, `kai_arg_has_placeholder`, `kai_nemit_atom`)
  in seconds, versus grepping ~7700 panic sites.
- **A new `ExprKind` (`EFixed`) and a new `KVal` (`KFixedV`) each touch
  ~40 walkers.** Most are leaf no-ops (`-> k`/`-> acc`); the load-bearing
  ones are synth, emit literal, raw arith, KIR lower, cache serialize.

## Fixtures added

`examples/numeric/`, wired to `test-numeric-fixed-width`
(.PHONY + TEST_LIGHT_TARGETS + test-fast):

- `int128_overflow.kai` — Int128 arithmetic past 2^63 (positive case).
- `fixed_width_protocols.kai` — every width + suffix, wrapping, Show/Eq/Ord.
- `ffi_fixed_honest.kai` + `ffi_fixed_shim.c` — honest int32/uint32/uint64 FFI.
- `kind_mismatch.err.kai` — negative: Int32 rejected against Int.

Coverage gap: the FFI fixture's native path is verified ad hoc (libc
`abs(x: Int32)` ran green on native) but the wired target only checks the
C backend with the shim; native FFI parity rides the manual check plus the
shared marshalling code. A native FFI shim-link target is a follow-up.

## Cost

Dominated by two things the brief did not budget: (1) a path-resolution
incident where absolute-bare-root Edit paths landed the bulk of the work
in the MAIN checkout instead of the worktree (recovered by copying the
verified superset back); (2) the RC desync debugging. The mechanical
walker-arm sweep (~80 arms across ~25 files) was the largest pure-volume
chunk.

## Follow-ups for lanes B / C

- **BigInt (lane B)** can reuse `kai_i128_parse` and the `EFixed` textual-
  literal pattern for arbitrary-precision literals; the `KFixedV` /
  `FfiRetFixedVal` kinds are the template for a `BigIntV` carrier.
- **Decimal-Int128 (lane C)** builds on `TyInt128` + its boxed runtime
  (`KAI_INT128`, `kai_int128`, `kai_i128_to_decimal`); a fixed-point
  Decimal with an Int128 carrier lifts the current `stdlib/decimal.kai`
  scale-18 limit.
- **Raw-unboxed fixed-width** (zero-cost arithmetic) is a deferred perf
  optimisation: it needs native KIR slots (SInt32/SInt128) and a
  consistent unboxable story across both backends, which the RC desync
  showed is not free.
- **Native FFI shim-link test target** to verify honest u32/u64 FFI on
  native end-to-end (not just C + ad-hoc libc).
- **Int128 FFI** is currently rejected; a `__int128` boundary type could
  be added if a fintech C API needs it.
