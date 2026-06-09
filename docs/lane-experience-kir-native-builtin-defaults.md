# Lane experience — KIR native builtin default-handlers (Lane 1.2, effects subset 2c)

Closes the gap that blocked essentially every output program on the
in-process libLLVM native backend (docs/kir-design.md §7.2): the native
walk's `kai_main_install_defaults` was an empty no-op (a leftover from the
pure-walk subset), so a program performing a BUILTIN effect — `Stdout.print`,
the most common — aborted the walk with `KPerform: no handler for effect
Stdout`. This lane installs the builtin default-handlers at `main` so the
native backend prints + exits at parity with the C-direct oracle.

## Scope as planned vs as shipped

**Planned:** install the synthetic builtins (Stdout / Stderr / Fail / Stdin)
first — they unblock hello-world and most output programs — and treat the
source-`default{}`-block class (Clock in stdlib/time.kai) as a possible
follow-up if significantly more work.

**Shipped:** BOTH classes, because they turned out to be ONE class. The
compiler injects the synthetic builtins as real `DEffect` decls carrying an
all-`$extern_handler` `default { }` block (driver.kai `builtin_stdout_decl`
&c.), so `find_effect_default_block` resolves them through the *same* path a
user effect with a source default block (Clock) uses. There was no second
path to defer: `kir_default_handlers` (kir_lower) handles every effect in
`install_order ∩ main_row` that carries a default block, builtin or user.
Verified at parity: Stdout, Stderr, Fail, and `Console = Stdout + Stderr`
(alias-expanded → both installed, a multi-handler install + reverse
teardown). Parametric effects (State[T], Reader[T]) have no default block,
so they drop out of `find_effect_default_block` naturally — out of scope as
the brief noted, but for free.

## Structural surprise the brief did not anticipate

The brief described "TWO classes of default" (synthetic-compiler vs
source-`default{}`-block) as the key complexity, and pointed at the deleted
hardcoded `default_<eff>_setup()` family. In THIS codebase (post issue #558)
there is exactly ONE emit-time path: every default — builtin or user — is an
AST `default { }` block reached via `find_effect_default_block`. The
synthetic builtins are injected as `DEffect` decls in `driver.kai` before
emit, so by lowering time they are indistinguishable from a user effect with
a default block. This collapsed the design: no `op→runtime-symbol` table to
hand-maintain on the native side, no special-casing — the op list + the
`kaix_default_<eff>_<op>` symbol both come from the injected default block's
clauses, in clause order (= field offset).

## How the install/teardown + perform dispatch were wired

- **KProgram extension (`kir.kai`):** a new `defaults: [KDefault]` field,
  `KDef(eff, [KDOp(op, runtime_sym)])`. Computed once at lowering (where the
  AST decls are in scope) by `kir_default_handlers` so the native translator
  never re-walks the AST — the KIR-design "single source of truth both
  translators read" principle. The C/LLVM-text emitters do NOT consume KIR,
  so this field is native-only and invisible to the C-path selfhost byte-id.
- **Install (`emit_native_def.kai`, new file, A++ 82 LOC):** per effect, a
  MODULE-LEVEL Ev blob `[16 x ptr]` + KaiEvidence node `[8 x ptr]` (NOT
  entry-block allocas — the evidence stays pushed for the program's whole
  life, so it must outlive `kai_main_install_defaults`'s frame; the C-direct
  oracle uses `static EvStdout` for the same reason), `kaix_fresh_handler_id`
  stamped into field 0, each op's `kaix_default_<eff>_<op>` forwarder stored
  at field `nfx_op_base + i` via `kaix_ev_set_op`, then a **3-arg**
  `kaix_evidence_push(node, name, ev)` — NO jmp_buf, because a default always
  resumes (the user-handler install in fx2 uses `..._push_with_jmp` for its
  discard landing pad).
- **Teardown:** one `kaix_evidence_pop()` per installed default; the reverse
  of the same `defaults` list IS the matching LIFO pop set.
- **Perform dispatch for builtins:** rather than thread `defaults` through
  the whole `nemit_fill_blocks → … → nemit_perform` chain, `ndef_synth_handlers`
  synthesises a `KHandlerDecl` per default (op names in declaration order)
  and prepends them to `kp.handlers`. `nemit_perform` (fx2) already resolves
  the op FIELD INDEX from `op_thunks` and loads the op fn-ptr at RUNTIME from
  the installed Ev (`kaix_ev_op_at`) — so the synthetic decl's thunk symbol
  is never called, only its op-name position is read. Zero change to
  `nemit_perform`; the builtin path reuses the user-handler dispatch exactly.

## The module-global Ev decision

The Ev + node must be process-lifetime, so an entry-block alloca in
`kai_main_install_defaults` (freed on return) is wrong. Added one C-API
forwarder `kai_llvm_add_global_zeroed(m, ty, name)` (internal linkage, null
initialiser — the `static` analogue) and built the Ev/node as module globals
via it. Registered in stage1's RP table, stage2's `native_prims`, and the
KAI_LLVM + stub sections of `stage2/runtime.h`.

## Soundness

- **Handle discipline (asu "risk #1"):** the two helpers returning a
  `Handle` (`ndef_ev_global`, `ndef_node_global`) are listed in stage1's
  `native_handle_fns()`; every other `let` binding a handle in the new file
  rides the handle-binder exclusion (RHS is a native prim). Forgot neither.
- **issue #115 / UBSan `-fsanitize=function`:** the install stores the
  non-static `kaix_default_*` forwarders and the perform indirect-call goes
  through the Ev fn-ptr; `-fsanitize=address,undefined,function` on the
  linked native executables is clean (no function-type violation, no UAF on
  the module-global Ev).

## Two bugs found along the way

1. **`install_order` is NOT the install SET.** `kir_install_order` returns
   all 19 builtins regardless of main's row (it is the canonical ORDER); the
   C/LLVM backends filter with `if list_has(main_row, eff)` at emit. The
   first cut installed all 19 for every program (segfault on the first
   default's NULL-ish op). Fix: `kir_default_handlers` intersects with
   `main_row_labels` — exactly the C-direct `default_setups_for` filter.
2. **The kaic1 `Option[String]` codegen bug bites lowering too.** The first
   `kir_default_ops` used `match extern_handler_c_symbol(body) { Some(c_sym) }`
   and got `Some("")` — the empty symbol crashed `get_or_declare_fn`'s
   `strlen(NULL)`. This is the SAME bug `lvm_extern_handler_c_symbol`
   (emit_llvm) documents: the Option form through a deeply-nested match
   silently empties the bound value. Fix: a String-returning
   `kir_extern_handler_c_symbol` guarded on `== ""`, mirroring the LLVM-text
   workaround. The native lowering runs in the kaic1-built KAI_LLVM kaic2, so
   the bug reaches here.

## Fixtures + gates

New fixtures (the effects-2a fixtures only checked exit codes — the
stdout-checked fixture is the real default-handler guard, closing that
lane's flagged gap):
- `examples/native/stdout_hello.kai` — stdout-checked: `Stdout.print` → exit.
- `examples/native/stdout_multi.kai` — two prints through one installed handler.
- `examples/native/effect_console.kai` — `Console` alias → both Stdout +
  Stderr installed (multi-handler install + reverse teardown).

Gates: native-parity 8/8 (stdout + exit vs C-direct); tier0 selfhost byte-id
`kaic2b.c == kaic2c.c`; backend-parity (C↔LLVM-text) 413 pass / 0 fail
(untouched); ASAN + UBSan (incl `-fsanitize=function`) clean on the linked
native executables; `km score` A++ (98.0) / 82 LOC / cogcom avg 0.5 max 1 on
the new file.

## Follow-ups left for next lanes

- The harness diffs only stdout + exit code, not stderr; `effect_console`'s
  stderr (`Stderr.eprint`) was checked at parity in this lane's manual run
  but is not gated. A stderr-diffing harness extension would gate it.
- Source-`default{}`-block builtins with multi-op blocks (Env, File, Clock,
  Random, NetTcp, …) go through the identical path; they are installed when
  in main's row but have NO native fixture yet (their ops need more of the
  native walk — Result/Option ctors, etc. — to exercise end-to-end). Add a
  Clock or Env fixture once those construct paths are corpus-covered.
- A future mem2reg lane on the native backend must keep the setjmp-bearing
  user-handler fns excluded (subset 2a's hazard); the default path has no
  setjmp, so it is unaffected.
