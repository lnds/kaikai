# Lane experience — #950 (C-free dev loop: native test/bench runner)

The distribution principle is Rust/Go-style: a brew-installed kaikai must
not need a C compiler to operate. It leaked: `kai test` / `bench` /
`check` / `watch` routed through the C backend, making `cc` a runtime
dependency. The root cause was two-layered — a wrapper mis-default AND a
genuine codegen gap (the test runner existed only in the C backend). This
lane closes the gap for test + bench, flips the wrapper, and leaves
`check` as the one documented hold-out.

## The native test runner — design

The C backend weaves the runner inline into a generated `int main`
(emit_c's `emit_main_wrapper`): per `BuildMode` it emits `_kai_test_<id>`
fns with a `setjmp(kai_test_jmp)` landing pad and a driver `int main` that
calls each and returns `kai_test_summary()`. Native cannot do this — its
`int main` lives in the fixed shim `stage0/runtime_llvm.c`, which always
calls `kai_main()`.

Three decisions made the native runner small and correct:

1. **The setjmp landing pad moves to a runtime helper.** `runtime.h` gains
   `kai_test_run_one(desc, KaiValue *(*body)(void))` holding the
   `begin / setjmp / body() / decref / pass` shape; `kai_bench_run_one`
   holds the warmup+timed loop. The body fn is emitted as an ordinary fn
   returning the block's final boxed value — no setjmp in ITS IR, so
   mem2reg still promotes the user code (the inline-setjmp alternative
   would have de-optimised the whole test body). The longjmp from a failed
   `kai_assert_check` unwinds into `kai_test_run_one`'s live frame — the
   legal setjmp case. The C backend is NOT touched: its inline shape is the
   byte-id oracle, and the new helper is invisible to it.

2. **The runner body is lowered as a KFn, reusing the whole pipeline.**
   `lower_to_kir` lowers each `DTest`/`DBench` body via the same `lower_fn`
   that lowers a `DFn` — so perceus/tcrec/unbox already ran on the body
   (perceus has a `DTest` arm), and the runner's RC is correct by
   construction. A `KRunner { sym, desc, kind }` index rides on `KProgram`.
   The one subtlety: the runner's emitted symbol MUST be the
   `__test__<line>_<col>` enc_fn key the lambda collector stamped on the
   body's lambdas, or `find_lam` fails to resolve a lambda inside a test
   body ("non-callable"). Using the position-keyed name as both symbol and
   enc_fn fixes it — the same key emit_c's `test_enc` uses.

3. **The synthetic `kai_main` driver is emitted by hand** (`emit_native_
   test.kai`): per runner, `kaix_<kind>_run_one(desc, &runner)`, then
   `kaix_test_summary_exit()` which prints the summary and `exit()`s with
   its code. Emitting the driver AS `kai_main` (the symbol the shim calls)
   means no shim change and no weak-symbol dispatch; the user's placeholder
   `fn main` is dropped from the native emit list in runner mode
   (`nfilter_user_main`) so it does not define a colliding `kai_main`. The
   `exit()` carries the summary code out without the shim's `int main`
   needing to know the build mode.

## Backend ⊥ build-mode — the driver refactor

The real blocker was that `Mode` collapsed the backend (`MEmitNative`) and
the build-shape (`MTest`/`MBench`/`MPropCheck`) into one mutually-exclusive
enum, so `--emit=native --test` could not be asked. Extracted a `Backend`
field (`BkC`/`BkNative`/`BkCModular`) onto `CliOptions`, orthogonal to
`Mode`; `--emit=native` / `--emit=c-modular` now set the backend, leaving
`Mode` for the build-shape. The dispatch computes `use_native` from the
backend and `bmode` from the mode independently. This removes the
impossible-state class (`--emit=c --emit=native`) rather than papering a
flag over it.

## bench / check audit

- **bench: native, done.** Same shape as test minus the setjmp pad (an
  assertion inside a bench should panic — `kai_assert_check` with
  `kai_test_in_progress == 0` already panics). `kaix_bench_run_one` holds
  the warmup + timed loop; the body fn returns the block value, decref'd
  per iteration.
- **check: native NOT done — documented hold-out.** A `check` block's
  `with`-clause params are generated and shrunk by the runtime, which
  needs a parametrised `bool(input)` predicate + the shrink loop the
  native lowering does not emit. `lower_runner_loop` skips `DCheck`, and
  `kai check` pins `--backend c` explicitly in the wrapper (with a comment
  saying why). This is the one dev-loop subcommand that still needs `cc`.
  Closing it is a follow-up: parametrise the predicate, move the shrink
  loop into a `kaix_check_run` runtime driver (the C path re-emits the body
  inline per shrink candidate — native should NOT replicate that; it should
  pass the input as a borrowed param).

## The wrapper flip

`cmd_test` / `cmd_bench` / `cmd_watch` resolve the backend through
`resolve_backend()` (native default) and pass `--backend`/`--backend-origin`
to `compile_to_binary`, exactly as `build`/`run` do. Graceful degradation
to C on a libLLVM-less kaic2 is inherited (the `implicit` origin path).
`cmd_check` pins `c`. The wrapper comments that claimed these "stay on the
C oracle" were corrected.

## No-C-compiler proof

Tracing `cc` invocations during a native `kai test`: the ONLY `cc` call is
the final object link (`foo.o -o foo-t -lm`) — no `.c` source is ever
compiled. Native emits an object and links it; it never invokes `cc` as a
C compiler. This is the Rust/Go model: the system linker is used (present
everywhere), the C *compiler* toolchain is not. The #949 symptom (invalid C
codegen for an unboxed binder) cannot bite, because no C is generated.

## The #820 collision

PR #954 (#820 L3+L4, evidence-frame transport) merged mid-lane into the
same files: `emit_native.kai`, `runtime.h`, `emit_c.kai`. The rebase
conflicted in `emit_native.kai` (both added a step + a helper at
`build_native_object`'s head) and the Makefile bundle list. Both were
complementary, resolved by keeping both pieces in sequence. `runtime.h`
auto-merged (my helpers sit far from the evidence changes). The deeper
integration check: my runners and the synthetic `kai_main` are NOT in the
AST `decls`, so `build_evidence_frames` gives them zero frame slots — they
behave exactly like `main` (frame-exempt, performs satisfied by the
startup defaults). A test body calling an effectful fn still works: the
call site builds the callee's frame from `kaix_evidence_lookup_node` at
runtime, independent of the caller's frame. The integration is transparent
by construction. A full clean rebuild (kaic1 → native kaic2) + the regress
fixture + serial parity re-ran after the rebase, not trusted to git.

A bootstrap trap surfaced here: #820 also touched `stage1/compiler.kai` to
teach kaic1 its new prims, so `make -C stage2` alone produced an `undefined
name native_ctx_frame_slot_count` from a stale kaic1 — the chain
kaic0→kaic1→kaic2 had to be rebuilt, not just stage2.

## Gate result

- Native `kai test` == C: same summary (`2/3 tests passed`) + exit 1, pass
  AND caught-fail, on the regression fixture and ad-hoc files.
- bench native runs (`--emit=native --bench` composes), exit 0.
- RC: native `leaked=0` on the runner path (cleaner than C's benign
  exit-without-teardown leak).
- No-C proof: native `kai test` compiles no `.c`.
- Fixture: `examples/native/test_runner_parity.kai` +
  `test-native-test-runner-950` gate, wired into tier1-native shard 2.

## Follow-ups

- Native `check` runner (the parametrised predicate + runtime shrink loop).
- The `*_summary_exit` `exit()` path skips the shim teardown; harmless in
  test mode (placeholder main installs no defaults) but worth revisiting if
  test bodies ever need a real default-handler teardown.
