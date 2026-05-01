# Lane experience report — tco-stage1-mirror (issue #42)

Best-effort retrospective by the implementing agent. The lane was not
instrumented at start (predates the brief asking for a TSV log), so the
build counts below are reconstructed from my conversation context and
the four landed commits, not from `/tmp/lane-*-builds.tsv`. See
limitations at the bottom.

## Objective metrics (reconstructed)

- Session start (first agent action in working tree, from the
  parent-shell `claude` process timestamp visible in `ps aux`):
  approximately **2026-05-01T12:18-04:00**.
- First commit (M1): **2026-05-01T12:54:31-04:00**.
- Last commit (M1 fixup): **2026-05-01T13:11:01-04:00**.
- CI tier1 green on the latest push: confirmed by the monitor event,
  approximately **2026-05-01T13:14-04:00**.
- Final report write time (this file): **2026-05-01T16:25-04:00**.
- **Wall-clock for the active implementation phase** (start through CI
  green, excluding the post-CI idle while I waited for the integrator
  message): **~57 minutes**. The trailing time was dominated by waiting
  on CI and the integrator's follow-up message; not part of authorship
  effort.

### Build/test invocations (approximate, from context)

The lane wasn't logged via TSV. Estimate by inspection of my own
tool calls:

- `make -C stage<N>` builds (`kaic0` / `kaic1` / `kaic2`): ~10
  invocations across the four iterations (initial stage 1 port,
  stage 0 port, band-aid removal, M1 fixup). All succeeded after the
  edits compiled clean.
- `make -C stage<N> selfhost`: 5 invocations (3 stage 1, 2 stage 2),
  all OK. Note: stage 1 selfhost passed *before* the M1 fixup —
  it's a false signal at stage 1 scale, since stage 1's
  `compiler.kai` is small enough that the missed-rewrite bug doesn't
  surface.
- `make -C stage<N> test`: 2 invocations (stage 0 once, stage 1
  once), both OK.
- `make tier1`: 3 invocations (band-aid present on macOS, band-aid
  removed on macOS under `ulimit -s 8192` soft, after M1 fixup under
  `ulimit -Hs 8192` hard). All three reported OK, but only the third
  was a real Linux-equivalent test — see "Friction points" below.
- `make tier0`: 2 invocations (post-band-aid removal under both soft
  and hard ulimit). The hard-ulimit run was decisive.
- `make clean && make ...`: 2 full-clean rebuilds.
- Direct `./kaic2 compiler.kai` runs under various `ulimit -s`
  values (8192 → 65520 stepping): ~7 invocations to bisect the
  minimum stack and confirm the SIGSEGV class.

Total active build/test invocations: **~25**, of which ~3 informative
failures (the kaic2 SIGSEGV under 8 MiB stack that revealed the
pipeline-order bug, and the matching CI failure on the first push).

## Compiler errors I encountered

1. **`Use of undeclared identifier 'TCO_TAIL_CALL'`** — at C compile
   of `stage0/emit.c` after my first edit. Cause: I placed
   `#define TCO_TAIL_CALL 0x1` near the TCO function block (line
   ~1520), but `emit_call` (line ~848) consumes the macro. Fixed by
   moving the `#define` up next to the forward declarations. 1 attempt.
2. **`Segmentation fault (core dumped)`** — at runtime in CI,
   reported as `make[1]: *** [Makefile:1608: selfhost] Error 139`
   during stage 2's selfhost step. Not a compiler error per se; it
   was a real stack overflow inside `kaic2`. Fixed by reordering
   `tcrec_rewrite_decls` to run before `perceus_pass` so it could
   see the unwrapped tail position. 1 fix attempt after diagnosis.

No type-checker, parser, or kaic2-side error messages featured in this
lane.

## Friction points

### macOS `ulimit` soft-vs-hard masking the real Linux constraint

The single biggest friction was discovering that
`( ulimit -s 8192 && make tier1 )` on macOS **does not actually
constrain the make sub-shell**. The make-spawned `/bin/sh` reports
`ulimit -s` as the *hard* limit (typically 65520 KiB on macOS), not
the soft limit the parent shell set. So my local validation
"tier0/tier1 green under 8 MiB" was a false signal — every binary
make invoked actually had ~64 MiB of stack.

This wasted one CI iteration: I shipped the first push with the bug
believing local validation had covered the Linux case. The CI
SIGSEGV on stage 2 selfhost forced me to look harder at *why* macOS
was passing, which is when I realised the soft-vs-hard distinction.
After switching to `ulimit -Hs 8192 && ulimit -s 8192`, kaic2
SIGSEGVed locally too — I could then bisect stack sizes (8/12/16/24/32/49/65520
KiB) to confirm the binary needed > 49 MB before the fix and < 8 MB
after. Saved this lesson into memory as
`feedback_macos_ulimit_hard_vs_soft.md` so future lanes don't repeat
it.

### Pipeline order: TCO before vs. after `perceus_pass`

The naive read of "TCO needs the most precise use information" said
*run TCO after perceus*, because perceus has already inserted the
`__perceus_dup` and `__perceus_drop` marks that the dropmask logic
references. I did exactly that in M1.

But perceus also wraps the body of every fn with multi-use params in
`EBlock([entry_drops…, SLet("__pcs_ret", body), exit_drops…],
Some(EVar("__pcs_ret")))`. After that wrap, the original
tail-position recursive call sits inside the SLet RHS — no longer in
tail position. My TCO walker matched on `EBlock`'s trailing value,
which is now `EVar("__pcs_ret")`, and returned 0. Result: **49 of
~560 self-tail-recursive fns in stage 2's compiler got the
goto-form rewrite** (~10%). Only the fns whose params were all
single-use bare (and so received no exit drops) survived the wrap.

Stage 2's source had this exact pattern in
`stage2/compiler.kai:32112`:

```
let pre_perceus_decls = if use_llvm { unboxed_decls }
                        else { tcrec_rewrite_decls(unboxed_decls) }
let perceus_decls = perceus_pass(pre_perceus_decls)
```

I missed this pin on the first read, despite the line-by-line port.
The kaikai-side fix in `compile_source` was three lines — the cost
was the time-to-diagnose, not the time-to-fix. Saved the lesson into
memory as `project_kaikai_pipeline_order_perceus_vs_passes.md`.

### Refcount discipline at the goto site (stage 0)

Stage 0 has no Perceus, only a single-use counter
(`emit_ident_value`'s `total_count == 1 && lambda_count == 0` →
bare emit, ref already transferred; otherwise → `kai_internal_dup`
wrap, slot retains ref). My drop predicate at the goto site mirrors
the negation of that single-use predicate, which is correct for the
common cases — but the existing stage 0 emit already leaks one ref
per multi-use local per fn call (the trailing kai_decref after
`return _r` doesn't release them). My TCO inherits the same per-
iteration leak rather than amplifying or fixing it. For ~33 k
iterations of `lex_loop` × a couple of multi-use locals that's a
few MiB; bounded for a one-shot bootstrap process and consistent
with stage 0's pre-existing semantics. Documented in the M2 commit
body.

## Spec ambiguities or interpretive choices

- **Scope of "stage 1 mirror"**: the brief said *lane discipline:
  solo stage 1 mirror*. But the acceptance criteria included
  removing the `RLIMIT_STACK` constructor entirely, which requires
  kaic1's *binary* to have O(1) stack — which requires stage 0 to
  also TCO. I expanded the lane to cover stage 0; the alternative
  (ship stage 1 only, leave the band-aid) would not have closed
  #42's acceptance criterion #1. Decision documented in commit M2.
- **`stage2/Makefile`'s `STACK := ulimit -s …`**: not strictly
  inside the "stage 1 build path" the acceptance criterion calls
  out, but it's the same workaround being retired. Reduced to an
  empty no-op variable rather than rewriting every recipe; leaves
  the diff narrow and the variable can be removed in a follow-up
  cleanup. Decision documented in commit M3.

## Subjective summary

- **Confidence in correctness**: medium-high. CI tier1 green on Linux
  is the load-bearing signal. Stage 1 + stage 2 selfhost both
  byte-identical. The `examples/tco/main.kai` 50 M-deep fixture
  still passes. The known incomplete piece is the per-iteration
  leak in stage 0's TCO emit, which mirrors the pre-existing
  leak shape and is documented in M2.
- **Hardest sub-task**: diagnosing the M1 bug. The symptom (Linux
  CI SIGSEGV) had three plausible root causes (stack overflow on
  some non-TCO'd fn, refcount UAF in my dropmask logic, or a
  kaikai-side typing issue) and I had to triangulate before I knew
  which one. The decisive signal was counting `_entry:;` labels in
  the emitted C: 49 in stage2.c (with the bug) vs 560 (after the
  fix). That's an ad-hoc, out-of-band signal — see compiler-help
  question below.
- **Easiest sub-task**: the actual stage 1 port. Stage 1 already
  had `last_use_for`, `pcs_count_non_lam_uses`, and `pat_bindings`
  — every helper the stage 2 rewrite assumed was already there. I
  could line-by-line port the `tcrec_*` block with only AST-shape
  edits (DFn arity, no `ty`/`mode` on Expr, only SLet/SExprStmt in
  Stmt).
- **Did the compiler help or hinder?** Mostly neither — this was a
  port between known-working sources, and the kaikai compiler
  itself wasn't on either side of any error message I saw. The
  C compiler's `Use of undeclared identifier` was a trivial
  ordering fix.

### Specifically: would `--effects-json` / `--effect-holes-json` have helped?

**No, not for this lane.** The bugs I hit were:

1. A C-level macro ordering issue (caught by `clang` directly).
2. A runtime SIGSEGV on Linux that the compiler couldn't see —
   the kaikai source typechecks fine, the C compiles fine, the
   binary just runs out of stack on a sufficiently large input.

The signal I actually used to diagnose the M1 bug was
**`grep -c "_entry:;" stage<N>/build/stage<N>.c`** — counting how
many functions received the rewrite by inspecting the emitted C.
That is an ad-hoc out-of-band signal. The kaikai-side equivalent
that *would* have helped is a query like `kai tco --rewrites-json
stage2/compiler.kai` returning the list of fns the rewrite fired
on, with their dropmasks. That doesn't exist.

The other useful signal was **bisecting `ulimit -s`** to find the
minimum stack size the binary needed — a coarse runtime-side
probe, not a compiler-side one.

Effect-typing JSON would have been the right primitive for a
*different* kind of lane (e.g. one where I needed to know which
effect rows a given fn was in scope for). For TCO, the relevant
property is per-fn (did the rewrite fire?) and the natural answer
shape is a list of fn names + dropmasks, not a hole / row /
effect-set.

## Limitations of this report

- **Not instrumented at start.** The TSV log
  (`/tmp/lane-${LANE}-builds.tsv` from the template) was never
  created — this brief reached me only at lane close. Build
  counts above are reconstructed from my conversation context,
  not from authoritative timestamps. Wall-clock is bounded by
  the parent-shell `claude` process timestamp (12:18 PM) and the
  CI green event (~13:14 PM), which is reliable; the per-make
  durations are not.
- **Self-report bias acknowledged.** I'm both the actor and the
  reporter on the friction points; I may be over-weighting the
  bugs I noticed and under-weighting steps that went smoothly.
- **Context truncation.** Counts and error lists exclude
  anything that fell out of my visible context window. The
  initial code-exploration phase (~30 min before the first
  commit) likely had more grep / Read calls than I can recall
  precisely; I focused the count on `make` and `kaic2`
  invocations because those are the durable build artefacts.
- **Single agent (Claude Sonnet 4.6, switched to Opus 4.7 mid-
  session via `/model`).** Not generalisable across LLMs.
