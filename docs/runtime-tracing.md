# Runtime tracing — `KAI_TRACE_RC`

Strict alloc tracing built into `stage0/runtime.h` to surface RC
imbalances on macOS the same way glibc's tcache strict check does on
Linux.

This is **diagnostic infrastructure**, not a fix. Tracking issue:
[#291 Track #2](https://github.com/lnds/kaikai/issues/291).

## Two modes

| Mode                        | Activation                  | Cost  | Output                                 |
|-----------------------------|-----------------------------|-------|----------------------------------------|
| Baseline (always on)        | env `KAI_TRACE_RC=1`        | ~ns   | `[KAI_TRACE_RC] alloc_total=…`         |
| Strict (build-flag gated)   | `cc -DKAI_TRACE_RC=1 …`     | small | `[KAI_TRACE_RC] STRICT alloc_total=…`  |

Default builds (no `-D`, no env) emit nothing. The baseline counters
keep running unconditionally because their cost is negligible
(~4 increments per alloc, 2 per free); the strict layer is opt-in
because the sentinel write and per-tag free counter add a second
small but real cost.

### Baseline mode (env-gated, always compiled)

```sh
KAI_TRACE_RC=1 ./build/myprog
```

Reports total allocs, frees, leaked count, live peak, and **per-tag
allocs only**. Used by `make rc-budget`. Survived from earlier
milestones.

### Strict mode (build-flag gated)

```sh
CFLAGS="$CFLAGS -DKAI_TRACE_RC=1" make -C stage2 selfhost
```

Adds:

1. **Per-tag free counters** alongside the existing per-tag alloc
   counters. The exit report prints `allocs / frees / live` per tag
   and flags `LEAK` (live > 0) or `DOUBLE` (frees > allocs).

2. **Sentinel-on-free.** Just before `free(v)` in `kai_free_value`,
   the chunk is overwritten with the recognizable poison pattern
   `0xDEADBEEFDEADBEEFULL` repeated across `sizeof(KaiValue)`. macOS's
   malloc happily reuses the chunk, but a stale pointer dereference
   surfaces `tag = 0xEFBE...`, `rc = 0xDEADBEEF` instead of stale-
   but-plausible content — the same kind of "garbage" that glibc's
   tcache check fires on.

3. **Optional per-chunk history** (env `KAI_RC_HISTORY=1`). Records
   the last 65 536 RC operations (alloc / incref / decref / free) in
   a ring buffer and dumps them at exit. Heavy — opt-in only — but
   lets a post-mortem trace which tag's RC drifted on a specific
   chunk.

The strict report fires on `atexit` unconditionally. Suppress with
`KAI_TRACE_RC_QUIET=1` if a test harness needs clean stderr.

## Workflow

```sh
# 1. Pin a baseline on main.
CFLAGS="$CFLAGS -DKAI_TRACE_RC=1" make -C stage2 selfhost \
  2>&1 | grep STRICT > /tmp/main-trace.txt

# 2. Switch to the suspect branch and re-run.
git checkout my-suspect-branch
CFLAGS="$CFLAGS -DKAI_TRACE_RC=1" make -C stage2 selfhost \
  2>&1 | grep STRICT > /tmp/suspect-trace.txt

# 3. Diff. Tags whose live count moved are where the imbalance is.
diff /tmp/main-trace.txt /tmp/suspect-trace.txt
```

For a stuck bug, layer `KAI_RC_HISTORY=1` on top to get the chunk-
level op log. The ring buffer holds the last 64 K events, which on
the kaic2 self-compile covers the very tail of execution — most
useful for end-of-run investigations (where leaks accumulate) rather
than start-of-run (where the bug seeds).

## Empirical baseline (2026-05-06, kaic2 self-compile, macOS arm64)

| Tag     | allocs   | frees    | live     |
|---------|----------|----------|----------|
| int     | 4.6 M    | 2.7 M    | 1.9 M    |
| str     | 36.0 M   | 34.1 M   | 1.9 M    |
| cons    | 8.1 M    | 3.5 M    | 4.6 M    |
| record  | 9.0 M    | 0.13 M   | 8.9 M    |
| variant | 77.5 M   | 13.4 M   | 64.1 M   |
| closure | 1.7 M    | 1.2 M    | 0.48 M   |
| array   | 0.19 M   | 0.17 M   | 0.02 M   |

Mainline already leaks ~82 M live values at exit. The emitter's
chronic RC discipline gap (`memory: project_emitter_no_rc_discipline`)
is empirically confirmed: `record` is freed at ~1.5 % of its alloc
rate; `variant` at ~17 %.

The macOS allocator tolerates this; Linux glibc's tcache strict
check does not. Track #1 (the fix lane) starts here.

## Limitations

- **Only the `kai_alloc` chunk is traced.** Strings (`v->as.s.bytes`),
  record `fields[]` arrays, variant `args[]` arrays, etc. are
  separately `malloc`'d and ride their own (untraced) lifetime.
  A leak counted in `record` means the `KaiValue` for the record
  leaked; the `fields[]`/`names[]` arrays leak with it but are not
  separately accounted.

- **Sentinel only catches use-after-free reads on the chunk header.**
  If the consumer reads `v->as.s.bytes` first and only crashes on
  the embedded `bytes` pointer (which is independently allocated
  and has its own lifetime), the sentinel doesn't help. The poison
  pattern catches the cases where the consumer's first deref is
  `v->tag` or `v->rc`.

- **History buffer is small (64 K).** kaic2 self-compile generates
  ~190 M RC operations; the buffer overflows at ~0.03 % of execution.
  History is useful for end-of-run investigations (leaks at exit),
  not start-of-run seeding.

- **Strict mode is not on in CI.** Tier 1 / Tier 1-ASAN keep the
  default build (no `-DKAI_TRACE_RC=1`) so selfhost stays byte-
  identical with main. Strict-mode runs are local-only diagnostics.

## Per-call-site attribution (issue #296)

Strict mode (`-DKAI_TRACE_RC=1`) also enables a **per-call-site
histogram** on top of the per-tag counters. Each `kai_alloc`
captures the wrapper's caller via `__builtin_return_address(0)` and
records `(allocs, frees)` keyed by that address in a 16 K-bucket
open hash table. At exit the runtime sorts sites descending by
leak (`allocs - frees`) and prints the top N (default 20;
`KAI_TRACE_RC_TOP=N` overrides):

```
[KAI_TRACE_RC] top sites by leak (showing 20 of 3039 distinct sites; total_leak=78437784)
[KAI_TRACE_RC] aslr_slide=0x28bc000 (subtract from site addresses for static symbolization)
[KAI_TRACE_RC] site  1 0x102a35c78 tag=variant allocs=27173839 frees=0 leak=27173839 leak%=100.0 share%=34.6
[KAI_TRACE_RC] site  2 0x102a35b70 tag=variant allocs=21911892 frees=0 leak=21911892 leak%=100.0 share%=27.9
…
```

Each entry: site index, runtime address, dominant tag, alloc
count, free count, leak count, leak rate (live / alloc) and share
of total leak.

### Symbolization

`__builtin_return_address(0)` returns the post-ASLR address. The
report prints `aslr_slide=…` so a post-mortem can subtract it to
recover the static address. The bundled helper does this:

```sh
CFLAGS="-DKAI_TRACE_RC=1" make -C stage2 selfhost 2> /tmp/site-trace.log
tools/symbolize-rc-trace.sh stage2/kaic2 /tmp/site-trace.log | grep "site "
```

Output rewrites every `site` line in place, naming the nearest
function symbol via `nm`:

```
[KAI_TRACE_RC] site  1 0x102a35c78 (_kai_synth_dim_pow + 0x1e4) tag=variant …
```

Resolution is function-level (no line numbers) — for line-level
detail run `atos -o stage2/kaic2 -l <slide> <addr>` if a dSYM is
present.

### Why this matters

#293 Phase 1 and Phase 2 added RC discipline at correct shapes
(record / variant ctors, then `_` discards) but failed to move
the kaic2 selfhost leak rate. The per-tag counts are too coarse
to localize the actual leakers — every emitter call site that
yields a `variant` is bucketed together. Per-call-site
attribution collapses the 64 M `variant` live count into the few
specific functions that account for most of it, turning the
audit from inspection into measurement.

### How wrappers are kept honest

`__builtin_return_address(0)` only points at the real emit site
if the calling wrapper isn't inlined into its parent. Under
`-DKAI_TRACE_RC=1`, every `kai_alloc` wrapper (`kai_int`,
`kai_record`, `kai_variant`, …) is marked `KAI_RC_NOINLINE`
(expanding to `__attribute__((noinline))`). Vanilla builds drop
the attribute (zero overhead).

### Limitations

- **Function-level resolution only.** `nm` only knows function
  starts; `atos` may yield line numbers if a dSYM is present and
  the right `-l` slide is passed, but this varies by toolchain.
  The `(symbol + 0xoffset)` form is enough to localize an audit
  to a specific function.

- **Wrapper chains collapse.** `kai_str(cstr)` calls
  `kai_str_from_bytes`, both `KAI_RC_NOINLINE`. The site captured
  inside `kai_str_from_bytes` for that path is `kai_str` itself,
  not the user's call site. Direct callers of
  `kai_str_from_bytes` still resolve to their own emit site, so
  ordering by leak surfaces the real culprits.

- **Hash table is fixed at 16 K buckets.** kaic2 selfhost emits
  ~3 K distinct sites — far below saturation. If a workload
  overflows, the report appends `TABLE SATURATED` and silently
  drops new sites; existing entries remain accurate.

- **Per-chunk attribution adds one word per `KaiValue` under
  `-DKAI_TRACE_RC=1`.** Vanilla builds keep the original two-word
  header.

### Empirical top-3 (kaic2 selfhost, 2026-05-06, macOS arm64)

| # | symbol                       | tag     | leak       | share  |
|---|------------------------------|---------|------------|--------|
| 1 | `kai_synth_dim_pow + 0x1e4`  | variant | 27,173,839 | 34.6 % |
| 2 | `kai_synth_dim_pow + 0xdc`   | variant | 21,911,892 | 27.9 % |
| 3 | `kai_keyword_kind + 0x41c`   | variant |  7,171,396 |  9.1 % |

Together, sites 1 + 2 (both inside `kai_synth_dim_pow`) account
for **62.5 %** of the total leak — exactly the localization the
tag-level report could not produce.

## `KAI_MAX_HEAP` — process heap ceiling (host containment)

A separate runtime knob (issue #878), unrelated to tracing: a hard
ceiling on total committed heap.

```sh
KAI_MAX_HEAP=4g  ./myprog     # cap at 4 GiB
KAI_MAX_HEAP=512m ./myprog    # cap at 512 MiB
KAI_MAX_HEAP=67108864 ./myprog  # cap in plain bytes
```

Accepts a plain byte count or a single `k`/`m`/`g` suffix
(case-insensitive). On cross, the runtime aborts clean:

```
kai: heap limit exceeded (KAI_MAX_HEAP=512m, used 536870912 bytes)
```

…and `exit(1)` — the same path the OOM checks use, triggered at the
configured limit instead of at OS exhaustion. **Unset, empty, or
unparseable → no cap** (a malformed value never installs a wrong
ceiling), and the unset path adds one predicted branch per heap-grow
boundary — selfhost stays byte-identical.

The counter is monotonic committed-heap high-water, charged at every
OS-commit grow point (slab/arena/cell-pool grow plus string/array
payloads), so no allocation path grows past the ceiling uncounted. One
global process ceiling; per-fiber limits are out of scope.

This is the **structural** host-safety defense. The per-harness
`timeout` / `ulimit -v` wrappers around benchmark and stress runs are a
band-aid that only protects the test that remembers to wrap; the ceiling
contains *any* run. It matters most on macOS, where `ulimit -v` is a
no-op and the RAM compressor masks exhaustion until the host hangs — so
without `KAI_MAX_HEAP` there is no ceiling at all on Darwin.
