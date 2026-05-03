# Phase 0 audit — Anga Roa wave (issues #118 #119 #120)

Status: **audit complete** (2026-05-03). Doc-only lane.

This audit informs Phase 1+ of the **Anga Roa wave** — the
post-Tongariki Perceus optimisation push. Three sibling issues,
all carved out of the retired meta-tracker #77 on 2026-05-02:

- **#118** Perceus: reuse-in-place for known-unique constructors.
- **#119** Perceus: drop specialisation — inline decref chains
  per type. *Previously closed as a doc-only PR on 2026-05-01
  with a negative result; re-evaluation paired with #118.*
- **#120** Perceus: opt-in regions for parser/lexer scratch
  buffers.

Precedent: `docs/unions-phase0-audit.md` (PR #189). Mirrors the
section structure but adapts to the Perceus / runtime domain.

The audit measures: candidate-constructor density, alloc-mix
drift if #118 lands, parser/lexer scratch-buffer pressure,
bootstrap-chain risk, and current RC discipline before any code
ships. It does **not** propose Phase 1's implementation.

---

## §1 — Reuse-in-place candidates inventory

A constructor `T(args)` is reusable in place when:

1. The receiver is **known-unique** (RC == 1 at compile time —
   the existing perceus pass last-use analysis already proves
   this for many fn parameters and let-bindings).
2. The new value has **the same shape** — same KaiTag, same
   arity. For records that means same field count; for
   variants, same arg count (the tag string changes for free
   under the runtime's variant representation).
3. The reuse is **sound**: no aliasing live elsewhere, no
   intermediate-state observers (the tail of a list stays
   immutable while the head cell is being rewritten).

The candidate sites below were located by grepping for
constructor-after-deconsume shapes:

- **List rebuild**: `[h, ...t] -> [..., ...t]` and
  `[h, ...t] -> [g(h), ...recur(t)]` — Koka's textbook
  reuse-in-place pattern.
- **Variant rebuild**: `T(a, b, c) -> T(a', b', c')` — same
  tag, same arity.
- **Record rebuild**: `R { a: x, b: y, c: z } ->
  R { a: x', b: y, c: z }` — same field set.

### A. List rebuild (`Cons` / `KAI_CONS`)

| File | Site count | Notes |
|---|---:|---|
| `stage2/compiler.kai` | **30** | Highest density. Every `apply_*`/`expand_*`/`subst_*` walker rebuilds list spines: `[h, ...t] -> [f(h), ...recur(t)]`. The compiler is its own first hot consumer. |
| `stdlib/core/list.kai` | **5** | `take` (line 37), `map_indexed_loop` (107), `zip_with` (326), `zip` (505), `map` (452). Plus `concat` via `list_append`. The canonical list combinators. |
| `demos/9d9l/huffman/main.kai` | **2** | `bump`, `pq_insert`. |
| `demos/forth/main.kai` | **1** | `TDup` rule rebuilds the stack. |
| `stdlib/crypto/hash.kai` | **1** | `sha_append1`. |
| `stage1/compiler.kai` | **1** | `pcs_rewrite_expr` mirror — stage-1's perceus pass. |

**Total: 40 sites** across the audited tree (`grep -rnE
'\[[a-z_]+,\s*\.\.\.[a-z_]+\]\s*->\s*\['`).

Headline: **75 % of cons-rebuild density lives inside
`stage2/compiler.kai` itself**. The selfhost workload is the
primary beneficiary; stdlib + demos contribute the long tail.

### B. Variant rebuild (`KAI_VARIANT`)

Variant-shape preservation under reuse is more demanding —
the tag string is part of the cell, so a change of variant
(`Some(x) -> None`) cannot reuse. Same-tag rebuilds are the
candidates. Concrete sites:

| File | Pattern | Notes |
|---|---|---|
| `stdlib/collections/map.kai` | `TNode(...) -> tree_node(...)` (8 sites: `tree_rotate_right`, `tree_rotate_left`, `tree_balance`, `tree_insert`, `tree_remove_min`, `tree_remove`, `tree_to_pairs_acc`, `tree_node` constructor wrapper itself) | **AVL tree spine.** Every insert / remove walks the spine and rebuilds `TNode(...)` with one or two children swapped. The variant arity is fixed at 5. Highest-value reuse-in-place target outside the cons spine. |
| `demos/9d9l/huffman/main.kai` | `Node(...) -> Node(...)` (encode/decode) | Tree-shaped but freq is small (one demo, modest input size). |
| `stage2/compiler.kai` | Many `EVar(name)`, `ECall(f, args)` etc. constructors used by `map_*` walkers, but most arity-changing (e.g. wrap → unwrap) | Lower payoff: the compiler's own AST rewrites change `ExprKind` shapes more often than they preserve them. |

For variants, **`stdlib/collections/map.kai`'s AVL tree
operations are the headline**: `tree_balance` produces
`tree_node(nk, nv, l, r)` from a `TNode(nk, nv, _, l, r)` it just
deconstructed — exact-shape, exact-arity, all-fields-known
reuse. The 5 rotation / rebalance fns each fire on every
`map_put` and `map_remove`.

### C. Record rebuild (`KAI_RECORD`)

| File / type | Site count | Notes |
|---|---:|---|
| `stage2/compiler.kai` `Parser` | **182** | Every `parse_*` call returns a fresh `Parser { ... }` record with one field changed (typically `rest` or `had_error`). `Parser` has 6+ fields; 5 of 6 fields are forwarded unchanged on each rebuild. |
| `stage2/compiler.kai` `Lexer`  | **75** | `lex_advance` and 30+ peer fns; same shape — 4 fields, 1 changes per call. |
| `stage2/compiler.kai` `FmtSt`  | **215** | Formatter state; `out`, `cmts`, `indent`, `at_line_start` — every `fmt_push`/`fmt_emit`/`fmt_newline` rebuilds a 4-field record. Highest-frequency record rebuild in the codebase. |
| `stage2/compiler.kai` other typed-AST records | (not enumerated) | `EBlock`, `EHandle`, type-env updates — many one-field-changed rebuilds. |

**These three records (Parser, Lexer, FmtSt) account for
≥472 explicit rebuild sites in stage2 alone.** Each rebuild
allocates a fresh record cell, copies the existing fields by
field, and abandons the previous instance. The previous
instance is provably dead the moment the new record is built
(parser combinators are linear in their state).

### Frequency from baseline

From `docs/perceus-basic.md` baseline (per-tag breakdown of
`kaic2` self-compile, post-m5 #7 + Tier-2 partials):

| tag | allocs | share |
|---|---:|---:|
| `variant` | 18.2 M | 46.7 % |
| `str` | 6.1 M | 15.7 % |
| `record` | 5.6 M | 14.4 % |
| `cons` | 4.6 M | 11.7 % |
| `int` | 3.3 M | 8.5 % |
| (other) | 1.3 M | 3.0 % |
| **total** | 39.0 M | 100 % |

(`unit`, `bool`, `nil`, small-int + char now hit constant pools
and don't allocate.)

If reuse-in-place fires on the cons-spine + AVL-tree paths +
Parser/Lexer/FmtSt — together they cover the 73 % of allocs
that are `variant + record + cons`. **Even partial coverage of
this slice would shift selfhost alloc by tens of millions per
compile**, the "big win on linked-list rewrites" the
Tongariki retro flagged.

### Provability of uniqueness

The existing perceus pass already produces last-use info via
`pcs_collect_uses_*` + `last_use_for(name).LUAt(line, col)`
(see `stage2/compiler.kai:28732` `perceus_pass`). For the
40-site cons-spine inventory and the 472-site
record-spine inventory, the pattern is uniform: **the receiver
of the deconstruction is referenced exactly once on the RHS,
inside a constructor call of the same shape**. This is exactly
the case the existing analysis proves, no escape analysis
needed for v1.

The variant-rebuild candidates (AVL tree) need a slightly
stronger property: the children that are *not* being rotated
through (e.g. `t1`, `t2` in `tree_rotate_right`) flow into the
new `TNode(...)` unchanged — the existing analysis already
treats those as "transferred" reads (last-use, no dup needed),
which is the correct discipline for reuse-in-place's children.

**Headline: the static-info needed for v1 already exists in
the perceus pass.** Phase 1 work for #118 is essentially
recognition + emit-side rewrite + a runtime hook
(`kai_reuse_or_alloc(consumed, ...)` that calls the existing
`kai_check_unique`-equivalent logic — which itself does not
yet exist in `stage0/runtime.h` and must be added).

---

## §2 — Drop specialisation interaction (#119)

The drop-spec lane (PR closed 2026-05-01, retro at
`docs/lane-experience-drop-specialisation.md`) implemented the
optimisation end-to-end and measured **−1.7 % wall at -O2**
and **+5.4 % regression at -O0**. Phase 2 unboxing (PR #38)
had already absorbed the bulk of the dispatch overhead. The
retro pinned the re-evaluation criterion to **post #118**:

> Drop specialisation, fuller scope — could revisit once
> reuse-in-place lands and the alloc mix shifts. Specifically:
> a TyCon → record/variant body-shape lookup would let the
> per-tag dispatch cover the ~61 % of allocs currently in
> KAI_RECORD / KAI_VARIANT, raising coverage from ~38 % to
> ~99 % of alloc events and possibly tipping the wall
> measurement past threshold.

### Will the alloc mix shift?

Reuse-in-place removes **paired alloc + free** events on the
candidate sites; it does not touch other allocs. The expected
shift for `kaic2` self-compile if §1's coverage estimate
holds:

| tag | pre-#118 allocs | post-#118 (estimate) | post-#118 share |
|---|---:|---:|---:|
| `variant` | 18.2 M | ~12 M (AVL tree rebuilds reused) | ~38 % |
| `record` | 5.6 M | ~1 M (Parser/Lexer/FmtSt reused) | ~5 % |
| `cons` | 4.6 M | ~2 M (compiler + stdlib spines) | ~10 % |
| `str` | 6.1 M | unchanged | ~30 % |
| `int` | 3.3 M | unchanged | ~16 % |
| (other) | 1.3 M | unchanged | ~6 % |
| **total** | 39.0 M | **~20 M** (rough) | |

The cell estimates are upper-bound generous (assume reuse
fires on every recognised site). Even halved, the post-#118
mix is **≈ 50 % str + int** — neither of which `kai_decref`
specialisation would touch differently than today (`str` and
`int` are already per-tag inlined post m5 #7's constant pool;
the dispatch overhead was on `record` / `variant` / `closure` /
`array`).

**Net: reuse-in-place shrinks the absolute count of the very
tags drop-spec wanted to specialise.** The 61 % of allocs
that were in `KAI_RECORD + KAI_VARIANT` pre-#118 fall to
roughly 43 % post-#118; the dispatch is fired fewer times
total, even before per-tag dispatch is considered. Drop spec's
v1 already-measured `-O2` win was **−1.7 %**; halving the
dispatch count could move the win toward 0 %, not toward 10 %.

### Tag granularity

Tag granularity (KAI_RECORD vs KAI_VARIANT vs KAI_CONS) was
the right boundary in v1 — but the drop-spec retro flagged
that **TyCon → body-shape lookup** would unlock per-record-name
and per-variant-name dispatch, the wider win. After
reuse-in-place, this lookup would specialise a smaller alloc
slice but still mainly inside the long-tail compiler walkers
(small per-record-name volumes).

### Recommendation for #119

**Keep #119 closed for now.** The pairing argument from the
retro assumed reuse-in-place would shift the mix *toward* more
tag-dispatch-bound allocs; in fact it shifts the mix *away*.
A per-record-name TyCon dispatch could be revisited as a Tier-3
"long tail" optimisation (out of MVP scope), but the headroom
implied by §1's projection is small enough that the lane
should not be re-opened on the strength of #118's mix shift
alone.

If a future bench surfaces dispatch hot spots that #118 did
not flatten (e.g., `closure` decref on long-running fiber
workloads), reconsider then.

---

## §3 — Opt-in regions scope (#120)

Issue #120 wants a `region { ... }` block that allocates inside
a bump arena and frees the entire arena at scope exit. The
candidates are sites that:

1. Have lifetime bounded by parsing a single declaration /
   expression / file.
2. Are created and destroyed many times.
3. Currently use full Perceus RC.

### Parser / Lexer state (the brief's headline)

The audit already counted these in §1.C: **Parser (182 sites),
Lexer (75 sites), FmtSt (215 sites)**.

Lifetime: a `Parser` instance lives from one combinator entry
to the next combinator entry. The "scope" boundary is *one
recursive call frame* — much smaller than "one file". Allocations
inside one `parse_decl` call are reclaimed inside the same call.

Region size: any region that wraps `parse_program` would have
to live for the entire compilation — ~40 K Parser cells per
file (rough proportion of the 5.6 M record allocs). That's
~5–6 MB of bump arena, tractable but no longer "scratch".

A finer-grained region around `parse_decl` would have the
right size (a few hundred records per declaration) but require
threading the region through every combinator's signature, a
large surface change.

**Verdict: the parser is *not* a clean fit for #120's
"opt-in regions" model**. The Parser cells are not "scratch
buffers used briefly and freed in LIFO order" — they form a
linear chain where each new state depends on the previous
state's content. Reuse-in-place (#118) is a better fit because
it keeps the linear-chain semantics intact.

### Lexer-only token list

`fn lex_all(src) : [Token]` produces the whole token list
upfront before parsing. The token list is then walked once by
`parse_program`. Lifetime: one compilation unit. Size: ~one
token per ~5 source bytes on stage2's 40 K-line source ≈
800 K tokens × ~5 fields ≈ ~4 M cells. **This is a real
candidate.** A region wrapping `lex_all` + `parse_program`
would free the entire token list at scope exit without RC
overhead per token.

The savings would be ~4 M decref events per `kaic2`
self-compile, currently fired by recursive cleanup at
`kai_decref(_result)` exit time. Wall-time impact: small,
because today these decrefs happen *once*, on process exit,
not per-iteration. Memory-pressure impact: also small, since
the tokens are only ~10 % of `live_peak`.

### Formatter `out` buffer

`FmtSt.out: [String]` is built in reverse and reversed at the
end. Each `fmt_push` cons's a new chunk. The buffer's lifetime
is one `kai fmt` invocation; size scales with the input file
(~one chunk per source token). Region-wrapping `fmt_format`
would amortise RC.

But: `fmt fmt` is not on the selfhost critical path — it's a
user-invoked tool that runs once per file, not per build. A
region win here would be invisible in `kaic2` self-compile and
modest at user invocation time.

### Other candidate sites

- **Effect handler intermediate state** (`stage2/compiler.kai`
  effects-row inference): builds intermediate `Subst` /
  `Constraint` lists that are discarded once unification
  finishes. ~few hundred K cells per compile. Plausible but
  the codebase already churns through these via cons spines —
  reuse-in-place via #118 wins here too.
- **Test harness scratch** (`stage2/compiler.kai`'s
  `--test-discovery`): negligible volume.

### Headline frequency

Outside the Parser/Lexer state pattern (which §1 already
covered better via reuse-in-place), the **token list** is the
single clean candidate, with ~4 M cells / compile. The total
addressable allocs for #120 in the audited tree is **~5–10 %
of the post-#118 alloc total** — meaningful but not
transformative.

### Recommendation for #120

**Defer #120 until after #118 lands and the post-#118 alloc
mix is measured.** The candidate inventory is real but
narrower than the issue text suggests. The right time to scope
the region surface is after seeing which allocations actually
remain hot post-#118.

If post-#118 selfhost still shows a multi-second wall regression
attributable to RC overhead on the token list, #120 is the
right next lane. If post-#118 wall is back near the
pre-flip 2.15 s baseline, #120 is a Tier-3 long-tail
optimisation, not a 1.0 deliverable.

---

## §4 — Bootstrap-chain risk

### Stage 0 (`stage0/*.c`, `stage0/runtime.h`)

`stage0/runtime.h` (~4.8 K LoC) holds the RC primitives
(`kai_alloc`, `kai_incref`, `kai_decref`, `kai_free_value`,
`kai_internal_dup`, `kai_internal_drop`, `kai_check_unique` —
**not present today**, would be added by #118). The constructor
allocators (`kai_cons`, `kai_record`, `kai_variant`,
`kai_closure`) live here.

For #118: stage 0 must gain a `kai_reuse_or_alloc` family of
helpers (one per shape: cons, record, variant). The
implementation is mechanical:

```c
static KaiValue *kai_reuse_or_alloc_cons(
    KaiValue *consumed, KaiValue *head, KaiValue *tail) {
  if (consumed && consumed->rc == 1 && consumed->tag == KAI_CONS) {
    kai_decref(consumed->as.cons.head);
    kai_decref(consumed->as.cons.tail);
    consumed->as.cons.head = head;
    consumed->as.cons.tail = tail;
    return consumed;  /* rc stays 1 */
  }
  /* fall through: normal alloc path */
  return kai_cons(head, tail);
}
```

`stage0/emit.c` (the C bootstrap compiler emitter) does **not**
need to learn about reuse-in-place — only `stage2/compiler.kai`
does, because reuse-in-place is a stage-2-only optimisation
recognising patterns in user code. Stage 0's emit walks
syntactic AST and emits `kai_cons(...)` regardless.

**Risk: low.** ~50 LoC of new runtime helpers + zero changes
to `stage0/emit.c`.

### Stage 1 (`stage1/compiler.kai`)

Stage 1 is the kaikai-minimal self-host. Its perceus pass
mirror (`stage1/compiler.kai:5083` `pcs_rewrite_expr`) is
**already running** post-m5.x-1-2; it produces `__perceus_dup`
/ `__perceus_drop` wraps the same way stage 2 does.

Stage 1 does **not** need reuse-in-place support because
nothing in stage 1's narrow language surface uses the pattern —
stage 1 is bootstrapping, not optimising. The perceus pass in
stage 1 is there only to keep the runtime's linear-consumption
discipline honest. Adding reuse-in-place would be premature.

**Risk: low — zero stage 1 changes.**

### Stage 2 (`stage2/compiler.kai`)

The perceus pass surface (41 `pcs_*` / `perceus_*` fns,
`stage2/compiler.kai:28732` onwards) is where #118 lands. The
emit-side change is:

- New `pcs_recognise_reuse` walker (after `pcs_rewrite_expr`)
  that detects constructor-after-deconsume patterns and
  rewrites them to a magic `__perceus_reuse(consumed, head,
  tail)` call.
- Magic-name handling in `emit_call_expr` (mirror of the
  existing `__perceus_dup` / `__perceus_drop` interception)
  that lowers to `kai_reuse_or_alloc_<shape>(...)`.

Stage 2's selfhost is the gate. Stage 2 self-compiles through
the perceus pass on ~40 K LoC of compiler source. The cons /
record / variant rebuild patterns named in §1 are *inside*
stage 2's source: when reuse-in-place fires, the kaic2 binary's
own internal allocs change. **This is good — it self-validates
the optimisation** — but the fixed-point check
(`make selfhost`) is the merge gate.

**Risk: medium.** The emit-side change is small (~200 LoC for
the recogniser + magic-name handling). The risk is that the
recogniser fires on a pattern where the existing perceus pass
*already* inserted a `__perceus_drop` for the consumed cell,
causing a double-decref. Phase 1 must order the passes carefully:
reuse recognition runs *before* drop insertion, and the drop
that would have fired on the consumed cell is suppressed
because it's reused.

---

## §5 — Selfhost / RC discipline risk

The runtime debt audit (~2 weeks ago, 2026-04-28; doc
inventoried in user-memory but not materialised on disk) found
the RC discipline was "fictional" pre-flip. The post-flip
state (after PR #202 closed #197/#198/#199) per
`docs/perceus-honesty-targets.md`:

- **leaked = 13.4 M** (was 25.4 M; was 127.2 M pre-m5).
- **alloc_total = 34.3 M** on `kaic2` self-compile.
- **free_total = 20.8 M**.

The tier-2 architectural debt is closed. What remains:

- **`kai_truthy` non-consuming** (intentional; LLVM
  short-circuit phi).
- **`kai_prelude_*` helpers leak params** (~1–2 days mechanical
  audit remaining; tracked in #82).
- **Stage 0 eager-dup retrofit cleanup** (~0.5–1 day
  remaining).

For #118, the **specific RC sites that must work correctly**
are:

| Site | Status |
|---|---|
| `kai_decref` chain-free on `KAI_CONS` / `KAI_RECORD` / `KAI_VARIANT` | ✅ Working (the post-flip discipline). Required — reuse-in-place decrefs the *children* of the consumed cell before overwriting them. |
| `kai_check_unique` predicate (rc == 1) | ❌ Not yet exposed as a primitive. Must be added in #118's runtime delta. The check is one line (`v->rc == 1`); the work is wiring it through the reuse helpers. |
| Last-use analysis correctness for fn parameters | ✅ Working. `pcs_collect_uses_*` + `last_use_for` are the foundation #118 builds on. |
| Last-use analysis for let-bindings | ✅ Working post-Tier-2 (pcs_rewrite_estr_span landed 2026-04-29). |
| Match-scrutinee linear consumption | ✅ Working post-Tier-2 (match-scrutinee real plug landed 2026-04-29). |
| `kai_field_borrow` for record-pattern tests | ✅ Working post-Tier-2. |

**Verdict: the discipline #118 needs is in place.** No
"RC-discipline preflight" lane required.

The two sources of incorrect behavior #118 must guard against:

1. **Non-unique receiver**: if the static analysis claims
   uniqueness incorrectly, reuse-in-place mutates a cell that
   another reference observes. The runtime guard
   (`if (v->rc == 1)`) defends against this by **falling back
   to alloc** when the prediction is wrong. The optimisation
   degrades to baseline gracefully; correctness is unaffected.
2. **Closure capture observing the cell**: if the consumed
   cell is captured by a not-yet-run lambda, `rc` will be
   ≥ 2 and the reuse path will not fire. Same fallback.

Both cases are handled by the rc == 1 guard, which is why this
optimisation is safe even when the static analysis is
imperfect.

---

## §6 — Perf measurement baseline

### Today's `kaic2` self-compile

Pulled from `docs/perceus-honesty-targets.md` (post-Tier-2
perceus, 2026-04-29) and `lane-experience-drop-specialisation.md`
(2026-05-01, post-Phase-2-unboxing):

| metric | value | notes |
|---|---:|---|
| alloc_total | 39.0 M | post-#38 unboxing baseline |
| free_total | 18.1 M | |
| leaked | 20.9 M | + 13.4 M from earlier honesty doc differs by post-#38 mixing; 20.9 M is the post-unboxing measurement |
| wall, `-O0` | 5.18 s | default `make all` build |
| wall, `-O2` | 2.92 s | `bin/kai` driver default |
| max RSS | ~3.0 GB | post-flip; was 6.25 GB pre-flip |

The "~2.0× C-ref under -O2" target from m12 is achieved by
Phase 2 unboxing (PR #38). #118's expected contribution is on
*alloc count*, not on per-op cost.

### Predicted post-#118 deltas

If §1's coverage estimate holds (~50 % of `variant + record +
cons` allocs are reused in place):

- `alloc_total` shrinks from **39.0 M → ~25 M** (−36 %).
- `free_total` shrinks proportionally (every alloc that didn't
  fire also didn't have a paired free).
- `leaked` shrinks because there are simply fewer cells to
  abandon.
- **wall time**: order-of-magnitude estimate −5 % to −15 % at
  `-O2`. The savings are per-call: each reused alloc skips the
  `kai_alloc` malloc-equivalent cost. At 14 M fewer allocs and
  ~50 ns per malloc on Apple Silicon, that's ~700 ms in the
  best case, ~140 ms in a more realistic case where the
  reuse path is itself ~20 ns. Saying "−5 % to −15 %" is
  honest; saying "−25 %" would be optimistic.
- **RSS**: smaller drop, maybe −10 to −20 %, since the peak
  was already reduced by the post-flip discipline.

These are predictions, not commitments. The bench gate should
be **wall-time ≥ 5 % at `-O2` on `kaic2` self-compile**, with
the alloc-count drop verified via `KAI_TRACE_RC`. If the
measured wall is below 5 %, #118 follows the drop-spec lane's
precedent and ships a doc-only PR with a measurement report —
fine, the alloc-count win still appears in the next pass that
amplifies it.

### Out-of-scope baseline

`bench-effects` (the current effect-path bench) is not
expected to move under #118 because effect handlers don't fit
the cons / variant rebuild shape. Demos like `collatz` and
`euler1` (`docs/perceus-basic.md` Phase 4 demos) might gain
visibly because they're list-allocation-heavy in tight loops,
but they're not on the gate.

---

## §7 — Risk summary + recommendation for the Anga Roa wave

### What the audit found

- **40 cons-rebuild sites** + **8+ variant-rebuild sites
  (AVL tree)** + **472+ record-rebuild sites
  (Parser / Lexer / FmtSt)**. The compiler is its own primary
  beneficiary: 75 % of cons-rebuild density and effectively
  100 % of record-rebuild density live in `stage2/compiler.kai`.
- **Coverage projection**: ~50 % of `variant + record + cons`
  allocs (currently 73 % of total alloc_total) are candidates.
  Order-of-magnitude alloc-count drop **−36 %** post-#118.
- **Static info needed already exists.** `pcs_collect_uses_*`
  + `last_use_for` produce the right last-use info; #118's
  recogniser is a new pass that consumes the existing analysis.
- **Runtime delta is small.** ~50 LoC of new
  `kai_reuse_or_alloc_<shape>` helpers; one `kai_check_unique`
  predicate.
- **Bootstrap-chain risk is low.** Stage 0 needs only runtime
  helpers; stage 1 is unchanged; stage 2's emit-side change is
  ~200 LoC for the recogniser + magic-name handling.
- **RC discipline is sound enough.** Post-Tier-2 perceus +
  Phase 2 unboxing have closed the major discipline gaps
  (match scrutinee, kai_field/pat_test, perceus_pass §4b).
  No preflight RC lane needed.
- **Perf prediction**: −5 % to −15 % wall at `-O2` selfhost,
  with confidence band ~5 % per side from drop-spec's
  measurement methodology.

### Recommendation for #118 implementation

**Proceed as planned.** The candidate density is high, the
static-info foundation is in place, the bootstrap risk is
contained to stage 2, and the runtime delta is minimal.

Phase 1 lane should:

1. Land runtime helpers (`kai_check_unique` predicate +
   `kai_reuse_or_alloc_<shape>` family) in `stage0/runtime.h`.
   Selfhost-byte-identical (no emit changes yet).
2. Land the recogniser pass (`pcs_recognise_reuse`) in
   `stage2/compiler.kai`, after `pcs_rewrite_expr` and before
   the existing drop insertion. Correctly suppress the
   would-be-drop on the reused cell.
3. Land the magic-name handling in `emit_call_expr` (mirror of
   `__perceus_dup` / `__perceus_drop`).
4. Bench fixture under `examples/perceus/` showing zero
   allocs on `map(known_unique_list, f)`.
5. Honesty target update in `docs/perceus-honesty-targets.md`.

**Bench gate**: wall ≥ 5 % at `-O2` selfhost. If below,
follow the drop-spec precedent and ship doc-only.

**Risk to flag**: pass-ordering bug — the existing
`pcs_prepend_unused_drops` may insert a drop on the consumed
cell that would double-decref it post-reuse. Phase 1 must
verify the recogniser runs first and explicitly removes the
about-to-be-reused cell from the unused-drop set. The single
test fixture this risk surfaces under is exactly the bench
fixture above — if it shows a leak or use-after-free under
`tier1-asan`, the pass ordering is wrong.

### Recommendation for #119 + #120 sequencing

**Keep #119 closed.** §2 shows reuse-in-place *shrinks*, not
grows, the alloc slice that drop-spec targets. The
re-evaluation criterion the drop-spec retro pinned ("alloc
mix shifts") shifts the wrong direction. A future TyCon →
body-shape lookup at per-record-name granularity remains a
Tier-3 long-tail option.

**Defer #120 until #118 ships and post-#118 selfhost is
measured.** §3 found the "scratch buffer" model fits the
**lexer token list** cleanly (~4 M cells/compile) but **not
the parser / formatter state** (those want reuse-in-place,
not regions). The decision on #120 should follow the
post-#118 measurement: if RC overhead on the remaining
non-rebuildable allocs (token list, intermediate effect-row
state) shows up as a dominant wall contributor, #120 is the
right next lane. If wall is back near the 2.15 s pre-flip
target, #120 becomes a Tier-3 power-user feature.

### Items the integrator may want to file

(Not opened by this audit, per lane discipline.)

1. **`kai_check_unique` runtime predicate** is a small
   prerequisite for #118 but does not have its own issue.
   Could be its own narrow runtime PR or absorbed into #118's
   landing. The integrator decides.
2. **TyCon → body-shape dispatch for drop spec** — flagged in
   the drop-spec retro as the path to ~99 % alloc coverage.
   Would warrant a fresh issue if the dispatch hot spot
   reappears post-#118.
3. **Token-list region** as the cleanest #120 candidate — if
   #120 is re-scoped to "lexer token list region only", the
   surface is much smaller than the current issue text
   suggests (~50 LoC arena alloc + free, no escape brand
   needed because the token list is consumed once and
   discarded inside `parse_program`). Could be filed as a
   narrower #120-prime if the broader region effect proves
   too costly for 1.0.

---

*End of audit.*
