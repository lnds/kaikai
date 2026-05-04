# Lane experience — issue-205-ufcs

## Objective metrics

- Start: 2026-05-03T21:21:41-04:00
- End: see end timestamp file
- Wall-clock: ~30-40 min
- Build/test invocations:
  - `make tier0`: 1 (OK)
  - `make selfhost`: 1 (OK)
  - `make tier1`: 1 (in flight at draft time)
  - `make test-ufcs`: ~3 (1 build of fixtures + 2 reruns after diagnostic refinement)
  - Direct `make kaic2`: ~5 (typer iteration)

Append the build TSV at the end of this doc.

## Compiler errors I encountered

### 1. `match` arm on `Bool` is not idiomatic

I wrote `match cond { true -> A false -> B }` — the existing codebase uses `if cond { A } else { B }` and never matches on Bool. Stage1 may or may not accept it; I rewrote to `if/else` proactively. **Cost**: ~30s.

### 2. `..._` (anonymous tail bind) might not be valid

I used `[first, ..._]` as a list-cons pattern with anonymous tail. The codebase uses named binds like `[h, ...rest]` or `[..._t]` (underscore-prefixed name). I switched to `..._rest`. Stage1 might accept the bare `..._` but I didn't probe. **Cost**: ~30s.

### 3. UFCS dispatch fired but emitter still ran old path — the cwd-cache trap

After implementing UFCS, the smoke compiled cleanly but the emitted C still contained `kai_op_field(kai_r, "result_is_ok")` instead of `kai_result__result_is_ok(kai_r)`. I spent ~3 min suspecting the dispatch wasn't firing at all. The actual cause: my smoke binary at `/tmp/ufcs_run` was a STALE artifact from a prior session — running `cc` didn't overwrite it because the C compile was failing with `runtime.h not found` (missing `-I`), which my Bash tool's cwd-reset behavior masked. The C output was correct from the first dispatch fire; I was running an old binary. **Cost**: ~3 min, mostly suspicion / debugging.

### 4. `result_is_ok` not in env when `--prelude` invoked from project root

When I tried to test the smoke from the project root with `--prelude stdlib/core/result.kai` etc., the typer reported `result_is_ok` as missing. The same flags worked when invoked from `stage2/` with `../stdlib/...` paths. There seems to be a path-resolution interaction with `--path stdlib` that requires the working directory to match. **Workaround**: invoke from `stage2/`. **Cost**: ~5 min, mostly variable-expansion debug (zsh vs bash word-splitting on `$PRELUDES`). Not in scope to fix; documented for the next lane.

### 5. Pre-existing Perceus bug: `let r = …; r!` panics

My initial `postfix_interaction.kai` used `let r : Result[String, Int] = Ok(42); let v : Int = r!`. The emitted C calls `kai_decref(kai_r)` BEFORE evaluating `r!`'s body, causing a use-after-free that surfaces as a runtime "non-exhaustive match" panic from the next match. The bug is in Perceus, not UFCS. **Workaround**: use an inline call expression as the receiver (`produce().result_map_err(...)!` instead of `let r = …; r.method(…)!`). The fixture documents this in its header. **Cost**: ~5 min to localize, decided not to chase.

## Friction points

### Did `head_module_for` from #207 cleanly extend?

**No — it wasn't extended at all.** On reading #205's lookup precedence, I expected to plug into `head_module_for`. But after reading the body more carefully, the realistic dispatch path for `r.result_is_ok()` is precedence rule #3 ("function in current scope with matching first-arg type"), NOT rule #2 ("function in receiver type's declaring module via `head_module_for`"). The reason: stdlib functions like `result_is_ok` are top-level `pub fn` decls, indexed by bare name in `ty_env`, NOT keyed under a `result.` module namespace yet. That migration is part of #203 (qualified-call rename milestone), which the issue body explicitly says #205 lands BEFORE.

So `head_module_for` infrastructure stayed untouched. UFCS dispatch is a direct `ty_env_lookup(env, ident)` followed by first-param unification — much simpler than I expected. The hard mental model wasn't the dispatch; it was realizing the head-module hop wasn't useful yet.

### Did Q3 over auto-declared types need a design call?

**No.** The implementation falls out for free. `head_module_for` doesn't need to know about `Red` because we don't go through that path — we go through ty_env scope lookup, which indexes every `pub fn` regardless of receiver origin. As long as `paint(Color)` is in scope and `Red` unifies with `Color` (which the existing union-upcast logic already handles), `red_value.paint()` resolves. The fixture confirms it.

### Did the diagnostic format need iteration?

Slightly. I started with a long-form diagnostic listing scope candidates (Q4-style "did you mean" hint with all candidates whose first arg unifies). I dropped the candidate-listing because:
1. Iterating env entries to filter by first-arg-unify is O(N) per failed call. The lane is small enough; perf is fine. But:
2. The candidate-listing requires a stable formatter for each scheme, which doesn't exist as a single helper. I would have had to add one or compose 3-4 existing ones. Out of scope.

The shipped diagnostic is the simpler form: name the receiver type, hint at first-param check + spelling. This matches what the issue body's mock-up looked like minus the candidate enumeration. Good enough; #203 will make this cleaner because then candidates have qualified names.

## Spec ambiguities or interpretive choices

### Q1 chaining: zero implementation cost

Q1 ("`a.f(x).g(y).h(z)` parses left-associative") was already true at the parser level via `parse_postfix_rest`. UFCS doesn't change parsing, only semantics, so the fixture is purely a regression-lock against future precedence tweaks.

### Q2 postfix interaction: `?` doesn't exist in stage 2

The issue body asks for both `?` and `!` postfix tests. Stage 2 only has `!`. The fixture exercises `!` and notes `?` is deferred until the operator lands.

### Q5 partial application: I made `synth_field` strict

The original `synth_field` for non-record receivers silently returned `TyAny`. With UFCS in place, this is wrong — `r.method` outside a call site should be a typed error to keep the rule local to call sites (issue's recommended decision). I added a Q5 diagnostic: when `synth_field` sees a non-record receiver AND `ident` is in env as a function, it emits `"X is a function in scope, not a field"`. When `ident` is NOT in env, the legacy silent TyAny stays (preserves cascade behavior).

This is mildly broader than the spec asked for (the spec only required Q5 to error; it didn't require detection that the name resolves to a function). My version makes the error more actionable.

### Field-vs-UFCS precedence edge case

`record_has_field` checks if the receiver's type is a TyCon resolving to a record AND the field exists. If both, return None (defer to standard field-access path). If field doesn't exist, fall to UFCS. This is the issue's Constraint #1.

Edge case I considered but didn't ship: a record with a callable field (function-typed). Today, `record.callable_field()` works because the existing `synth_field` returns the field's type and `synth_call` unifies it with `(args) -> ret`. UFCS is bypassed — field always wins. The `field_wins_over_function.kai` fixture covers the simpler case (String field with same name as a free function); the callable-field case isn't its own fixture but is tested implicitly by every existing record-callable-field call site (e.g., the Stdout effect handlers).

## Subjective summary

- **Confidence**: high. The change is small (~120 lines net in compiler.kai), local to the synth_call dispatch, doesn't touch emit/perceus/runtime. Selfhost byte-identical confirms zero ripple.
- **Hardest**: the cwd-cache trap (item 3 above). The actual UFCS implementation took 30 min; debugging the test harness took longer than that.
- **Easiest**: extending `synth_call` with another `try_*` helper. The pattern is well-established (`try_resume_call`, `try_op_call`); UFCS just slots in.
- **Compiler help/hinder**: the existing `synth_pipe_dispatch` (#207) was a near-perfect template — I copied its open-row + expected-row pattern verbatim. Without that template, I'd have had to learn the row-tracking machinery from scratch. **Help: 9/10.**

## Limitations of this report

- I didn't run tier1-asan; the issue spec says it's path-gated CI, not local-required. tier0 + tier1 + selfhost is the local gate per CLAUDE.md.
- I didn't measure dispatch performance (no allocations changed; pure typer-time). Worth confirming via an alloc-count probe if perf matters in a future lane.
- Q4's "list candidates whose first arg matches" is implemented as a single hint string, not a structured enumeration. A future lane with proper symbol-table iteration could enrich this.
- `head_module_for` is unused by UFCS in this lane. When #203 migrates stdlib to qualified module names, UFCS dispatch should add a precedence-#2 path that uses `head_module_for` to disambiguate. That's deferred.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-03T21:45:42-04:00	tier0	OK	-
2026-05-03T21:48:24-04:00	selfhost	OK	-
2026-05-03T21:52:55-04:00	tier1	OK	-
2026-05-03T21:54:08-04:00	tier1	OK	-
2026-05-03T21:54:15-04:00	selfhost-llvm	OK	-
2026-05-03T21:54:58-04:00	selfhost-llvm	OK	-
2026-05-03T21:56:26-04:00	tier1-asan	OK	-
```

End: 2026-05-03T21:56:26-04:00. Wall-clock: ~35 minutes.
