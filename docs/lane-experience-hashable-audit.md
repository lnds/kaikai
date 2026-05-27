# Lane experience — Hashable audit (closes #373, refs #374)

## Scope as planned vs. as shipped

The brief enumerated five audit findings against the `Hash` protocol,
to be fixed in one PR ahead of the future HashMap (#374):

1. `impl Hash for String` was `string_length(x)` — every same-length
   string collided. Fix: a runtime `string_hash` builtin (C FNV-1a).
2. `impl Hash for Real` was missing.
3. Auto-derive `Hash` for records + sum types ("foundation").
4. `Ok`/`Err` (and `Some`/`None`) variant mixing used additive
   offsets that cross-collided (`Ok(1)` == `Err(0)` == 3).
5. Document the negative-hash contract for HashMap.

**Shipped: 1, 2, 4, 5 as planned. #3 was already done** — derive
`Hash` for records and sum types had shipped with the m12.8 protocol
system and survived the issue-#677 extraction into
`stage2/compiler/protos.kai` (`derive_hash_impl`,
`derive_hash_record_body`, `derive_hash_sum_body`, plus the
`validate_derive_sum_variants` Hash arm). The brief's premise that
derive lacked Hash was stale. We verified it end-to-end (record +
sum, both backends) before declaring it done, then *extended* the
derive path minimally: `Real` was missing from the
`derive_builtin_impl` Hash whitelist, so a record/sum carrying a Real
could not derive Hash even though the new `impl Hash for Real` exists.
One-line fix.

So the riskiest item (derive, which the brief said to defer + consult
asu/linus if it needed a typer-architecture change) cost ~one line,
because the architecture was already in place.

## The builtin wiring — two runtime sites + five compiler sites, ×2 stages

`string_hash` and `real_bits` are prelude builtins. Mirroring
`string_byte_at_int` (the #592 precedent), each touches:

**Runtime (stage0):**
- `runtime.h`: `kai_prelude_string_hash` (full-width 64-bit FNV-1a
  over `s->as.s.bytes`, returned via `kai_int((int64_t) h)` — wraps
  negative, intended) + `kai_prelude_real_bits` (`memcpy` the double's
  8 bytes into a `uint64_t`, return as Int) + their `_thunk` wrappers.
- `runtime_llvm.c`: `kaix_prelude_string_hash` / `kaix_prelude_real_bits`
  thin shims (the LLVM backend needs externally-linkable symbols).

**Compiler (stage2/compiler):**
- `resolve.kai` (~182): added to the builtin-name list (closure
  analyser must not promote them to free vars).
- `infer.kai` (~2981): `TyEntry` for each — `String -> Int`,
  `Real -> Int`.
- `emit_c.kai` (~830): `EP("string_hash", "kai_prelude_string_hash", 1)`
  and the Real entry.
- `emit_llvm.kai`: `declare` lines (~633) + `LP(...)` table (~1173).

**Surprise: the builtin tables are mirrored in *three* stages, not
one.** The brief listed only stage2. But `stage1/compiler.kai` carries
its own prelude-name list (~2877) + `EP` table (~3400), and stage0's
`check.c`/`emit.c` carry yet another. Stage1 compiles only the
compiler bundle — which never calls `string_hash`/`real_bits` — so it
does not strictly *need* the entries. We added them to stage1 anyway
for mirror parity (the documented discipline: the builtin tables are
parallel and should not silently diverge). We left stage0's lists
alone: stage0 is the frozen minimal C bootstrap, it only compiles
`stage1/compiler.kai`, and that file never names the new builtins.

`cache.kai` does **not** enumerate builtins for serdes — it merely
*uses* `string_byte_at_int` as a decode primitive — so no cache change
was needed (verified by grep: every match was a call site, not a
registration).

## The Real approach — bit-cast, not truncation

`int_of_real` truncation would collapse the fraction: `1.5` and `1.9`
both truncate to `1` and would hash identically — unacceptable. We
chose an IEEE-754 bit reinterpret (`real_bits`), which separates every
distinct double. Caveats documented at the impl: `+0.0` and `-0.0`
have different bit patterns, and NaN payloads vary, so the Hash↔Eq
consistency on Real is the caller's concern (a HashMap keyed on Real
is already a smell). A bit-cast needs a builtin because kaikai has no
surface reinterpret operator; the wiring is identical to `string_hash`.

## Variant mixing — multiplicative tag

`Some(x)`/`Ok(t)`/`Err(e)` now mix the variant tag *multiplicatively*
against the FNV-1a multiplier (`tag * 1099511628211 + hash(payload)`)
instead of the old `tag + hash(payload)`. The additive form made
`Ok(1)` (=3) collide with `Err(0)` (=3); the multiplicative form keeps
distinct variants far apart even with equal payloads. We reused the
FNV prime rather than introduce a new constant; stdlib has no `const`
precedent so it is inlined as a literal with an explanatory comment.

## Fixtures

- `examples/protocols/hash_string_distribution.kai` — cat/dog/foo
  (same length) and the tca/act anagram pair hash distinctly; equal
  strings hash equally. Dual-backend via `test-protocols`.
- `examples/protocols/hash_real_and_result.kai` — 1.5≠1.9, 0.0≠1.0,
  1.5==1.5; Ok(1)≠Err(0); Some(0)≠None.
- `examples/protocols/hash_derive_real_field.kai` — `#[derive(Hash)]`
  on a record + sum with Real fields type-checks and hashes
  field-sensitively (exercises the new Real entry in the derive
  validator + the String FNV in a field position).

Fixtures assert **comparisons** (same/diff), not raw digests — FNV-1a
and the bit-cast are byte-deterministic across platforms, but pinning
the exact 64-bit value is brittle and obscures the property under test.

**Regression caught by the gate:** `examples/stdlib/poly_hash_containers.kai`
pinned the *old* additive offsets (`Some(0)==1`, `Ok(0)==2`,
`Err("")==3`) despite a header comment claiming it pinned "only
invariants." The String/variant fixes invalidated those magic
constants. Rewrote it to assert distinctness invariants (the actual
intent), including the historical `Ok(1)`/`Err(0)` collision as a
named guard. The golden is unchanged (still seven `true`).

## Structural surprise — Real-hash exposed latent signed-overflow UB

The brief promised "Int overflow WRAPS, two's complement, verified on
both backends — no masking needed." True at the *result* level, but the
implementation relied on signed `int64_t` arithmetic, which is UB in
C99 on overflow (the hardware wraps, but the standard does not say so).
The codebase compiles every test/demo with `-fsanitize=undefined`
(tier1-asan) and had **no** `-fno-sanitize=signed-integer-overflow` —
so it was UBSan-clean only because nothing overflowed in practice.

`impl Hash for Real` broke that: `real_bits(1.5)` is a ~4.6e18 Int, and
the derive-Hash mix `acc * 31` overflows immediately. UBSan flagged it
in two places:

- the boxed path `kai_op_mul` in `runtime.h`, and
- the **inlined raw path** in `emit_c.kai`'s `emit_kind_raw` EBinop arm,
  which renders unboxed Int arithmetic as a direct C `a * b` (`* 31LL`).

Fixed both to compute Int `+`/`-`/`*` via a `uint64_t` round-trip
(`(int64_t)((uint64_t)a * (uint64_t)b)`) — well-defined modular
arithmetic that produces the byte-identical wrapping result. New helper
`raw_arith_is_wrapping_int(op, ty)` gates the emit_c change on TyInt so
Real arithmetic and Int `/`/`%` keep the plain form. Consulted asu: this
codifies the Int-wraps semantics the docs already promise rather than
expanding scope, and the alternative (a separate `wrapping_mul` surface
or scoped `-fno-sanitize`) either fragments Int or hides the UB at the
other three `kai_op_mul` call sites. Scope held to `+`/`-`/`*`; division
and comparison stay signed.

The emit_c change altered the emitted C (now wrapped), but selfhost
determinism (`kaic2b.c == kaic2c.c`) holds — both self-compilations emit
the same new form. LLVM `mul i64` already wraps (no `nsw`), so the LLVM
backend needed no change; verified clean under UBSan via the .ll path.

## Gate

- `make selfhost`: byte-identical (`kaic2b.c == kaic2c.c`). Hard line —
  we edit the builtin tables the compiler self-compiles through.
- `make tier0`: green (selfhost + demos baseline 34).
- `make tier1`: green after the poly_hash_containers fixup.
- `make tier1-asan`: green — the FNV loop reads `s->as.s.bytes` over
  `s->as.s.len`, ASAN would catch an OOB; clean.
- `make test-protocols`: 78 OK, both C and LLVM backends.

## Cost vs. estimate

Lower than the brief budgeted, because derive (#3, the flagged risk)
was already implemented — the lane was really "wire two builtins +
two stdlib impls + fix one variant-mix + repair one stale fixture."
The one genuine cost overrun was a self-inflicted detour: the first
round of edits landed in the *main* checkout instead of this worktree
(the file context resolved to the non-worktree path); caught when the
rebuilt compiler still emitted the old behavior. Recovered by diffing
main, reverting it clean, and 3-way-applying the patch into the
worktree (stage1 needed a manual re-apply, different base commit).

## Follow-ups for #374 (HashMap)

- HashMap MUST normalise the (possibly negative) full-width hash to a
  bucket: `((h % n) + n) % n`, or `h & (n-1)` for power-of-two `n`.
  Documented at the `impl Hash for String` site and here. This is
  HashMap's contract, not Hash's.
- Real-keyed maps: surface the +0.0/-0.0 and NaN caveats if/when a
  HashMap example keys on Real.
