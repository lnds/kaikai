# Lane experience — issue #1136: sized-exact variant representation

Closes #1136 (Eje 2 of `docs/koka-parity-redesign-vision.md`). The lane
set out to give variant constructors monomorphized, exact-sized unboxed
layouts — "RBNode becomes a block of exactly its fields". The
load-bearing discovery: most of that letter had already shipped
incrementally before the issue was filed. The FAM/slab lanes (2026-06)
gave variants `8 B header + n×8` blocks with per-arity pools; #1100
laid Int slots raw; the ENUM slot kind stores colors as immediate tags
on the C backend. An RBNode was already 48 bytes — byte-parity with
Koka's Node — and the C-backend fill loop was already ~1.15× Koka.
What genuinely remained, and what this lane shipped, is (a) true
exactness below the old 40 B floor, and (b) the NATIVE backend's
residual "uniform-format tax": runtime slot-mask lookups + re-boxing
on every match sub-test, an out-of-line generic constructor shim per
construction, and boxed key comparisons.

## Scope as planned vs as shipped

- **Exact block sizing** (shipped): `kai_var_block_size` no longer
  clamps to `sizeof(KaiValue)` — a 0/1/2/3-slot ctor block is
  8/16/24/32 bytes instead of the uniform 40 B floor. RBNode (5
  slots) stays 48 B = Koka's Node. Pools, reuse recognisers and the
  free path already keyed on true arity, so the floor was only wasted
  bytes; the risk surface is hoisted slot loads past a block end,
  guarded since the smaller-sibling fix and watched by the ASAN tier.
- **Static-kind slot tests in the KIR decision tree** (shipped): the
  variant match sub-tests (enum slots, Int-literal slots, nested-ctor
  slots) now read the slot at the ctor's compile-time kind —
  `KProjBorrow` direct `.ptr` for kind-0, raw `.i64` load + inline
  `i==` for kind-1 literals — via the same `slot_kind_of` /
  `bind_slot_op` resolver the binder reads already used (one verdict).
  Before, every test went through the generic `KProj`: a runtime
  mask-table lookup, a kind switch, a re-box (a `kai_alloc` for a
  Real slot) — visible as `is_red` alone eating 23% of the native
  bench. After, `is_red` is small enough that LLVM inlines it into
  the descent loop and it vanishes from the profile. Composes with
  #1149's `var_seal_ctor_of` refactor (rebased mid-lane).
- **Force-inlined native ctor shims** (shipped): the P2-linked
  `kaix_variant_masked` / `_at_masked` / `_at_argv_i64` /
  `_reuse_at_i64` now carry `alwaysinline`; with the constant `n` at
  each call site the copy loop unrolls and SROA dissolves the
  `[n x i64]` buffer into direct stores — the monomorphized-ctor
  shape, without teaching the emitter a second layout encoding.
- **Write-side raw ctor-slot permit** (attempted, NOT shipped): the
  #1110 follow-up. Extending `native_arm_binder_raw_safe` to accept a
  binder stored into a kind-1 Int ctor slot made store-only binders
  lower raw end to end and passed every fixture on both backends —
  and then the native-selfhost gate SIGSEGV'd exactly as the old
  comment warned ("still crashes the native self-compile with no
  reduced repro"). This lane leaves it out and instead contributes
  the missing evidence (below): the crash context, a two-probe
  bisect, and the compare+store mode-loss that separately blocks the
  permit's payoff.

## Measured — rb-tree @1M LCG inserts, M4 Pro, quiet machine

Fill loop (internal timer, best of 5 interleaved runs):

| implementation | fill @1M | vs Koka |
|---|---:|---:|
| Koka 3.2.3 (`rbtree-ck.kk` algorithm, LCG driver) | 0.249 s | 1.00× |
| kaikai-C | 0.276 s | 1.11× |
| kaikai-native (before lane) | 0.471 s | 1.89× |
| kaikai-native (after lane, ship config) | 0.440 s | 1.77× |

Whole process (`/usr/bin/time -l`, single run, machine shared with a
parallel lane build — treat as indicative):

| implementation | wall | peak RSS |
|---|---:|---:|
| Koka | 0.34 s | 48.5 MiB |
| kaikai-C | 0.43 s | 55.2 MiB |
| kaikai-native | 0.59 s | 55.2 MiB |

Bytes/node: RBNode = 48 B before and after (8 B header + 5×8 slots),
already exactly Koka's Node; the floor removal changes nothing for the
rb-tree and shrinks 1/2/3-arity ctors from 40 B to 16/24/32 B
program-wide (Option/Result-shaped sums, small AST nodes).

Counters (the #1104 parity, KAI_TRACE_RC): unchanged — the donation
fixtures still pin `alloc_total=100015` over 100k sequential inserts
(1.00 alloc/insert) and `incref+decref=200000`. Reuse-in-place,
token donation and TRMC all fire exactly as before.

## The honest verdict on the issue's premise

The issue predicted "the remaining gap lives in the representation:
every kaikai heap value is a uniform ~48-byte KaiValue box". That was
stale at filing time: variant blocks had been arity-sized since the
FAM lane and the rb-tree node was already at byte parity. The standing
1.35×-C table conflated three artefacts: driver asymmetry (the kaikai
bench does a size AND a height traversal; the Koka driver only size),
process start-up, and machine load during the original measurement.
Measured clean, the C backend's fill loop is 1.17× Koka — the residue
the vision doc assigns to Axis 3 (the pre-Perceus inliner:
out-of-line `is_red`/`balance_*` with RC at call boundaries), not to
representation. The native residue (~1.8×) is codegen quality, of
which this lane removed the representation-shaped part (mask-dynamic
projections, out-of-line ctors); what remains is dominated by the
still-boxed compare binders (below) and the general native
instruction overhead the two-necks note bounds.

## Structural surprises

1. **The 40 B floor was load-bearing for nobody.** Removing it was a
   two-line change; pools, reuse and frees already keyed on arity.
   The only true dependency was the guard comment's fear itself.
2. **Native was still mask-dynamic on every match sub-test.** The
   #1149 lane fixed WHAT the tests check; this lane fixed HOW they
   read. The `is_red` disassembly showed a runtime mask-registry
   load, a 4-way kind switch and a `kai_alloc` (Real re-box path) for
   a single color compare the C backend does in one load + one cmp.
3. **The write-side permit is not shippable yet — two independent
   blockers, now with evidence.** (a) With the permit (plus an
   existence-scan `kslot` discriminant, needed because perceus
   rebuilds arm-top `__perceus_dup` nodes with the default mode), the
   natively-built compiler SIGSEGVs compiling a hello-world:
   `EXC_BAD_ACCESS` in `perceus__perceus_decl_b`, dereferencing a raw
   i64 (value 4) as a pointer at slot offset 8 — a raw word reaching
   a `.ptr` consumer. A two-probe bisect isolates it: static tests
   reverted + permit on → still crashes; static tests on + permit
   reverted → the full gate passes (COMPILE + LINK + RUN +
   SELF-COMPILE byte-identical). (b) Independently, **a compare+store
   arm binder loses its seeded raw mode between unbox and KIR
   lowering** — the perf half that got away. Repro
   (`variant_raw_rebuild_1136` shape, reduced): `Cell(rest, kx, vx)`
   with `k > kx` in the arm; the seeding accepts kx (debug:
   `int=y free=y natok=y`) yet at `bind_slot_kslot` time the
   post-rewrite body holds 3 `EVar(kx)` reads with 0 carrying
   `MUnboxed`, while the store-only sibling `vx` keeps 2/2 and lowers
   raw. The loss reproduces with the C-liberal seeding too
   (`nat=n`), so it PREDATES this lane's permit — the permit merely
   exposed it (before, such binders were rejected outright, so the
   loss was invisible). Consequence: `insert_loop`'s `k < kx` still
   routes through `kai_op_lt` on native (~8-10% of the bench). The
   fix needs the pass that rebuilds those reads (tcrec/TRMC or the
   guard-walk) to preserve `Expr.mode`; left as the documented
   follow-up with this evidence.
4. **The rc-detector caught a one-verdict violation the local gates
   missed.** `match_shared_tag_subdiscrimination` declares a user type
   named `Exited` — colliding with the runtime-minted builtin ctor.
   Construction resolves the mask by name (first match = the builtin,
   tag < user base → mask 0, boxed slots), but `slot_kind_of` had no
   builtin-tag guard and classified the slot from the user
   declaration (Int → kind 1). The new static literal test then read
   the tagged word `(0<<1)|1 = 1` as a raw i64 and `Exited(0)` matched
   the `Exited(1)` arm. Fixed by mirroring `ls_ctor_mask`'s guard in
   `slot_kind_of`; the lesson is the #1147 rule again — any read-side
   classifier must be evaluated against the same registry entry the
   write side used, INCLUDING its guards, not just the same kind
   function.
5. **Build failures can masquerade as stale results**: `make … |
   tail && echo OK` reports OK on a failed pipe head. Two debug
   cycles were lost to a compiler binary that never contained the
   probe (and one more to a bundle-name collision: a helper named
   `expr_kind_name` duplicated a bundle symbol). Check `$?` of make
   itself, and never name a compiler-source helper generically.

## Fixtures added (wired)

- `examples/perceus/variant_static_match_1136.kai` — every
  static-kind discrimination shape through one match family (enum
  slot shared-tag group, Int-literal slot, nested payload ctor,
  catch-alls), golden on both backends.
- `examples/perceus/variant_raw_rebuild_1136.kai` — the write-side
  permit shape (binder compared AND stored into the rebuild), 200k
  ascending build + one full-depth TRMC descent, value-integrity fold.
- `examples/perceus/variant_poly_frontier_1136.kai` — sized variants
  through generic map / eq / string interpolation (the box/unbox
  frontier).
- Makefile: `test-perceus-1136-sized-variant` (TEST_LIGHT_TARGETS) +
  `test-perceus-1136-sized-variant-native` (tier1-native.yml step).

## Cost vs estimate

The mechanism work (floor, static tests, alwaysinline, permit) was
the planned size. The unplanned majority cost was diagnostic: the
compare+store binder mode-loss chase (three instrumented compiler
rebuilds at ~5 min each, plus two lost to masked build failures), and
re-deriving the honest baseline after discovering the standing table's
numbers were inflated by driver asymmetry and machine load. A
mid-lane rebase over #1149's refactor of the same functions cost one
conflict round.

## Follow-ups (documented, not filed)

1. The write-side raw ctor-slot permit (surprise 3): root-cause the
   raw-word-into-ptr-consumer escape in the compiler self-compile
   before re-landing, and fix the compare+store binder mode-loss that
   caps its payoff. Together they are the single remaining
   representation-shaped native lever on this bench (~8-10%).
   Reduced repro, crash stack and the bisect protocol in this retro;
   the `variant_raw_rebuild_1136` fixture already pins the shape's
   CORRECTNESS on both backends (binders simply stay boxed today).
2. Native full-mask parity: the native i64 mask is Int-only
   (`variant_i64_mask`); enum and Real slots stay physically boxed on
   native while the C backend lays them immediate/raw. Migrating
   native to the full `variant_slot_mask` is a coupled reshape
   (construction + reads + drop walker + registration in one lane).
3. `kai_variant_reuse_at` in runtime.h could carry `alwaysinline`
   like the runtime_llvm.c shims (it still appears as a call in the
   profile, ~2%).
