# Where the self-compile's 192M live nodes come from

Measured on `05b040db`, macOS arm64, both backends, self-compile of
`stage2/main.kai` (~138k LOC of compiler + stdlib).

The question this answers: is ~790 live nodes per line **inherent to the
representation** (everything boxed, everything a list) or are **passes
retaining derived structures they no longer need**? The answer is the
first, with one small measured exception.

## 1. The discriminator

(A) and (B) differ in *when* memory is held, not how much. Under (A)
retention tracks the AST and grows smoothly with every pass that builds a
node. Under (B) it steps up at one pass and never comes back down.

The compiler already stops at named points without any behaviour change,
so the curve is directly observable. `KAI_TRACE_RC=1` reports
`alloc_total / free_total / leaked / live_peak` and the counters are
always compiled in, so a stock `make kaic2` binary measures it with the
cell pool **active** — no instrumentation bias.

The cut points used, and what each runs:

| flag | stops after |
|---|---|
| `--tokens` | lexing only (the root file, not the import graph) |
| `--ast` | parse + `expand_imports` over the whole program |
| `--check` | the build arm's full front-end, before monomorphisation |
| `--dump-purity` | mono → closure-spec → cells → unbox → perceus → tcrec |
| *(none)* | codegen + emit |

`--dump-typed` / `--dump-mono` are **not** usable as cut points: they
process only the root file (33 lines here), and their `alloc_total`
matches `--check`'s to within 199 allocations of 173M. Only `--check` and
`--dump-purity` ride the build arm over the whole program.

## 2. The curve — C backend

| stage | alloc | free | live@exit | Δlive | % of total |
|---|---:|---:|---:|---:|---:|
| lex only | 629,826 | 163,876 | 465,950 | 465,950 | 0.2% |
| parse | 79,532,496 | 25,463,282 | 54,069,214 | 53,603,264 | 27.9% |
| front-end | 181,133,949 | 61,609,470 | 119,524,479 | 65,455,265 | 34.1% |
| mono+unbox+perceus+tco | 256,195,906 | 91,416,151 | 164,779,755 | 45,255,276 | 23.6% |
| full build | 325,075,794 | 133,065,527 | 192,010,267 | 27,230,512 | 14.2% |

**There is no step.** Every pass adds between 14% and 34% and none
dominates. Retention rises monotonically with the amount of tree each
pass has built, which is the (A) signature. A (B) staircase would show
one pass contributing most of the total; the largest single contributor
here is the front-end at 34.1%, and that is also the pass that builds the
most structure.

`live_peak == leaked` at every cut (within ~12k), so each number is the
exit state, not a transient.

## 3. The curve — native backend

| stage | alloc | free | live@exit |
|---|---:|---:|---:|
| front-end | 181,253,125 | 61,609,471 | 119,643,654 |
| mono+unbox+perceus+tco | 257,367,387 | 91,906,954 | 165,460,433 |
| full build | 588,669,169 | 366,386,683 | 222,282,486 |

The two backends agree to within **0.4%** at every cut through perceus,
and diverge only at emit: native allocates 331.3M where C allocates
68.9M, frees 274.5M more, and retains 30.3M more (222.3M vs 192.0M).

Split by tag, the divergence is almost entirely `str` — 254.3M native
allocations against 42.8M on C, a **5.9x** ratio, with `variant` and
`cons` differing by 20% and 29%. The native emitter builds its IR
identifiers as strings and releases them; it is churn, not retention.

**This refutes the method note on issue #1902** ("C and native produce
identical counters, digit for digit"). That holds through perceus and
stops holding at emit. It is consistent with what #1952 found on the
fixture corpus: a backend difference in `alloc_total` is an
allocation-count difference, not a retention difference. Reading `leaked`
as a single number would attribute 30.3M of emitter string churn to
retention.

## 4. What is retained, by tag (C, full build)

| tag | allocs | frees | live | retained | share of live |
|---|---:|---:|---:|---:|---:|
| variant | 110,765,915 | 23,968,523 | 86,797,392 | 78.4% | 45.20% |
| cons | 107,079,103 | 37,509,618 | 69,569,485 | 65.0% | 36.23% |
| record | 48,679,485 | 15,181,644 | 33,497,841 | 68.8% | 17.45% |
| str | 42,783,703 | 40,898,316 | 1,885,387 | 4.4% | 0.98% |
| closure | 11,943,838 | 11,751,485 | 192,353 | 1.6% | 0.10% |
| array | 752,665 | 723,018 | 29,647 | 3.9% | 0.02% |
| char | 54,355 | 16,499 | 37,856 | 69.6% | 0.02% |
| int | 3,016,703 | 3,016,424 | 279 | 0.0% | 0.00% |
| real | 27 | 0 | 27 | 100.0% | 0.00% |

`variant + cons + record` is **98.9%** of everything live. Those three
tags are the AST itself: a node, a list cell, a struct.

## 5. The scalar-boxing question, answered

The brief asked how many of the live 48 B nodes hold an unboxable scalar,
since that bounds what a representational fix could ever recover.

**38,162 nodes — 0.0199% of the retention.** `int` retains 279 live nodes
of 3.0M allocated (0.0%), `real` 27, `char` 37,856. Scalars are allocated
in quantity and freed almost perfectly; they are not what is held.

So unboxing scalars recovers nothing here. The 48 B size class that an
earlier `heap(1)` pass measured at 80.1% of live bytes is not boxed
integers — `int` contributes 279 of those nodes. It is `variant` and
`record`, which genuinely use their slots. The ceiling for a
scalar-unboxing change is ~0.02% of retention, and the honest target for
a representational fix is the **`cons` share: 36.2%**, addressable by
replacing list spines with arrays, not by unboxing.

## 6. Scaling: the one superlinear term, and why it does not explain the total

Independent discriminator, in case the curve above were an artefact of
where the cuts fall. N structurally identical 5-line functions in one
file, N from 100 to 6400, C backend, `leaked` with the n=0 base
(2,209,362) subtracted:

| n | user-attributable live | per fn | vs. half |
|---:|---:|---:|---:|
| 100 | 414,514 | 4,145 | — |
| 200 | 838,870 | 4,194 | 2.02x |
| 400 | 1,715,980 | 4,290 | 2.05x |
| 800 | 3,593,759 | 4,492 | 2.09x |
| 1600 | 7,829,674 | 4,894 | 2.18x |
| 3200 | 18,233,661 | 5,698 | 2.33x |
| 6400 | 46,717,010 | 7,300 | 2.56x |

Two readings, and both matter:

**The linear term is the answer to the headline question.** ~4,100 live
nodes per 5-line function is ~820 per line, which reproduces the
self-compile's ~790/line at 1/20th the scale on completely different
code. A per-line cost that is stable across two orders of magnitude of
input is the representation's own cost, not a pass holding something.

**The superlinear term is real but bounded.** The log-log exponent is
1.13 over the full range and 1.29 over the last three points. Site
attribution (`-DKAI_TRACE_RC=1 -DKAI_TRACE_RC_LEAKSITE=1`) puts 50.2% of
the n=6400 front-end retention on a single scope: `bucket_append`
(`infer.kai:19649`), 20,819,193 cons allocations, 99.5% retained.

The mechanism is a quadratic accumulator. `bucket_append` appends one
decl to a per-home bucket with
`list_append(m.decls, [d])`, and `kai_core_list_append` copies the whole
spine of its left argument. Called once per declaration into a bucket
that grows to N, that is N²/2 cons cells. The prediction holds to the
digit: N=6401 predicts 20,483,200 and measures 20,819,193 (+1.6%); N=1601
predicts 1,280,800 and measures 1,391,193 (+8.6%). The second
`list_append(list_reverse(done), …)` pays the same cost on the
already-walked prefix.

**At compiler scale this term is contained by the modular partition.**
The buckets are per home module, and the compiler's 11,770 top-level
decls spread over 286 modules with the largest at 1,156. Summing
Nᵢ(Nᵢ−1)/2 per bucket predicts ~1.6M cells (0.85%) instead of the 69.3M a
single bucket would cost. Measured on the real self-compile,
`bucket_append` ranks 7th at **2.2%** of retention.

That is why extrapolating the synthetic exponent fails: fitting
`4090.7·n + 0.502·n²` to the small series predicts 495M live nodes at
138k LOC when the true figure is 192M. The quadratic is a property of
one-module-with-N-decls, not of program size.

## 7. Attribution on the real self-compile

Top scopes by retained nodes, whole-program C build (9,209 distinct
allocation sites; 3,226 distinct `(scope_fn, tag)` pairs):

| # | alloc | scope | allocs | leak | ret% | share |
|---:|---|---|---:|---:|---:|---:|
| 1 | variant | `inode_mk` | 44,199,282 | 38,478,288 | 87.1% | 20.0% |
| 2 | cons | `list_minus_loop` | 15,079,456 | 15,067,453 | 99.9% | 7.8% |
| 3 | variant | `xp_all` | 6,799,020 | 6,799,020 | 100.0% | 3.5% |
| 4 | variant | `xp_positions_kind` | 8,700,145 | 6,605,092 | 75.9% | 3.4% |
| 5 | cons | `inode_prepend` | 4,525,483 | 4,525,482 | 100.0% | 2.4% |
| 6 | cons | `fns_filter_other_module_pruned` | 4,469,632 | 4,469,404 | 100.0% | 2.3% |
| 7 | cons | `bucket_append` | 5,447,254 | 4,163,404 | 76.4% | 2.2% |
| 8 | cons | `xp_all` | 3,399,510 | 3,399,510 | 100.0% | 1.8% |
| 9 | cons | `ty_env_prepend_batch` | 2,839,529 | 2,839,529 | 100.0% | 1.5% |
| 10 | cons | `ty_env_prepend_batch_loop` | 2,839,529 | 2,839,529 | 100.0% | 1.5% |

The distribution is flat: rank 1 is 20.0%, the top 10 sum to ~46%, and
the tail runs to 3,226 pairs. `inode_mk` builds the persistent AVL behind
`TyEnv`, which is live from resolve through emit by construction — the
type environment is *supposed* to be reachable at exit.

Note the instrumentation is unbiased here in the way that matters: the
`-DKAI_TRACE_RC=1` binary reports `alloc_total=325,075,794`, identical to
the stock binary's, so attribution does not move the totals it explains.

## 8. Verdict

**(A), at approximately 97–98%.**

The evidence:

1. No pass steps. The staircase rises 0.2 / 27.9 / 34.1 / 23.6 / 14.2 %
   across five cuts — the profile of a program that keeps building tree,
   not of a pass that allocates a table and drops it on the floor.
2. Per-line retention is scale-invariant. ~820 nodes/line synthetic
   against ~790 nodes/line on the real compiler, on unrelated code.
3. What is retained is the AST: `variant + cons + record` = 98.9%.
   Derived side tables would show up as `record` or `array` spikes with
   low allocation counts; `array` retains 29,647 nodes total.
4. Retention rates are uniform across tags (65–78% for the three AST
   tags). A leaked derived structure retains ~100% while the AST around
   it retains far less; that separation does not appear.

The (B) component that does exist is measured and small: `bucket_append`'s
quadratic spine copy, **2.2%** of the self-compile — the only site whose
cost is superlinear rather than proportional to the tree. It is a genuine
defect (§9) but it is not the bulk, and fixing it leaves ~97.8% standing.

**The ceiling for a representational change**, per §4/§5:

- Scalar unboxing: **~0.02%**. Not worth doing for memory.
- Replacing list spines with arrays: **up to 36.2%** (the `cons` share).
  A cons cell is 16 B holding one element and one pointer; the same
  elements in an array pay the pointer once.
- The `variant`/`record` share (62.7%) is the AST's actual content. It
  shrinks only by making nodes smaller or by not keeping the whole
  program in memory at once — a `KaiValue` is 48 B (8 B header + five
  8 B slots), so a node with two live slots wastes 24 B.

The last one is worth stating plainly, because it reframes the issue:
`live_peak == leaked` means the compiler holds every AST it has ever
built until exit. Nothing is released because nothing *can* be — the
driver keeps `all_decls`, `typed_prog`, `mono_decls`, `unboxed_decls` and
`perceus_decls` simultaneously reachable in one expression scope. That is
whole-program compilation working as designed, and it is why the number
is large without anything being wrong.

## 9. Defect found while measuring (recorded, not fixed)

`bucket_append` (`stage2/compiler/infer.kai:19649`) is O(N²) in the
number of declarations sharing one home module, from
`list_append(m.decls, [d])` copying the bucket spine per append.

Repro, no build needed:

```sh
python3 - <<'EOF' > /tmp/s6400.kai
for i in range(1, 6401):
    print(f"fn f{i}(x: Int) : Int = {{\n  let a = x + {i}\n  let b = a * 2\n  if b > 10 {{ b - 1 }} else {{ b + 1 }}\n}}")
print('fn main() : Unit / Stdout = {\n  Stdout.print("r=#{f1(1)}")\n}')
EOF
KAI_TRACE_RC=1 stage2/kaic2 --check --path stdlib /tmp/s6400.kai 2>&1 >/dev/null | head -1
```

`leaked=41,221,774` against `leaked=2,874,464` for the same shape at
n=400 — 16x the input, 14x the per-function cost. Halving the file count
per module bounds it; an accumulate-then-reverse rewrite removes it.

Impact today is 2.2% of the self-compile because no compiler module has
more than 1,156 decls. It matters for a single large generated file,
where it is the dominant term.

## 10. What could not be determined

- **Mono separately from perceus.** No cut point exists between them, and
  adding one changes compiler behaviour, which was out of scope. The
  23.6% in row 4 of §2 is `mono + closure-spec + cells + unbox + perceus +
  tcrec` together. Splitting it needs a new dump mode.
- **Whether the native `variant_full_mask` ledger difference #1952 found
  is real release or a counting difference.** This lane measured the
  self-compile, where the C/native gap is `str` churn in the emitter and
  clearly an allocation-count difference (§3). That does not settle the
  fixture case, which has the opposite signature (equal allocs, unequal
  frees). Do not read §3 as evidence either way for it.
- **Which reference holds each live node.** The tooling attributes a
  node's *birth* site, not the binder keeping it alive to exit. §8's
  claim that the driver holds every stage's decls simultaneously is read
  off the source, not measured; an ownership side table would be needed
  to prove it per node.
- **Whether the 62.7% `variant`+`record` share can be reduced at all**
  without changing what the compiler keeps in memory. This lane bounded
  what unboxing and array-backed lists could recover; it did not evaluate
  a streaming or per-module-release architecture.
