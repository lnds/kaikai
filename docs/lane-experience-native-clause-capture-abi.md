# Lane experience — native clause-capture/evidence ABI

## Scope as planned vs as shipped

**Planned (brief):** resolve the `clause-param-origin` family of the
native (in-process libLLVM) backend at the root — the full
capture/evidence ABI for handler-clause params (`PCapture`/`PEvidence`),
closing the 7+ build-failed gaps that depend on it
(`ctor_field_effect_row`, `stream_abort_leak`, `stream_mock_disk`,
`stream_skip_policy`, `r9_clause_capture`, `net_dns_resolve`,
`cross_package_effects/consumer`). The known trap: naïvely reusing the
lambda-thunk `kaix_capture(self, j)` makes the fixtures COMPILE but emit
WRONG output (`r9` printed `[]` where C printed `[P]`), because a clause's
`self` is the dispatched `Ev<Eff>` blob, not a closure env.

**Shipped:** the capture half of the ABI, in full and general. 6 gaps
closed byte-identical against the C-direct oracle (45 → 39 macOS; measured
pass=425 fail=37 skip=55 of 517 file-mode fixtures, **0 regressions**):
`ctor_field_effect_row`, `r9_clause_capture`, `stream_abort_leak`,
`stream_mock_disk`, `stream_skip_policy` (file-mode) +
`cross_package_effects/consumer` (package-mode, verified byte-id manually).
`net_dns_resolve` is NOT closed — it turned out to be a SEPARATE root (see
below), documented and left for a follow-up rather than forced. The
`PEvidence` (by-id alias) half was confirmed NOT needed by any of the 7
targets and is left aborting loud, as the brief's subset boundary intended.

## The root cause

The native install (`nemit_install_with`, `emit_native_fx2.kai`) stamped
the Ev blob's `handler_id` (field 0), `state` (field 2, byte 16), and the
op fn-ptrs (fields 3+), but **never filled `env` (field 1, byte 8)**. So a
handler clause that captured an enclosing binding had nowhere to read it
from — `nemit_seed_clause_loop` hit the `_` arm and aborted with `clause
param origin (alias = subset 2b alias dispatch)`. The whole `EvE.env`
channel the C-direct backend threads (`emit_handle_env_alloc` →
`_ev.env = &_env` → clause prologue `kai_internal_dup(_env->kai_<name>)`)
simply did not exist on the native side.

## The trap the brief warned about — and the SHARPER one underneath it

The brief's documented trap (clause `self` ≠ lambda `self`) is real, but
diagnosing it surfaced a second, subtler ordering hazard that the canonical
fixture `r9_clause_capture` is **false-green** for.

A clause capture indexes by NAME in emit_c (`_env->kai_<name>` — the
env-struct field order only has to agree between install and read, both of
which use `handle_captures_at`). The native backend has no nominal struct
in IR; it indexes the env array by INTEGER position. So the index a clause
uses MUST be the capture's slot in the handle's cross-clause union
(`handle_captures_at`), NOT its position in that clause's OWN capture list.

`r9`'s `make_tagged_logger` has two clauses — `log` captures `[tag, level]`,
`checkpoint` captures `[tag]` — but `tag` sits at union slot 0 in BOTH, so
a per-clause index and a union index coincide by accident. A backend that
used the per-clause index would pass `r9` and still be wrong for any clause
that captures a strict subset starting past slot 0. The
`clause_capture_subset_order.kai` fixture (added this lane) forces them
apart: `loud` captures `[prefix, suffix]`, `mark` captures only `[suffix]`,
so `suffix` is at union slot 1 and `mark`'s lone capture must resolve to
slot 1, not its per-clause slot 0. The C oracle is the truth; native must
match. (asu flagged this precisely; the LLVM-text backend — `emit_llvm.kai`
line 314 — independently confirmed the union-index semantics by reading
`lvm_handle_captures_at(cls, line, col)`, never the per-clause caps.)

## The fix

A KIR-side split plus a native install/read pair plus three additive
runtime shims:

1. **`PEnvCapture(Int)`** — a NEW `KParamOrigin` constructor
   (`kir.kai`), distinct from the lambda `PCapture(Int)`. The two read
   from DIFFERENT physical locations (`self->env[j]` at byte 8 vs
   `self->as.clo.captures[j]`) and index DIFFERENT lists (the handle
   union vs the closure capture order). Encoding the address in the
   constructor — not inferring it from `KFnKind` at the backend — mirrors
   the existing `PEvidence`/`PResume` separation (asu's call: do NOT
   overload `PCapture`, the churn is one arm in dump/perceus and the
   clarity is permanent).

2. **`KHandlerDecl.env_caps: [String]`** — the cross-clause capture union
   (`kir_handle_captures_at`, mirror of emit_c's `handle_captures_at`),
   resolved once at lowering so install (env fill) and read (env index)
   agree by construction. `lower_clause_fn` now assigns `PEnvCapture(j)`
   with `j` = the capture's index in this union, threading the whole
   program's clause list so each clause sees its handle's full union.

3. **Native install** (`nemit_install_env`): alloca a `[16 x ptr]` env
   array in the entry block (the "allocas in entry only" rule; 16
   over-covers any core handle), fill each union slot from the install
   frame's local register for that surface name (a plain BORROW copy, no
   dup — the dup lives on the read side, exactly as emit_c's
   `_env.kai_<c> = kai_<c>`), then `kaix_ev_set_env(ev, env)` points Ev
   field 1 at it. No captures => Ev field 1 stays NULL.

4. **Native clause prologue** (`nemit_seed_clause_envcap`):
   `kaix_clause_env_get(self, j)` reads `self->env` (byte 8), indexes
   slot `j`, returns it dup'd — the read side of emit_c's
   `kai_internal_dup(_env->kai_<name>)`. The dup is MANDATORY (asu, and
   verified): the native path runs the same per-clause Perceus, which
   assumes a private reference it may consume; the env holds one borrow
   scoped to the install stmt-expr. Omitting it is a mechanical
   double-free / UAF.

Runtime additions (additive, `stage0/runtime_llvm.c`): `kaix_ev_set_env`
(byte-8 write), `kaix_env_set` (env-array slot write), `kaix_clause_env_get`
(byte-8 read + index + dup). All three sit next to the existing byte-16
`kaix_clause_state_get`/`set` and keep the layout knowledge on the C side,
the same discipline as the state helpers.

## Soundness: stack env under one-shot resume

The env array is stack-local to the install frame (like emit_c's `_env`).
This is sound ONLY because resume is one-shot: the clauses run inside the
handle's stmt-expression, which does not return until the body unwinds. A
multi-shot or fiber-escaping resume would dangle the env. The structural
guard already exists — `check_resume_one_shot` (kir_lower_fns.kai) flags a
clause that both resumes and has a back-edge at COMPILE time. The day a
captured clause moves to another fiber (Spawn clones the evidence stack),
that env must move to the heap; until then stack matches C and is correct.
ASAN + UBSan clean on every clause-capture fixture confirms it (no
stack-use-after-return, no UB).

## net_dns_resolve — a separate root, not clause capture

`net_dns_resolve` STAYS a gap. Isolating it: `resolve("localhost")` with
the NetDns *default* handler returns `Ok([127.0.0.1])` IDENTICALLY in both
backends (len=1). But `resolve_first("localhost")` — which does
`match NetDns.resolve(host) { Ok([]) -> Err; Ok([a, ..._]) -> Ok(a); ... }`
— gives `Err` native, `Ok` C. The divergence is in the default-handler
result reaching a list-cons `match` in a different call context, NOT in
clause capture (the fixture's only `with`-handler, `with_dns`, is exercised
by the `check_empty` path which passes). This is the NetDns-default +
list-match-in-Result family, outside this lane's zone. Left listed,
documented in the baseline header; a follow-up owns it.

## Gates

- 6 fixtures byte-id native vs C-direct (stdout + exit), re-measured on a
  CLEAN `rm -f stage2/kaic2 && make kaic2 KAI_LLVM=1` (the brief's hard
  lesson — a stale kaic2 gave false results for a whole session prior).
- `make -C stage2 selfhost KAI_LLVM=1` — byte-id (`kaic2b.c == kaic2c.c`).
  The KIR change does not perturb the C self-reproduction.
- `make -C stage2 test-kir` — GREEN, including the new `clause_capture.kai`
  golden whose dump shows `prefix: box @envcap0` (the KIR-side regression
  guard for the new origin).
- `make -C stage2 test-trace` + `test-trace-asan` — GREEN, including
  `clause_capture_subset_order` (C golden + ASAN/UBSan clean).
- `test-backend-parity` (NATIVE_PARITY_RATCHET): pass=425 fail=37 skip=55,
  0 NEW gaps, the 6 closed entries removed from
  `tools/native-parity-baseline.txt`.
- `KAI_TRACE_RC`: native matches C on the same fixtures
  (`decref_total` > 0, `incref_total=0` is the KNOWN pre-existing tracer
  ifdef asymmetry, #817 — identical shape in BOTH backends, so not a new
  leak; ASAN is the real no-leak proof and it is clean).
- `km score` on every edited file: kir_lower_fns A (92.5),
  emit_native_fx2 A+ (94.4), emit_native_term A+ (95.2),
  emit_native_fx A++ (97.0). No file degraded; all well above the B floor.

## Fixtures added

- `examples/effects/clause_capture_subset_order.kai` (+ `.out.expected`)
  — the env-INDEX-ORDER guard. Wired into `TRACE_FIXTURES` (C golden +
  ASAN) and the native parity corpus. Distinguishes per-clause-index from
  union-index, which `r9_clause_capture` cannot.
- `examples/kir/clause_capture.kai` (+ `.kir.fns` + `.kir.expected`) — the
  KIR golden showing `@envcap0` on a capturing clause, so the new origin
  has a dump-shape regression guard.

## Adversarial review (linus) — bugs caught before merge

A code-review pass on the diff surfaced five issues; four fixed, one
documented as a pre-existing architectural gap.

- **(fixed) Out-of-scope capture → unbound-register IR.** The first cut
  filled the env array by `nemit_load_reg(c)` over the union, with no
  guard that `c` was a live register. emit_c filters the install caps via
  `list_filter_in(.., lcs)` and leaves out-of-scope fields zero. The fix:
  iterate the FULL union with an incrementing index (keeping install≡read
  by position) and write a NULL slot when `native_ctx_reg_slot(c) < 0`,
  instead of loading a non-existent register. Matches emit_c's zero-init.

- **(fixed) `[16 x ptr]` env array with no bounds check.** A handle with
  17+ captures would overrun the fixed alloca silently. The fix:
  `nfx_env_cap()` is the single capacity constant (shared by the alloca
  reservation and the install), and `nemit_install_env` aborts loud
  (`nemit_unsupported`) when the union exceeds it. No core handle is near
  16, but a silent stack overwrite is unacceptable.

- **(fixed) `PEnvCapture(-1)` from a union miss.** `list_index_of_str`
  returns -1 if absent; a -1 index would OOB the env array. The clause's
  caps are a subset of the union by construction, so it can't happen, but
  `lower_env_captures` now `eprint`s loud on -1 (defence in depth) rather
  than emit the poisoned origin.

- **(fixed, nitpick) clause-env seed read raw param 0.** Changed to
  `nemit_load_reg(ctx, "__self")`, the same source `nemit_resume2` uses,
  so the prologue is order-independent.

- **(documented, NOT fixed — pre-existing) nested same-effect installs in
  one fn share the reg alloca.** `nfx_env_reg(eff)` is keyed by effect
  name only; two `with State` in the SAME fn resolve to the same alloca
  (the ctx reg table is append-only, `find_reg` returns the first match).
  This is PRE-EXISTING — `__ev_<eff>` / `__node_<eff>` collide the same
  way — and orthogonal to clause capture; my env array inherits it, does
  not introduce it. CRUCIALLY, none of the 6 fixtures this lane closes
  has two same-effect handlers in one fn (the ctx resets per fn, so r9's
  `make_logger` / `make_tagged_logger` — separate fns — never collide).
  The fixtures that DO (`m8_12_self_delegating_handler`,
  `m7b_15_nested_alias`) already fail for their own reasons and stay
  listed. The real fix is per-SITE reg naming across the whole install
  family (`nfx_*_reg(eff, line, col)`), a separate lane — see follow-ups.

## The `let _ = <Handle>` trap (kaic1 RC over handles)

A first attempt at the nitpick fix kept the now-unused `fnval: Handle`
param and discarded it with `let _ = fnval`. That made kaic2 itself
**bus-error** when emitting native code: kaic1 (which compiles kaic2,
type-blind, no `TyHandle`) runs Perceus over the `let _ = <handle>` and
emits a decref on a raw LLVM `Value*`, clobbering it. The fix was to drop
the param entirely (it was genuinely unused after switching to
`nemit_load_reg("__self")`). Lesson, already a known stage1 trap
([[project_kaikai_kir_native_fix_stage1_handle_abi]]): never bind a
backend `Handle` to a discardable `let` — it must flow into a use, or not
be bound. A clean rebuild (`rm -f stage2/kaic2`) was essential to catch
it; the brief's stale-binary warning earned its place here.

## Structural surprises

- **The LLVM-text backend already solved the ordering question.** Reading
  `emit_llvm.kai` (the abandoned-but-oracle text backend) showed it indexes
  the env by the handle union (`lvm_handle_captures_at`), not per-clause —
  a free confirmation of the correct semantics before writing a line. When
  a native ABI question has an emit_llvm precedent, read it first; it cut
  the design risk to near zero here.
- **Generic handlers monomorphise their clauses.** The first KIR-golden
  fixture used a `[e]`-polymorphic `with_prefix`, and the dump showed the
  clause TWICE (one per monomorph instance) with the handler-decl op listed
  twice. Benign (both instances byte-id, and r9 — also generic — passes),
  but it makes a golden depend on instantiation count. Fixed by making the
  KIR fixture non-generic; the behavioural coverage of the generic case
  lives in `r9` and the parity corpus, not the golden.

## Follow-ups left for next lanes

- **`net_dns_resolve`** — NetDns default handler + list-cons match in
  `resolve_first` returns the wrong arm under native. Separate root; needs
  its own diagnosis (likely the default-install result materialisation or
  a list-pattern lowering in a `Result` context).
- **`PEvidence` (by-id alias capture)** — a clause that captures an
  `__alias_id__<a>` handler-id sentinel still aborts. emit_c keeps it boxed
  and unboxes via `kai_intf` at the dispatch; the native walk's
  `m7b_15_nested_alias` gap is the marker. Out of scope here (the 7 targets
  needed value captures, not id captures).
- **Heap env for fiber-escaping clauses** — when Spawn-cloned evidence can
  outlive the install frame, the stack env must move to the heap. Gated
  today by `check_resume_one_shot`; revisit when actor/Spawn handlers land
  in the native backend.
- **Per-SITE install reg naming** — `nfx_*_reg(eff)` (Ev / node / jmpbuf /
  discard / env, ALL of them) collides for two same-effect handlers in one
  fn (append-only reg table, first-match lookup). Pre-existing; the env
  array makes it carry data. Fix: thread (line, col) into the reg names so
  each install site gets its own frame. A separate lane (it touches the
  whole install family, not just clause capture); `m8_12_self_delegating`
  and `m7b_15_nested_alias` are the markers.
