# Lane experience — llvm-parity-cleanup (refs #622, #575)

Closes the cleanup lane of the LLVM↔C parity plan
(`docs/llvm-parity-plan-2026-05-26.md`). No codegen touched: pure
bookkeeping on `tools/backend-parity-skips.txt` plus the verification
that justifies each move. eric (systems review) recommended splitting
the flat skips file into bug / exempt / harness-categorization
sections; this lane does that and re-classifies two entries the #622
triage got wrong.

## Scope as planned vs as shipped

Planned: remove the stale `trace_basic` skip, exempt three
non-deterministic fixtures (`rb_tree_bench`, `m8_4_cancel_caught`,
`poker_dealer`), and split the file into three labelled sections.

Shipped: same structure, but the verification step changed *which*
fixtures land where. Of the three "nondet" candidates, only **one** was
actually nondet. The brief anticipated exactly this — "if either turns
out deterministic-wrong … it is a BUG, not nondet … do NOT exempt a
real bug" — so the surprise was budgeted for, not a scope blowout.

## Verification results (the load-bearing part)

Each candidate built cleanly on both backends, then ran multiple times
per backend to separate run-to-run nondeterminism from deterministic
divergence.

- **`trace_basic`** — C exit 0, LLVM exit 0, stdout byte-identical.
  The skip ("LLVM emit/link fails") was false; the #618/#704 emitter
  fix already closed it. **Unskipped** (removed from the file).

- **`poker_dealer`** — SURPRISE. 4 runs per backend, all stable
  run-to-run, and C == LLVM **byte-identical** (14 lines, `cmp` clean).
  The RNG seed is fixed and the dealing is deterministic, so there is
  no divergence at all. It was never nondet *and* it already passes.
  **Unskipped** (stale, same disposition as `trace_basic`), not
  exempted — exempting a fixture that passes would hide it from the
  harness for no reason.

- **`m8_4_cancel_caught`** — SURPRISE, and the important finding.
  4 runs per backend, both **stable** run-to-run, but C ≠ LLVM
  **consistently**. The fixture documents its own contract in the
  header: `Cancel.raise()` discards the continuation, so
  `Stdout.print("never reached")` must NOT execute and the `handle`
  value is 42 (the clause return), not 99 (the never-executed body
  tail). C is correct (`42`, no "never reached"). LLVM prints
  "never reached" AND returns 99 — it does **not** discard the
  continuation. This is a deterministic LLVM Cancel-handler codegen
  bug, NOT scheduler/observable-order nondeterminism. The #622 triage
  mis-filed it under "scheduler nondeterminism — parity-EXEMPT". This
  lane **leaves it skipped under #622** with a corrected, explicit
  reason — it is a real bug for a follow-up lane.

- **`rb_tree_bench`** — confirmed timestamp-only: full diff is one
  line (`elapsed: 3.808s` vs `3.142s`); identical modulo that line.
  Genuine wall-clock nondeterminism. **Exempted** under
  `exempt-nondet`.

So: of 3 nondet candidates, 1 genuinely nondet (`rb_tree_bench`),
1 already passing and stale (`poker_dealer`), 1 a real deterministic
bug (`m8_4_cancel_caught`).

## File structure chosen

Three labelled sections inside the existing
`tools/backend-parity-skips.txt` — no new file. eric's plan suggested
two physical files (`parity-bugs.txt` / `parity-harness-skips.txt`),
but the harness reads exactly one path and keys only on the
`^<path>:` prefix; splitting into two files would have meant a harness
change for marginal benefit. Comment-header sections give the same
clarity within the parser's existing contract:

1. `=== parity bugs (must empty before Orongo) ===` — the real
   #622/#618 LLVM divergences.
2. `=== parity-exempt: non-determinism ===` — `rb_tree_bench` only,
   marked `exempt-nondet`.
3. `=== harness categorization (#626 / #357) ===` — unchanged content,
   now explicitly labelled as a suite-membership concern (these fail on
   both backends identically; not parity bugs).

## Harness parser tweak

None. `is_skipped()` does `grep -q "^${fixture}:"`, so comment headers
and the `exempt-nondet` marker in the issue-number slot both parse as
before. I considered the inline `// skip-backend-parity` annotation for
`rb_tree_bench` (the file header offers it for by-design divergence),
but `//` is not a valid kaikai comment (kaikai uses `#`) and the
annotation must sit on the fixture's first line — it would break the
parse. So the exemption stays in the skips file. Updated the file's
header block to document the `exempt-nondet` marker and the three
sections.

## Real-bug inventory after this lane

`process_basic`, `m8_4_cancel_caught` (newly correctly classified),
`issue_107_signal_trap`, `m8_fiber_stack_overflow`, `r9_clause_capture`,
`rc_discipline_record_variant`, `demos/spiral` — **7 lines**. The brief
expected 6; the 7th is `m8_4_cancel_caught`, promoted from the (wrong)
nondet bucket. That promotion is the lane's substantive finding.

## Fixtures added / coverage gaps

No new fixtures — all four candidates already existed. The lane's
output is the corrected classification, not new coverage. Coverage gap
worth noting for a follow-up lane: the Cancel-handler continuation-drop
bug (`m8_4_cancel_caught`) has no minimal repro fixture beyond itself;
the fix lane should add one.

## Cost vs estimate

Roughly as estimated — the only cost overrun was re-running the three
candidates enough times to be confident the divergence was (or was not)
order/seed/clock. That confidence is what turned a "exempt 3" task into
"exempt 1, unskip 1, keep 1 as bug".

## Harness state observed (not introduced by this lane)

Running the full harness after the edit surfaced 4 divergences that
are NOT in the skips file and were NOT touched by this lane (only the
.txt + this retro changed; `git status` confirms two files). They were
verified pre-existing by re-running the harness against the HEAD skips
file (identical failure set). Logged here for the next parity lane:

- `issue_141_log_default` — diverges ONLY on the log timestamp
  (`01:39:57Z` vs `01:40:00Z`); same wall-clock-nondet class as
  `rb_tree_bench`. Candidate for `exempt-nondet`, but out of this
  lane's scope (it was never skipped, so adding it is new triage, not
  cleanup).
- `issue_682_cancel_sibling_handler` — LLVM drops `worker: cancelled
  cleanly`. Same family as the `m8_4_cancel_caught` Cancel-handler bug.
- `examples/packages/auto_install/main.kai` — C build fails (missing
  `greet` module); a #626-class harness/package-setup gap, not parity.
- `examples/packages/cross_package_effects/consumer/main.kai` — LLVM
  `array_get: not an array`, exit 1 vs C 0. Real LLVM bug.

These are pre-existing debt the harness already reported; the cleanup
lane neither caused nor (per scope) fixed them. Confirmed by running the
harness against the HEAD (pre-edit) skips file via `git stash`: the
failure set was identical (plus a flaky `demos/blackjack` timestamp
diff), and `trace_basic` / `poker_dealer` appear in neither version's
failures. The lane's edit introduced no new divergence — both unskipped
fixtures pass.

Process note: running two parity harnesses + tier0 in parallel while
`git stash`-ing the skips file caused a double-`stash pop` (the
comparison script popped its own stash, then a manual pop applied an
unrelated `main`-branch stash, leaving 4 files in conflict). Recovered
by `git checkout HEAD --` on the non-mine files; the `.txt` edit was
never lost. Lesson for the next lane: do not `git stash` a working file
while background jobs may also stash, and run the gate serially.

## Follow-ups left

- Fix the LLVM Cancel-handler continuation-drop bug
  (`m8_4_cancel_caught` + `issue_682_cancel_sibling_handler` share the
  shape) — tracked under #622, no new issue filed.
- Triage the 4 pre-existing harness divergences above for the next lane.
- The remaining #622 bugs are untouched by this lane (cleanup only).
