# Lane experience — #1104 Lane 1: donate the arm token across the balance boundary

SPIKE-then-implement lane. Recover the reuse tokens the rb-tree hot path wastes
at the `insert_loop → balance_left/right` call boundary. Plan: `docs/koka-parity-plan.md`
(Lane 1 authoritative).

## Scope as planned vs. as shipped

- **Planned**: pick mechanism (a) Koka-route (pre-Perceus inline of `balance_*` +
  an `Available` token pool) or (b) kaikai-specific hidden reuse-token param, via
  a spike gated on the `reuse_freed` counter; then the full lane with parity +
  self-host gates.
- **Shipped**: mechanism **(b)** — a hidden `KaiReuse _donor` parameter on
  donation-capable functions, threaded as a linear owned resource. New module
  `stage2/compiler/emit_reuse_donate.kai` (246 LOC, `km` A++). Detection is
  **structural, not name-based** (the user chose the general route): any function
  whose body rebuilds a variant through a Perceus reuse-variant arm is capable,
  so the mechanism is not rb-tree-specific — `Result.transpose` and other stdlib
  rebuilders picked it up automatically.

## The spike finding (the deliverable the brief asked for)

**Rejected (a).** Koka's `Available` pool is inseparable from its inliner: the
pool has nothing to thread until inline brings `balance`'s constructors into
`ins`'s scope. kaikai has **no inliner and no simplify pass** (`lex → parse →
resolve → infer → monomorph → perceus → lower`). Replicating the pool without
the inliner is a no-op — an empty pool at the site that matters — and building a
general inliner is a pipeline-wide arch lane whose blast radius (it reorders
every ownership decision Perceus then consumes) is exactly what the brief's "one
bounded lever on the rb-tree shape" excludes. asu confirmed and sharpened it: the
correct precedent to cite is **not** Koka-reuse (which resolves by inline) but
**linear owned resource transfer** (Cyclone/ATS/Rust `fn f(x: Owned<Cell>)`).
"favor precedent (a)" resolves cleanly against (a): its precedent's premise (the
inliner) does not exist here.

**Root cause (read in the emitted C).** `insert_loop`'s Black arm captures
`_arm_ru = kai_drop_reuse_token(_scr, 5)` — a unique 48-byte RBNode shell whose
children already moved out. The `is_red` sub-branch tails into
`balance_left(insert_loop(l,k,v), kx, vx, r)` out-of-line; the token cannot cross
the call, so the arm epilogue frees it (`kai_reuse_free`). Inside balance, the
rotation already gets 2-of-3 cells in place (outer overwrite + inner token
steal); the **third** cell is a fresh `kai_variant_u_fast` — the same 48 bytes as
the token that just died in the caller. Attribution probe: `balance_calls ==
reuse_freed == 423634` at 100K — **100%** of the wasted tokens are that parent
shell dying at the balance call.

**Validated by a manual C patch before touching the compiler.** Adding a `_donor`
param to `balance_*`, moving `_arm_ru` into it at the call site, and donating it
to every rotation fresh-alloc (unique AND shared branch) took the counters to
Koka parity: alloc `5.24 → 1.00` per insert, `reuse_freed 4.24 → 0`, size/height
bit-identical, ASAN clean at 1M. The compiler implementation reproduces those
numbers exactly.

## Results (C backend, N=100K trace / N=1M wall)

| metric | baseline | shipped | gate |
|---|---:|---:|---:|
| variant alloc / insert | 5.24 | **1.00** | ≤1.5 ✓ |
| reuse_freed / insert | 4.24 | **0** | <0.1 ✓ |
| size / height (1M) | 1e6 / 29 | 1e6 / 29 | identical ✓ |
| wall (median-of-7, 1M) | 0.41 s | **0.37 s** | falsifier: <~0.36 ✓ |

**Honest wall caveat.** The counters hit Koka parity exactly (the plan's primary
gate — "counters ARE the gate"). Wall dropped ~10% and passes the falsifier (it
moved below ~0.40), but lands at ~1.4–1.5× Koka (0.37 vs 0.26), the **high edge**
of the plan's 0.28–0.33 prediction, not the centre. On arm64 macOS `malloc`/`free`
is cheap, so roughly half the instruction win hides in the allocator — the plan's
own Lane-3 note warned this class of saving often hides under the memory system.
Closing to 1.1–1.3× Koka is Lane 2's job (fuse the residual RC pairs), not this
lane's; not forcing green, not overselling.

## Design decisions

- **(b) over (a)**: linear-owned-token transfer, no inliner, Perceus untouched.
  A capable fn is compiled **once** with one extra param (no code duplication, no
  new monomorph interaction, no compile-speed hit).
- **Structural capability detection** (user's call): the signal is the Perceus
  marker `__perceus_reuse_variant` / `__perceus_reuse_cons` in an arm tail AND the
  arm deconstructs an INNER cell (a sub-pattern that is itself a PVariant with
  payload — the rotation shape `RBNode(_, RBNode(Red, ..), ..)`). Narrowed to the
  rotation shape on purpose: a FLAT reuse arm (`ElPlain(x)`, a simple cons-reuse)
  reuses only its own shell, has no fresh third cell for a donated token to pay
  for, and its call sites do not all route through `emit_user_call` — marking it
  capable gave the compiler's own `map_elem_exprs` a `_donor` param whose external
  call site passed none (a selfhost arity mismatch). Rotation-only keeps the ABI
  off every simple reuse fn. NOT a bare ctor head, NOT the function name — general,
  zero hardcoding.
- **Donate in BOTH unique and shared branches.** Donating only the unique-branch
  third cell left 1.79 freed/insert (all from balance calls whose returned child
  is shared). The donor (the parent shell) is unique independent of the child's
  sharing, so the shared branch's nested fresh-alloc consumes it soundly.
- **`emit_user_call` is the single funnel**: every call to a capable callee fills
  the `_donor` slot — a live-token call MOVES `_arm_ru` in (`rd_wrap_call`), every
  other call passes `kai_reuse_null` (`rd_join_null_donor`). No tail-position
  analysis needed: the move is always sound (first consumer wins, the rest see
  null and fresh-alloc), the worst case is a missed reuse, never a double-free.

## Soundness contract of the `_donor` param

- **MOVE at the call site**: null the caller's `_arm_ru` copy so the arm epilogue
  free is a no-op. Token is move, never copy.
- **consume-OR-free on every callee return path**: leftover `_donor` freed at the
  capable body's single return (`rd_wrap_body`).
- **size match**: `kai_variant_at` writes only when the token's arity equals the
  ctor's; else it fresh-allocs and the token rides to the exit free — never writes
  an M-slot ctor into an N-slot cell.
- **fiber-local**: the token belongs to the capturing fiber; a capable fn stays
  pure/local (no cross-`spawn` propagation). balance is pure, so N/A today.

## Structural surprises the brief did not anticipate

1. **`string_slice`'s third arg is a LENGTH, not an end index.** My first cut of
   `rd_replace_first`/`rd_splice_*` assumed end-index; every rewrite silently
   no-op'd (the donation emitted param + call-move + leftover-free but never
   consumed a fresh-alloc). Copied the working `replace_first` shape from emit_c
   (uses `find_substring` + `hn - (at + nn)`). This is the single highest-leverage
   trap in the lane.
2. **`-Wunsequenced` in the shared branch.** The manual spike reused ONE
   `_donor_tmp` across two nested fast-allocs in one statement — formal UB (worked
   only because the 2nd saw null). Fixed with a per-site scratch temp (`site` 0 =
   unique, 1 = shared), declared at the branch-block top, NOT inside the
   `(KaiVarSlot[]){...}` initializer where a decl is illegal.
3. **`balance_left` is NOT emitted through the path I first instrumented as a
   direct `EMatch` body.** Post-desugar the body is `EBlock(2 stmts, tail=other)`
   with the match in an `SLet` statement and a `__perceus_drop` in the tail; the
   capability scan had to walk statements, not just the block tail.
4. **The rebuild head is `EVar(__perceus_reuse_variant)`, not `RBNode`.** Perceus
   rewrites the arm's tail ctor into its reuse marker before emit; matching a bare
   uppercase ctor head found nothing.
5. **`EmitCtx` is a 14-constructor record.** Two new fields (`rd_active`,
   `rd_capable`) meant two mechanical sweeps; the "New ExprKind/field sweep trap"
   in memory is real. Did it with scoped `sed` on the ctor tails.
6. **`make KAI_LLVM=1 kaic2` no-ops if the C `kaic2` is already fresh** — the
   timestamp is up-to-date so it does not relink libLLVM. Must `rm stage2/kaic2`
   to force a native rebuild. Cost a confusing "native not built into this kaic2"
   round-trip.
7. **The `_donor` ABI breaks separate compilation — donation is OFF under
   `emit_program_modular`.** The capability is decided from the callee's *body*
   (does it have a reuse-variant marker?), which a cross-module call site cannot
   see. Under separate compilation a capable callee in module B gets a `_donor`
   param while its caller in module A passes none → a link-time arity mismatch
   (`too few arguments to function call`). This surfaced ONLY in selfhost — the
   compiler self-compiles modular, so its own `subst_unit`/`map_elem_exprs` (both
   capable) mismatched their cross-module call sites; the rb-tree bench never hit
   it because it emits single-TU (`emit_program`, one `.c`). Fix: `rd_capable = []`
   in `emit_program_modular` — no fn takes `_donor`, identical to the pre-lane ABI.
   Donation applies only to single-TU compilation, where every call site sees
   every body. Widening it to separate compilation needs the capability encoded
   in the callee's *exported signature*, not its body — a later lane. This is the
   classic ABI-change-under-separate-compilation trap and the single biggest
   design constraint the brief did not anticipate.

## Fixtures added

- `examples/perceus/balance_token_donation_1104.kai` + `.out.expected` (golden
  `size: 100000 / height: 22`). Self-contained (no imports), sequential keys to
  force a rotation per insert.
- `stage2/Makefile` target `test-perceus-1104-balance-token-donation` — native
  backend, gates `alloc_total < 150000` (1.5/insert ceiling; baseline ≈524K) AND
  no `reuse_freed` line (every token consumed). Wired into
  `.github/workflows/tier1-native.yml`. Native on purpose: an RC-boundary change
  SIGSEGVs native even when C is green (#709/#1102).

## Code review (medium effort) — 3 findings, all fixed before close

1. **HIGH, would break modular builds.** The `_donor` ABI was decided at the
   definition/forward/thunk by `rd_body_is_capable(body)` but at the call site by
   `rd_is_capable(csym, cx.rd_capable)`. Under separate compilation `rd_capable=[]`
   silenced the call sites but NOT the signatures — a capable fn with a direct
   caller emitted `kai_f(.., _donor)` in its prototype yet `kai_f(..)` at the call
   → `too few arguments`. The "empty set = no `_donor`" claim was false. **Fix:**
   all four sites now key on `cx.rd_capable` (the single source of truth); the
   modular build of the capable fixture compiles and runs (regression-verified).
2. **LOW/latent.** `rd_wrap_body`'s raw-return path hardcoded `int64_t`, which
   would truncate a `double`/pointer return. **Fix:** `rd_donatable_filter` drops
   any raw-return fn from the capable set — it never takes `_donor`.
3. **LOW/latent leak.** A capable body containing a TRMC step kept the `_donor`
   param but skipped the leftover-free (the wrap a `goto` bypasses). **Fix:** the
   same filter drops TRMC-step fns from the capable set. (The reviewer confirmed
   there is no TRMC use-after-free — the sentinel scan correctly suppresses the
   wrap; only the leak was possible, and only for a mixed capable+TRMC body.)

The reviewer also verified as sound (not reported): the nested-paren `_dn` splice
(last `)` is always the outermost call close), MOVE atomicity across multiple/
non-tail capable calls, the `string_slice` length-arg usage, and the distinct
`_donor_tmp0`/`_donor_tmp1` temps.

## Coverage gaps / follow-ups

- **Donation is single-TU only** (`emit_program`). Under separate compilation
  (`emit_program_modular`) it is off — the `_donor` ABI needs the capability in
  the callee's exported signature, not its body (see surprise #7). A user program
  compiled single-TU (the default for a self-contained bench) gets the win; a
  modular build does not. Widening it is a follow-up lane.
- Wall to 1.1–1.3× Koka needs **Lane 2** (fuse residual RC dup/drop pairs in the
  legacy `pcs_` match protocol); independent of this lane's mechanism.
- The donation is measured on the rb-tree shape. Other capable fns (`transpose`,
  stdlib rebuilders) now take a `_donor` param and pass null at every call — a
  latent win when any of them is ever called from an arm with a live token, but
  unmeasured here.
- Instruction-count delta (callgrind) not re-run; wall + counters were the gate.
