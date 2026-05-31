# Lane experience — fix-default-shim (arity-0 `Default` boxed shim + protocol-impl ABI audit)

## Scope

**As planned.** Extend the PR #732 boxed-shim mechanism (fix-proto-eq-abi)
to the `Default` protocol, and audit every stdlib protocol impl so no
other arity/shape slips through the same ABI gap. Close the bug class
that broke the LLVM smoke test of `scripts/build-release.sh` on the last
three releases (v0.85.0 / v0.85.1 / v0.86.0).

**As shipped.** Same scope. Two-line compiler change + one Makefile gate
+ one restored fixture. The audit confirmed `Default` (arity-0) was the
*only* remaining gap; every other scalar-signature builtin impl was
already covered by #732, and every boxed impl correctly needs no shim.

## The bug

The runtime impl table (`KaiImplEntry[]`, `kai_register_impls`) stores
each protocol impl as a `void *` that the dispatchers
(`kai_op_eq` / `kai_op_lt` / `kai_default_for` / …) cast back to a
uniform boxed shape `%KaiValue* (%KaiValue*, …)`. A builtin impl whose
C signature is *unboxed* (raw scalar params/return, e.g.
`Default_Int_default() : Int` emits `int64_t f(void)` /
`define i64 @…()`) cannot be stored directly: the LLVM emitter bitcasts
it as `%KaiValue* ()*` while its `define` says `i64 ()*`. clang's LLVM
verifier with **non-opaque pointers** (the release/CI toolchain) rejects:

```
@kai_protocols____pimpl_Default_Int_default defined with type
i64 ()* but expected %KaiValue* ()*
```

PR #732 fixed this for scalar-*param* impls (Eq/Ord/Hash/arith) by
emitting a boxed shim `kai_<csym>__boxed` and registering the shim
instead of the raw pimpl. But `pimpl_row_is_unboxed` keyed the decision
on `any_param_scalar(pts)` — at least one **scalar param**. `Default` is
**arity-0** (`default() : Self`), so `pts = []`, `any_param_scalar([]) =
false` → the row was treated as boxed → the raw `i64`-returning pimpl was
registered direct → the bitcast mismatch.

### Why tier1 missed it (four compounding reasons)

1. No LLVM-backend `Default`-on-scalar fixture in tier1. `default_basic.kai`
   exists but `default_polymorphic.kai` (the dynamic-dispatch shape) had
   been **deleted** in commit `9d5a8a8` ("drop default_polymorphic").
2. The **C backend** gets an implicit `(void *)` cast for the same
   registration, so `make tier1` (C-path heavy) never surfaced it.
3. `build-release.sh`'s LLVM smoke uses `hello.kai`, which does **not**
   instantiate `Default` → its impl table has no `Default` rows.
4. **Local clang uses opaque pointers**: `i64 ()*` and `%KaiValue* ()*`
   both become `ptr`, so the bitcast is a no-op and even the *baseline*
   IR compiled RC=0 on the dev machine. Only the release CI's stricter
   (non-opaque) clang rejects it. This is why the local smoke "passed"
   while three real releases failed.

The honest gate is therefore **not** "does local clang accept the IR"
but "does the impl table register every scalar-define impl through a
`__boxed` shim" — an IR-grep assertion independent of clang's pointer
mode.

## The fix (3 files, emit-layer only)

1. **`stage2/compiler/emit_shared.kai`** — `pimpl_row_is_unboxed`:
   ```diff
   - Some(US(pts, _)) -> any_param_scalar(pts)
   + Some(US(pts, rt)) -> any_param_scalar(pts) or ty_is_scalar(rt)
   ```
   A row now needs a shim if it touches a raw scalar at *either* ABI
   boundary — a scalar param OR a scalar return. The return-only branch
   is exactly the arity-0 `Default` case. Shared by both backends, so a
   single edit fixes C and LLVM symmetrically (the row→shim/register
   decision is read in `emit_c.kai` and `emit_llvm.kai`).

2. **`stage2/compiler/emit_c.kai`** — `emit_shim_param_list`:
   ```diff
   - [] -> ""
   + [] -> if i == 0 { "void" } else { "" }
   ```
   An arity-0 shim's C prototype must be `KaiValue *f(void)`, not
   `KaiValue *f()` — the latter is a K&R unspecified-args prototype, not
   a zero-arg one. The LLVM side needed **no** change:
   `llvm_shim_param_list` already returns `""` for arity-0, and LLVM `()`
   *is* zero-arg.

3. **`stage2/Makefile`** — extended `test-proto-scalar-dispatch` with a
   clang-pointer-mode-independent IR assertion: `kaic2 --emit=llvm`
   `default_basic.kai`, then assert each of `Default_{Int,Real,Bool}`
   registers via `__boxed` and **not** raw. Verified adversarially: with
   the fix reverted the target exits 1 ("registered RAW … the bug that
   broke v0.85/0.86 releases"); with the fix it exits 0.

### Mechanism — the raw fast-path is preserved

The raw pimpl `define i64 @…Default_Int_default()` is **unchanged** by
the fix (it is a UFn; `llvm_emit_fn` still emits the native-scalar
define and all monomorphic call sites still `call i64`). The shim is
**additive**: `define %KaiValue* @…__boxed()` calls the raw `i64` entry
and boxes the result with `kaix_int`; only the *table registration*
points at the shim. So the static-monomorphised `let i : Int =
default()` path keeps its zero-cost raw call, while the runtime table
holds an ABI-correct `%KaiValue* ()*`.

### RC

`Default` is arity-0: the shim takes no args, so there is nothing to
incref/decref — it only boxes the return (`kai_int`/`kai_real`/`kai_bool`,
which intern small scalars). No new alloc discipline, no leak. ASAN on
`default_basic` is clean.

## Audit table — all 15 dispatchers × builtin impls

Risk classes for a builtin impl's raw C signature:
- **(a) scalar PARAM** → `any_param_scalar = true` → shimmed since #732.
- **(b) NO scalar param + scalar RETURN** → was the gap; now covered by
  `or ty_is_scalar(rt)`.
- **(c) fully boxed** (param + return are `%KaiValue*`) → no shim, direct
  register is correct.

Empirically classified by `kaic2 --emit=llvm` over every
`examples/protocols/*.kai` plus the full compiler self-IR (76 register
rows), scanning each `register_one_impl` target's `define` return type:
`BUG_COUNT=0`, `DIRECT_SCALAR_COUNT=0`.

| Protocol | Op | Builtin impls | Raw C sig (scalar types) | Class | Shimmed before #732 | Shimmed now |
|----------|-----|---------------|--------------------------|-------|----------------------|-------------|
| Eq | eq(Self,Self):Bool | Int Real Bool Char | `bool f(scalar,scalar)` | (a) | ✓ (#732) | ✓ |
| Ord | cmp/min/max | Int Real Char | `int/scalar f(scalar,scalar)` | (a) | ✓ (#732) | ✓ |
| Hash | hash(Self):Int | Int Real Bool Char | `i64 f(scalar)` (1 scalar param) | (a) | ✓ (#732) | ✓ |
| Add/Sub/Mul/Div/Rem | op(Self,Self):Self | Int Real | `scalar f(scalar,scalar)` | (a) | ✓ (#732) | ✓ |
| Show | show(Self):String | Int Real Bool Char String Unit | `%KaiValue* f(scalar)` — **String return** boxed; param scalar | (a) | ✓ (#732) | ✓ |
| Serialize | to_string(Self):String | Int Real Bool String | scalar param, String return | (a) | ✓ (#732) | ✓ |
| Serialize | from_string(String):Result[_,Self] | Int Real Bool String | **boxed String param, boxed Result return** | (c) | n/a | n/a (no shim, correct) |
| BinSerialize | to_bytes(Self):Array[Byte] | Int Real Bool String | scalar param, boxed return | (a) | ✓ (#732) | ✓ |
| BinSerialize | from_bytes(Array,Int):Result[_,BinCursor] | Int Real Bool String | boxed/scalar params, boxed return | (c)/(a) | covered | ✓ |
| **Default** | **default():Self** | **Int Real Bool** | **`scalar f(void)` — arity-0, scalar return** | **(b)** | **✗ (the gap)** | **✓ (this lane)** |
| Default | default():Self | String Option List | `%KaiValue* f(void)` — boxed return | (c) | n/a | n/a (direct, correct) |

**Key finding:** `Default` on `{Int, Real, Bool}` is the *sole* class-(b)
member among the builtins. `Serialize.from_string` / `BinSerialize.from_bytes`
return `Result[…]` (boxed), so `ty_is_scalar(rt) = false` → they are
class (c) and correctly need no shim. The `or ty_is_scalar(rt)` predicate
covers the gap without over-shimming any boxed impl (the audit shows
String/List/Option/Result/Unit impls all still register direct).

## Fixture

- `examples/protocols/default_polymorphic.kai` — restored from
  `git show 9d5a8a8^:…` (parametric `Default` for `[T]` / `Option[T]`),
  golden `default_polymorphic.out.expected` = `0\n0\nNone\nNone`. Runs in
  both backends via `test-protocols` (which executes every
  `examples/protocols/*.kai` under `KAI_BACKEND=llvm`).
- `examples/protocols/default_basic.kai` (already present) exercises
  scalar `Default` (Int/Real/Bool/String) — its LLVM IR is the artifact
  the new Makefile assertion greps.
- New deterministic gate in `test-proto-scalar-dispatch` (already in
  tier1): asserts `Default_{Int,Real,Bool}` register via `__boxed`, not
  raw — independent of clang's opaque-pointer mode, so it catches the
  regression even where the dev clang would silently accept it.

## Gates (all green at lane close)

- `scripts/build-release.sh /tmp/relfix`: **exit 0** — `c backend OK`,
  **`llvm backend OK`** (lines 178-179, the central gate that failed
  v0.85.0/v0.85.1/v0.86.0), `manifest project OK`, kai.toml LLVM OK,
  "done — release tree".
- `make selfhost`: **`selfhost determinism: OK (kaic2b.c == kaic2c.c)`** —
  byte-identical self-compile.
- `make -C stage2 test-llvm`: **exit 0**.
- `make -C stage2 test-proto-scalar-dispatch`: **exit 0** —
  `proto scalar dispatch OK (C backend)`, `proto scalar dispatch OK
  (LLVM backend)`, `arity-0 Default boxed-shim OK (Int/Real/Bool register
  via __boxed, not raw)`. Adversarially confirmed: with the fix reverted
  the target exits **1** ("Default_Int_default registered RAW …").
- `make tier1` (includes tier0): **exit 0**, 0 FAIL / 0 DIFF; includes
  `test-protocols` (`protocols LLVM OK`, runs `default_polymorphic.kai`
  under `KAI_BACKEND=llvm`) and the new Default assertion.
- ASAN on `default_basic` (C backend, `-fsanitize=address`,
  `detect_leaks=1`): exit 0, output `0 / 0.0 / false / []`, 0 ASAN error
  markers.

## Cost vs estimate

Compiler change is two lines; the bulk of the lane was *diagnosis*:
reproducing a release-only failure on a dev machine whose clang has
opaque pointers (so the bug is invisible to local `bin/kai run`/clang).
The load-bearing realization — local clang permissiveness masks the
mismatch — is why the new gate asserts on the IR text, not on clang's
acceptance.

## Follow-ups

- None blocking. If a future builtin protocol adds an arity-0 op or a
  scalar-return op for a scalar Self, the `or ty_is_scalar(rt)` predicate
  already covers it and the IR-grep gate will catch any miss.
- Consider generalizing the IR-grep assertion into a standalone
  `test-proto-abi` target scanning *all* `register_one_impl` rows for the
  DIRECT-SCALAR anti-pattern, rather than only `Default_{Int,Real,Bool}`
  — would future-proof against the whole class in one check. Deferred:
  the current targeted assertion plus the #732 coverage already span
  every shape the stdlib emits today.

## Name disambiguation (preserved from the placeholder this retro replaced)

The file name `fix-default-shim` collides with two unrelated efforts:

1. **PR #732 (branch `fix-proto-eq-abi`)** shipped the original boxed-shim
   mechanism for *scalar-param* unboxed builtin impls (Eq/Ord/arith). It
   was **not** on a branch named `fix-default-shim`. See the
   `project_kaikai_proto_eq_abi_regression` memory. This lane is the
   arity-0 `Default` extension of that mechanism.
2. **`default_shims_for` / `default_setups_for` (emit_c.kai)** is the
   *effect-handler* default-shim machinery (m7a #7) — installing default
   handlers for builtin effects (Console, Random, …) around `main`. Same
   word "shim", entirely different subsystem; untouched by this lane.
