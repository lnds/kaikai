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
