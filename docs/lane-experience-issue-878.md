# Lane experience — issue #878 (KAI_MAX_HEAP heap ceiling)

## Scope as planned vs as shipped

**Planned (issue #878 + brief):** a `KAI_MAX_HEAP` env var capping total
committed heap. When a program would cross it, abort clean (`fprintf` +
`exit(1)`) — converting "OOM hangs the host" into "the process dies with a
message", like JVM `-Xmx` / Go `GOMEMLIMIT`. Unset → byte-identical, zero
overhead. The brief named a single chokepoint — `kai_slab_alloc` — and
"O(1) per slab, not per object".

**Shipped:** the cap, the suffix parser (`k`/`m`/`g` + plain bytes), a
monotonic committed-bytes counter, both runtime.h copies in lockstep, a
fixture, a Makefile gate wired into tier0 + tier1, and the doc. But the
counter does NOT live only at `kai_slab_alloc` — see the next section: the
brief's "single chokepoint" premise is only partly true, and shipping it
literally would have left a containment hole a one-line program walks
through. The shipped placement is "charge at every OS-commit grow point",
which is what a ceiling actually requires.

## The load-bearing decision — where the counter lands

The brief asserts "all heap funnels through `kai_slab_alloc`". Reading the
runtime, that is false in two ways:

1. **stage2** — `kai_slab_alloc` is the dominant *variant-block* path
   (~6.96M nodes on the rb-tree bench), but cells go through `kai_alloc`
   (calloc, cell pool), arena chunks through `kai_arena_chunk_new`, and
   string/array payloads through direct `malloc`/`realloc`. A string
   runaway (`s ++ s` in a loop) or an array `push` loop never touches a
   slab grow — it would slip a slab-only cap entirely.
2. **stage0** — has **no slab allocator at all.** Every cell is a
   per-object calloc, every variant block a per-object malloc. There is
   no coarse chokepoint to hook; a slab-only cap simply does not exist for
   stage0.

A containment ceiling has exactly one property that justifies it: no
trivial runaway evades it. A cap that misses strings/arrays and is absent
in stage0 is not a ceiling, it is a heuristic that sometimes fires. So the
counter is charged at **every place that commits new OS heap and retains
it**, via a single `kai_heap_charge(size_t)` plus thin
`kai_heap_malloc`/`kai_heap_realloc` wrappers:

- slab grow (256 KiB) + oversized slab `malloc` (stage2)
- cell `calloc` on pool-miss (both)
- variant-block fallback `calloc`/`malloc` (stage2 non-pool) and
  `kai_alloc_var` (stage0, the only variant path)
- slot-array `malloc` on pool-miss (stage0)
- arena chunk grow (both)
- string payloads: `from_bytes`, `concat`, `concat_all`, `join`,
  region copy-out, file-read (both)
- array payloads: `make`, `grow` (realloc delta), copy-out, read-to-array

**Counter discipline: monotonic commit, not live.** The value heap never
returns slabs to the OS, so the running total is the process high-water —
exactly the containment metric. Nothing is discharged on decref/free. The
one subtlety that bit during design: *transient* scratch buffers that are
`free`d within the same function (the region copy-out `fields`/`slots`
temporaries; the native shim's variant slot-overflow `heap` arrays) are
deliberately NOT charged — charging-without-discharging a freed temp would
inflate the high-water spuriously. Only buffers that survive the call are
charged.

The brief's "O(1) per slab" intent is honoured where there is a coarse
boundary (slab/arena/cell-pool grow). Strings/arrays are charged
per-object because there is no intermediate pool there — the payload
malloc *is* the commit. That is per-rare-object, not per-hot-node, so it
costs nothing measurable on the variant-churn hot path.

asu was consulted on this exact tension (slab-literal vs complete
coverage) and came down decisively on complete coverage; the `s ++ s`
fixture is the test that distinguishes a real ceiling from a leaky one.

## Suffix-parse decision

`KAI_MAX_HEAP` accepts a plain byte count and a single `k`/`m`/`g` suffix
(case-insensitive). Parse once at startup, cache via an `inited` flag
(precedent: `kai_bench_*` lazy-cache). Defensive on garbage: a junk
suffix, trailing junk after the suffix, `0`, empty, or overflow all fall
back to **no cap** rather than a wrong cap — a malformed env var must not
silently install a 1-byte ceiling that aborts everything. Verified: `64m`,
`64M`, `50000000`, `abc` (→ no cap), empty (→ no cap) all behave.

## Native backend coverage

Native (in-process libLLVM) is the default backend; C-direct is the
oracle. The change is pure `runtime.h`, which the native shim
(`stage0/runtime_llvm.c`) includes via `#include <runtime.h>` — ONE
runtime, so the cap applies to native identically. The native shim's own
direct `malloc`s (lines ~282/537/592/623/677) are transient slot-overflow
scratch (`free`d in the same function); the persistent variant block they
feed is built by `kai_variant_u` → `kai_alloc_var`/`kai_slab_alloc`, which
ARE charged. So native committed value heap is fully covered. Could not
exercise native locally (no `llvm-config` in PATH — C-only kaic2); the
gate runs the fixture on the C backend, and the runtime is shared, so the
cap holds on both. CI's tier1-native compiles the same runtime.h.

## Fixture

`examples/perceus/heap_limit_runaway_878.kai` — doubles a 64-byte seed 22
times toward a bounded ~256 MB target (`len=268435456`). Gate
`make test-heap-limit` (stage2/Makefile, wired into top-level tier0 + the
stage2 `test` aggregate) asserts three outcomes:

- `KAI_MAX_HEAP=64m` → abort exit 1 with `heap limit exceeded`, no result
  printed (used ~33 MB before the next ~64 MB doubling would cross 64m).
- `KAI_MAX_HEAP=2g` → completes, `len=268435456`.
- unset → completes, `len=268435456`.

Every run is wrapped in `timeout` as a host-safety backstop (on macOS
`ulimit -v` is a no-op, so `timeout` is the real ceiling while building
the fix). The target is bounded so the high/unset runs terminate; even a
regression that broke the cap could not then exhaust the host.

**Fixture trap that bit once:** the first draft seeded with
`string.repeat("x", 1048576)`. `string.repeat` is non-tail recursive
(`string_concat(s, repeat(s, n-1))`), so a million-deep count overflowed
the stack (SIGSEGV, exit 139) — on the *unset* path, which immediately
proved it was a fixture bug, not the cap. Rewrote the seed as a literal
doubled in shallow (22-frame) recursion. Lesson: a growth fixture must
grow via the allocator under test, not via deep recursion through an
unrelated O(n)-stack stdlib function.

**Makefile trap:** under `set -e`, `cmd; rc=$?` aborts on the failing
`cmd` before `rc` is captured. The low-cap run *correctly* exits 1, which
killed the recipe. Fixed with `rc=0; cmd ... || rc=$?` so a deliberate
non-zero exit is captured, not fatal.

## Overhead

Unset path = one cached-bool branch per OS-commit grow point, perfectly
predicted (one global outcome). On the hot variant path that branch sits
at slab grow (every 256 KiB), not per node. selfhost stayed byte-identical
(`kaic2b.c == kaic2c.c`), which is the proof the emitted code is unchanged
— the cost lives entirely in the linked runtime, not the generated binary.
Did not run native-perf benches uncapped (host-safety); reasoned: a
well-predicted bool check amortised over 256 KiB of allocation is below
measurement noise.

## Follow-ups left for next lanes

- **Per-fiber limits (BEAM-style)** — explicitly out of scope (issue #878
  "Out of scope"). This is one global process ceiling. A per-fiber cap
  would need the counter scoped to each fiber's private heap; the slab
  globals are process-global today (single OS thread), so a per-fiber
  variant is a real design lane, not an extension of this one.
- **Soft-limit / GC-pressure semantics** (`GOMEMLIMIT`-style backpressure)
  — kaikai has no GC to pressure; the hard abort is the v1.
- **Recovering from the limit as an effect** — abort-only for v1, like the
  existing OOM path.
- **Charging the small fixed runtime tables** (`_kaix_v2h_heap`,
  `_kaix_impls_heap` reallocs in the native shim) — bounded by program
  size, not runaway drivers, so left uncharged; revisit only if a
  pathological generated program makes them material.
