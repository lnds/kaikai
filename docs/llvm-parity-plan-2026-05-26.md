# LLVM↔C backend parity — triage + plan (2026-05-26)

Status: planning. Supersedes the cluster hypotheses in issue #622.
Reviewed by asu (language architect), linus (pragmatism), eric (systems design).

## Why now

LLVM is the definitive backend for Orongo; the C backend
(`stage0/runtime.h`) is the maturity oracle. Every LLVM-only divergence
is a site where the production backend is wrong and the reference is
right. The `tier1-backend-parity` gate (#575) is diagnostic, not a
required check — so these don't block merges, but they are load-bearing
for LLVM being trustworthy at 1.0. The stage-2 modularization (#677
phase 1, 24 modules) was pure source-split; codegen untouched, so the
divergences persist byte-identical — confirmed by reproducing each.

`tools/backend-parity-skips.txt` lists ~21 fixtures under #622 + 2 under
#618 + a large #626 block (fixture-categorization, not parity bugs).

## Empirical triage (reproduced fixture-by-fixture, 2026-05-26)

The #622 umbrella hypothesized "11 segfaults = default-handler gap" and
filed the Unicode/JSON failures as separate. Live triage **corrects
both**. The real clusters by root cause:

### Cluster A — Char→String dynamic render diverges (HIGH impact, one cause)

`int_to_char(n)` interpolated via `"#{c}"` produces garbage bytes only
under LLVM. Path: `kai_to_string(KAI_CHAR)` → `Show for Char`.

- `A`(=0x41) renders `'1'`; `café`→`'caf1x'`; `ñ`→`'1b'`; `😀`→`'0532'`.
- Fixtures: `json_basic`, `json_surrogate_decode`, `json_surrogate_encode`
  (#618), `r9_clause_capture` (char `P` lost → prints `[[` not `[P`).
- The static literal `EChar('A')` at `emit_llvm.kai:1184` emits
  `int_to_string(char_to_int(c))` which is **correct** for raw unboxed
  Char; not the literal.
- **ROOT CAUSE (refined 2026-05-26, supersedes the runtime hypothesis):**
  it is NOT the dynamic runtime render — `kaix_to_string` forwards to the
  shared `kai_to_string`, identical on both backends. The bug is the
  **emitter dropping `\xNN` hex escapes in string literals.**
  `llvm_encode_body_loop` calls `decode_kai_escape`, which has no `\x`
  case, so `"\x41"` emits bytes `x`,`4`,`1` instead of `A`. The C
  backend emits the literal verbatim and the C compiler decodes `\x41`.
  One-line repro: `"\x41\x42"` → C `AB` (len 2), LLVM `x41x42` (len 6).
  The JSON garbling is downstream: `json_byte_table()` in
  `stdlib/encoding/json.kai` is a `"\x01..\xff"` literal that
  `json_utf8_encode` slices — mis-encoded under LLVM corrupts every
  decoded codepoint. asu's "two sub-bugs" guess was wrong: **one cause,
  one patch** (add `\xNN` decoding to the emitter, advancing the loop
  cursor by 4 not 2).

### Cluster B — default-handler null deref in kai_main (NARROWED to 4)

Segv in `kai_main+40..52` dereferencing 0x0 before `kaix_cont_init_identity`,
before any output. LLVM `kai_main` does not install the user effect's
default handler.

- `issue_558_user_effect_default_main_install` (`kai_main+40`)
- `m7a_7_default_file` (`+48`), `m7a_7_default_stdin` (`+48`)
- `process_basic` (`kai_main+52`)

**The #622 "11 segvs" was wrong** — only these 4 crash in `kai_main`.
The other 7 split into C, E below.

### Cluster C — stack overflow corrupts heap / fiber stack sizing (LLVM)

Two sub-signatures:

- `m7a_7_default_env` prints literal `kai: fiber stack overflow`, exit 139
  (runtime detects + aborts).
- `m8_fiber_stack_overflow` exit 138 SIGBUS (hardware detects, runtime
  doesn't — missing guard page?). Check `.out.expected`: this fixture
  may be designed to overflow.
- `unbox_bench`, `unbox_bench_real` crash in `libsystem_malloc`
  (`_xzm_free`, fault address in the **stack** region, write) — a stack
  overflow corrupting the malloc arena. Same family: LLVM fiber stack
  sized smaller than C, or frame layout differs.

C runs all of these clean (larger/correct stack).

### Cluster D — emit produces invalid LLVM IR (build-fail, CHEAPEST)

Oracle is `clang` accept/reject; no runtime semantics. Two independent
emitter bugs:

- `reuse_record_basic`: `%t6626 = phi %KaiValue* [ %t6622, %entry ]` →
  "use of undefined value '%entry'". Perceus-reuse lowering emits a phi
  referencing a basic block that doesn't exist in that function.
- `trace_prefix`: `redefinition of global '@eff.disp.name.6865'`
  (constant `"Stdout\00"`) → effect-dispatch global names not
  deduplicated when the same effect name appears twice.

### Cluster E — stale skip + read-back/variant crashes (mixed, LOW)

- `trace_basic` **already passes** (build OK + stdout identical). Stale
  skip — remove it.
- `rb_tree_bench` crashes in `kai_variant_u` (unboxed-variant construct,
  not a handler).
- `os_env_mutate` crashes in `kai_read_back+44` (RC/unbox read-back path).
- `stdin_read_bytes` needs stdin the harness doesn't provide — likely a
  #626-class harness gap, not a parity bug. Re-file under #626.

### Cluster F — heterogeneous exit-code diffs (NOT one cause; one is a C bug!)

The reviewers assumed "RC silenced by LLVM". Live triage **refutes** it:

| Fixture | `.out.expected` | C | LLVM | Verdict |
|---|---|---|---|---|
| `passthrough_let_dup_drop` | `0` (exit 0) | panic "non-exhaustive match", exit 1 | `0`, exit 0 | **C is wrong, LLVM correct** |
| `rc_discipline_record_variant` | `38` | `38`, exit 0 | "type mismatch in +", exit 1 | LLVM unbox/RC feeds wrong type to `+` |
| `demos/spiral` | grid | grid, exit 0 | "array_set: index 25 out of range (len=16)", exit 1 | LLVM Array bounds/indexing bug |

`passthrough_let_dup_drop` **breaks the "C is the oracle" premise**: the
golden says exit 0, C panics, LLVM is right. This is a C-backend match
exhaustiveness codegen bug — possibly higher urgency than LLVM parity.

### Scheduler nondeterminism — parity-EXEMPT, not fixes

- `m8_4_cancel_caught` (fiber-cancellation observable order)
- `demos/poker_dealer` (RNG seed / scheduler nondet)

Per Go/Erlang precedent, forcing byte-exact stdout across two schedulers
turns an implementation property into a spec property. These move to a
`parity-exempt` category with a documented reason — **after** a fixed-seed
run confirms the divergence is nondet, not a hidden bug.

## Plan — 6 lanes, order D → A → B → C → (E+F triage) → exempt

Unanimous reviewer consensus: **D first** (build-fails are cheapest,
isolated to the emitter, oracle is clang, and they unblock clean signal
before touching runtime). Then A (highest impact, also unmasks B's
stdout). Then B, C. Then triage E/F. Each lane: one root cause, adds a
fixture, removes its skip lines, re-runs `tools/test-backend-parity.sh`,
writes a retro.

1. **Lane D — invalid IR.** reuse_record phi-to-missing-block +
   trace_prefix global dedup. Verify with clang. ~2 patches in emit_llvm.
2. **Lane A — Char→String runtime.** Fix `kai_to_string(KAI_CHAR)` in
   runtime_llvm.c to match runtime.h. Closes #618. Repro: surrogate_decode.
   Expect ASCII + UTF-8 sub-fixes.
3. **Lane B — default-handler in kai_main (LLVM).** 4 fixtures. Re-triage
   after A (A may change B's now-observable stdout).
4. **Lane C — fiber stack sizing / guard page (LLVM runtime).** env +
   m8_fiber_stack_overflow + unbox_bench{,_real}. Deepest; post A/B.
5. **Lane E/F triage.** Remove trace_basic skip. Diagnose rb_tree_bench
   (variant_u), os_env_mutate (read_back), the 3 Cluster-F exit diffs.
   **File the C-backend match-exhaustiveness bug as its own issue.**
6. **parity-exempt category.** Fixed-seed run on poker/cancel; if nondet
   confirmed, move to an exempt list with reason.

## Structural follow-ups (eric)

- **Symbol-coverage script in tier0** (~50 lines): extract `kai_*` from
  runtime.h + `kaix_*` from runtime_llvm.c, fail if a `kai_` helper lacks
  its mirror. Detects shim drift at the commit that introduces it. The
  root cause of clusters A/B/E is the hand-written `runtime_llvm.c`
  mirror drifting from `runtime.h`.
- **Split the skips file**: `parity-bugs.txt` (clusters A–F, must empty
  before Orongo) vs `parity-harness-skips.txt` (#626 stable: negative
  tests, aspirational, multi-package, no-main). Today's flat file
  conflates implementation debt with suite-categorization noise.
- Audit `emit_llvm.kai` for every "emit inline instead of calling a
  `@kaix_` wrapper" decision — each is a divergence candidate (the
  mechanism behind Cluster A).
