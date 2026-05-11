# Lane retro: L1 LLVM driver flag

**Branch**: `lane-l1-llvm-driver`
**Closes**: phase L1 of LLVM-direct refactor (DoD #6 — Anga Roa
compile-time target).
**Real cost**: ~1 day, matched estimate.

## Scope

L1 wires `bin/kai build --backend=llvm` (and `kai run --backend=llvm`)
to invoke the *existing* `kaic2 --emit=llvm` path and link the
resulting `.ll` with an external `clang` against
`stage0/runtime_llvm.c`. No compiler changes; no libLLVM dependency;
no behaviour change for existing users (the C backend remains the
default).

In scope:
- `--backend=<c|llvm>` flag on `kai build` and `kai run`.
- `CLANG` / `CLANGFLAGS` env vars; clang-in-PATH detection with a
  clear error and exit code 2.
- `stage0/runtime_llvm.c` resolution (dev layout) and the
  `share/kaikai/include/runtime_llvm.c` lookup that the future
  installed layout will populate.
- `examples/llvm/driver_smoke.kai` fixture + `tools/test-llvm-driver.sh`
  shell harness (parity check, help text, validation, missing-clang).
- README sentence + `usage_build` / `usage_run` help update.

Out of scope (recorded as L2/L3 follow-ups below).

## Why external clang now, libLLVM linked later

The brief considered three shapes for the LLVM path:

1. **Cache-only** (no LLVM at all, just speed up incremental builds
   on the C path).
2. **External clang** (this lane).
3. **Static-linked libLLVM** in the driver / kaic2.

(1) doesn't move us toward the Anga Roa compile-time target on cold
builds. (3) is the eventual destination but adds a 50–80 MB build
dependency, a non-trivial linker story across macOS / Linux, and
locks the driver to one LLVM major. (2) ships value on day one with
zero new dependencies for users who already have clang (which is
every macOS dev box and most Linux ones), and the codepath we wire
up here — `kaic2 --emit=llvm | clang` — is exactly the one that
becomes `kaic2 --emit=llvm | libLLVM` when L3 lands. The driver
stays the contract; the implementation underneath swaps.

## Driver flag wiring

`compile_to_binary` was the natural pinch point: it already
centralises kaic2 invocation, prelude flags, manifest path lookup,
and the link line. Refactor was minimal:

- Two leading `--backend <name>` arguments are consumed and stripped
  before the original `src tmp out sub` quad is parsed. This keeps
  every existing call site (`cmd_test`, `cmd_bench`, `cmd_check`,
  `watch_run_once`) working unchanged — they pass no `--backend` and
  default to `c`.
- A single `if [ "$backend" = "llvm" ]; then ... else ... fi`
  bifurcates the kaic2-stdout / link-line pair. The C branch is
  byte-identical to the prior implementation.
- `cmd_build` and `cmd_run` parse `--backend=<v>` and `--backend <v>`
  forms before forwarding. `cmd_test` / `cmd_bench` / `cmd_check`
  deliberately don't expose the flag in L1 — those commands are
  iteration loops where the C backend wins on parity coverage today.

`--backend=foo` is rejected with an explicit
`--backend must be 'c' or 'llvm'` message. `--holes` /
`--holes-json` are mutually exclusive with `--backend=llvm` (the
hole report doesn't produce a binary, so backend selection is
nonsensical).

## clang detection strategy

`ensure_clang` runs only on the LLVM branch and uses
`command -v "$CLANG"`. The honour-the-override pattern matches the
existing `CC` shape, and means CI can pin a specific clang version
without touching driver code. Failure prints both lines from the
brief verbatim:

    kai: error: --backend=llvm requires clang in PATH (or set CLANG env var)
           falling back to C backend not implemented yet

The "fallback not implemented" sentence is kept intentional — silently
falling back would mask the user's explicit choice, and the failure
mode is one PATH edit away from being fixed. If a future lane decides
silent fallback is the right thing, the message change is one line.

## runtime_llvm.c resolution

Two-step lookup, mirroring `RUNTIME_INC` / `STDLIB_ROOT`:

1. `$RUNTIME_INC/runtime_llvm.c` (matches the future installed
   layout under `share/kaikai/include/`; not present today).
2. `$ROOT/stage0/runtime_llvm.c` (dev checkout — always present).

Empty string → driver dies cleanly. Today the dev layout is the
only one that resolves, which is fine because we don't ship a
prebuilt tarball yet. When tarball packaging lands, the install
script needs to copy `runtime_llvm.c` into
`share/kaikai/include/`; this is captured in the L2 follow-up.

## Structural surprises

Two small ones:

1. **Stdlib preludes still hit emitter gaps under `--emit=llvm`.**
   `bin/kai` injects 25+ prelude files via `--prelude` on every
   compile. Several of them — `Mutable.kai` and friends — exercise
   constructs the LLVM emitter doesn't yet handle (`kai_bit_and`
   missing, closure-build failures on lambda params used in string
   interpolation). The `make test-llvm` recipe sidesteps this by
   never passing `--prelude`. For L1 the realistic scope is *the
   subset that builds clean without preludes*, gated by
   `KAI_NO_STDLIB=1`. The driver does not auto-set it (silent flag
   manipulation behind the user's back is a worse failure than a
   loud error), so the README, help text, and smoke fixtures all
   document the requirement explicitly. Closing those emitter gaps
   is the L2 lane.

2. **`-w` is load-bearing on the link line.** The IR kaic2 emits
   triggers benign clang warnings (unused-parameter on shim
   signatures, type-tag bitcasts). Without `-w` the driver output
   is hundreds of lines of noise on every successful build. The
   `stage2/Makefile` test-llvm rule already does this; we mirror it
   in the driver's default `CLANGFLAGS`.

## Fixtures + coverage

- `examples/llvm/driver_smoke.kai` — small fixture covering int
  arithmetic, fn calls, if/else (the M3b emitter's confirmed
  surface). Stays in `examples/llvm/` next to `m3b.kai` so future
  test rules can pick it up.
- `tools/test-llvm-driver.sh` — six checks: help-text advertises
  `--backend`, bad backend value rejected, missing clang rejected,
  parity for hello/driver_smoke/m3b. Returns non-zero on any
  failure. Skips with exit 0 (instead of failing) when clang is
  missing — the existing `make test-llvm` already fails loud in
  that case, no need to duplicate.

Not wired into a Makefile target in this lane: the brief deferred
that to a follow-up. `tools/test-llvm-driver.sh` runs cleanly
standalone and can be invoked from CI / a future `make
test-llvm-driver` rule whenever L2 starts.

## Real cost vs estimate

Brief estimated ~1 day; actual was ~1 day. The estimate held because
`compile_to_binary` already had the right shape (centralised
kaic2 + link invocation), and the LLVM emit + link recipe was
already proven by `stage2/Makefile`. The only research cost was
discovering the prelude-gap surprise (item 1 above), which would
have hit any approach.

## Follow-ups left for next lanes

- **L2 — prelude support under LLVM.** Close the LLVM emitter gaps
  (`kai_bit_and` runtime symbol, lambda-in-interpolation closure
  build) so `kai build --backend=llvm` can run with the full
  stdlib loaded. Then promote LLVM to the default for `kai test`
  and `kai bench`, where the iteration speedup pays off most.
- **L3 — libLLVM static link.** Replace the external-clang shell-out
  with an in-process libLLVM call. Driver flag stays, link line goes
  away. Requires a vendored or linked libLLVM and a build-time
  toggle for users who don't want the dependency.
- **Tarball packaging copies `runtime_llvm.c`.** Whenever the
  installed layout lands (`libexec/kaikai/`, `share/kaikai/`), the
  packaging step must copy `stage0/runtime_llvm.c` into
  `share/kaikai/include/runtime_llvm.c`. The driver already looks
  there first.
- **Wire `tools/test-llvm-driver.sh` into a Makefile target** (e.g.
  `make test-llvm-driver`) and gate it in tier1 once L2 closes the
  prelude gaps and the parity surface is large enough to be
  meaningful in CI.
- **`kai test --backend=llvm` / `kai bench --backend=llvm`.** Held
  back from L1 because the typical iteration-loop user benefits from
  prelude support, which L2 unlocks. Trivial to add once L2 lands —
  same flag-parse pattern as `cmd_build`.
