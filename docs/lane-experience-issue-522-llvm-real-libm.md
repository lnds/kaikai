# Lane experience — issue #522 (LLVM real_* libm bindings)

## Scope

Issue #522 reported that 20 of the 24 `real_*` libm bindings declared
in `stdlib/math/real.kai` / the C-backend `prelude_table` were missing
from the LLVM backend's three parallel tables. Programs touching any
of `real_sqrt`, `real_sin`, `real_cos`, `real_tan`, `real_asin`,
`real_acos`, `real_atan`, `real_atan2`, `real_sinh`, `real_cosh`,
`real_tanh`, `real_exp`, `real_log`, `real_log2`, `real_log10`,
`real_pow`, `real_cbrt`, `real_signum`, `real_is_nan`, or
`real_is_inf` under `--backend=llvm` failed to link with
`use of undefined value '@kai_real_<name>'`.

Mirrors the #513 / #524 pattern: the LLVM backend keeps three
hand-synchronised tables (LP entry, IR-prologue `declare` line, C
forwarder) and the libm block was missed when the C-side prelude
table was extended in #343. The closing PR for #364 (`real_rem` for
`impl Rem for Real`) wired the first entry; the remaining 20 stayed
unwired until this lane.

## Scope-as-planned vs scope-as-shipped

Planned: 23 missing names. Shipped: **20**.

The brief and issue header both count "24 declared, 1 wired ⇒ 23
missing". Counting against the C-backend `prelude_table` libm block
(stage2/compiler.kai:12047-12067) the actual surface is:

- 21 libm-shaped entries (`real_sqrt` through `real_rem`), of which
  `real_rem` was already wired.
- 3 non-libm conversion entries (`real_to_string`, `int_to_real`,
  `real_to_int`) which were already wired in the LLVM table
  separately.

So the gap is exactly the 20 names the issue's *Symptom* section
listed. The "23" in the brief came from subtracting 1 (the wired
`real_rem`) from a "24 declared" header that itself included the
already-wired conversions. The shipped count matches the symptoms.

## Design decisions

- Kept the three sites in the same relative order as `real_to_int` in
  the LLVM table and IR prologue, grouping the libm block immediately
  after the existing `real_to_string` / `int_to_real` / `real_to_int`
  triple (which mirrors the C-backend prelude_table layout). Avoids
  scattering libm entries.
- Cited issue #522 in all three sites' comments, mirroring the
  precedent from #513 (`file_read_bytes`/`file_write_bytes`) and
  #364 (`real_rem`).
- `real_signum`, `real_is_nan`, `real_is_inf` are kept inside the
  libm block even though they're not strictly libm calls — they share
  the IEEE-754 pass-through semantics and the C prelude table groups
  them together.
- Arities verified against the C prelude table: 18 unary + 2 binary
  (`real_pow`, `real_atan2`).

## Structural surprises

None. The lane was as mechanical as expected: the only friction was
the off-by-one in the "23 missing" framing (resolved by trusting the
*Symptom* list of explicit names).

## Fixtures

`examples/stdlib/real_libm_basic.kai` already existed (created when
#343 landed the C-backend bindings). It exercises every name in the
new LLVM surface, including domain-error paths (`asin(2)`, `log(0)`,
`sqrt(-1)`). No new fixture needed.

Before the fix, `KAI_BACKEND=llvm bin/kai run …` aborted at clang
with "use of undefined value '@kai_real_acos'". After the fix, the
fixture passes 37/37 assertions under both backends.

## Coverage gaps

None new. `examples/stdlib/real_libm_basic.kai` already covers the
surface end-to-end; this lane just makes it reachable under
`KAI_BACKEND=llvm`.

## Cost vs estimate

- Estimated: 0.5 day (brief).
- Actual: ~45 minutes including selfhost rebuild + tier0 + tier1.

The three-site edit fit on a single screen per site; no tooling
surprises, no arity ambiguity, no need to touch user-facing surface.

## Follow-ups for next lanes

Companion issue #523 (mailbox bindings) is the next instance of the
same gap-class. The structural recommendation from the Linus audit
on #513 (one-shot collapse of the three tables into a generated
manifest) is still on the table but out of scope here — each gap
lane closes one issue and ships one PR.
