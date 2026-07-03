# Lane experience — issue #1061

## Scope

`kai test --backend=native` failed at link time with `undefined kai_main`
on any file carrying `test`/`bench` blocks; `--backend=c` on the same file
worked. The native test/bench shard (`test-native-test-runner-950`) was the
one red keeping `tier1-native` from being made a required check. Preexisting
since ≥#1050, not a regression of the reverts.

Scope-as-planned == scope-as-shipped: a symbol-emission fix in the native
backend, no fixture change (`examples/native/test_runner_parity.kai` already
existed).

## Root cause — (a) symbol naming, one link deeper than "linkage"

The brief offered three hypotheses to discriminate with `nm`:
(a) wrong linkage on the synthetic driver, (b) runner-mode never activated,
(c) link order. `nm` on the `--test` object settled it in one shot:

- normal build → `_kai_main` (clean) **T**
- `--test` build → `_kai_main.294` **T**, no clean `_kai_main`

The `.294` suffix is LLVM's rename-on-collision marker: something else already
held the name `kai_main` when the synthetic driver called
`llvm_add_function(m, "kai_main", …)`, so LLVM renamed the driver.

The colliding declaration came from `nemit_declare_fns(ctx, all_fns)`:
`all_fns` is `kp.fns` **unfiltered**. `nfilter_user_main` (and the modular
`nmod_filter_main`) only dropped the user `fn main` from the *define* set
(`define_fns`), not from the *declare* set (`all_fns`). So the user main's
`kai_main` was still **declared** (bodyless), the driver got `kai_main.294`,
and the runtime's `int main` — which calls `kai_main()` — found only the
undefined declaration → link failure.

So it was a variant of (a): the driver's linkage was fine; the bug was that a
stray *declaration* stole the driver's name.

## Fix

Filter the user main from `all_fns` too, inside `nemit_build_module`, in
runner mode. In runner mode the user main is unreferenced (the driver owns
`kai_main`), so removing it from the declare set / call-site lookup / thunk
set is safe and eliminates the name collision. One point covers both the
whole-program and modular native paths (both flow through
`nemit_build_module`). 5 lines.

## Verification

- `nm` on the `--test` object: now `_kai_main` clean, no `.294`.
- `make -C stage2 KAI_LLVM=1 test-native-test-runner-950` → green
  (native == C: summary 2/3, exit 1; RC leaked=0).
- `kai test --backend=native/c test_runner_parity.kai` → byte-identical
  output, exit 1 both.
- Generality: a second fixture with test blocks **plus a user `fn main`**
  links + runs native == C, exit 0, and the user main is correctly skipped
  (its message never prints) — proves the filter drops the right fn and the
  fix is not fixture-specific.
- `kai bench` native == C, normal native build unaffected.
- selfhost C byte-id green (change touches native emit only; C emit shape
  unchanged), tier0 green.

## Surprises / follow-ups

- The pre-existing `nfilter_user_main` was a half-fix: it named the right
  intent ("driver owns `kai_main`") but only applied to the define set. The
  declare set is the one that mints the LLVM `Function`, so that is where the
  collision was born. Filtering at the single `all_fns` binding in
  `nemit_build_module` is the natural home — every downstream use of
  `all_fns` (declare, call-site lookup, thunks) then sees the filtered set
  consistently, and none of them want the user main in runner mode.
- With this green, `tier1-native` has no remaining red on this shape and is
  ready to be promoted to a required check (separate CI-config lane).
