# Lane experience — native bitcode link (P2)

**Lane:** `native-bitcode-link` · **Date:** 2026-06-16 · **Plan:**
`docs/native-codegen-perf-plan.md` §P2 · **Backend:** in-process libLLVM
(KIR Lane 1.5) · **LLVM:** 18.1.8 static

## Scope as planned vs. as shipped

**Planned (P2 of the perf plan):** compile `runtime_llvm.c` to LLVM bitcode
once and `LLVMLinkModules2` it into the in-process module *before* the
`default<O2>` pass, so O2 sees the bodies of `kaix_cons` /
`kaix_variant_arg` / the list spine / the arithmetic helpers and can
inline/specialise them into hot heap-bound loops (`list_fold`, `rbtree`).
The plan's own constraint: one source of truth (not the double-source
inline-in-the-walk anti-pattern that killed the text-LLVM backend), no LTO
toolchain dependency.

**Shipped:** exactly that mechanism, plus two structural corrections the
plan did not anticipate (symbol model + cross-platform bitcode). The link
step lives in `kai_llvm_link_runtime_bc` (`stage2/runtime.h`), called from
`kai_llvm_emit_object` between the target-layout setup and the opt pipeline.
The bitcode is generated at **build time** by `tools/gen-runtime-bc.sh`
(clang 18, version-gated), wired into `stage2/Makefile`'s `kaic2` target; it
is NOT vendored. `bin/kai` and `tools/test-native-parity.sh` consume it via
`KAI_NATIVE_RUNTIME_BC` and drop `runtime_llvm.c` from the final `cc` link
when present.

## The measured win

Best-of-3, dev mac arm64 (M-series), `tools/native-perf/benches`:

| bench | C | native legacy | native P2 | P2 vs legacy | P2 vs C |
|---|---|---|---|---|---|
| `list_fold` (600M cells) | 5.27 s | 12.22 s | 10.07 s | **1.21×** | 1.91× |
| `rbtree_corpus` (2M ins) | 1.23 s | 3.01 s | 2.88 s | **1.05×** | 2.34× |

Inlining is confirmed at the machine level (objdump on the `list_fold`
binary):

- `bl` to `kaix_*` per binary: **4704 → 573** (~88 % removed).
- `kaix_*` symbols present (proxy for DCE): **547 → 58**.
- `kaix_cons` went from an external `declare` to an `internal` (local)
  definition that O2 inlined the spine of, leaving only the irreducible
  allocating call sites.

Output is byte-identical to both the legacy native binary and the C oracle.

**Honest magnitude.** The win is real and measurable but modest (1.05–1.21×
on the heap-bound benches), well short of closing the full 2–3× residual.
The reason is structural and was confirmed by experiment: raising
`-inline-threshold` to 1000 moved `rbtree` from 2.91 s to 2.90 s — i.e. the
inliner has already folded everything worth folding. The dominant remaining
cost is the `malloc`/slab allocation inside `kaix_cons`/`kaix_variant`
itself, which P2 cannot remove (it is the same inevitable heap work C pays;
C is faster here only because its allocations are unboxed-scalar-adjacent and
the loop arithmetic is native). P2 removes the *call-barrier* tax on the
runtime ops, not the *allocation* tax. That barrier removal is what the
1.05–1.21× buys. P2 was always scoped as "closes *part* of the residual";
the part it closes is the inlining barrier, and that part is now closed.

## Design decisions and alternatives considered

Two load-bearing calls, both `asu`-reviewed against measured data.

### 1. Symbol model: full link + internalize (Model X), not `available_externally` (Model Y)

The plan said "the final cc link line stays IDENTICAL", which reads like
Model Y: graft the runtime bodies in as `available_externally` (phantom
copies for inlining) and keep linking `runtime_llvm.c` with cc for the real
definitions. **`asu` flagged this as the exact two-convergent-runtimes
anti-pattern that killed text-LLVM** — the phantom body and the real `.o`
are two sources that can drift (different `-O`, different inlining of inner
helpers), so you inline one version and link another: a silent mis-codegen.

Chosen: **Model X.** `LLVMLinkModules2` physically merges the full runtime
(all 291 `kaix_*` + `main` + the kaikai entry hooks) into the module. Then
`internalize`-except-`main` (a hand-rolled `LLVMSetLinkage(internal)` over
every non-`main` function + global, more auditable than the `internalize`
pass string) lets O2 inline **and** DCE the merged bodies. The object is now
self-contained, so the cc link **drops** `runtime_llvm.c` (re-adding it
would be a duplicate-symbol error). The plan's "cc line identical" was the
one part that had to change — documented here and in the code, because the
literal reading was the unsound one.

Audit that made internalize safe: `grep` confirmed no `dlsym`/`dlopen` and
no string-keyed runtime lookup of `kaix_*` names — they are referenced as
strings only in the *compiler* (to emit the call), never resolved by name at
runtime. So internalizing them cannot break a dispatch table.

### 2. Bitcode: generated at build time (Model B), not vendored (Model A)

The first cut vendored a single `runtime_llvm.bc` (clang 18) + a staleness
`.sha256`, per `asu`'s initial recommendation. Then a measurement killed it:
the mac `.bc` carries `target datalayout = "e-m:o-..."` (Mach-O); a Linux
module is ELF (`e-m:e-p270:32:32-..."`). The link step reconciles the
*string* triple/layout, but **the bitcode's GEP byte-offsets and struct
padding were computed under the original layout** — a mac `.bc` linked into
an ELF module mis-codegens silently. A single vendored `.bc` is not
portable.

Re-reviewed with `asu` on this data → **Model B:** generate the `.bc` at
build time with the build host's clang, so its data layout is the platform's
native one (correct by construction). The objection that originally sank
build-time generation — the host `cc` might be clang > 18 and emit bitcode
the libLLVM-18 reader rejects (measured: Apple clang 21 → `Unknown attribute
kind (102)`; brew clang 22 text `.ll` → `expected type`) — is neutralised by
a **strict version gate**: `gen-runtime-bc.sh` refuses any clang that is not
major 18 and opts out (no `.bc` → native falls back to the legacy
cc-links-runtime_llvm.c path, identical behaviour, just no inlining). The
`.bc` is `.gitignore`'d, regenerated from the *current* `runtime_llvm.c` on
every build — no vendored binary to drift, no per-target matrix to maintain.

Two guards `asu` required to make the silent opt-out safe:
- **Observable status** — `tools/gen-runtime-bc.sh --status` prints
  `active`/`optout`, so a perf regression from a dropped clang-18 is
  diagnosable, not invisible.
- **Release/CI assert** — `tools/assert-runtime-bc.sh` fails the build if P2
  is *not* active where it must be (ubuntu 24.04 clang-18 / release Docker /
  brew llvm@18), turning a base-image change that loses clang-18 from a
  silent slow-path ship into a red build.

## Structural surprises the brief did not anticipate

1. **The cc link line could not stay identical.** The brief (echoing the
   plan) said it would. Model X makes the object self-contained, so the cc
   line *must* drop `runtime_llvm.c`. This is not a deviation from the
   mechanism — it is the consequence of choosing the sound symbol model over
   the double-source one the literal wording implied.
2. **A single vendored bitcode is not cross-platform.** The brief's "una
   vez, reproducible … el release debe generar/vendorizar" assumed a vendored
   artefact. The data-layout measurement showed that is unsound across
   Mach-O/ELF; build-time-per-platform is the correct shape. The brief's
   reproducibility intent is preserved (CI/release on clang-18 always
   regenerate), but the *artefact* is local, not committed.
3. **The triple mismatch warning is real on the in-process path.** clang-18
   stamps the `.bc` with its SDK triple (`arm64-apple-macosx16.0.0`) while
   the module carries `LLVMGetDefaultTargetTriple()`
   (`...-darwin25.5.0`). The link works but warns. Fixed by reconciling the
   source module's triple + data layout to the destination's *before*
   `LLVMLinkModules2` (the layout reconcile is also why a non-native-layout
   `.bc` would be wrong — see decision 2).
4. **The native parity harness links its own runtime**, not via `bin/kai`.
   It had to be taught the P2 path (`KAI_NATIVE_RUNTIME_BC` + drop
   `runtime_llvm.c`) or CI would only ever exercise the legacy link — P2
   would ship untested by the parity gate.

## Fixtures added and coverage gaps

No new `.kai` fixture: P2 is a *codegen-assembly* change, not a language
surface or a new node — its correctness is "every existing native fixture +
the full parity corpus still match the C oracle, now with the bitcode
linked." That is exactly what `tools/test-native-parity.sh` (curated
fixtures) and `tools/test-backend-parity.sh` (full corpus ratchet) prove,
and both were wired to run *through* the P2 path when the `.bc` is present.
The gate is behavioural parity + RC balance + the speedup measurement, not
object byte-identity (the symbol model changed on purpose, so the `.o` is
intentionally different).

Coverage gap: the P2 path is only exercised in CI on a clang-18 runner
(ubuntu 24.04 qualifies). A runner without clang-18 silently tests only the
legacy path — acceptable because that path is unchanged, and the release
assert guarantees P2 is active where it ships.

**Cross-platform proof.** The Model B decision (per-platform bitcode) was
validated in Docker `ubuntu:24.04` (clang 18.1.3): the generated `.bc`
carries the ELF data layout (`e-m:e-...`, `aarch64-unknown-linux-gnu`),
links cleanly into the Linux in-process module, and the parity harness
passes 15/15 through the P2 path. This is exactly the case a single vendored
mac `.bc` would have mis-codegen'd — the build-time-per-platform shape is
correct by construction. Two portability bugs surfaced and were fixed there:
(1) `gen-runtime-bc.sh` used `shasum` (mac-only); Debian has `sha256sum`, so
the stamp silently failed → resolve whichever exists; (2) the Docker repro
needs `zlib1g-dev`/`libzstd-dev` for the apt libLLVM link (pre-existing, not
a P2 issue — the kaic2 link failed before `gen-runtime-bc.sh` even ran).

## Follow-ups left for next lanes

- **The allocation tax is the real residual.** P2 removed the call barrier;
  the remaining 1.9–2.3× vs C on heap-bound code is the boxed-cons
  `malloc`/slab cost. Closing it needs reuse-in-place / unboxed cons cells in
  the native walk (a KIR-level change), not a link-time one. Worth a perf
  issue once someone measures the C allocator's edge precisely.
- **`variant_match` super-linear collapse** (perf plan §3.4) is untouched by
  P2 and still open — a distinct defect (time grows ~100× per 2× rounds with
  a near-constant live set), flagged for its own investigation with
  `KAI_TRACE_RC` + an allocator profile.
- **Inline threshold tuning** gave nothing measurable (2.91 → 2.90 s on
  rbtree); the inliner is already at its useful ceiling for these shapes.
  Not worth pursuing without a new lever.
