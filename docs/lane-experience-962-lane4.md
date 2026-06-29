# Lane experience — #962 Lane 4: batch-list mode (parse-once)

## Scope as planned vs as shipped

**Planned (brief):** convert the harness from process-per-file to
one-typecheck-N-files against a live `TyEnv`, reaping the ~20× CI
speedup, and `closes #962`.

**Shipped:** an internal `--batch-list` mode in `stage2/compiler/driver.kai`
that loads (lexes + parses) the core module set **once** and compiles N
inputs against that shared `PreludesLoaded`. Measured **1.60×** on
emit-only build over a 50-file corpus; every batch `.c` is
**byte-identical** to the standalone `.c`. The PR is **`refs #962`, not
`closes`** — the redundant *re-typecheck* the issue targets is still
paid per file.

The gap between planned and shipped is deliberate, decided with the
maintainer after measurement (below). The core TYPECHECK reuse — the
issue's actual ~20× lever — was scoped OUT of this lane: it is surgery
in the 19.6k-LOC `infer.kai` / 4.6k-LOC `driver.kai` F-monoliths with a
real byte-id risk, and the measurement showed the harness it was meant
to accelerate cannot benefit from it anyway (see below). It is deferred
to a follow-up lane with a fully-validated design (see *Follow-ups*).

## The measurements that redirected the lane

Three numbers, taken on an M-series mac with a C-only `kaic2`, reframed
the whole lane:

1. **Cost decomposition of a standalone compile** (hello-world, core
   cache OFF — the CI default): `--tokens` (load_core = lex+parse of the
   ~40 core modules) = 0.06s; `--check` (full typecheck) = 0.13s; full
   `kai build` (emit + cc/link) = 0.93s. So of the 0.93s a build costs,
   **0.73s (78%) is the `cc`/link step** — per-binary and untouchable by
   any batch driver — and only ~0.13s is the fixed core overhead a batch
   can amortise.

2. **Batch vs fork, emit-only, 50-file corpus:** fork 12.2s → batch
   7.6s = **1.60×**, byte-id 50/50. This is the real win of parse-once.

3. **Implied harness speedup:** since the parity harness builds *and
   links* each fixture, hoisting the ~0.13s core overhead off a 0.93s
   per-file cost is **~14%** total — not 1.6×, not 20×. Converting the
   delicate `bin/kai`-wrapping `test-backend-parity.sh` (which would
   require splitting emit from cc and duplicating the runtime/bitcode/
   link logic) for a 14% gain, with real regression risk, did not pay.

The issue's "~12 min of redundant re-typecheck" premise assumed the
overhead was the *typecheck*. Measured, the standalone overhead splits
~0.06s parse + ~0.07s typecheck, and **both are dwarfed by cc/link in a
real build**. The 20× was only ever reachable for a typecheck-*only*
corpus pass (no codegen, no cc) where the core typecheck is the whole
cost — which the harness is not.

## Design decisions and alternatives considered

- **Output sink: `write_file` vs stdout multiplexing.** `compile_source`
  prints the `.c` to stdout (the wrapper captures it). For N files in
  one process, stdout would collide. Considered a delimiter-marker
  protocol on stdout (`===FILE: path===`) parsed by the harness;
  rejected as more fragile than writing each `.c` directly. `write_file`
  is a first-class builtin (stage0 prim), so the batch writes each
  output itself. One trap: `print` appends a trailing newline,
  `write_file` does not — matched it (`cat2(out, "\n")`) so the batch
  `.c` is byte-identical to the stdout-captured fork output. This was
  the *only* byte-id divergence; caught immediately by the gate.

- **Where the batch code lives.** `compile_source` / `load_core` / `PLS`
  are all private to `driver.kai`. Moving the batch loop to a new
  A-grade module would have meant making half the driver public. Kept
  the ~70 LOC in `driver.kai` as small, low-complexity functions
  (`parse_batch_entries`, `run_batch_list`, `batch_compile_loop`). They
  do not lower the file's (pre-existing F) grade; extracting them would
  cost more in broken encapsulation than it buys.

- **Mode vs flag.** `--batch-list` is a new `Mode` (`MBatchList`) with
  the list-file path in the existing `CliOptions.path`, not a new
  `CliOptions` field — avoids threading a field through ~12
  `cli_with_*` helpers. Backend (`c`/`native`) still composes via
  `--emit=native`.

## Soundness — why parse-once is trivially safe

The shared artifact is the immutable `PreludesLoaded` (parsed core
decls + module table + segments). No user file mutates it; each
`compile_source` runs the full pre-typer → typecheck → codegen fresh
over `core ++ user_i`. Cross-file contamination is structurally
impossible because nothing typecheck-level is shared. The byte-id gate
(batch `.c` == standalone `.c`) is the proof, and it is **trivially
green today** — it becomes load-bearing the moment a follow-up lane
shares the core *typecheck* (where a mutable shared `TyEnv` could leak).

## Fixtures added

- `examples/oracle/lane4/{file_a,file_b}.kai` — an adversarial pair:
  both declare a root `fn tag` with INCOMPATIBLE signatures
  (`Int -> String` vs `String -> Int`). Compiled in one batch, each
  must keep its own `tag`; a leak would retype the other's call and
  diverge the `.c`.
- `tools/batch-isolation.sh` — the gate: batch `.c` byte-identical to
  standalone `.c` for both files. Wired as `make test-batch-isolation`
  into `TEST_LIGHT_TARGETS` (runs in tier1 + every shard).

## Coverage gaps

- The isolation gate covers the C backend only. The native `.o` path
  (`build_native_object` → `native_object_path`) shares the same
  `compile_source` body and the same shared-`PLS` discipline, so it is
  covered by construction, but no native-specific fixture asserts it.
  When the typecheck-once follow-up lands, the gate should also diff the
  native `.o` (or its disassembly) batch-vs-fork.

## Real cost vs estimate

The lane spent most of its budget on (a) the asu design consult for the
typecheck-once path (which then did not ship) and (b) fighting my own
overlapping background measurements (a `pkill -f batch-list` killed an
in-flight measurement's children mid-run, twice). The actual shipped
change is small. The design consult was not wasted — it is the
validated blueprint for the follow-up.

## Follow-ups left for next lanes

**Typecheck-once (the real ~20× lever), fully designed by `asu`:**

- Reusable artifact = the partial core `TypedProgram` PRE-`resolve_protocol_calls`
  **complete** (decls + `insts` + `op_res`, not just decls — `monomorphise`
  reads `tp.insts`).
- `typecheck_module(target, inherited=core_delta, all_program_decls=core++user_i)`
  is byte-identically the full fold for that file's target bucket. Fold the
  core buckets once, capture `(core_delta, partial TypedProgram)`, reuse it.
- Per file, recompute `all_program_decls`, `head_owners`, `proto_impls`,
  `local_arities`; run `resolve_protocol_calls` + `monomorphise` + emit over
  the concat (mono specialises core by the user's types — correct).
- **Gate = `.c` byte-id batch-vs-fork** (subsumes the Lane-3 oracle for the
  batch path; covers `insts`/`op_res` that `--dump-typed-core` does not).
  No gensym to reset: the compiler has zero top-level `var`; `fresh` is
  re-derived from a constant seed, emit temps are keyed by line/col, mono
  spec names by type mangle. The byte-id gate is the acceptance — verified
  the existing parse-once batch already produces byte-identical `.c`, so the
  framing is sound.
- This is the lane that genuinely `closes #962`.

**Harness conversion** only pays once typecheck-once exists AND the
target is a typecheck-only pass (e.g. a future `--batch-list --check`
corpus gate), where cc/link is not in the per-file cost.
