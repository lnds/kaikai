# Why the C and native RC ledgers disagree

#1952 found 50 of 240 perceus fixtures whose RC ledger differs between the
two backends, in two shapes: one where `alloc_total` diverges and `free_total`
matches, one where `free_total` diverges and `alloc_total` matches. #1953
located the onset at emit. This note answers which backend is right.

**Both ledgers are honest. Neither counts wrong.** Each divergence is a real
difference in the code the two emitters produce, confirmed against peak RSS —
the instrument is trustworthy; what it reports is two different programs.

## The bitcode/O2 hypothesis is refuted

The native backend links the runtime as bitcode inlined before O2, the C
backend compiles `runtime_llvm.c` with `cc`. The proposed explanation was that
O2 deletes allocations across the inlined boundary, or detaches the counter
increment from the allocation it counts.

Measured by removing `runtime_llvm.bc` / `runtime_inline.bc` so
`gen-runtime-bc.sh` opts out and native falls back to linking `runtime_llvm.c`
with `cc` — no code change, the cleanest discriminator available:

| fixture | native, P2 active | native, P2 opted out |
|---|---|---|
| `unbox_bench_real` | alloc 3,000,003 / free 2 | alloc 3,000,003 / free 2 |
| `variant_full_mask_1157` | alloc 2,228,131 / free 1,620,083 | alloc 2,228,131 / free 1,620,083 |

Identical to the digit. The divergence does not come from the runtime link.

Two structural facts agree. The counters are one shared instance across the
boundary: `KAI_RT_COUNTER` expands to `extern KAI_TLS` with a single
owner-side definition under `KAI_SEPARATE_COMPILATION` (`stage2/runtime.h`),
so inlined runtime code and the owner object update the same per-thread
ledger. And `kai_rc_count_alloc` / `kai_rc_count_free` are `noinline` and sit
beside the libc call whose result the program uses, so O2 cannot drop one
without the other.

## `unbox_bench_real`: native allocates 3× — and native is telling the truth

`alloc_total` 1,000,002 (C) vs 3,000,003 (native); `free_total` 2 on both.

The ledger is not inflated. `live_peak` tracks `alloc_total` on both backends
(1,000,002 and 3,000,003), and peak RSS separates them the same way:

| backend | peak RSS | live at exit |
|---|---:|---:|
| C | 60.1 MB | 1,000,000 |
| native | 156.5 MB | 3,000,001 |

The extra two million `Real` cells per run exist in memory. Native genuinely
emits three box points per iteration where C emits one.

**The cause is not the ABI.** Native passes and returns `Real` as raw `f64`:
raw param/return slots (`kir_lower.kai`), raw call args and raw call result
(`kir_lower_walk.kai`), raw `if`-join, raw LLVM signature
(`emit_native_fn.kai`) — none gated on the backend. The one native-only
unboxing restriction in the compiler (`unbox.kai` + `unbox_native_raw.kai`,
the non-total load-border) is scoped to Int binders of variant match arms, and
this fixture has no variant match.

Isolated by minimal fixtures, 1000 iterations, counting `alloc_total`:

| shape | C | native |
|---|---:|---:|
| arithmetic only, no call | 3,002 | 3,002 |
| `Real`-returning call, result consumed by arithmetic | 2 | 1,002 |
| same, result bound by `let` first | 2 | 2,002 |
| `Int`-returning call, same shape | 2 | 506 |

Parity holds without a call, and breaks the moment a Kai→Kai call's raw scalar
result meets a consumer. C consumes the raw result directly; native re-boxes it
at that border, and a `let` binding adds a second box. It is not `Real`-specific
— the Int twin diverges too, less severely.

This contradicts `docs/native-parity-gaps.md`, which records "Real box/unbox
(unbox_bench_real) — CLOSED 2026-06-13". Either the closure was partial or it
regressed; the gap is live today.

## `variant_full_mask_1157`: native frees 199,972 more — and native is right

`alloc_total` identical within 5; native's `free_total` is 199,972 higher. The
allocation tags are identical (`variant` 406,020 on both), and `incref_total`
is identical (808,009), while `decref_total` differs by exactly 199,972 — the
same number as the frees. Native emits decrefs C does not.

Reduced to a 20,000-node TRMC spine rebuild, the signature reproduces exactly,
with the delta equal to `n`:

| | alloc | free | live at exit | decref | peak RSS |
|---|---:|---:|---:|---:|---:|
| C | 160,002 | 100,001 | 60,001 | 80,003 | 15.45 MB |
| native | 160,002 | 120,001 | 40,001 | 100,003 | 14.70 MB |

The structure held live at exit is a 20,000-cell spine: 20,001 `variant` plus
20,000 `Real` ≈ 40,001 objects. **Native reports 40,001. C reports 60,001 —
20,000 above what the program can still reach.** RSS agrees: C holds ~0.75 MB
more, which is 20,000 × 48 B.

So C is retaining one `Real` per rebuilt spine node. On the full fixture that
is 199,972 cells and 9.6 MB of RSS (49.8 MB vs 40.2 MB).

The C TRMC step decides what to release through a per-call-site `dropmask`
whose own criterion is documented as conservative
(`tcrec_compute_site_dropmask`, `emit_c.kai`): a param read exactly once at
`LUAt` gets no drop, on the assumption the single read transfers the reference
to the recursive call's matching argument. In a TRMC spine step the rebuilt
node's scalar slot does not transfer that way, and the reference is dropped on
the floor. This is the mechanism the evidence points to; the exact predicate at
fault was not isolated, and fixing the C emitter is outside this lane.

## What this means for the instrument

The ledger is sound on both backends. It is not the case that a leak measured
on one backend is uninformative about the other — but the two columns are not
interchangeable either, because they measure two genuinely different programs.
`tools/rc-leak-baseline.txt` pinning both columns is correct, and stays correct.

The important consequence is the reverse of the one feared: a fixture whose
columns differ is not a counting artefact to be normalised away. It is a
report that the backends compile that program differently, and in both cases
examined here the difference is a real defect — a native re-boxing gap, and a
C retention.

## Coverage of the 50 divergent fixtures — not established

This note explains two fixtures and their two mechanisms. **How many of the 50
each mechanism accounts for was not measured.** Classifying the rest means
running all 50 on both backends and partitioning by whether the delta sits in
`alloc_total` or `free_total`, then reducing each class. That was not done
here, so the remainder is unattributed. What can be said is that the two shapes
#1952 named are each a real defect rather than an artefact of counting.

## Also found

`-DKAI_TRACE_RC=1` cannot be used on the native backend. It adds `alloc_site`
to `KaiValue`, moving the slot array from offset 8 to 16, which trips a
`_Static_assert` in `stage0/runtime_llvm.c` requiring offset 8 for the native
emitter's layout. The per-allocation-site attribution report — the runtime's
finest leak-localisation tool — is therefore C-only.
