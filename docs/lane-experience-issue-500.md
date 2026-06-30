# Lane experience — issue #500 (release/debug build modes + DWARF debuginfo)

## Scope as planned vs. as shipped

**Planned (brief):** wire user-facing build profiles (`--release` / `--debug` /
default) and DWARF emission via `LLVMDIBuilder` from the kaikai source positions
the AST already tracks, plus kaikai-source panic stack traces in `--debug`. The
issue had already been re-scoped twice in its comments: the flag plumbing
(`--release`/`--debug` → `KAI_NATIVE_OPT` opt level) shipped earlier, so the real
remaining work was the debug-info half — DWARF emission, symbol stripping on
`--release`, and `.kai:line` panic traces.

**Shipped:** all of it, verified end-to-end on the native (default) backend.

- **Build profiles on two axes.** The default and `--release` share an opt level
  (`-O2`) yet differ on stripping, and `--debug` is the only DWARF mode. A single
  axis (`KAI_NATIVE_OPT`) could not express this — `--release` and default both
  map to `2`. Added `KAI_BUILD_MODE` (debug/release/default) for the strip +
  debug-info decisions, threaded by `bin/kai`.
- **Strip on `--release`.** Plain `strip` (not `strip -S`) — the bulk of the
  symbols are *local* (every emitted kaikai fn + `_.str` constant), which `-S`
  leaves untouched. 95 KB → 84 KB on hello, 303 → 27 symbols, binary still runs.
- **DWARF emission.** `build_native_object` takes the source path and, in
  `--debug`, opens a DIBuilder + DIFile + compile unit; each `KFn` opens a
  DISubprogram at its first source line; each statement/terminator sets the
  builder's current location from the `KPos` the KIR already carries (KIR was
  *designed* for this — `kir.kai` §"Position metadata (Risk 7)" names #500).
- **Panic stack trace.** A strong `__kai_build_debug = 1` global (vs the
  runtime's weak `0`) flips on the backtrace; a panic resolves its return
  addresses to `<file>.kai:<line>` via `atos`/`addr2line` over the emitted DWARF.

## Structural surprises the brief did not anticipate

1. **The Mach-O linker drops DWARF.** The native `.o` carries full `.debug_*`
   sections (verified), but on Mach-O the linker leaves only a *debug map*
   pointing at the `.o` — which `bin/kai` deletes from `$tmp` on exit. The DWARF
   never reaches the executable. Fix: run `dsymutil "$out"` after the link (while
   the `.o` still exists) to collect the `.dSYM` bundle `lldb` auto-loads. On ELF
   (Linux/CI) the linker copies `.debug_*` straight into the binary, so no extra
   step — the check script handles both (binary OR `.dSYM`).

2. **The verifier's "wrong subprogram" rejection.** A fn's prologue (allocas /
   param store / entry br) is emitted before the first statement's `set_loc`, so
   it inherited the *previous* fn's lingering builder location — whose scope is
   the previous fn's DISubprogram. LLVM's verifier rejects a `!dbg` whose scope
   is not the enclosing fn's subprogram. Fix: clear the location per fn in
   `begin_fn`, and have the subprogram-open *seed* the location to the fn's own
   line under the new scope, so the prologue carries a valid in-scope `!dbg`.

3. **`make` caches `stage2.c`.** A `runtime.h` edit did not take effect because
   `make -C stage2` reused a stale `build/stage2.c`. `rm stage2/build/stage2.c`
   forces the regen (already in the build-traps memory; it bit again here and
   cost a confused debugging detour before I trusted the memory over the symptom).

4. **stage1 carries its own `rprelude_table`.** A new native prim needs the entry
   in BOTH `stage2/compiler/native_prims.kai` AND `stage1/compiler.kai` — kaic1
   (stale, type-blind) is what compiles the stage2 bundle and emits the C call.
   Six DI prims + the debug marker × two tables. Both derive their resolver names
   from the table, so one edit per stage covers resolver + emit.

5. **`bin/kai` compiles a `$tmp` copy, not the user's file.** To avoid dropping a
   `.o` in the user's tree, the native path copies the entry `.kai` to `$tmp` and
   compiles that — so kaic2 baked the *ephemeral* `$tmp` path into the DIFile +
   `comp_dir`. `lldb` then could not open the source, and the panic trace named a
   path that no longer existed. Caught only by trying `lldb breakpoint set --file
   … --line …` (the DWARF-present + line-table checks all passed against the
   `$tmp` path). Fix: `bin/kai` hands kaic2 the original absolute path via
   `KAI_DEBUG_SRC`; `native_di_enable` splits it into (dir, base). Lesson: a green
   "DWARF is present" check is necessary but not sufficient — only an actual
   `lldb`/`gdb` open proves the paths are usable.

6. **`dladdr` needs `_GNU_SOURCE` on glibc — caught only by CI.** The panic
   backtrace uses `dladdr` / `Dl_info` (a GNU extension, not C99). glibc hides
   them behind `_GNU_SOURCE`; macOS libSystem exposes them regardless. So the
   full mac build (native AND C-only) passed locally, but the strict `-std=c99`
   C-only bootstrap on Linux CI failed with "undeclared identifier Dl_info". This
   is the mac-clang-hides-GNU-extensions trap: a mac `-std=c99 -pedantic-errors`
   over the same header does NOT reproduce it, because the symbols are visible on
   mac no matter the feature-test macros. Fix: `#define _GNU_SOURCE` in the Linux
   branch before any include, alongside the existing `_XOPEN_SOURCE` /
   `_DEFAULT_SOURCE`. Lesson: any new libc call that is a GNU/BSD extension must
   be assumed gated behind a feature-test macro on glibc, and a mac-only local
   pass proves nothing about the C-only Linux bootstrap — that gate lives in CI.

## Why the panic trace is best-effort, and what it does NOT do

The trace resolves via a shell-out to the platform symboliser (`atos`/`addr2line`)
over the DWARF — the option chosen over an embedded address→line table (which
would duplicate the DWARF and need a fiber-aware unwinder) and over printing only
the panic-site line (not a real trace). Consequences accepted as in-scope:

- Frames the symboliser cannot map to a `.kai` (runtime/libc) are filtered out,
  so the trace shows only kaikai positions.
- At `-O0` a recursive panic site can collapse so the innermost `.kai` frame is
  the *caller* of the panicking fn, not the `panic(...)` line itself. The
  acceptance bar — "panics show kaikai filename + line" — is met (the trace names
  the `.kai` and a real line); a frame-perfect trace is not promised.
- Release/default binaries keep the weak `__kai_build_debug = 0`, so they print
  the one-line panic and pay no symboliser cost.

## Fixtures + coverage

`examples/build-modes/panic_trace.kai` (a 3-level panic) + `check.sh` assert all
three criteria, wired as `test-build-modes` into `tier1-native.yml` (native shard
2). The script SKIPs on a cc-only kaic2 (DWARF is native-only), so it is safe in
the C-only tier1 and meaningful under `KAI_LLVM=1`. No `.out.expected` golden —
the assertions are structural (DWARF present, sizes ordered, trace names the
file), not output-diff, because the `.dSYM`/DWARF bytes and absolute temp paths
are not reproducible.

Coverage gap: the trace is asserted to *name* the `.kai`, not to land on an exact
line — a frame-perfect golden would be brittle against `-O0` codegen drift.

## Quality

`emit_native_di.kai` (the one new file) scores **A++ (98.9)** on `km score` — 108
LOC, cognitive complexity well under the bar. The edits to the existing F-grade
monoliths (`runtime.h`, `driver.kai`) neither raised nor lowered their grade. No
new duplicate groups.

## Cost vs. estimate

The issue estimated ~3-4 days. The DWARF emission itself was a day's worth; the
two days the brief did not see were (1) the Mach-O dsymutil discovery and (2) the
"wrong subprogram" verifier fight — both LLVM-platform integration details, not
language work. The KIR position metadata being already in place (Risk 7) saved
the position-tracking half outright.

## Follow-ups left for next lanes

- **Frame-accurate traces.** Inlining/recursion at `-O0` still collapses some
  frames; a `DW_AT_call_*` or `-fno-omit-frame-pointer`-style discipline could
  recover the innermost panic frame. Out of scope per #500 (split DWARF / dSYM
  bundling were explicitly deferred).
- **Linux native CI proof.** The `.kai:line` trace is verified on macOS dev; CI
  exercises the same path on ELF, where the DWARF is in-binary (no dsymutil). If
  `addr2line`'s PIE base handling differs, the check's de-slide (`dladdr`) is the
  load-bearing piece to watch.
