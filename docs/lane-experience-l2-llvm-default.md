# Lane retro: L2 — LLVM emitter gaps + default flip

**Branch**: `lane-l2-llvm-default`
**Closes**: phase L2 of the LLVM-direct refactor (follow-up to L1 /
PR #495, per `docs/lane-experience-l1-llvm-driver.md`).
**Real cost**: ~1 day end-to-end.

## Scope

L2 has two halves that landed together:

### Part A — close the LLVM emitter gaps

The L1 retro flagged three blockers that forced LLVM users to set
`KAI_NO_STDLIB=1` to compile anything beyond toy programs. All three
were chased to root cause:

1. **`kai_bit_and` and family undefined.** The C backend lowers
   `bit_and / bit_or / bit_xor / bit_not / bit_shl / bit_shr /
   bit_ushr / bit_count / bit_test / bit_set / bit_clear / bit_toggle`
   inline as GNU statement-expressions on `KaiValue->as.i`
   (`emit_call_expr` ~line 12151). The LLVM emitter fell through to
   the default `@kai_<name>` symbol path — and since there is no
   runtime helper by that name (m13 made them intrinsics), `clang`
   refused to link with "undefined value `@kai_bit_and`". Fixed by
   adding `kaix_bit_*` wrappers to `stage0/runtime_llvm.c` mirroring
   the C statement-expression body (read `->as.i`, apply the op,
   `kai_int` to box, decref operands), declaring them at the top of
   the IR module, and routing the recognised intrinsic names through
   `llvm_emit_bit_intrinsic` from `llvm_emit_call`. Twelve symbols,
   one shared dispatcher.
2. **`pattern kind not supported at 1227:7`.** `stdlib/regexp.kai`
   uses `RxParseError { msg: m, pos: p }` — a `PVariantRecord`
   pattern. The LLVM `llvm_emit_pattern_test` only handled
   `PWild / PBind / PLit / PVariant / PList / PHole / PAs` and fell
   into the catch-all that logged the diagnostic and skipped the
   bindings. Fixed by adding three arms: `PVariantRecord` (check the
   variant tag, then bind fields), `PRecord` (just bind fields), and
   `PNarrow` (treat as `PBind`, defensive — the resolver normally
   rewrites it away before codegen). All three share a new
   `llvm_emit_pat_record_fields` helper that walks fields and calls
   `@kaix_field` per name, recursing into sub-patterns through
   `llvm_emit_pattern_test` for nested destructuring.
3. **`cannot build closure for 'p' / 'm'`.** Secondary cascade from
   (2): when the `PVariantRecord` arm silently failed, the field
   names `p` and `m` were never bound as locals; later `EVar`
   references treated them as global functions and routed through
   `llvm_emit_fn_value` → `llvm_closure_error`. Closing (2) eliminates
   the cascade transitively. No code change needed in the closure
   path itself.

### Part B — flip the default backend

Now that the LLVM emitter compiles the full stdlib, the driver
auto-detects clang and prefers LLVM when available:

- New `resolve_backend` helper in `bin/kai` with three-tier
  precedence: `--backend` flag > `$KAI_BACKEND` env > auto-detect
  (`clang` in `PATH` → `llvm`, else `c`).
- `cmd_build` and `cmd_run` parse the CLI flag into `backend_cli`,
  then resolve at the call site. `--holes` mode rejects an explicit
  `--backend` (it runs the typer only) but no longer requires the
  default to be `c`.
- `usage_build`, `usage_run`, and the env-vars section in the top
  banner all document the new default.
- `tools/test-llvm-driver.sh` widened: drops `KAI_NO_STDLIB=1`,
  picks up `02_fizzbuzz.kai` and `bits_basic.kai`, and adds
  precedence fixtures for `KAI_BACKEND=c` / `KAI_BACKEND=llvm`,
  `KAI_BACKEND=bogus`, and `--backend=c` overriding
  `KAI_BACKEND=llvm`. 11 checks total, all green.
- `docs/install.md` is new (the prior docs had no install page);
  documents auto-detect, env knobs, and clang-on-macOS/Linux.
- `README.md` section on backend selection refreshed.

## What was tried, what shipped

The brief originally said L2 = "flip default", but the L1 retro had
already pinned the prerequisite: emitter gaps had to close first or
the flip would break every stdlib-using program. Confirmed
empirically before doing any driver work — `bin/kai build
--backend=llvm examples/minimal/hello.kai` failed on a stock checkout
with all three error families. Course-correcting before writing
driver code saved a wasted PR. The user caught this in-flight and
re-briefed; the corrected scope is what shipped.

The runtime-wrapper approach for `bit_*` was chosen over inlining IR
for two reasons:

- Symmetry with `kaix_add` / `kaix_sub` and the rest of the
  `kaix_*` family. Anyone reading the LLVM emitter sees a uniform
  "call a runtime symbol" pattern.
- Refcount discipline is concentrated in one place (the C wrapper)
  instead of being replicated across twelve IR snippets.

`PVariantRecord` deliberately reuses `llvm_emit_pattern_test`
recursion through `llvm_emit_pat_record_fields` so nested
destructuring (`{ inner: SomeVariant(x) }`) works without further
plumbing. This was cheaper than mirroring the C path's
`emit_pat_test_record_fields` + `emit_pat_binds_record` split, which
runs test-then-bind in two passes; the LLVM side already does
test-and-bind in one walk.

## Structural surprises

1. **The L1 retro was load-bearing.** Reading it carefully — past
   the "wires the flag" headline, into the "out of scope" list —
   surfaced the actual L2 work and avoided a wasted lane. The
   secondary "cannot build closure" cascade also looked like an
   independent bug at first glance; the retro discipline of writing
   down what failed and why is what made the connection obvious.
2. **Bit ops are intrinsics, not stdlib functions.** Searching for
   `kai_bit_and` in any runtime file returned nothing because they
   were never runtime symbols — m13 (the dotted-bit `bit.x` surface)
   registered them in the typer's intrinsic table and lowered them
   inline. Easy to misdiagnose as "stdlib has a missing helper" if
   you don't read the C-emit chain.
3. **`PVariantRecord` is independent of `PRecord`.** Both lower the
   same way for the *field-binding* part, but the variant version
   needs the tag check first. The C path puts the tag check in
   `emit_pat_test_variant_record` and the field walks in a separate
   helper; LLVM consolidates because `llvm_emit_pattern_test` is
   already recursive.

## Fixtures + coverage

- `tools/test-llvm-driver.sh` now exercises 11 driver-side checks
  (was 6). The five parity fixtures include `02_fizzbuzz.kai`
  (closures, interpolation) and `bits_basic.kai` (every bit op),
  with the full stdlib loaded.
- No new compiler unit test for `PVariantRecord` under LLVM yet —
  the regexp / fizzbuzz fixtures exercise it end-to-end through
  the smoke harness, which is what the L1 lane's precedent
  recommended. A targeted `examples/llvm/pat_variant_record.kai`
  fixture would be cheap follow-up if `tools/test-llvm-driver.sh`
  ever gets a tighter loop than parity-diff.

## Real cost vs estimate

| Estimate            | Actual   |
|---------------------|----------|
| Part A (emitter)    | 0.5 day  | ~3 hours |
| Part B (driver)     | 0.5 day  | ~1.5 hours |
| Smoke + docs + retro| 0.5 day  | ~1.5 hours |

Total ~6 hours, under estimate because the inventory step in Part A
exposed all three failures together (and (2) subsumed (3)), so a
single recompile validated each fix.

## Follow-ups left for next lanes

- **`make test-llvm-driver` rule in `stage2/Makefile`** — the L1
  retro noted this would land "whenever L2 starts"; L2 didn't add
  it because the harness already exercises every fixture the rule
  would. Worth adding when the script grows expensive enough to
  warrant a wrapping target.
- **CI gating.** Both halves of L2 are exercised only by the
  smoke harness today, run manually. Tier1 currently does not
  invoke `tools/test-llvm-driver.sh`. Hooking it in is L3 territory
  (alongside the libLLVM link).
- **`kai test --backend=llvm` / `kai bench --backend=llvm`.** Still
  pending from the L1 retro; the auto-detect now means these
  *would* use LLVM by default if the parsers accepted `--backend`.
  Trivial flag-parse mirror of `cmd_build`.
- **Brew formula docs.** `docs/install.md` says clang is opportunistic
  and the formula has no `depends_on "llvm"`; the formula itself
  lives elsewhere and may want a `recommends` note. Out of scope
  here.
- **`docs/lane-experience-l1-llvm-driver.md` "L2 unlocks" sentences**
  could be flipped to past tense by a future doc sweep; deliberately
  not touched here to keep the L2 PR diff focused.
