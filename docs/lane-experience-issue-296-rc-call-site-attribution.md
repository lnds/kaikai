# Lane experience — issue #296 (per-call-site leak attribution)

## Objective metrics

- Branch: `issue-296-rc-call-site-attribution`.
- Started: 2026-05-06T15:00 (approx).
- Ended:   2026-05-06T16:10.
- Lane wall: ~70 minutes (well under the 2-3 h calibrated budget).
- Build outcomes:

  | timestamp                  | cmd                  | outcome |
  |----------------------------|----------------------|---------|
  | 2026-05-06T15:35           | tier0 vanilla        | OK      |
  | 2026-05-06T15:44           | selfhost (-DKAI_TRACE_RC=1) | OK |
  | 2026-05-06T16:04           | tier0 after fixture  | OK      |
  | 2026-05-06T16:09           | tier1 after fixture  | OK      |

  Tier1-ASAN green (separate run before the fixture target was added;
  the fixture target only adds work to `make -C stage2 test`, which
  ASAN does not traverse).

- Selfhost byte-identical (vanilla and `-DKAI_TRACE_RC=1`).
- Selfhost-llvm byte-identical (vanilla).

## Implementation

Three additions inside `stage0/runtime.h`, all gated behind
`#ifdef KAI_TRACE_RC`:

1. **Per-chunk site stamp.** `struct KaiValue` gains a `void
   *alloc_site` field under the flag (vanilla layout unchanged).
   `kai_alloc` writes it, `kai_free_value` reads it, and the
   matching site's `frees` counter is incremented before the
   chunk is poisoned.

2. **Histogram hash table.** `KaiRcSite kai_rc_sites[16384]` —
   open-addressed, linear probing, keyed by `void *`. Saturation
   sets a sticky flag and the report appends `TABLE SATURATED`.
   Empirically kaic2 selfhost emits ~3 K distinct sites, ~18 %
   load factor.

3. **Address capture.** `kai_alloc(tag)` is redefined as a macro
   that expands to `kai_alloc_traced(tag,
   __builtin_return_address(0))`, evaluated **inside** the
   wrapper that called it. Wrappers (`kai_int`, `kai_record`,
   `kai_variant`, `kai_fiber_value`, `kai_pid_value`,
   `kai_array_make`, `kai_closure`,
   `kai_string_concat{,_all_impl,_join_impl}`,
   `kai_str{,_from_bytes}`, `kai_cons`, `kai_real`, `kai_char`,
   `kai_prelude_read_file`) are marked `KAI_RC_NOINLINE`
   (`__attribute__((noinline))` under the flag, empty in vanilla
   builds). This guarantees `__builtin_return_address(0)` points
   at the user's emit site, not the wrapper's parent.

The exit dump (`kai_rc_site_report`) sorts the compacted hash
table descending by leak count, prints the top N (default 20;
`KAI_TRACE_RC_TOP=N` overrides), and emits an `aslr_slide=…`
line so post-mortem tools can recover static addresses.

`tools/symbolize-rc-trace.sh <binary> <trace.log>` rewrites every
`site` line in place using `nm <binary>` for nearest-symbol
resolution. Resolution is function-level — `atos` may yield line
numbers if a dSYM is present and the toolchain cooperates.

## Top-20 sites in kaic2 self-compile (THE HEADLINE)

```
[KAI_TRACE_RC] top sites by leak (showing 20 of 3039 distinct sites; total_leak=78437784)
[KAI_TRACE_RC] aslr_slide=0x28bc000
 1  0x102a35c78 (kai_synth_dim_pow + 0x1e4)              variant   alloc=27,173,839  leak=27,173,839  leak%=100.0  share%=34.6
 2  0x102a35b70 (kai_synth_dim_pow + 0x0dc)              variant   alloc=21,911,892  leak=21,911,892  leak%=100.0  share%=27.9
 3  0x1028caef8 (kai_keyword_kind + 0x41c)               variant   alloc=16,186,475  leak= 7,171,396  leak%= 44.3  share%= 9.1
 4  0x1028c4938 (kai_lex_string + 0x0d0)                 cons      alloc= 2,488,492  leak= 2,289,533  leak%= 92.0  share%= 2.9
 5  0x1028cfe20 (kai_emit_source_caret_from_src + 0x064) variant   alloc= 1,839,102  leak= 1,754,885  leak%= 95.4  share%= 2.2
 6  0x1028be330 (kai_compile_source + 0x3b8)             str       alloc= 7,813,652  leak= 1,543,566  leak%= 19.8  share%= 2.0
 7  0x1028f25c4 (kai_parse_mul_rest + 0x588)             record    alloc=   778,366  leak=   775,824  leak%= 99.7  share%= 1.0
 8  0x1028f2598 (kai_parse_mul_rest + 0x55c)             variant   alloc=   778,366  leak=   775,824  leak%= 99.7  share%= 1.0
 9  0x1028caccc (kai_keyword_kind + 0x1f0)               int       alloc= 4,641,313  leak=   555,303  leak%= 12.0  share%= 0.7
10  0x1028c5204 (kai_lex_char + 0x4cc)                   record    alloc= 1,861,677  leak=   470,804  leak%= 25.3  share%= 0.6
11  0x1028eb4a0 (kai_parse_row_labels + 0x82c)           variant   alloc=   497,812  leak=   459,802  leak%= 92.4  share%= 0.6
12  0x1028eb4cc (kai_parse_row_labels + 0x858)           record    alloc=   497,812  leak=   420,387  leak%= 84.4  share%= 0.5
13  0x1028eb484 (kai_parse_row_labels + 0x810)           variant   alloc=   497,812  leak=   420,387  leak%= 84.4  share%= 0.5
14  0x10292cbfc (kai_inject_builtin_effects + 0x82c)     cons      alloc=   764,044  leak=   387,204  leak%= 50.7  share%= 0.5
15  0x102a05d5c (kai_effects_collect_kind + 0x324)       variant   alloc=   374,153  leak=   374,153  leak%=100.0  share%= 0.5
16  0x1028c4cf8 (kai_lex_string + 0x490)                 record    alloc=   348,116  leak=   348,116  leak%=100.0  share%= 0.4
17  0x1028c4154 (kai_lex_ident + 0x4b8)                  cons      alloc=   348,116  leak=   348,116  leak%=100.0  share%= 0.4
18  0x1028cc09c (kai_lex_number_decimal + 0x174)         str       alloc=27,510,051  leak=   274,008  leak%=  1.0  share%= 0.3
19  0x1028d44b0 (kai_tk_is + 0x1770)                     record    alloc=   371,081  leak=   265,214  leak%= 71.5  share%= 0.3
20  0x102a05e14 (kai_effects_collect_kind + 0x3dc)       variant   alloc=   255,643  leak=   255,643  leak%=100.0  share%= 0.3
```

### Reading the table

- **Sites 1 + 2 (`kai_synth_dim_pow`) account for 62.5 % of the
  total leak.** Both are `variant` allocations with **0 frees** —
  the matching `_synth_dim_pow` emit path never returns its
  result through any decref discipline. This is the single
  highest-leverage target for #297.
- Site 3 (`kai_keyword_kind + 0x41c`) is also a `variant` with
  ~44 % leak rate. Together with sites 1 + 2, three sites
  account for **71.6 %** of the total live count.
- The next cluster (sites 4-8) is the lexer + parser path.
  `kai_parse_row_labels` (sites 11-13) and `kai_lex_string` (sites
  4 + 16) suggest the row / string lexer-parser chain doesn't
  decref intermediate variants when discarding a partial parse.
- Site 6 (`kai_compile_source + 0x3b8`, str, leak=1.5 M) is a
  string-concat hot path; the leak rate is only ~20 % so the
  pattern is mostly correct but loses one in five.

These are the call sites #297 (perceus_pass last-use audit) and
#298 (closure capture lifecycle) should target first. They were
hidden by tag-level reporting because the `variant` aggregate
mixed contributions from 3,039 distinct sites; sorting by leak
reveals that **two adjacent emit sites in one function dominate
the entire compiler**.

## Vanilla build verification (zero overhead)

Vanilla `tier0` runs every step of the existing test suite with
no `-DKAI_TRACE_RC=1`. Selfhost byte-identical with main confirms
that:

- The `alloc_site` struct field is gated under the flag
  (`#ifdef KAI_TRACE_RC` inside `struct KaiValue`).
- `KAI_RC_NOINLINE` expands to empty when the flag is off.
- The `kai_alloc(tag)` macro is only defined under the flag —
  vanilla builds bind the literal function name.
- `kai_rc_sites[]`, `kai_rc_site_*` helpers, and the per-site
  registration are all inside `#ifdef KAI_TRACE_RC` blocks.

`bin/kai run examples/effects/rc_trace_callsite.kai` produces the
golden output identically to before.

## Friction points

- **Non-genericity in stage 2.** First fixture draft used `Pair`
  as a non-parameterised record type; stage 2 instantiates
  records as auto-generic and reported `expected: Pair / found:
  Pair[Int, Int]`. `bin/kai` (stage 1) accepted both forms but
  stage 2 (the gate) did not. Renamed the type to `IntPair`,
  resolved without needing parameters.

- **macOS atos requires the dyld slide.** Initial direct `atos
  -o stage2/kaic2 0xADDR` returned the address verbatim — atos
  was operating on a stale load address. Fix: capture
  `_dyld_get_image_vmaddr_slide(0)` at exit and print it
  alongside the table; the helper subtracts before symbolizing.
  `nm` lookup turned out simpler than `atos` and produces
  function-level resolution without requiring a dSYM.

- **Diagnostic noise from clang LSP.** Several pre-existing
  diagnostics from the IDE flagged `<unnamed struct>` issues at
  unrelated lines (kai_string_split temp). Verified these are
  pre-existing C++ mode false positives; clean cc compilation
  proceeds without warning.

## Subjective summary

The lane went smoothly. The biggest payoff was the empirical
top-3: two sites in `kai_synth_dim_pow` account for 62.5 % of the
runtime leak, which is the kind of localization the issue brief
predicted but the per-tag report could not produce. #297 and
#298 now have a concrete starting list of three call sites to
fix, rather than a vague "variant-tag has 64 M live values".

The implementation cost was lower than budgeted because the
existing `KAI_TRACE_RC` infrastructure (PR #292) already had the
right shape — adding a hash table and a side field to KaiValue
plus the macro shim was straight-line work. The wrapper-NOINLINE
pattern is a one-line change at every alloc helper; a fresh
worktree might prefer to use a function attribute on the file
level (`-fno-inline-functions` under tracing) to avoid touching
each definition, but the explicit annotations are clearer about
intent.

## Limitations

- **Symbolization is offline.** The runtime emits hex addresses
  and the slide; `tools/symbolize-rc-trace.sh` translates them
  via `nm`. There is no in-process symbolization (would require
  linking libbfd or invoking a child `atos`).
- **No per-chunk attribution beyond the alloc site.** The chunk
  stores a single `void *alloc_site`. If the same chunk is
  later transferred (e.g. via reuse-in-place rewrite), the field
  still points at the original alloc — by design; the histogram
  asks "where did this allocation originate," not "who currently
  holds it."
- **Only `KaiValue` chunks are traced.** Strings (`v->as.s.bytes`),
  record `fields[]` arrays, variant `args[]` arrays, etc. are
  separately `malloc`'d and ride untraced lifetimes. A leak
  counted under `tag=record` means the `KaiValue` for the record
  leaked; the `fields[]`/`names[]` arrays leak with it.
- **Wrapper chains collapse on `kai_str(...) → kai_str_from_bytes(...)`.**
  Both wrappers are NOINLINE, so the address captured inside
  `kai_str_from_bytes` for that path is `kai_str` itself. Direct
  callers of `kai_str_from_bytes` resolve to their own emit site,
  so ordering by leak still surfaces the real culprits.
- **Hash table fixed at 16 K buckets.** kaic2 selfhost emits
  ~3 K sites — far below saturation. Larger workloads may
  saturate; the report flags `TABLE SATURATED` and existing
  entries remain accurate.
