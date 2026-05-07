# Perceus leak attribution — kaic2 self-compile

Diagnostic snapshot produced by Lane DIAG (`perceus-diag-leak-attribution`),
post-#304 baseline. Total leaked chunks: **21,105,881** across 9 tags
and 1,092 distinct `(scope_fn, tag)` pairs.

The ranking maps every leaked chunk to the kaikai source function whose
body executed when the allocation happened. That fn is the natural
candidate for the missing `kai_decref` — either because the fn
constructs a value it forgot to consume, or because it hands the value
to a sibling/callee whose emit shape never decrements it on the dead
edge. Either way, the fix lane (Lane FIX) starts here: pick the top
scope_fn, read its emit shape next to the leaked tag, find the missing
drop.

## How the data is collected

Two pieces of state at `kai_alloc_traced`:

- `kai_current_scope_fn`: a `static const char *` set by the stage 1
  emitter at the top of every generated kaikai function, via a
  generated `kai_set_scope_fn(<name>)` call gated under
  `#ifdef KAI_TRACE_RC_LEAKSITE`.
- the allocator's `tag` parameter, which determines the
  constructor (`kai_variant`, `kai_record`, `kai_cons`, …).

Aggregated keyed on `(scope_fn, tag)` in an open-addressed table
(8,192 buckets; load factor here ≈ 13%). At process exit a `qsort`
ranks the entries by leak count descending and emits the top-N (default
20, override with `KAI_TRACE_RC_LEAKSITE_TOP`).

The instrumentation is fully gated: vanilla builds (without
`-DKAI_TRACE_RC_LEAKSITE`) compile without the new field on `KaiValue`,
without the TLS variable, and without the per-allocation hash lookups.
Selfhost is byte-identical on both backends.

## Top 20 leak sites — kaic2 self-compile (2026-05-06)

| Rank | alloc_fn    | scope_fn (kaikai)         |     allocs |      frees |       leak | leak%  | share% |
|-----:|:------------|:--------------------------|-----------:|-----------:|-----------:|-------:|-------:|
|    1 | kai_str     | prelude_table             |  1,280,880 |          0 |  1,280,880 | 100.0% |  6.1%  |
|    2 | kai_cons    | list_has                  |  1,328,055 |     60,070 |  1,267,985 |  95.5% |  6.0%  |
|    3 | kai_record  | mk_expr                   |  1,287,465 |     39,455 |  1,248,010 |  96.9% |  5.9%  |
|    4 | kai_record  | map_expr_kind             |  1,176,968 |          2 |  1,176,966 | 100.0% |  5.6%  |
|    5 | kai_variant | ok_e                      |    811,056 |      2,542 |    808,514 |  99.7% |  3.8%  |
|    6 | kai_record  | ok_e                      |    778,960 |      2,542 |    776,418 |  99.7% |  3.7%  |
|    7 | kai_variant | map_expr_kind             |    738,665 |     24,119 |    714,546 |  96.7% |  3.4%  |
|    8 | kai_cons    | tok                       |    696,636 |      2,035 |    694,601 |  99.7% |  3.3%  |
|    9 | kai_cons    | map_expr_kind             |  1,355,867 |    706,692 |    649,175 |  47.9% |  3.1%  |
|   10 | kai_variant | prelude_table             |    640,440 |          0 |    640,440 | 100.0% |  3.0%  |
|   11 | kai_cons    | prelude_table             |    640,440 |         59 |    640,381 | 100.0% |  3.0%  |
|   12 | kai_variant | extract_return_tycon      |    629,796 |          0 |    629,796 | 100.0% |  3.0%  |
|   13 | kai_int     | lex_advance               |  1,914,872 |  1,361,847 |    553,025 |  28.9% |  2.6%  |
|   14 | kai_record  | lex_advance               |  1,908,717 |  1,391,640 |    517,077 |  27.1% |  2.4%  |
|   15 | kai_variant | apply_tys                 |    522,629 |     30,712 |    491,917 |  94.1% |  2.3%  |
|   16 | kai_record  | tok                       |    348,318 |          0 |    348,318 | 100.0% |  1.7%  |
|   17 | kai_variant | tok                       |    348,318 |      2,543 |    345,775 |  99.3% |  1.6%  |
|   18 | kai_record  | rewrite_nursery_caps_kind |    336,914 |          0 |    336,914 | 100.0% |  1.6%  |
|   19 | kai_record  | p_advance                 |    374,368 |    105,921 |    268,447 |  71.7% |  1.3%  |
|   20 | kai_variant | list_has                  |    194,487 |        274 |    194,213 |  99.9% |  0.9%  |

The top-20 alone account for **12,729,408 of 21,105,881** leaked chunks
— **60.3% of total leak**.

## Per-tag totals (all sites, full agg)

These numbers are produced by the same atexit dump and match the
`KAI_TRACE_RC` strict per-tag totals chunk-for-chunk — confirming
the side-table is exhaustive (no cap, no dropped chunks):

| tag      |       leak |
|:---------|-----------:|
| variant  |  5,847,644 |
| record   |  6,871,548 |
| cons     |  5,361,788 |
| str      |  1,969,740 |
| int      |    557,880 |
| closure  |    484,477 |
| char     |      4,528 |
| array    |      8,274 |
| real     |          2 |

Summing the top-20 entries by `alloc_fn`:

| alloc_fn    | top-20 sum  | tag total   | top-20 share of tag |
|:------------|------------:|------------:|--------------------:|
| kai_record  |   4,672,150 |   6,871,548 | 68.0% |
| kai_variant |   3,825,201 |   5,847,644 | 65.4% |
| kai_cons    |   3,252,142 |   5,361,788 | 60.7% |
| kai_str     |   1,280,880 |   1,969,740 | 65.0% |
| kai_int     |     553,025 |     557,880 | 99.1% |

In every dominant tag, **two-thirds of the leak concentrates in the
top-20 scope_fns**. Patching even the top-3 sites of each tag should
move the per-tag total visibly.

## Three dominant scope_fns

The top-20 collapses into a small set of repeat offenders. Three
account for the bulk:

### `prelude_table` (rows 1, 10, 11) — 2,561,701 leak (12.1% of total)

A function that builds the prelude environment (strings + variants +
cons cells). Every allocation here leaks unconditionally
(leak% = 100% on all three rows, frees = 0 or 59). The shape is a
build-once / never-decref table — likely treated as a process-lifetime
constant by intent, but the chunks are not flagged as singletons so
they accumulate in the alloc/free counters as leaks. The fix is either
to mark the chunks immortal (`rc = INT32_MAX`, the same trick already
used for `kai_unit` / nullary variants per #300) or to free the table
in a teardown path. Either erases ~12% of the total leak.

### `mk_expr` + `map_expr_kind` (rows 3, 4, 7, 9) — 3,788,697 leak (17.9% of total)

The AST builder and the kind-mapping rewriter for expressions. Both
allocate Records (the AST node payload) and Variants (the kind
enum), and both leak ≥ 95% of what they allocate (one row, `kai_cons`
inside `map_expr_kind`, leaks only 47.9% — i.e., that row's drop
lowering does fire on roughly half the paths but misses the other
half). These two scope_fns sit on the hot AST construction path, so
their emit shape is shared across many syntactic forms — which means
fixing either of them propagates broadly.

### `list_has` + `tok` (rows 2, 8, 16, 17, 20) — 2,851,492 leak (13.5% of total)

`list_has` is the prelude membership test, called from many lex/parse
paths. `tok` is the lexer's token constructor. Both leak Records,
Variants, and Cons cells — the classic "constructor-with-no-decref"
shape. Note `tok` shows three separate rows in the top-20, all with
leak% near 100%; this is the strongest evidence that the lexer's
inner loop is not draining its intermediate values.

Together these three clusters account for **9,201,890 of 21,105,881 = 43.6%
of total leak**. Patching their emit shapes is where Lane FIX should
start; per the lane brief, each batch should aim for ≥ 20% per-tag
delta.

## Caveats

- **Attribution is to allocator, not consumer.** A chunk allocated
  inside `parse_expr` and held by a consumer that never decref's it is
  attributed to `parse_expr`, not to the consumer. This is sometimes
  the right answer (the allocator forgot a temp-scope drop) and
  sometimes a starting point (the allocator legitimately returns the
  chunk; the bug is upstream). Lane FIX must read the actual emit
  shape next to the ranking to decide.
- **Inheritance through unhooked frames.** kaikai functions emit
  `kai_set_scope_fn` at their top; runtime helpers (`kai_internal_dup`
  etc.) do not, so chunks allocated by, e.g., a prelude C helper called
  from `tok` are credited to `tok`. This matches the lane intent —
  the kaikai source shape is what we want to attribute to.
- **Singleton allocs are not filtered.** Some leaks here may be
  intentional immortals (e.g. `prelude_table` strings) that are
  conceptually correct as "never freed". The counter does not yet
  distinguish them from real bugs; the fix lane must inspect each
  scope_fn before patching.

## Reproducing the data

```sh
make -C stage1 kaic1
make -C stage2 kaic2     # produces stage2.c with kai_set_scope_fn calls

cc -std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0 \
   -DKAI_TRACE_RC=1 -DKAI_TRACE_RC_LEAKSITE=1 \
   -Wl,-stack_size,0x8000000 -I stage0 \
   stage2/build/stage2.c -o stage2/build/kaic2-leaksite

KAI_TRACE_RC_LEAKSITE_TOP=20 \
   ./stage2/build/kaic2-leaksite stage2/compiler.kai \
   > /dev/null 2> /tmp/leaksite.log
```

The dump goes to stderr; cat `/tmp/leaksite.log` to read it.
