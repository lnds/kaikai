# Lane experience — issue #990: move c-modular external-linkage to the emitter

## Scope as planned

Issue #990 (surfaced by the #963 Part A consult) flagged that the cross-module
external-linkage decision under `--emit=c-modular` lived in the wrong place: a
fragile textual whitelist in the `bin/kai` wrapper stripped a leading `static `
only when a top-level line matched a closed set of return-type prefixes
(`KaiValue *`, `int64_t`, `double`, `int`, `uint32_t`). `raw_c_type` also emits
`void *` (for `TyHandle`), which is outside that set, so a cross-module def of
that shape stayed file-local `static` and failed the cross-`.o` link with an
undefined symbol. The plan: move the linkage decision into the emitter and
simplify the wrapper to a pure splitter.

## Scope as shipped — the gap was LIVE, not latent

The #963 retro recorded this as "latent, not live" because no *user-level*
`Handle`-returning cross-module generic could be confirmed constructible. That
honesty was right about user surface but understated the impact: the gap is
**live against the compiler itself**. Compiling the stage-2 compiler through
`--emit=c-modular` (kaic2 over `stage2/main.kai`, 84 TUs) compiles every TU but
fails the link with exactly **6 undefined `void *` symbols** — native-emit
context-handle helpers:

```
_kai_emit_native_fn__nemit_call0 / nemit_call1 / nemit_call_boxed / nemit_null /
nslot_type   (emit_native_fn, 5)
_kai_emit_native_term__nemit_declare_clause   (emit_native_term, 1)
```

Each is emitted `static void * kai_...` (a `TyHandle` return lowers to `void *`),
which the whitelist does not strip. Reproduced live before the change.

## asu design consult — verdict and chosen approach

Two shapes were on the table:

- **A — emitter post-pass.** A standalone `String → String` pass run once from
  `emit_program_modular`: drop a column-0 `static ` from every top-level
  function def/prototype, keep `static const` data tables file-local. Delete the
  wrapper whitelist. The single-TU path is untouched, so byte-identity is free.
- **B — thread linkage at the source.** Add a linkage parameter to the ~8
  signature/helper emitters so modular mode emits `""` and single-TU emits
  `"static "`.

**asu ruled A, decisively.** B's "type-driven purity" is illusory — both strip
the same literal `static ` token — while B scatters the policy across ~20 sites
in an F-grade monolith and risks the byte-identity invariant at each one. A
centralizes the policy in the producer, keeps the diff tiny, and removes the
return-type incompleteness by construction (it is exhaustive over return types,
not a whitelist). asu verified the sharpest precondition live: there is no
`static inline` anywhere in `emit_c.kai`, so the shared-header
multiple-definition trap does not apply.

## Soundness — external linkage without the whitelist

The claim "externalize every column-0 non-`const` `static` function symbol" is
sound because:

- **No name clash.** Every emitted top-level name is uniquely mangled (the #748
  fix); measured across the 84-TU compiler stream there are **zero** duplicate
  `static` symbol names, so external linkage cannot collide.
- **No false positives.** A top-level `static` always sits at column 0;
  function-body locals are indented, and emitted C string literals stay on one
  physical line (`\n` escapes, never a real column-0 break). Measured: every
  residual column-0 `static ` line in the stream is either a uniquely-mangled
  function or a `static const ..._tbl[]` data table.
- **Data tables stay file-local.** Excluding `static const` keeps the root-only
  proto tables (`_kai_impl_table_entries`, `_kai_variant_to_head_tbl`, …) and
  the registrar internal, narrowing the blast radius to exactly the
  cross-module-called functions.

The pass is `stage2/compiler/modular_linkage.kai` (`km score` A++, 22 LOC),
`string_split` → per-line rewrite → `string_join`. It is invoked on `decls_h`
(shared prototypes) and each TU body; `kai_types.h` (typedefs only) is left as-is.

## Verification

- **Selfhost byte-identical:** `make -C stage2 selfhost` → `kaic2b.c == kaic2c.c`.
  The post-pass runs only on the c-modular path, so the default single-TU output
  is unchanged.
- **The 6 undefined symbols resolve.** After the change, the compiler's c-modular
  stream split (plain, no whitelist) compiles and the undefined-symbol count for
  the native-handle helpers is **0**.
- **Fixture links + runs at -O2** (below).
- **`test-modular-build`** (all 11 fixtures, now a plain split) green.

## Fixtures added

`examples/multi-module/issue-990-handle-linkage/` — the first runnable user-level
reproduction of the `void *` cross-`.o` leak:

- `handle_box.idh(h: Handle) : Handle` — a cross-module helper whose return
  lowers to `void *` (`static void * kai_handle_box__idh` before the fix).
- `producer.get_handle() : Handle` — a diverging Handle source. No constructible
  Handle value exists at user level, so the body is `panic(...)`. Living in its
  own module is load-bearing: the cross-module prototype carries no `_Noreturn`,
  so the caller's reference survives `-O2` DCE and stays a real cross-`.o`
  reference. (A same-TU `panic`-fed call is eliminated at `-O2` and would not
  guard.)
- `main.kai` calls `handle_box.idh(producer.get_handle())` in a branch never
  taken at runtime; prints `touch = 0`.

Before the fix the modular split fails the link at `-O2` with two undefined
`void *` symbols (`idh`, `get_handle`); after, it links and prints `touch = 0`,
matching the single-TU golden (`main.out.expected`) so default == modular. Wired
into `MODULAR_FIXTURES` (`test-modular-build`, a `TEST_LIGHT_TARGETS` member).

## What was simplified

- `bin/kai` modular split: the whitelist awk (return-type-gated `static`-strip)
  is gone; the wrapper is now a pure file splitter, like the FFI split beside it.
- `stage2/Makefile` `test-modular-build`: same simplification, so CI exercises
  the same plain split the wrapper ships.

## Follow-up discovered — full-compiler modular link blocked downstream (NOT #990)

Fixing the linkage uncovers the *next* blocker on the full-compiler c-modular
build, which is unrelated to #990 and pre-exists it. With the 6 symbols
resolved, the 84-TU link now fails with an ADRP-out-of-range error from a ~7.2 GB
`__bss`: `runtime.h` declares large `static` memory pools
(`kai_slot_pool[9][1048576]`, `kai_var_block_pool[9][1048576]`,
`kai_cell_pool[1048576]`, `kai_rc_history[65536]`, …) — ~88 MB per TU. Being
file-local `static` in a header included by every TU, they get a fresh copy in
each of the 84 translation units (one copy in single-TU, which links fine).
84 × ~88 MB ≈ 7.2 GB overflows the macho image.

This is a runtime-header / modular-path scaling issue (share the pools via
`extern` + a single defining TU), orthogonal to the linkage decision this lane
owns, and out of its one-thing-per-worktree scope. The linkage post-pass does
not touch `runtime.h` or `kai_types.h`, so it neither causes nor worsens this.
Filed as follow-up #1005. Normal-scale modular programs (≤ ~40 TUs, incl. every
`MODULAR_FIXTURES` case and the new fixture) link well under the limit and run
correctly; only the whole-compiler self-build trips it.

Other standing modular debt from #963 (lambda→root dependency inversion;
`.kaii` ABI for Part B separate compilation) is unchanged by this lane.

## Cost vs estimate

Close to estimate. The linkage move itself is ~30 lines (a 22-LOC module + a
3-line call site + two awk simplifications). Most of the time went to (a)
manufacturing a runnable user-level fixture for a compiler-internal type —
`Handle` is not user-constructible, so the trick is a cross-module diverging
producer whose reference survives `-O2` — and (b) diagnosing the downstream
`__bss` blocker enough to attribute it correctly and keep it out of scope.
