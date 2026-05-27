# Lane experience — LLVM handler clause-capture env rebind (Cluster E/F, refs #622)

## Scope as planned vs as shipped

**Planned.** Close the `r9_clause_capture` parity divergence: under LLVM the
fixture exited 1 with two symptoms — captured `String`/`Char` values rendering
corrupted in clause bodies (`[P]` → `[[`, `svc/info` → `/`) and a continuation
"resumed twice" crash. The brief flagged the double-resume as *possibly
structural* (a continuation-lifecycle RC bug in `runtime_llvm.c`) and asked the
lane to determine whether symptom #2 was independent of #1, consult asu/linus if
the continuation fix touched load-bearing effect-RC, and otherwise ship the
localized emit fix.

**Shipped.** Exactly the localized emit fix, in one file
(`stage2/compiler/emit_llvm.kai`), one helper family mirrored from the C
backend. **The double-resume was a CONSEQUENCE of the capture corruption, not an
independent bug** — fixing #1 made #2 vanish with no change to the runtime or to
`resume` lowering. No structural work; no asu/linus consult needed.

## The two symptoms, and why #2 was a consequence of #1

The brief's framing was that there might be *two* faults: a wrong env-slot
read (#1) and a one-shot-resume lifecycle bug (#2). Live triage refuted the
split. Both stem from a single gap.

### Root cause (#1) — the LLVM clause body never rebound captures

The C backend threads a handler clause's enclosing-fn captures (`prefix`, `tag`,
`level` in the fixture) through the per-handle evidence env (`EvE.env`), per
`docs/effects-impl.md` §"Op clause as ordinary function":

- **Install side** (`emit_handle` / `emit_handle_env_alloc`): allocate a
  per-handle env struct, store each captured local into its field, stash a
  `void *` to it in `_ev.env`.
- **Read side** (`emit_clause_env_prologue`): cast `self->env` back to the
  struct type and rebind each `kai_<name>` as a clause-local so the body's
  references resolve.

The LLVM backend implemented **neither half**. `llvm_emit_handle` always wrote
`store i8* null` into the EvX `env` field (struct field 1), and
`llvm_emit_clause_body` discarded the `ClauseInfo` `caps` field outright (the
match arm bound it to `_` with a standing comment "the LLVM clause body still
ignores it … Mirror is a follow-up"). So when the clause body referenced
`prefix`, the emitter found no local, fell through `llvm_emit_expr`'s EVar path
to `llvm_emit_fn_value`, failed to resolve `prefix` as a prelude/EFn symbol, and
emitted **`undef`** plus the diagnostic `llvm: cannot build closure for 'prefix'`.
The `undef` `%KaiValue*` fed into `kaix_*` string concat produced the corrupted
`[[` / `/` output.

This is the **identical failure shape** as the nested-record-pattern lane
(#713): an unbound name → `cannot build closure for X` → `undef` operands →
garbage. Same family, different binder source (clause env vs let-destructure).

### Symptom #2 (double-resume) — a downstream cascade, not a runtime bug

With the capture set to `undef`, the clause body's control flow was operating on
garbage values. The "continuation resumed twice (handler #1)" runtime abort was
a *consequence* of feeding `undef` through the clause — not a defect in the
LLVM lowering of `resume` and not a one-shot-RC bug in `runtime_llvm.c`. The
moment the env rebind was wired (so `resume(())` ran on a well-formed clause),
the double-resume disappeared. Verified: the fixture exits 0 with output
byte-identical to the C oracle; `resume` lowering and `runtime_llvm.c` were
never touched. The brief's "possibly structural" branch did not fire.

## The fix

All in `stage2/compiler/emit_llvm.kai`, mirroring `emit_c.kai` (per this file's
138-helper mirror discipline — the C originals stay untouched, proven by
selfhost byte-identity):

1. **Mirror helpers** (`lvm_merge_string_set`, `lvm_handle_captures_at`,
   `lvm_handle_enc_fn_at`, `lvm_handle_env_struct_name`, `lvm_list_filter_in`):
   compute the cross-clause capture union and the stable field order, identical
   to the C side so install and read agree on layout.
2. **Env-struct named types** (`lvm_handle_env_typedefs`): one
   `%_kai_<enc>__env_<l>_<c> = type { %KaiValue*, … }` per capturing handle,
   emitted at module top alongside the `%EvX` structs.
3. **Install side** (`llvm_emit_handle`): replace `store i8* null` in EvX field
   1 with — when the handle captures — an `alloca` of the env struct, one
   `store` per captured local (filtered to names in scope via
   `llvm_local_names` + `lvm_list_filter_in`), a `bitcast` to `i8*`, and the
   pointer store.
4. **Read side** (`llvm_emit_clause_body` + `lvm_emit_clause_env_prologue` +
   `lvm_emit_clause_env_reads`): GEP EvX field 1, load the `i8*`, cast to the
   env-struct pointer, and for each capture GEP+load+`kaix_internal_dup` (the
   dup matches the C `kai_internal_dup` — each clause body is Perceus-analysed
   in isolation and may consume its captured locals; the env holds a borrow),
   then `llvm_push_local` under the surface name.

The EvX ABI was already `{ i64 handler_id, i8* env, %KaiValue* state, …fnptrs }`
(confirmed against `stage0/runtime.h` `KaiRtEvCancel` prefix) — field 1 was the
pre-existing `env` slot, previously always null. No struct-shape change.

## Structural surprises

- **The mirror discipline made this cheap.** Because the C backend's
  clause-capture machinery (#60 / R9) was already correct and the LLVM file
  documents the "copy with `lvm_` prefix" convention, the fix was a faithful
  transcription, not a design. The one LLVM-specific twist: C uses a named
  `typedef struct`, LLVM a named `%type` — same field order, same symbol stem.
- **The handler-id sentinel (`__alias_id__<a>`) needs no special read.** In the
  C lambda path it is boxed/unboxed; in the clause env path `llvm_emit_op_dispatch`
  already expects the sentinel local to be a boxed `%KaiValue*` and unboxes it
  itself, so `lvm_clause_cap_bind` just re-pushes the boxed register under the
  same sentinel name. The fixture has no aliases, so this path is untested by
  r9 — kept parity-faithful with C's install-before-alias ordering (the sentinel
  is not yet in scope at env-fill time on either backend; a clause capturing an
  aliased op is a latent gap shared with C, out of this lane's scope).
- **An unused duplicate typedef is emitted.** `lc.clauses` carries both the
  pre-mono (`make_logger__env_…`) and post-mono (`make_logger__mono__Any__env_…`)
  ClauseInfo, so `lvm_handle_env_typedefs` emits a typedef for each. Only the
  mono one is referenced; clang accepts the unused named type. Harmless; left
  as-is to keep the helper a pure mirror of `emit_handle_env_typedefs`.

## Fixtures + coverage

- `examples/effects/r9_clause_capture.kai` is the coverage — unskipped (its line
  removed from `tools/backend-parity-skips.txt`). It already exercises both
  shapes: single-capture (`make_logger`, one `prefix`) and two-capture
  (`make_tagged_logger`, `tag` + `level`), validating field order
  (`svc/info`, not `info/svc`). The minimal isolation did **not** differ from
  the existing fixture, so no new fixture was added (per brief).

## Verification

- Fixture: stdout + exit **identical** on C and LLVM (`diff` clean, matches
  golden). Run 3× each — stable.
- `make selfhost`: **byte-identical** (`kaic2b.c == kaic2c.c`) — proves the C
  backend is untouched.
- `make tier0`: green (selfhost deterministic, demos baseline 34 holds).
- `make tier1`: green.
- `make tier1-asan`: green (the double-resume was a use-after-free risk; ASAN's
  domain — clean confirms #2 is genuinely gone, not masked).
- `tools/test-backend-parity.sh`: `r9_clause_capture` now passes; no **new**
  divergence introduced by this lane (verified by stashing the change and
  reproducing — see below).

### Pre-existing parity divergences surfaced (NOT this lane)

The parity sweep reported three FAILs that are **not** in the skip file and are
**pre-existing** — reproduced identically on a clean HEAD compiler with this
lane's change stashed:

- `examples/effects/issue_141_log_default.kai` — false positive: the only diff
  is the wall-clock timestamp in the log lines (`04:55:56` vs `04:55:59`).
  Clock nondeterminism; belongs in the exempt-nondet section.
- `examples/effects/issue_682_cancel_sibling_handler.kai` — LLVM deterministically
  drops `worker: cancelled cleanly` (a cancel-handler codegen gap, same family
  as the already-skipped `m8_4_cancel_caught`). Reproduced on clean HEAD.
- `examples/packages/auto_install/main.kai` — C build fails ("cannot open module
  'greet'"); a #626-class harness/package gap, fails on both backends.

These are a skip-file maintenance gap from a prior lane, out of this lane's
scope (lane discipline: one worktree fixes one thing). Flagged here for the
integrator; not fixed inline.

## Cost vs estimate

Faster than budgeted. The brief allowed for a structural continuation-RC
investigation with an asu/linus consult; in practice the diagnosis (`cannot
build closure` → env channel unimplemented) pointed straight at a C-mirror
transcription, and #2 fell out of #1 with no runtime work. ~1 file, ~6 helpers
added, no runtime edits, no struct-ABI change. Single PR, atomic commits.
