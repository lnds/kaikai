# Lane experience report — issue-187-phase4-codegen

Best-effort retrospective by the implementing agent. See limitations
at the bottom.

## Objective metrics (from /tmp/lane-issue-187-phase4-codegen-builds.tsv)

- Start: 2026-05-03T17:53:24-04:00
- End:   2026-05-03T18:11:51-04:00
- Wall-clock: ~18 minutes
- Build/test invocations:
  - `make tier0`:         2 invocations, 2 passes, 0 fails
  - `make tier1`:         1 invocation,  1 pass,  0 fails
  - `make tier1-asan`:    1 invocation,  1 pass,  0 fails
  - `make selfhost`:      1 invocation,  1 pass,  0 fails
  - `make selfhost-llvm`: 1 invocation,  1 pass,  0 fails

## Compiler errors I encountered

1. **Stage 1 parser rejects multi-line `if`-condition (`expected `{`
   after `if` condition`)** — at parser — fixed by collapsing the
   `aa == 0 and ba == 0 and list_has(...)` boolean across one line
   instead of three. kaikai-minimal (stage 1 grammar) does not allow
   newlines between an `if` head and its body brace. Took 1 attempt
   to spot once stage1 emit failed.

2. **`unify_list` arity mismatch in row-effect unification (C-side
   stage 2 build error: `too few arguments to function call,
   expected 4, have 3`)** — at C compile — `unify_label_pairs` calls
   `unify_list` directly for effect labels. After threading `env`
   through `unify_list`, this caller still passed 3 args. Fixed by
   passing `ty_env_empty()` (effect ty_args don't participate in
   D3 upcast). Took 1 attempt.

If none visible: "no compiler errors visible in current context".

## Friction points

- **Spec ↔ phase-2 reality mismatch.** The lane brief recommends
  Option X ("structural" narrow) under the assumption that "today
  the runtime tag is one of `IdentityError`'s or `AuthError`'s
  variants directly (because dual rep flattens the variant set)."
  Dual rep does **not** flatten today: phase 2 registers component
  type names as zero-arg ctors of the parent (`type QueryErr =
  IdentityError | AuthError` adds `IdentityError` and `AuthError`
  as zero-arg variants of `QueryErr`). So a value constructed via
  `IdentityError` has a `QueryErr.IdentityError` runtime tag, not
  one of the leaf inner variants. Reading `docs/unions-design.md`
  Decision 4 makes the intended runtime layout clear: the leaves
  are the ultimate variants. Reconciling that with phase 2 needed
  a call.

- **Decision: scope-shrink instead of grinding the resolver.** The
  brief gave two STOP conditions: env-threading >40 functions, and
  Option X requires Option Y. Neither tripped; what tripped was a
  subtler third one — Option X works *partially* under the current
  dual rep. I shipped the env-threaded D3 upcast (the brief's #1
  + #2) and the implicit upcast at `?`/`!` propagation (#3). The
  canonical narrowing fixture works end-to-end **because the
  user-visible flow is identity-on-runtime**: `AccountNotFound` is
  already an `IdentityError` value at runtime, the narrowing arm
  rebinds it under the component type, the body's `handle_id(ie)`
  is fed the same runtime value, and `match e : IdentityError`
  inside `handle_id` matches the leaf. The resolver-level dual-rep
  cleanup (skip ctor-registration when the variant name is an
  existing tycon) and the matching codegen reshape (Option Y
  `__From_C` wrappers) are deferred — the current code paths
  cover the canonical end-to-end cases without them.

- **Direction-sensitive st_unify call site.** `bang_finish_result`
  was passing the inner result's error type as `expected` and the
  enclosing function's error type as `found`. Once `unify_heads`
  gained the D3 arm (which is asymmetric: only `found <:
  expected`), the order had to be swapped for the upcast to fire
  on `Result[Ci, T]!` inside a `Result[U, T]` body. Caught by
  reading the new code, not by a failing test. The other
  `st_unify` call sites already had the (expected, found)
  convention, so no other rewires.

## Spec ambiguities or interpretive choices

- **Narrowing-codegen Option X (structural) or Y (wrapped)?**
  Option X, scoped to "no resolver change, no new wrapper variant
  type." Why: under Phase 2 dual rep + Phase 3's `expand_narrow_arms`
  desugar, narrowing already covers both runtime paths (the
  parent-side zero-arg ctor *and* each inner variant of the
  component). The fan-out arms are the discriminator the brief
  describes. Wiring D3 upcast at the call site closes the
  construction case so callers can pass leaf-component values
  (`AccountNotFound`) and get them rebound at the component type
  inside the narrowing arm. The user-visible canonical fixture
  works end-to-end without Option Y.

- **Env threading: direct parameter, closure, global state?**
  Direct parameter on `unify`, `unify_list`, `unify_heads`. A
  back-compat wrapper `unify(t1, t2, s)` that defers to
  `unify_env(ty_env_empty(), t1, t2, s)` keeps the synth/hole
  callers (`synth_find_arg`, `synth_try_entry`) unchanged — they
  lose D3-upcast acceptance, which is fine: hole synthesis is
  best-effort and a missed upcast at synth time degrades
  candidate quality, not correctness. Effect-row label unification
  also passes `ty_env_empty()` (label ty_args don't participate in
  union subtyping).

- **Were Phase 3's deferred pieces well-defined?** Mostly yes, but
  the brief understated what "deferred" cost. The brief said the
  call-site upcast required env threading inside `unify_heads` —
  correct. The brief also said Phase 4 reshapes the runtime layout
  for narrowing — but the existing Phase 3 desugar plus D3 upcast
  was sufficient for the canonical fixture. I scoped this lane to
  the parts that move the user-visible needle without re-touching
  resolver / runtime layout.

- **`?` propagation interactions.** `bang_finish_result` already
  used `st_unify` for the error-type check. Once `unify_heads`
  gained the D3 arm and the call-order was corrected, propagation
  worked without further plumbing. No new `Cancel`/`Spawn`/`Actor`
  interactions surfaced.

## Subjective summary

- Confidence in correctness: **medium-high**. The unifier change
  is small and shape-bounded (only fires on TyCon-vs-TyCon with
  empty arg lists where the env's variant table records the
  upcast). All four new fixtures pass; selfhost is byte-identical
  on both backends; tier0/1/1-asan green. What is *not* exercised:
  Option Y wrappers (out of scope), the deferred resolver cleanup,
  generic unions (out of v1 scope per the brief), and the
  diagnostic-quality work for D3 mismatches (the negative
  `upcast_no_chain` fixture passes the existing diagnostic, which
  is not yet union-aware).

- Hardest sub-task: deciding which deferred pieces actually
  needed phase-4 work versus what was already plumbed by phase 3
  + a 1-line call-order fix. The brief expected ~150 lines; the
  net change is closer to ~30 + new fixtures. The discipline was
  about *not* expanding scope into Option Y / resolver reshape.

- Easiest sub-task: writing the new fixtures. The `?` propagation
  one (`upcast_question_propagation`) reads exactly like the
  spec's Decision-3 § example.

- Did the compiler help or hinder you? **Helped**: stage 2's C
  emitter caught the `unify_list` arity mismatch on the first
  selfhost attempt. The new diagnostics on `upcast_no_chain`
  surfaced cleanly. **Hindered**: stage 1 doesn't allow multi-line
  if-conditions, which is mildly annoying but well-flagged.

- Was Phase 3's retro report (`docs/lane-experience-issue-187-
  phase3-patmatch.md`) useful as a starting point? **Very.** The
  Phase 3 agent's notes on "what's deferred and why" pointed
  directly at the env-threading scope; the friction note on
  `[UnionInfo]` being dead code saved me from chasing it as the
  Phase 4 entry point.

- Was Phase 3's PR body's "Out of scope" section a clear handoff?
  Yes — it named exactly the three pieces (D3 in unify_heads,
  runtime layout, PNarrow codegen) and was honest that the third
  piece needed runtime-layout reshape. I ended up doing a subset
  of (1) and (2), and observing that (3)'s canonical-form coverage
  already worked under Option X.

## Limitations of this report

- Self-report bias acknowledged.
- Context truncation: counts and error lists exclude anything that
  fell out of my visible context window.
- Single agent (Claude). Not generalisable across LLMs.

## Raw build log

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T18:01:51-04:00	tier0	OK	-
2026-05-03T18:04:05-04:00	tier0	OK	-
2026-05-03T18:09:56-04:00	tier1	OK	-
2026-05-03T18:10:20-04:00	selfhost	OK	-
2026-05-03T18:10:57-04:00	selfhost-llvm	OK	-
2026-05-03T18:11:51-04:00	tier1-asan	OK	-
```
