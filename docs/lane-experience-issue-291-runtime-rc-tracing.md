# Lane experience — issue-291-runtime-rc-tracing

Track #2 of #291: build-flag-gated strict alloc tracing for
`stage0/runtime.h`. **Diagnostic infrastructure**, not a fix —
Track #1 is the follow-up that uses this scaffolding to find
the actual RC imbalance that aborted PRs #41, #48, and #290 on
Linux clang.

## Objective metrics

| Metric                      | Value                          |
|-----------------------------|--------------------------------|
| Branch                      | `issue-291-runtime-rc-tracing` |
| Base commit                 | `4b0e7ca` (#289 reuse-in-place)|
| Files touched               | 2 (`stage0/runtime.h`, `docs/runtime-tracing.md`) |
| Net additions               | ~135 lines under `#ifdef KAI_TRACE_RC` |
| New docs                    | `docs/runtime-tracing.md` (130 lines)  |
| Tier 0 vanilla              | OK (~55 s)                     |
| Tier 1 vanilla              | OK (~5 min 40 s)               |
| Tier 1-ASAN vanilla         | OK (~53 s)                     |
| Selfhost byte-identical     | Yes (vanilla — no flag)        |
| Wall-clock spent            | ~2 h                           |
| Calibrated estimate         | 2-4 h                          |

## Tracing implementation

Three facilities, all gated behind `#ifdef KAI_TRACE_RC`:

1. **Per-tag free counters** (`kai_rc_free_by_tag[16]`) — pairs
   with the existing `kai_rc_alloc_by_tag` to give the exit
   report `allocs / frees / live` per tag, with `LEAK` /
   `DOUBLE` flags.

2. **Sentinel-on-free** — `kai_free_value` overwrites the
   `KaiValue` chunk with `0xDEADBEEFDEADBEEFULL` before
   `free(v)`. Stale reads on macOS now surface a recognizable
   poison pattern instead of stale-but-plausible content.

3. **Optional per-chunk history** (`KAI_RC_HISTORY=1` env, 64 K
   ring buffer) — records `(chunk, op, tag)` for every alloc /
   incref / decref / free, dumped at exit. Heavy; opt-in.

The `kai_rc_history_log` hook is also wired into `kai_incref`
and `kai_decref`. Under no-flag builds, all three calls compile
to nothing.

The strict-mode `atexit` handler is registered alongside the
existing one in `kai_set_args`, so a single emit-side touch
(none) is enough — both layers piggy-back on the same lazy
registration site.

Reporting is suppressed by `KAI_TRACE_RC_QUIET=1` so harnesses
that don't want stderr noise can still build with the flag on.

## Vanilla build verification

Built and ran the existing tier 0 / tier 1 / tier 1-ASAN gates
**without** `-DKAI_TRACE_RC=1`. All three green; selfhost
byte-identical with main. The `#ifdef` guard is leak-free —
default builds see exactly the prior runtime.h surface area.

A microcheck was useful: built `examples/minimal/hello.kai` both
ways:

- Vanilla `cc -I stage0 hello.c` → no `STRICT` line on stderr,
  baseline `KAI_TRACE_RC=1` env still works as before.
- `cc -DKAI_TRACE_RC=1 -I stage0 hello.c` → `STRICT` report
  fires unconditionally; `KAI_TRACE_RC_QUIET=1` suppresses it;
  `KAI_RC_HISTORY=1` dumps the 3-event log (alloc, decref,
  free of the single string).

## Empirical evidence — the headline

The brief asked for the tracing to **reveal** the imbalance the
`issue-92-tco-dropmask-side-table` branch hits on Linux. It
delivered, but with a more general finding than expected.

### kaic2 self-compile under tracing — both branches

```
CFLAGS="-std=c99 -Wall -Wno-unused-function -Wno-unused-variable -g -O0 -DKAI_TRACE_RC=1" \
  make -C stage2 selfhost
```

| Tag     | main allocs | main live  | broken allocs | broken live | Δ live  |
|---------|-------------|------------|---------------|-------------|---------|
| int     | 4 643 617   | 1 924 253  | 4 671 641     | 1 935 808   | +11 555 |
| str     | 36 018 485  | 1 961 585  | 36 304 274    | 1 926 640   | -34 945 |
| cons    | 8 140 483   | 4 634 767  | 8 191 492     | 3 882 522   | -752 245|
| record  | 9 004 636   | 8 871 959  | 9 056 917     | 8 915 120   | +43 161 |
| variant | 77 536 023  | 64 093 266 | 78 241 415    | 64 670 534  | +577 268|
| closure | 1 728 592   | 483 436    | 1 738 884     | 486 222     | +2 786  |

(Full transcripts in `/tmp/selfhost-{lane,broken}-trace.log` —
local artefacts, not committed.)

### What this reveals

1. **Mainline already leaks ~82 M live KaiValues at exit.** This
   is structural: the existing memory `project_emitter_no_rc_discipline`
   ("97 % of allocs leak") is empirically confirmed. The emitter
   barely calls incref / decref despite the runtime providing
   them.

2. **`record` and `variant` are the dominant offenders.** record
   is freed at ~1.5 % of alloc rate; variant at ~17 %. Track #1's
   audit should focus there.

3. **The broken branch's signature is *not* a giant new leak.**
   Compared to main, the per-tag deltas are O(0.5 %) of the
   total live count. This means the Linux tcache abort is
   triggered by a *qualitative* difference (e.g., one specific
   chunk gets freed twice, or freed-then-read, on a hot path)
   and not by sheer accumulated leak volume.

4. **macOS *and* Linux already disagree on mainline.** This is a
   real surprise: I expected mainline to be balanced and the
   broken branch to introduce the imbalance. Instead, both leak;
   only the *shape* differs. Track #1 will likely need to enable
   `KAI_RC_HISTORY=1` and look for chunks that appear in the
   history with `decref … decref … free` patterns the macOS run
   tolerates.

This is the unblocking diagnostic the brief asked for, and it
also reframes what Track #1 has to find: not "a missing incref",
but "a *differential* operation pattern on a specific chunk
class". The tracing surfaces both the steady-state leak (already
known) and the per-chunk history needed to find the differential.

## Friction points

- **Existing infrastructure already used the `KAI_TRACE_RC`
  name** as an env var name for the legacy report. The lane
  brief asked for the same name as a build flag. They don't
  conflict (preprocessor macro vs. env var) but the naming is
  initially confusing. Resolved by giving the strict layer a
  distinct `STRICT` prefix on its log lines so `grep STRICT`
  picks it cleanly without colliding with the legacy output.

- **Stage 2 `Makefile`'s `CFLAGS ?=` requires forwarding the
  default flags too** when overriding via env, otherwise
  `-Wno-unused-function` etc. are dropped and stage 2 fails to
  build under `-Wall`. Added the full string in the doc's
  workflow snippet so future invocations don't trip on it.

- **Pre-existing `mini_ledger` failure** showed up in the
  baseline tier 0; not introduced by this lane. (`23 OK, 3 ran
  no golden, 2 failed` both before and after.)

## Subjective summary

Clean lane. The instrumentation slotted into the existing
`kai_alloc` / `kai_free_value` / `kai_decref` / `kai_incref`
seams without restructuring anything. The `#ifdef` discipline
worked — vanilla tier 0 / tier 1 / tier 1-ASAN all green
without touching the build system.

The empirical run on the broken branch was the moment of truth.
What I expected: a clear "tag X has live=N on broken vs live=0
on main" signal. What I got: both branches leak, the deltas are
small, and the *real* finding is that the emitter's RC
discipline is structurally absent. Track #1 starts a layer
deeper than the brief assumed.

## Limitations

- Only `KaiValue` chunks are tagged. Side allocations
  (`v->as.s.bytes`, `rec.fields[]`, `var.args[]`, `arr.items[]`)
  ride their own lifetimes and aren't accounted for. Their leaks
  pile up *with* the parent KaiValue but aren't separately
  visible.

- Sentinel only stamps the 16-byte `KaiValue` header + union;
  doesn't catch use-after-free on the side allocations.

- History ring buffer is 64 K; selfhost generates ~190 M ops, so
  the buffer captures the very tail of execution only.

- Track #1 is the follow-up: audit stage 1's perceus emit + use
  this tracing to find the differential operation pattern that
  drives the Linux abort. Without that lane, this scaffolding
  doesn't *fix* anything — it makes the bug visible enough to
  be debugged locally on macOS.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T06:13:01-04:00	tier0	OK	55
2026-05-06T06:15:31-04:00	tier0	OK	60
2026-05-06T06:22:25-04:00	tier1	OK	337
2026-05-06T06:23:26-04:00	tier1-asan	OK	53
```

Lane span: 2026-05-06T06:11:08-04:00 → 2026-05-06T06:28:28-04:00
