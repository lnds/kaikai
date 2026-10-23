# WebAssembly target — what runs, what is missing, and the PRs

A plan, not an implementation: nothing in the compiler, runtime, stdlib,
Makefiles or CI changes with this document. It re-measures the spike recorded
in issue #2538 on today's tree, lists the gaps by the code they live in, sets
out the decisions that belong to the owner, and orders the work into PRs.

**Landed so far: PR 1 (#2783).** Its entry in section 4 describes what
merged, and what it measured is folded into sections 2 and 4.

**Decided by the owner: the browser comes first, `wasm32-wasi` is a later
phase.** Section 4 is ordered around that. Every other decision in section 3 is
open; this document recommends and does not decide.

`CLAUDE.md` and `docs/design.md` still say WebAssembly is post-MVP and not to
be designed against. Adopting this plan changes that scope on purpose; those
lines are the owner's to edit.

## How to read the claims

Every factual claim carries one of two labels.

- **(M)** measured for this document, on kaikai 0.147.0 (`d2395a81d`), on an
  arm64 Mac, with the unchanged compiler. Tools: wasi-sdk 34.0 (clang 23.1.0),
  emscripten 6.0.12, wasmtime 49.0.2, binaryen 133, Chrome 155 headless,
  node 23.10.0, deno 2.8.2 (V8 14.9), bun 1.3.14 (JavaScriptCore),
  LLVM 22.1.7.
- **(R)** read: from the code, or from the source cited next to it. Engine
  release numbers are read from the WebAssembly feature table
  (`WebAssembly/website`, `features.json`, fetched 2026-10-13).

- **(M1)** measured for PR 1 on its own tree (#2783), on an arm64 Mac, on
  the host and not under WebAssembly: a build whose `CFLAGS` set every
  `KAI_HOST_*` macro to 0, C backend unless the claim names the native one.

Firefox and Safari were not driven. Wherever they appear, the claim is (R).

The scratch material is small and is described in the appendix so it can be
rebuilt: a force-included header, four empty headers, a stub file, one changed
line in a copy of `runtime.h`, and a 48-line JavaScript host.

## 1. What runs today

### The route

The compiler is unchanged. `kai build --backend=c` emits C; a WebAssembly C
toolchain compiles it together with `stage2/runtime.h`. The wrapper already
honours `CC` and `CFLAGS`, so the whole build goes through `bin/kai` (M):

```sh
CC=<wasi-sdk>/bin/clang \
CFLAGS="--target=wasm32-wasip1 -O2 -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false \
        -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS \
        -D_WASI_EMULATED_GETPID -include shim.h -I stubs stubs.c \
        -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks \
        -lwasi-emulated-getpid -lsetjmp" \
KAI_INCLUDE=<dir with the patched runtime.h> KAI_MODULAR=0 KAI_BACKEND=c \
  kai build app.kai -o app.wasm
```

Two things in that line are forced, not chosen (M):

- `KAI_MODULAR=0`. The default modular C build fails at link: the wrapper adds
  `-Wl,-stack_size,0x8000000` because the *host* is a Mac
  (`stack_ldflags` in `tools/kai/cli_toolchain.kai`), and `wasm-ld` rejects it.
- `-mllvm -wasm-use-legacy-eh=false`. Without it clang lowers `setjmp` to the
  legacy exception encoding, which wasmtime 49 refuses
  (`legacy_exceptions feature required for try instruction`).

The unmodified runtime does not compile against wasi-libc: 169 errors and 4
missing headers (`execinfo.h`, `netdb.h`, `sys/wait.h`, `ucontext.h`) (M). All
of them sit in code a pure program never calls; section 2 names the functions.

### The same module in a browser

The module built above imports 16 functions, all under
`wasi_snapshot_preview1` (M): `args_get`, `args_sizes_get`, `environ_get`,
`environ_sizes_get`, `clock_time_get`, `fd_close`, `fd_fdstat_get`,
`fd_fdstat_set_flags`, `fd_prestat_get`, `fd_prestat_dir_name`, `fd_read`,
`fd_seek`, `fd_write`, `poll_oneoff`, `proc_exit`, `sched_yield`. It exports
`memory` and `_start`.

A browser has no WASI. A 48-line JavaScript host that defines those 16
functions is enough: `fd_write` hands the text of fd 1 and 2 to a callback,
`environ_get` supplies `KAI_THREADS=1`, `proc_exit` throws, the rest return a
constant. With it, a page that inlines the module ran the programs below in
headless Chrome 155 and wrote their output into a `<pre>` (M). No module was
built that names no `wasi_snapshot_preview1` import; the import names are
wasi-libc's, the implementation is the page's.

### Program classes

One fixture per class, output and exit status compared with the C backend on
the host at `KAI_THREADS=1`.

| class | wasmtime 49 | Chrome 155 (48-line host) | emscripten, node |
|---|---|---|---|
| arithmetic, recursion, `Real` | identical (M) | identical (M) | identical (M) |
| records, sum types, `match`, list map over strings | identical (M) | identical (M) | identical (M) |
| handler, tail `resume` | identical (M) | identical (M) | not run |
| handler, non-tail `resume(10) + 100` | identical (M) | identical (M) | not run |
| handler that never resumes | identical (M) | identical (M) | identical (M) |
| `Seq` (lazy sieve over `lazy [2..]`) | identical (M) | identical (M) | identical (M) |
| actor on the main fiber (`with_mailbox`, `send`, `receive`) | identical (M) | identical (M) | identical (M) |
| handler that keeps `resume` (`examples/segments/generator.kai`, `continuation_escape_as_value.kai`) | exits 1: `kai: stack segments are not supported on this target` (M) | same (M) | not run |
| `nursery` / `spawn` | dies at `getcontext` (M) | same (M) | not run |

Without exception handling (a `setjmp` that returns 0 and a `longjmp` that
aborts), the pure, non-tail-resume and benchmark programs are still identical;
the handler that never resumes dies at `longjmp` (M).

### The parity corpus

The corpus `tools/test-backend-parity.sh` walks (`tools/lib/corpus.sh`): 1 269
entry points, of which the harness's own rules skip 130 and 2 are
package-mode fixtures that do not build as a file. The remaining 1 137 were
built for the host and for `wasm32-wasip1` and run under wasmtime, both at
`KAI_THREADS=1`. Every one of them built for WebAssembly (M).

| outcome | fixtures |
|---|---|
| output and exit status identical | 961 |
| fibers: dies at `getcontext` | 120 |
| a handler keeps `resume`: no segment switch | 20 |
| the reactor: `kai: reactor pipe() failed` | 18 |
| sockets, DNS, processes, signals: `Function not implemented` | 17 |
| the engine's stack: a recursion 20 000 deep (`examples/perceus/variant_spine_free_1083.kai`) | 1 |

Twelve of the 961 need the runner to grant what the fixture assumes: ten write
under `/tmp`, two read `PATH`. With only the working directory preopened and
no environment passed they differ; with `--dir /tmp` and the variable they are
identical (M). No fixture printed different output for any other reason: each
of the 176 that differ stops at one of the five diagnostics in the table.

### Size

The arithmetic fixture (the others are within 17 KB of it) (M):

| build | bytes | gzip -9 |
|---|---|---|
| wasi-sdk, `-O2`, as linked (carries libc's debug sections) | 479 485 | — |
| the same after `wasm-opt -Oz --strip-debug` | 170 180 | 47 223 |
| the same built without exception handling, `-Oz` | 168 501 | 46 667 |
| emscripten `-O2`: module | 168 791 | 45 880 |
| emscripten `-O2`: generated JavaScript | 66 880 | 18 729 |
| host binary, C backend | 953 976 | — |
| host binary, native backend | 335 240 | — |

Where the linked code comes from, before `wasm-opt` (linker map, code section,
204 203 bytes) (M): the program object 118 422 (the emitted C plus the
`static inline` runtime it pulls in), wasi-libc and compiler-rt 45 871, the
runtime owner object 39 658. The libc share is 101 archive members;
`dlmalloc` (10 977) and `vfprintf` (9 945) are nearly half of it.

### Speed

`fib(34)` plus a 2 000 000-element list built, mapped and summed.
Whole-process wall time, minimum of 9 runs alternated serially; the machine
was not quiet (load average about 5), so read the ratios, not the
milliseconds (M):

| build | min | against the C backend on the host |
|---|---|---|
| host, C backend | 106.1 ms | 1.00× |
| host, native backend | 174.7 ms | 1.65× |
| wasmtime, precompiled, `-O2` | 137.3 ms | 1.29× |
| wasmtime, precompiled, plus `wasm-opt -O3` | 132.0 ms | 1.24× |
| wasmtime, precompiled, built without exception handling, `wasm-opt -O3` | 133.7 ms | 1.26× |
| the same with Asyncify applied | 236.4 ms | 2.23× |

Inside a JavaScript engine, timing `_start` alone with the module already
compiled, minimum of 7 (M): V8 (deno) 155.7 ms, JavaScriptCore (bun) 99.2 ms,
node 23.10 with `--experimental-wasm-exnref` 233.9 ms.

### Recursion depth

A non-tail recursion over a list (`h % 7 + walk(t)`) runs 1 000 000 deep on
the host. On WebAssembly the engine's own stack is the limit, not linear
memory: giving the module an 8 MiB linear stack changes nothing (M).

| engine | deepest that ran | first that failed |
|---|---|---|
| wasmtime 49, defaults | 6 000 | 8 000 |
| wasmtime 49, `-W max-wasm-stack=67108864` | 400 000 | not reached |
| Chrome 155 | 6 000 | 8 000 |
| V8 in deno 2.8.2 | 8 000 | 12 000 |
| JavaScriptCore in bun 1.3.14 | 32 000 | 48 000 |

In a browser the failure is a JavaScript `RangeError: Maximum call stack size
exceeded` thrown out of `_start`, not a kaikai diagnostic. A page cannot raise
the limit.

### The segment switch on a target with no assembly path

`KAI_SEG_SWITCH` is defined in `stage2/runtime.h` only for x86_64 and for
aarch64 under clang or ELF (R). Elsewhere the same section defines
`kai_seg_start`, `kai_seg_suspend`, `kai_seg_resume`, `kai_seg_request` and
`kai_seg_escape` as calls to `kai_seg_unsupported`, which prints the message
in the table and exits 1 (R, M). The compiler does not know: `clause_need` in
`stage2/compiler/resume_segment.kai` picks a segment handle from the shape of
the clause alone, so the program builds and fails at its first kept
continuation (M). Handlers whose clauses resume in tail position, resume
once in non-tail position where the split applies, or never resume do not
reach a segment and work (M).

### The native backend through `llc -mtriple=wasm32`

The IR was dumped with `KAI_NATIVE_DUMP_IR` from whole-program native builds
of three fixtures.

- The IR carries no triple, no datalayout, no `musttail` and no inline
  assembly (M).
- It does carry exception-handling constructs the spike did not see: the
  arithmetic fixture has 437 `invoke`, 75 `landingpad … cleanup` and 42
  `personality ptr @__gcc_personality_v0` clauses (M). They are the
  landing pads that release references when a trap unwinds.
- `llc -mtriple=wasm32-wasip1 -O2` accepts the IR and writes an object (M).
  With no exception model selected LLVM lowers every `invoke` to a call, so
  the cleanup pads are dead.
- `llc … -wasm-enable-eh -exception-model=wasm -mattr=+exception-handling`
  crashes in the function pass manager (M). WebAssembly exception handling in
  LLVM takes funclet-style pads (`catchswitch` / `cleanuppad`), not
  `landingpad` (R).
- The native runtime owner does not compile for wasm32:
  `stage0/runtime_llvm.c` fails `kaix_evidence_fits_node`, the check that
  `KaiEvidence` fits the `[8 x ptr]` the emitter reserves (M). That is one
  error; nothing past it was linked or run.
- The emitter types a value's slots as `[16 x i64]` (`nkaivalue_header_type`
  in `stage2/compiler/emit_native_slot.kai`) and the native runtime pins
  `offsetof(KaiValue, as) == 8` (R). Whether the 8-byte slot stride is what
  made the spike's list fixture print the wrong total was not re-run.

### Against the spike

| claim in #2538 | spike | today |
|---|---|---|
| unmodified runtime against wasi-sdk 34 | 169 errors, 5 missing headers | 169 errors, 4 missing headers (M) |
| shim | 108-line header, 5 empty headers, 8-line stub file | 107-line header, 4 empty headers, 50-line stub file, 1 changed line of `runtime.h` (M) |
| pure, records, list map | identical | identical (M) |
| handlers with exception handling | correct | identical, with the exnref encoding selected explicitly (M) |
| handlers that keep the continuation | did not exist | exit 1, no segment switch (M) |
| `nursery` / `spawn` | dies at `getcontext` | the same (M) |
| size raw / optimised / gzip | 403 KB / 182 KB / 50 KB | 479 KB / 170 KB / 47 KB (M) |
| speed against the host C backend | about 1.4× | 1.24× to 1.29× (M) |
| native IR | no `invoke`, no `landingpad` | 437 `invoke` and 75 `landingpad` in the smallest fixture (M) |
| native runtime layout check | fails at `runtime_llvm.c:1551` | still fails, now `kaix_evidence_fits_node` (M) |
| recursion depth | not measured | 6 000 frames in Chrome against 1 000 000 on the host (M) |
| JavaScript imports, suspension | not measured | `extern "C"` symbols link as imports; JSPI suspends `main` (M, section 3) |

## 2. What is missing

Each gap names the code and why WebAssembly cannot do it as written. Line
numbers are left out on purpose; the function names are stable.

**Runtime, `stage2/runtime.h`.**

1. **Headers and feature gates.** The include block at the top pulls
   `<execinfo.h>`, `<netdb.h>`, `<sys/wait.h>` and `<ucontext.h>`, which
   wasi-libc does not have, and `<signal.h>`, `<sys/mman.h>`,
   `<sys/resource.h>` and `<setjmp.h>`, which stop with `#error` unless an
   emulation flag or exception handling is on (M). Under emscripten only
   `<execinfo.h>` is missing (M).
2. **Fibers.** `kai_fiber_init_ctx_sized`, `kai_ctx_set_stack`,
   `kai_uc_link_target` (`getcontext`, `makecontext`); `kai_sched_yield`,
   `kai_sched_park_linked`, `kai_worker_loop` (`swapcontext`);
   `kai_fiber_trampoline`, `kai_bootstrap_trampoline`,
   `kai_fiber_uc_link_landing` (`setcontext`). WebAssembly keeps return
   addresses and locals on a stack the program cannot address, so no library
   can save one stack and install another. `kai_default_spawn_spawn` and the
   `N > 1` branch of `kai_sched_bootstrap` both reach these. At one thread
   `kai_sched_bootstrap` calls `user_main` directly, which is why programs
   without `spawn` run (R, M).
3. **Kept continuations.** `kai_seg_switch`, `kai_seg_boot` and
   `kai_seg_frame_init` are assembly for two architectures; the fallback is
   `kai_seg_unsupported`. Same reason as fibers.
4. **Non-local exits.** `longjmp` in `kai_fiber_pad_jump`, `_longjmp` in
   `kai_seg_escape` and in the dispatch path that lands a discarded
   continuation on its handle, and the `_setjmp` / `_longjmp` pair the C
   emitter writes for every handle that can be abandoned
   (`stage2/compiler/emit_c.kai`). WebAssembly has no `setjmp`; clang lowers it
   onto the exception-handling proposal or not at all.
5. **Trap unwinding.** `kai_trap_unwind` walks the frames with
   `_Unwind_ForcedUnwind` and `kai_trap_stop` reads `_Unwind_GetCFA`, so the
   frames a contained trap crosses release their references. There is no
   unwinder for C frames in wasi-libc. With a stub that reports failure the
   function falls through to `land` at once (M): correct control flow, and the
   references held by the crossed frames leak.
6. **Stacks, guard pages, overflow.** `kai_stack_map` (`mmap` plus a guard
   page), `kai_install_thread_sigaltstack`,
   `kai_install_fiber_sigsegv_handler`, `kai_fiber_sigsegv_handler`,
   `kai_main_fiber_stack_size` (`getrlimit`). No `mprotect`, no signals. The
   overflow diagnostic kaikai prints on the host becomes the engine's trap,
   and the depth is the engine's (section 1).
7. **The reactor.** `kai_reactor_init` opens self-pipes with `pipe` and
   installs a `SIGCHLD` handler; `kai_reactor_wait` blocks in `poll`;
   `kai_reactor_init_filepool` starts pthreads. Fixtures that reach it fail
   with `kai: reactor pipe() failed` (M). In a browser a blocking wait is not
   available at all: the thread that would block is the page.
   **`File` is behind it.** Every default handler of `File` (`read_file`,
   `write_file`, `open_read`, `read_chunk`, `open_write`, `write_chunk`,
   `close_file`) calls `kai_reactor_init` and then runs its work through
   `kai_reactor_run_in_pool`, on the file pool's threads (R). So do
   `Stdin.read_line` and `Stdin.read_bytes`, which park on the reactor for
   readiness, and `Clock.sleep_ns` (R). With the reactor absent
   `File.read_file` and `Clock.sleep_ns` stop with
   `kai: this host has no reactor` (M1).
8. **Host facilities with nothing behind them.** Sockets and DNS
   (`_kai_net_record_fd`, `kai_default_nettcp_listen`, `kai_default_netudp_bind`,
   `kai_default_netudp_send`, `kai_default_netudp_recv`,
   `kai_default_netdns_resolve`, `kai_default_netunix_listen`,
   `_kai_unix_*`); processes (`kai_default_process_start`, `_kai_proc_*`,
   `kai_default_process_kill`, `kai_default_process_kill_group`); signals
   (`kai_default_signal_on`, `kai_default_signal_off`, `_kai_signal_*`); the
   panic backtrace (`kai_capture_frames`, `kai_panic_backtrace`: `backtrace`,
   `dladdr`, `popen`). `File` compiles for the target, and a browser has no
   file system to give it; whether it runs under wasmtime is open again.
   The first version of this document said it does, and the corpus table of
   section 1 counts ten fixtures that write under `/tmp` among the identical
   ones; `examples/effects/m7a_7_default_file.kai` is a fixture of that corpus
   that writes there through `File.write_file` (R). That does not fit entry 7: the handler opens
   the reactor's pipes first (R), and on the host with the reactor absent it
   stops there (M1). PR 2 re-measures it under wasmtime before it relies on
   either statement.
9. **The random source.** `_kai_securerandom_fill` and the `#error` above it
   know Linux and the BSD family. wasi-libc has `arc4random_buf`; a browser
   has `crypto.getRandomValues` behind an import.
10. **Threads.** `kai_read_nthreads` takes `KAI_THREADS` or the CPU count.
    wasi-libc's pthreads are single-thread stubs, so the calls compile, and
    any `N > 1` needs fibers first (gap 2).

**Wrapper, `tools/kai`.**

11. **There is no target.** `resolve` in `cli_toolchain.kai` reads the host's
    `uname` into `os` and `arch`; `stack_ldflags` and `dead_strip` choose
    linker flags from the host; `host` folds the host into the object-cache
    key, so a WebAssembly runtime object is cached beside the host's under a
    key that does not say which it is (M: it landed in `nmodular/runtime/`);
    `reads_scheduler_tls` in `cli_plan.kai` runs the host's `nm`; `cli_build.kai`
    parses no `--target`. Nothing emits the JavaScript that loads the module,
    and `kai run` executes the output as a host binary.
12. **`[native]` in `kai.toml`.** Vendored `sources` compile with whatever
    `CC` is, so they would build; `libs` names system libraries that do not
    exist for the target.

**Compiler, `stage2/compiler`.**

13. **The entry point's row is checked without a target.**
    `check_main_row_handlable` in `infer.kai` accepts any effect that has a
    default handler. On WebAssembly `Process`, `NetTcp`, `Signal` and, until
    fibers exist, `Spawn` have none that works, so the program builds and
    fails at run time, where a row type could have refused it at compile time.
14. **Foreign declarations know one linker.** `parse_extern_decl` in
    `parse.kai` accepts the ABI strings `"C"` and `"kai"`; `ExternAbi` in
    `ast.kai` is `AbiC | AbiKai`; the symbol override must be a C identifier.
    A WebAssembly import has a module name and a field name, and neither is a
    C identifier in general (`console.log`). A function-typed parameter is
    rejected, so JavaScript cannot call back. An `extern "C" opaque` box
    (`KAI_FOREIGN`) frees nothing when it dies, by design.
15. **User effects cannot carry a default written in kaikai.** A
    `default { }` block accepts only `$extern_handler("<C symbol>")` in
    codegen (`kai info effects`). A stdlib effect whose default is kaikai code
    over a JavaScript import therefore cannot install itself at `main` today.
16. **The native backend was a host backend (closed by phase 4).** The triple and CPU came from
    `LLVMGetDefaultTargetTriple` (native section of `runtime.h`); the IR uses
    `landingpad`; the layout is pinned to 64-bit pointers in the emitter and
    in `stage0/runtime_llvm.c`; `tools/gen-runtime-bc.sh` builds
    `runtime_llvm.bc` and `runtime_inline.bc` for the host; the FFI aggregate
    classifier models AArch64 and x86-64 SysV only (`kai info ffi`).

**Language surface.**

17. **`Int` is 64 bits.** An `Int` crossing to JavaScript is a `BigInt`, not a
    `Number` (M). The fixed-width boundary types (`I32`, `U32`, `F32`, …)
    exist for C and would be the ones to cross as `Number`.
18. **`main` runs once and the module is finished.** The module is a WASI
    *command*: `_start` runs `main`, flushes and tears libc down. Nothing may
    call into it afterwards, which is what an event handler does.

## 3. Decisions that are the owner's

### 3.0 Decided

- **The order of targets.** Browser first, `wasm32-wasi` later. Section 5
  says what the two share.
- **The route.** The C backend, compiled by a C compiler that targets
  WebAssembly. The native backend emitting WebAssembly directly (phase 4) is
  the destination, because it leaves nothing outside `kai`; it does not gate
  the first page. Everything in phases 1 and 2 except the choice of C
  compiler is used unchanged by phase 4.
- **How a user gets the toolchain.** `kai` fetches it: the first build for a
  WebAssembly target downloads a pinned toolchain, verifies its checksum and
  keeps it in the user cache. A user installs nothing by hand, as on the host
  targets. A toolchain named by an environment variable is used in its place.
- **Phase 1 is go** (2026-10-22), with the recommendations below adopted for
  it: wasi-libc with the page as host (3.1, pinned wasi-sdk), exnref for
  non-resuming handlers (3.2), one thread (3.7), accept and diagnose the
  engine's recursion limit (3.9), and the target spelled `wasm32-browser`.
  Phase 1's engine baseline is the engines that ship exnref (Chrome and Edge
  137, Firefox 131, Safari 18.4); the JSPI baseline of 3.8 waits for phase 2.
  `File` runs its work in place when there is no reactor, and `main` refuses
  on `wasm32-browser` the effects the page cannot run (PR 4). The playground
  (#2540) is out of scope.
- **Phase 2 is go** (2026-10-22), with the recommendations adopted: JSPI
  behind the existing seams (3.3); events as values `main` waits for, no
  callbacks from JavaScript (3.4); `extern "js"` with its own capability
  `Js`, type-checked on every target and refused at `main` where it cannot
  run (3.5); the browser stdlib as ordinary effects over `extern "js"` in
  `web.dom`, `web.fetch`, `web.storage` and `web.timer`, installed by a
  wrapper (3.6). Pages that suspend (fibers, waits, events) need the JSPI
  engines of 3.8 (Chrome and Edge 137, Firefox 153, Safari 27); pages that
  never suspend keep phase 1's exnref baseline.
- **Phase 3 is go, without fibers** (2026-10-23). `wasm32-wasi` ships on
  wasi-libc preview 1 with `File`, `Env`, `Clock`, `Random`, `Process.exit`
  and stdin on real WASI calls. `Spawn`, `Cancel`, `Link`, `Monitor`, `Actor`
  and kept continuations are refused at `main` on this target, as phase 1
  did for the browser, until the stack-switching proposal is usable in
  wasmtime; `Signal`, the `Net*` effects and `Js` are refused too. `kai run`
  and the parity leg use `wasmtime` from `PATH`.

### 3.1 Toolchain and libc

The C backend needs a C compiler that targets WebAssembly and a libc. With no
WASI in the browser, something must stand under `printf`, `malloc` and the
rest.

- **wasi-libc, with the page supplying its 16 imports.** One sysroot serves
  the browser and the later WASI phase. Module 170 KB (47 KB gzip), host 48
  lines (M). The runtime needs guards for what wasi-libc lacks: the 169
  errors. A user needs wasi-sdk or an equivalent clang, `wasm-ld` and sysroot;
  the wasi-sdk tarball for an arm64 Mac is 180 MB (M).
- **Emscripten.** Its libc declares the POSIX surface, so the unmodified
  runtime is 9 errors and one missing header away from compiling, and links
  with seven stubs (M). Module 169 KB plus 67 KB of generated JavaScript
  (46 KB + 19 KB gzip) (M). It brings its own answers for suspension
  (Asyncify or JSPI), threads (workers) and sockets (WebSocket emulation). It
  costs a 1.8 GB SDK with Python and node inside (M), a loader whose shape is
  emscripten's, and it shares nothing with a WASI phase.
- **A freestanding subset on `wasm32-unknown-unknown`.** A bare compile stops
  at the first libc header (M). The subset to write or vendor is what the
  module links today: 101 archive members, of which the allocator and
  `vfprintf` are half. The ceiling of the saving is the 46 KB libc occupies
  before optimisation, 22% of the code (M). It was not built.

Recommendation: **wasi-libc with the page as host.** It is the smallest set of
moving parts that was measured working in a browser, the guards it needs are
the same ones a browser needs in any case (a socket has nothing behind it
under any libc), and it is the only option whose work the WASI phase reuses.
Emscripten's advantage is a one-time saving in guards.

How a user gets the toolchain is decided in 3.0: `kai` fetches it once and
verifies it by checksum, the way `KAIC_BOOT=release` fetches a compiler. That
fixes which toolchain is fetched to the outcome of this decision, and makes
its download size (180 MB for wasi-sdk on an arm64 Mac, 1.8 GB for
Emscripten) a cost every user of the target pays once.

### 3.2 Non-resuming handlers

A handler that drops its continuation leaves through `longjmp`.

- **WebAssembly exception handling, exnref encoding.** Works today with two
  clang flags and `-lsetjmp` (M). Cost: 1.0% of module size (170 180 against
  168 501 bytes) and nothing measurable in time (132.0 against 133.7 ms) (M).
  Ships in Chrome 137, Firefox 131, Safari 18.4 (R); ran in Chrome 155, deno,
  bun and wasmtime 49 (M); node 23 needs a flag (M).
- **The legacy encoding.** What clang emits by default. Chrome 155 still
  validates it (M); wasmtime 49 rejects it (M). It is the superseded form.
- **A lowering without `longjmp`.** The abandoning clause returns a marker and
  every call it can cross checks for it. No engine feature needed. It is a new
  lowering in `emit_c.kai` and in the native emitter, on a path every existing
  platform runs.

Recommendation: **exnref.** It is the standard, it is in every current engine,
and it costs nothing measured. It also rules out Asyncify as built by binaryen
133, which aborts on a module that contains `try_table` (M) — see 3.3.

### 3.3 Continuations and fibers

Three things need a second stack: fibers (`spawn`, `nursery`, spawned actors,
cancellation across fibers), waits (`Clock.sleep`, `receive_timeout`, any
future `fetch`), and handlers that keep `resume`.

- **Asyncify** (a binaryen pass that rewrites every function able to reach a
  switch so it can unwind and rewind). Needs no engine feature. Measured on a
  module built without exception handling, with no switch ever taken: size
  171 071 → 290 347 bytes (+70%; gzip 46 259 → 72 994), and the benchmark
  1.77× slower under wasmtime, 1.43× under V8, 1.76× under JavaScriptCore (M).
  Telling it to ignore indirect calls saves a quarter of the growth and
  no time beyond noise (M).
  binaryen 133 aborts in its flatten pass on a module that uses exception
  handling (M), so it forces the third option of 3.2 as well. Each switch
  copies the live stack.
- **JSPI** (JavaScript Promise Integration: an import wrapped in
  `WebAssembly.Suspending` suspends the WebAssembly stack until its promise
  settles; an export wrapped in `WebAssembly.promising` returns a promise).
  Ships in Chrome 137, Firefox 153, Safari 27 (R); present in Chrome 155,
  deno 2.8.2 and bun, and in node 23 behind a flag (M). Measured with the
  unchanged compiler: a kaikai `main` called an import that sleeps 50 ms,
  `_start` returned a promise, a timer callback ran on the event loop, and
  `main` continued — in Chrome 155, deno and node (M). JSPI adds nothing to
  the module: a program that never suspends is the module of section 1, at
  the same size and speed. One
  suspend-and-resume round trip costs 59 to 77 ns in V8 and 555 ns in
  JavaScriptCore (M); the segment switch it would stand in for costs 4.9 ns on
  an M4 by the comment in `runtime.h` (R). It exists only where JavaScript
  does: wasmtime has none.
- **The stack-switching proposal** (`cont.new`, `resume`, `suspend`,
  `switch`). The exact shape of `kai_seg_switch`, with no trip through
  JavaScript. Phase 3; Chrome has it behind a flag, Firefox and Safari not at
  all (R). The probe module does not validate in Chrome 155, deno or bun, and
  wasmtime 49 on arm64 answers that the feature is not supported in this
  configuration (M). clang cannot emit it from C.
- **A CPS lowering for this target.** Functions whose row carries a switching
  effect are compiled to continuation-passing form; the row says exactly which
  ones. No engine feature. It is a second lowering of the effectful part of
  the language, in a compiler whose first principle after safety is to stay
  fast, and the parts of the runtime that park a fiber are C and stay outside
  it.

Recommendation: **JSPI**, behind the two seams the runtime already has — the
`kai_seg_*` entry points, which already have a second implementation for
targets with no switch, and the fiber context operations of gap 2. It is the
only option measured working with no compiler change and no cost to programs
that never switch, and waiting on the host is what a page's fibers mostly do.
A host with no JSPI refuses a program that needs it when the module loads,
with a diagnostic; it never runs it wrong. The stack-switching proposal is the
better mechanism and should replace JSPI inside the same seams once two
engines ship it. Asyncify as a fallback is not recommended: it is a second
mechanism to test on every change, it does not compose with 3.2, and its cost
lands on every program. The WASI phase needs its own answer here; see
section 5.

### 3.4 The event loop

A kaikai `main` is a function that returns; a page is a thing that stays. Two
shapes:

- **Events are values `main` waits for.** `main` runs under JSPI and a
  blocking-looking `Dom.next_event()` (or an actor mailbox fed by the host)
  suspends until the page has something. Nothing in JavaScript ever calls a
  kaikai closure, so there is no closure lifetime to manage and no question of
  which handlers are in scope: the code that reacts runs inside the `handle`
  blocks `main` installed. It is the model `Actor.receive` already has. It
  needs 3.3 for any interactive page.
- **JavaScript calls kaikai closures.** The module is built as a *reactor*
  (no teardown after `main`), exports one dispatcher, and keeps registered
  closures in a table. It needs no suspension for a click handler, so it works
  on an engine without JSPI. It needs an answer for what a callback's effect
  row means when no `handle` of `main` is on the stack, a reference held by
  the table until the handler is removed, and re-entrancy rules. The FFI
  rejects callbacks from C today for the same reason: a closure is a box with
  captures, not a function pointer.

*Waits (PR 7).* When every fiber is parked, `kai_reactor_wait` calls the
import `kai.wait_until(deadline_ms)`, a JSPI suspension that the host settles
at the deadline or earlier on a wake. The host's `hostEvents()` queue is the
entry for the page: `post(event)` queues a value and wakes a pending wait, or
is latched for the next one. A deadline within one frame waits for
`requestAnimationFrame` instead of `setTimeout` and the runtime counts up to
half a frame of earliness as elapsed, so a 16 ms loop lands on every frame.

Recommendation: **events as values first.** It adds no new kind of entry into
the program and it fits effects. If 3.3 is not JSPI, this recommendation
flips: without suspension only the callback shape gives an interactive page.

### 3.5 JavaScript interop: `extern "C"` or `extern "js"`

**As built (PR 5).** The path carries four forms: `a.b` (call, or read a
non-function with no arguments), `a.b=` (write), `.m` (member of the first
argument, call or read) and `.m=` (write a member of the first argument). The
host cannot see WebAssembly types, so the import name encodes the signature as
`<param letters>:<return letter>:<path>` (`I` i64, `N` number, `B` bool, `S`
string as pointer and length, `H` handle, `U` unit). A handle box carries a
marker that makes its drop call the host's `release`; the host builtin
`kai.handles` counts live handles. `[js]` in `kai.toml` takes one key,
`module`, read from the entry package and from each dependency. `Js` is accepted in `main`'s row on every target
and refused by `build_target` on a host build; the native backend refuses the
declaration.

What works today with no language change (M): three `extern "C"` functions
with no definition, linked with `-Wl,--allow-undefined`, became the imports
`env.js_add`, `env.js_title`, `env.js_sleep`; `Int` crossed as `BigInt`,
`String` as a pointer the host read as NUL-terminated bytes.

- **Map `extern "C"` to imports.** No syntax. But the import module is always
  `env` and the field must be a C identifier; allowing undefined symbols
  turns every misspelt C symbol from a link error into a failure when the
  module loads; the boundary types stay C's; and `extern "C"` would mean a
  linked symbol on one target and a host import on another.
- **`extern "js"`.** A third ABI string beside `"C"` and `"kai"`.

What `extern "js"` would be, if chosen:

- *What it declares.* One WebAssembly import. The parenthesised string is the
  import's field, and a dotted path is legal there because import names are
  arbitrary strings: `extern "js"("console.log") fn log(s: String) : Unit / Js`.
  The import module is one fixed name. The alternatives are two strings,
  module then field (MoonBit's and Grain's form), or a module named after the
  declaring file (AssemblyScript's default); one fixed module keeps the host a
  single lookup.
- *Who writes the JavaScript.* `kai build` emits the host, and the host
  resolves a field by walking the path from `globalThis`, binding the call to
  its parent object. A package that needs code which is not a global ships a
  module of its own through a `[js]` table in `kai.toml`, the counterpart of
  `[native]`, and its exports win. No JavaScript inside `.kai` source.
- *What crosses.* `Real` as `f64`. `Int` as `i64`, which JavaScript sees as
  `BigInt`; the fixed-width types as `Number`. `String` by copy, both ways,
  pointer plus length through the host. The JS string builtins proposal ships
  (Chrome 130, Firefox 134, Safari 26.2) (R) but it operates on host string
  references, not on bytes in linear memory, so it removes no copy for a
  language whose strings live in linear memory.
- *JavaScript values.* `extern "js" opaque Node` is an index into a table the
  host keeps, parked in the box `extern "C" opaque` already uses. One thing
  must differ from C: when the box dies the slot has to be released, or the
  table grows for ever. Perceus frees the box at its last use, so the release
  is deterministic. `externref` is the alternative representation; it cannot
  be stored in linear memory, which is where every kaikai value lives.
- *Callbacks.* Rejected in the signature, as for C, while 3.4 stands as
  recommended. If they are wanted later, a closure handed to JavaScript is a
  reference the host's table owns: the closure and everything it captures
  live until the handler is removed, and one that is never removed is a leak
  no reference count can see. wasm-bindgen documents three closure lifetimes
  for exactly this reason.
- *The effect row.* Either `Ffi`, or a capability of its own, `Js`.
- *On a target that is not WebAssembly.* Three choices: an error at the
  declaration, an error at the use, or the declaration type-checks everywhere
  and the capability has no default handler off the web, so a program whose
  `main` reaches it is refused at `main` by the check of gap 13.
- *`extern "C"` on WebAssembly.* Unchanged: a C symbol, compiled from
  `[native]` sources by the same compiler and linked into the module. An
  undefined one is a link error, as it is on the host.

The compiler side is small and mechanical (R): one arm in `parse_extern_decl`,
a validator for the path beside `parse_optional_extern_c_override`, one
constructor in `ExternAbi`, and the twelve modules that name its constructors
today — a `match` over the type that lacks the new arm does not compile, so
the compiler lists the sites. The C
emitter writes the declaration with clang's `import_module` and `import_name`
attributes where it writes `extern <ret> sym(<params>);` today. The native
emitter answers with a diagnostic until phase 4.

Precedent, read from each project's documentation on 2026-10-13 (R):

- **MoonBit**: two explicit strings, module then field
  (`fn cos(d : Double) -> Double = "math" "cos"`); the host supplies the
  import object by hand; inline JavaScript only on its JavaScript backend;
  JavaScript values as `externref` with no reference counting; opting into JS
  string builtins is a link option of its GC backend.
  (`docs.moonbitlang.com/en/latest/language/ffi.html`)
- **Rust, wasm-bindgen**: an attribute on an `extern "C"` block; import names
  are generated, not chosen; the tool writes the JavaScript; strings are
  copied with `TextEncoder` / `TextDecoder`; a JavaScript value is an index
  into a host-side table, released by `Drop`; closures have three documented
  lifetimes; off WebAssembly an import panics when called.
  (`wasm-bindgen/guide`, *importing-js*, *js-objects-in-rust*,
  *passing-rust-closures-to-js*)
- **Kotlin/Wasm**: `external` declarations resolved on the global scope or an
  ES module, with generated glue; `js("…")` puts JavaScript text in a function
  body; `@WasmImport(module, name)` is the raw form; JavaScript interop is
  refused on the WASI target. (`kotlinlang.org/docs/wasm-js-interop.html`)
- **AssemblyScript**: `declare`, with the module defaulting to the file path
  and `@external(module, name)` overriding; bindings generated on request,
  where module `env` means `globalThis`; no closures.
  (`assemblyscript.org/concepts.html`)
- **Zig**: a bare `extern fn`, satisfied from module `env`; the user writes
  the import object. (`ziglang.org/documentation/master`, *WebAssembly*)
- **Grain**: `foreign wasm name: Type from "module"`, with `@externalName`
  for the field. (`grain-lang/grain`, `stdlib/runtime/wasi.gr`)

Explicit module-and-field strings are the common form; generated hosts and
handle tables are wasm-bindgen's; nobody but Kotlin and wasm-bindgen puts
JavaScript in source, and wasm-bindgen discourages it.

Recommendation: **`extern "js"`, with its own capability `Js`, type-checked on
every target and refused at `main` where it cannot run.** Overloading
`extern "C"` saves a parser arm and costs the link check on every C symbol. A
separate capability keeps `Ffi` meaning what it means today, lets a signature
say that a function needs a JavaScript host, and turns "this library is for
the web" into a row error at build time instead of a missing import at load
time. Refusing at `main` rather than at the declaration matters because
kaikai has no conditional compilation: an error at the declaration would make
a package that supports both the web and the host impossible to write.

### 3.6 The stdlib surface in the browser

What has no meaning on a page: `Process`, `Signal`, `NetTcp`, `NetUdp`,
`NetDns`, `NetUnix`, and `File` (the modules `os.process`, `net.*`, `fs.*`).
What keeps its meaning with a different default behind it: `Stdout` and
`Stderr` (the console, and an element when the page provides one), `Clock`
(the host's clock; sleeping needs 3.3), `Random` and `SecureRandom`
(`crypto.getRandomValues`), `Env` (empty, or what the page passes), `Log`.
Everything pure is unaffected.

What a page adds: the document, `fetch`, storage, timers.

- **In the compiler and runtime**, as builtin effects with C defaults, the way
  `File` and `NetTcp` are. The compiler and `runtime.h` learn the browser.
- **In the stdlib**, as ordinary effects (`Dom`, `Fetch`, `Storage`, `Timer`)
  whose handlers are kaikai code over `extern "js"`. The compiler knows a
  target, an ABI string and a capability, and nothing about the DOM.

The second needs one of two things, because of gap 15: either kaikai-bodied
`default { }` clauses, or an installer the program wraps its body in — the
shape `with_mailbox` has for `Actor` — which needs nothing new.

Recommendation: **stdlib over `extern "js"`, installed by a wrapper**
(`web.run { … }`), and effects that cannot run on the target refused at
compile time through the row of `main`. The excluded effects stay declared, so
code that merely mentions them in a row still type-checks, and a test can
still handle them with a scripted handler. Which module names and which
operations is a stdlib design of its own and is not attempted here.

### 3.7 Threads

- **One thread.** `KAI_THREADS` is fixed at 1. Fibers and actors, once 3.3
  lands, are concurrent and not parallel.
- **Workers with shared memory.** WebAssembly threads ship everywhere
  (Chrome 74, Firefox 79, Safari 14.1) (R), and wasi-sdk carries a
  `wasm32-wasip1-threads` sysroot (M). A page gets `SharedArrayBuffer` only
  when it is cross-origin isolated: a `file://` page in Chrome 155 does not
  have it (M), and a served one needs two response headers. The main thread
  may not block, so the scheduler's condition-variable parking cannot run
  there; `main` would have to live in a worker, away from the DOM.

Recommendation: **one thread for the browser phase.** Parallelism is a
decision to take after fibers work, with its own measurements.

### 3.8 The baseline of engines

The standing rule is current systems only, with nothing legacy carried.

| feature | needed by | Chrome | Firefox | Safari |
|---|---|---|---|---|
| exception handling, exnref | every program, under 3.2 | 137 (R), yes in 155 (M) | 131 (R) | 18.4 (R) |
| JSPI | fibers, waits, kept continuations, under 3.3 | 137 (R), yes in 155 (M) | 153 (R) | 27 (R) |
| stack switching | nothing yet | flag only (R), no in 155 (M) | no (R) | no (R) |
| threads | nothing, under 3.7 | 74 (R) | 79 (R) | 14.1 (R) |
| JS string builtins | nothing, under 3.5 | 130 (R) | 134 (R) | 26.2 (R) |
| legacy exception handling | nothing | 95 (R), yes in 155 (M) | 100 (R) | 15.2 (R) |

Recommendation: **the engines that ship both exnref and JSPI — Chrome and
Edge 137, Firefox 153, Safari 27 — and nothing older.** No legacy exception
encoding, no Asyncify build for engines without JSPI. One point needs the
owner's eye before it is written down: the Safari on the Mac these
measurements ran on is 26.6.2, below the version the table gives for JSPI, and
Safari was not driven. If Safari 27 is not yet what users have, the choice is
between waiting for it and shipping interactive pages that Safari cannot run.

### 3.9 Recursion depth

A browser gives a kaikai program about 6 000 frames of non-tail recursion
where the host gives a million (section 1). Tail calls are unaffected: the 22
fixtures of `examples/tco`, mutual recursion ten million calls deep among
them, give identical output under wasmtime (M).

- **Accept the engine's limit**, turn the `RangeError` into kaikai's
  stack-overflow diagnostic in the host, and document it.
- **Audit the stdlib** for non-tail recursion over data and make those
  functions iterative. Worth doing on its own merits; it helps the host too.
- **Run deep code on stacks the runtime allocates.** That is 3.3 again, and
  neither JSPI nor the stack-switching proposal lets a module choose a stack's
  size.

Recommendation: **accept and diagnose**, and treat the audit as ordinary
stdlib work.

## 4. The PRs, in order

Two properties hold for every PR below and are checked the same way each time,
so they are stated once.

- **No existing platform regresses.** WebAssembly is reached only through the
  C backend and a cross compiler; the five required checks (`tier1`,
  `tier1-native`, `tier1-tsan`, `tier1-mn-corpus`, `tier1-macos-mn`) run
  unchanged on every PR that touches code. A PR that edits `runtime.h` also
  triggers `tier1-asan` and `linux-aarch64` by their path gates. Selfhost
  byte-identity is the proof that a PR which is not supposed to change
  emitted C did not.
- **The compiler does not get slower.** A PR that touches `stage2/compiler`
  is measured the way the repository already measures compile cost: the tier 0
  gate that the compiler allocates within its ceiling on a fixed program, and
  an instruction-count comparison of the self-compile against `main` on a
  quiet machine. A cost above noise blocks the merge until it is recovered.
  PRs 1 to 3, 6, 7 and 9 do not touch `stage2/compiler`.

The WebAssembly gates are new jobs inside `tier1.yml`, behind `needs: build`
so they consume the `kaic2-c` artifact, path-gated the way `tier1-asan` is,
and **not required** until the owner says so — the shape `linux-aarch64` has.
The hosted Ubuntu image already carries Chrome 154 with ChromeDriver and
Firefox 157 with geckodriver (R), so a real browser costs no install; the C
toolchain for the target is fetched by pinned URL and checksum.

What each kind of runner is honest for:

- **node with the generated host**: the module's semantics, the host's
  marshalling, and suspension plumbing, on V8. Not the DOM, not a real event
  loop, not the engine's stack limit (V8 in deno ran 8 000 frames where Chrome
  ran 6 000), not Firefox or Safari. The image's node is 22 and the feature
  table lists exnref from node 24.15 (R), so the job pins a current one.
- **Headless Chrome, and Firefox, through their drivers**: that a page loads
  the module and that what the program printed or changed is in the document,
  on a real event loop. The only honest proof of a DOM effect.
- **wasmtime**: that the runtime computes the same thing off the host
  architecture. It proves nothing about a browser and belongs to phase 3.

### Phase 1 — a page

**PR 1 — `refactor(runtime): host facilities sit behind capability macros`** — landed, #2783

- *Touched*: `stage2/runtime.h`; two new scripts, `tools/test-host-minimal.sh`
  and `tools/runtime-preprocess-diff.sh`; one target in `stage2/Makefile` and
  its line in `stage2/test-lists/light.txt`; one step in
  `.github/workflows/tier1.yml` (job `tier0`) and one in `linux-aarch64.yml`;
  `docs/build-system.md`; eight fixtures under `examples/runtime/`.
- *Does*: eight macros, each 1 unless the build defines it otherwise. With a
  macro at 0 the runtime includes none of the facility's headers and imports
  none of its libc entry points; reaching the facility prints
  `kai: this host has no <facility>` and exits 1, with the two exceptions the
  table names.

  | macro | facility | at 0 |
  |---|---|---|
  | `KAI_HOST_FIBERS` | context switch by `ucontext` (gap 2); on WebAssembly, through the host (PR 6) | one thread, `KAI_THREADS` not read; creating a fiber stops (`fibers`) |
  | `KAI_HOST_STACK_GUARD` | mapped stacks, guard page, overflow handler (gap 6) | `KAI_SEG_SWITCH` is not defined either, so a handler that keeps `resume` takes the path of a target with no switch: `kai: stack segments are not supported on this target` |
  | `KAI_HOST_REACTOR` | self-pipes, `poll`, the file pool (gap 7) | `kai_reactor_init` stops (`reactor`): `File`, `Stdin` reads; on WebAssembly `kai_reactor_wait` parks through `kai.wait_until` (PR 7), so sleeps and `receive_timeout` run |
  | `KAI_HOST_SOCKETS` | sockets and DNS (gap 8) | every `NetTcp`, `NetUdp`, `NetUnix`, `NetDns` handler stops (`sockets`) |
  | `KAI_HOST_PROCESS` | child processes (gap 8) | every `Process` handler but `exit` stops (`processes`) |
  | `KAI_HOST_SIGNALS` | signal subscription (gap 8) | every `Signal` handler stops (`signals`) |
  | `KAI_HOST_BACKTRACE` | the debug panic's trace (gap 8) | does not stop: the panic prints, then `kai: this host has no stack trace`, and exits as it did |
  | `KAI_HOST_UNWIND` | the frame unwinder (gap 5) | does not stop: `kai_trap_unwind` lands at once, the path it already took when a walk could not start; the frames crossed keep their references |

  The unwinder lands instead of stopping because a failed `assert` under
  `kai test` leaves through it: a stop would end a test run at its first
  failing test. A file with one failing, one trapping and one passing test
  prints the same with and without it (M1).

  Three combinations are refused with `#error`: `KAI_HOST_FIBERS` without
  `KAI_HOST_STACK_GUARD` (a fiber runs on a mapped stack);
  `KAI_HOST_SOCKETS`, `KAI_HOST_PROCESS` or `KAI_HOST_SIGNALS` without
  `KAI_HOST_REACTOR` (their waits park on it); `KAI_MAIN_STACK_SIZE` without
  `KAI_HOST_FIBERS` (`main` moves to its stack by a context switch).
- *A user can run*: nothing new. It is the ground the next two stand on.
- *Proof*: `make -C stage2 test-host-minimal`, in the tier 1 light pool, runs
  `tools/test-host-minimal.sh` on the C backend with every macro at 0.
  `host_minimal_pure`, `host_minimal_handler` and `host_minimal_seq` print
  their `.out.expected`, as a default build does. `host_minimal_spawn`,
  `_sleep`, `_socket`, `_process` and `_signal` exit 1 with exactly their
  `.run.err.expected`, one per facility a program can ask for. No binary
  imports a libc entry point of a facility (`nm -u` against the list in the
  script): the spawn fixture's binary went from 107 imports to 47, 27 of the
  107 on the list and none of the 47 (M1). The runtime also compiles with
  each facility off on its own.
- *Risk to existing platforms, as checked*: `tools/runtime-preprocess-diff.sh`
  compares `cc -E -P` of the runtime at the base with the working tree, blank
  lines removed, in the seven shapes a build instantiates it (single unit, C
  owner, C module, native owner, native hot bitcode, RC ledger, sized main).
  All seven were identical on an arm64 Mac under Apple clang and GCC 15, on
  Linux x86_64 under GCC and clang, and on Linux aarch64 under GCC (M1; the
  two Linux results are from the PR's CI). A PR carrying the label
  `runtime-text-identical` is held to the comparison by the two workflow
  steps; the label is read when the step runs. PR 2 carries it.
- *Not covered*: the native backend. See phase 4.

**PR 2 — `feat(runtime): the runtime compiles for wasm32 and a host module runs it`**

- *Touches*: `stage2/runtime.h` (the macros of PR 1 derive from `__wasm__`;
  the include block; the random-source gate; one thread); a new
  `stage2/web/kai_host.mjs`; `tools/test-backend-parity.sh` (a `wasm` value
  for `TARGET_BACKEND`); a new `tools/wasm-parity-baseline.txt`;
  `.github/workflows/tier1.yml` (job `tier1-wasm`); `docs/build-system.md`.
- *Does*: the shim of the appendix disappears into the runtime. The build is
  still driven by `CC` and `CFLAGS`.
- *A user can run*: a documented environment recipe that produces a `.wasm`,
  and `node kai_host.mjs app.wasm`. Not a page yet.
- *Proof*: the parity corpus under node with the shipped host, against the C
  oracle, as a ratchet: the baseline file lists the fixtures allowed to
  differ, a new difference fails, a closed one must be removed. The starting
  point is the table in section 1. New fixtures: `examples/web/` with one
  program per row of the class table.
- *`File`*: with `KAI_HOST_REACTOR` at 0, which is what this PR derives from
  `__wasm__`, every `File` handler stops (entry 7 of section 2). PR 2 first measures what
  `File.write_file` does under wasmtime with the shim, then takes one of:
  - **Run the work in place when there is no reactor.** The handlers' work
    functions (`_kai_file_*_thunk`) are ordinary blocking calls; the pool
    exists so a read does not hold a scheduler thread, and with one thread and
    one fiber there is nothing else to hold (R). `kai_reactor_run_in_pool`
    calls `work(arg)` directly and the seven sites do not open the reactor.
    The edit sits in the branch a default build does not compile, so the
    preprocess comparison still applies. Not measured.
  - **A macro of its own for the file pool**, apart from the reactor. Needed
    only if some host has `poll` and no threads, or the reverse; none in this
    plan does.
  - **Leave `File` stopping** in the browser, where 3.6 already recommends
    refusing it at `main`, and take the first option in PR 9 for WASI.

  The first is the one that keeps `File` fixtures in PR 2's corpus; the third
  is the smallest. `Stdin` reads and `Clock.sleep_ns` are the same question
  with a different answer: they wait, which is 3.3 and 3.4.
- *Depends on*: PR 1; decisions 3.1 and 3.2.
- *Risk*: `runtime.h` again, but every edit is inside `#if defined(__wasm__)`
  or sets a macro whose default is unchanged; the same preprocess comparison
  applies, under the `runtime-text-identical` label.

**PR 3 — `feat(kai): --target=wasm32-browser builds a module and the page that loads it`**

- *Touches*: `tools/kai/cli_build.kai` (the flag), `cli_toolchain.kai` (a
  target in `Toolchain`; compiler, flags, `stack_ldflags`, `dead_strip` and the
  cache key follow the target, not `uname`), `cli_plan.kai` (the `nm` gate),
  `cli_csingle.kai`, `cli_cmodular.kai`, a new `cli_wasm.kai`;
  `scripts/build-release.sh` and `install.sh` (the host ships under
  `share/kaikai/web`); a new `tools/test-web.sh`; `tier1.yml`; a new
  `docs/info/wasm.md`.
- *Does*: `kai build --target=wasm32-browser app.kai -o out/app` writes
  `app.wasm`, `app.mjs` (two lines: it imports `run` from the `kai_host.mjs`
  copied beside it and runs `app.wasm`) and `app.html` (a
  `<pre id="kai-stdout">` and one module script). The host
  line-buffers fd 1 and 2 to `console.log` and `console.error` and appends
  the same text to the element when it exists. The first build for the
  target fetches the pinned toolchain into the user cache and verifies its
  checksum; a failed download or a checksum that does not match is a
  diagnostic, never a fallback to whatever compiler is on `PATH`.
- *A user can run*: **the first page.** Build, serve the directory, open it:
  the program's output is on the page and in the console. This is the smallest
  thing that is a kaikai program in a browser: one module, one host file of
  about fifty lines, one HTML file, a `main` that runs to completion.
- *Proof*: `tools/test-web.sh` builds each fixture of `examples/web/`, serves
  it locally, drives headless Chrome and Firefox through their drivers, and
  compares the element's text with `.out.expected`. The same modules also run
  under node as the fast leg. An installed-layout leg proves the tarball
  ships the host. The fetch is proved from an empty cache: the job starts
  with no toolchain, and a second fixture with a wrong checksum pinned must
  stop with the diagnostic. CI and users take the toolchain by the same path.
- *Depends on*: PR 2; decision 3.8. The spelling of the target is the
  owner's.
- *Risk*: the wrapper is on every build. Covered by `make test-kai-cli` and
  by the wrapper fixtures in `examples/cache`; the object-cache key gains a
  field, so a first build after upgrading recompiles the runtime object once.

**PR 4 — `feat(typer): an effect the target cannot run is refused at main`**

- *Touches*: `stage2/compiler/driver.kai` (the target reaches the compiler),
  `infer.kai` (`check_main_row_handlable`), `tools/kai/cli_plan.kai`,
  `tools/test-negative.sh` (a fixture may name its target),
  `docs/info/effects.md`, `docs/effects-stdlib.md`.
- *A user can run*: `kai build --target=wasm32-browser` on a program whose
  `main` performs `Process` stops with a diagnostic naming the effect and the
  target, where today it builds and fails on the page.
- *Proof*: negative fixtures `examples/negative/wasm_main_process.kai` and
  `wasm_main_nettcp.kai` with `.err.expected`; a positive one that handles
  `Process` with a scripted handler and builds.
- *Depends on*: PR 3; decision 3.6 for the list of effects.
- *Risk*: the check runs on every program. Host builds pass no target and
  take the path they take today; selfhost byte-identity confirms it.

### Phase 2 — a page that does something

**PR 5 — `feat(ffi): extern "js" declares a JavaScript import`**

- *Touches*: `stage2/compiler/ast.kai`, `parse.kai`, `extern_fn_types.kai`,
  `extern_fn_dup.kai`, `fnreg.kai`, `lint_effects.kai`, `driver.kai`,
  `emit_ffi_c.kai`, the three native FFI emitters (a diagnostic), the builtin
  effect list for `Js`; `stage2/runtime.h` (the release hook of a handle
  box); `stage2/web/kai_host.mjs` (path resolution, the handle table, string
  copies); `tools/kai` (`[js]` in `kai.toml`); `docs/info/ffi.md`,
  `docs/info/syntax.md`, `docs/effects-stdlib.md`.
- *A user can run*: a program that sets `document.title` and logs through
  `console.log` with two declarations and no JavaScript of their own.
- *Proof*: `examples/web/js_import/` on the page gate (title and console
  asserted); a handle fixture that creates and drops ten thousand nodes and
  asserts the host's table is empty at exit; negative fixtures for a
  function-typed parameter and for a host build whose `main` reaches `Js`.
- *Depends on*: PR 4; decision 3.5.
- *Risk*: a new constructor in a type twelve modules name. The compiler
  refuses to build until every `match` over it is handled, which is the check.
  The `extern "C"` and `extern "kai"` fixtures (`examples/ffi`) run unchanged.

**PR 6 — `feat(runtime): fibers and kept continuations suspend through the host`**

- *Touches*: `stage2/runtime.h` (a third implementation of `kai_seg_*`; the
  fiber context operations of gap 2 behind the same seam; `kai_stack_map` and
  the guard machinery stay out); `stage2/web/kai_host.mjs` (the promise
  plumbing; the feature check that refuses a program needing suspension on a
  host without it); `tools/kai/cli_wasm.kai`; `tools/wasm-parity-baseline.txt`.
- *A user can run*: `nursery`, `spawn`, spawned actors and generators on a
  page.
- *Proof*: the fixtures of `examples/actors` and `examples/segments`, and the
  spawning fixtures of `examples/effects`, leave the baseline — the ratchet only moves down. Run
  under node and on both browsers.
- *Depends on*: PR 2; decision 3.3. Independent of PR 5.
- *Risk*: none to hosts if the edits stay under the target's `#if`; the
  `kai_seg_*` seam already has two implementations and gains a third.

> **Shipped (2026-10-22):** the seam is `stage2/runtime/wasm_ctx.h`, under
> `__wasm__` only. A context is a WebAssembly stack the host keeps suspended;
> a `ucontext_t` stand-in and `kai_seg_switch` both go through the
> `kai.ctx_switch` import, which the page wraps in `WebAssembly.Suspending`
> and which dispatches the target explicitly: it wakes the target's pending
> promise or starts it by calling a table entry through
> `WebAssembly.promising`. Each context owns a malloc'd C stack, with
> `__stack_pointer` saved around the switch and set by the host for a fresh
> one, so a fiber stack has no guard page. A module imports
> `kai.needs_suspension` only when it creates a fiber or a segment; the host
> refuses such a module at load on an engine without JSPI, and a program that
> never suspends runs without it. `main` no longer refuses `Spawn`, `Cancel`,
> `Link`, `Monitor` or `Actor`. Waits still stop with `this host has no
> reactor` until PR 7.

> **Stack checks (2026-10-22):** a fiber or segment runs on a malloc'd C stack
> with no guard page, so an overflow would overwrite the heap. The emitter
> writes `KAI_STACK_CHECK();` at the entry of every kaikai function; it is
> empty on the host, and on wasm it compares `__stack_pointer` with the
> running context's floor (the stack's start plus the smaller of a quarter
> of its size and 8 KiB, kept by the shim across switches) and stops with
> `kai: fiber stack overflow`, exit 139, as the host's guard page does.
> Runtime functions are unchecked: their recursion is bounded. The main
> stack needs no check: the linker places it first in memory, so an overflow
> runs off address 0 and traps. Measured (M, node 23, `-O2`, check on against
> the macro empty): loop-compiled mandelbrot +2 %, `fib(42)` +34 %, the two
> together +24 %, module +272 bytes. A check per call is the cost of
> memory safety here; only call-bound code pays more than a few percent.
> `wasm-opt --stack-check` did not instrument these modules, and an
> out-of-line hook per function cost +70 %.

**PR 7 — `feat(runtime): waits park on the host event loop`**

- *Touches*: `stage2/runtime.h` (the reactor: `kai_reactor_wait` and the timer
  wheel hand the wake-up deadline to the host instead of `poll`),
  `stage2/web/kai_host.mjs`.
- *A user can run*: `Clock.sleep` and `receive_timeout` on a page that stays
  responsive meanwhile.
- *Proof*: a fixture that sleeps while a host timer ticks, asserting the
  interleaving on the page; the timer fixtures of the corpus leave the
  baseline.
- *Depends on*: PR 6; decision 3.4.

**PR 8 — `feat(stdlib): the document, fetch, storage and timers as effects`**

- One PR per module is the reviewable unit: `web.dom` with events delivered
  as values, `web.fetch`, `web.storage`, `web.timer`. Each is kaikai over
  `extern "js"`, with `#[doc]` on every `pub` symbol and an entry in
  `docs/stdlib-layout.md` and `docs/stdlib-roadmap.md`.
- *A user can run*: a counter that reacts to clicks; a page that fetches and
  renders JSON.
- *Proof*: one page fixture per module on both browsers.
- *Depends on*: PRs 5 to 7; decisions 3.4 and 3.6; the owner's standing rule
  that additions to `stdlib/` are asked first.

A UI layer above these effects is a design of its own and is not planned here.

### Phase 3 — `wasm32-wasi`

**PR 9 — `feat(kai): --target=wasm32-wasi builds a module for a WASI runtime`** — done

- *Touches*: `tools/kai` (the target, `kai run` through a runtime found on
  `PATH`), `stage2/runtime.h` (`File`, `Env`, `Clock` and the random source on
  real WASI calls), the parity harness (a wasmtime leg and its own baseline),
  `tier1.yml`, `scripts/build-release.sh`.
- *A user can run*: `kai run --target=wasm32-wasi app.kai`.
- *Proof*: the parity corpus under wasmtime; the measured starting point is
  the table in section 1.
- *Depends on*: PRs 1 to 4. Fibers are a decision of this phase (section 5).

### Phase 4 — the native backend

**Done** (`feat(kai): the native backend emits WebAssembly`). The native
backend builds both targets: `kai build --backend=native --target=wasm32-wasi`
and `--target=wasm32-browser`. It is the default for them. The
gaps of entry 16 closed as follows:

- *Triple and data layout* come from the target (`KAI_NATIVE_TARGET`), with the
  WebAssembly LLVM target initialised under `KAI_LLVM_WEBASSEMBLY`; a kaic2
  without it says so.
- *Layout.* The emitter did not hard-code the pointer width; three places
  assumed it: the evidence blob and node (now `i64` cells, offsets from
  `offsetof` in `runtime_llvm.c`), cons cells (an 8-byte stride between
  `head` and `tail`, as the emitter reads them as variant slots), and the
  `kaix_evidence_fits_node` check.
- *Cleanup and non-local exit.* `invoke` and `landingpad` survive only under
  the wasm exception model, which the LLVM C API cannot select; a 10-line C++
  shim (`stage2/runtime/llvm_wasm_eh.cpp`) sets it. `_setjmp` is renamed
  `setjmp` for the wasm sjlj lowering and cleanup pads take
  `__gxx_wasm_personality_v0`.
- *Runtime bitcode per target* (`runtime_{llvm,inline}.<target>.bc`), written by
  the libLLVM-matching clang against a wasi-sdk sysroot (wasi-sdk's own clang
  is newer than the libLLVM). A `.macros` file records the `-DKAI_*` macros each
  was built with; `kai` stops a build whose `CFLAGS` set others, and kaic2
  refuses bitcode of another architecture. This also covers the host pairing
  the first paragraph below describes.

Host output is byte-identical (an emitted object compared with `cmp`).
`extern "js"` is a WebAssembly import on the native backend too (module
`kai_js`, the C path's import names, strings as pointer and length, handles as
host-table indices); struct-by-value `extern "C"` is unsupported on the native
backend for every target. Native is the default for both targets where the
kaic2 has the WebAssembly LLVM target, and C otherwise.

Measured (macOS arm64, native kaic2 against Homebrew LLVM 22, clean core
cache, corpus of 1 411 fixtures, parity against the C oracle on the host):
`wasm32-wasi` native, 1 050 pass, 41 differ (the C baseline minus one, plus 3
struct-by-value fixtures), 173 refused by the target; `wasm32-browser` native,
933 pass, 20 differ (17 of the C baseline and the same 3), 311 refused.
`tools/test-web.sh` under node with the native backend passes every fixture,
including `examples/web_js*`. The legs are `native-wasm` in `tier1-native.yml`,
with `tools/{wasi,wasm}-native-parity-baseline.txt`.

**The macros of PR 1 do not reach the native runtime whole.** It is two
halves: the owner object, which `kai` compiles per build with `$CFLAGS`, and
the hot bitcode (`runtime_llvm.bc`, `runtime_inline.bc`), which
`tools/gen-runtime-bc.sh` generates with fixed flags when the compiler is
built and which is merged into the emitted objects (R). Flags that zero a
macro therefore change the owner and not the bitcode. On a compiler that has
the bitcode, a native build with every macro at 0 runs the pure and `Seq`
fixtures and stops the spawn fixture with its diagnostic, and the handler
fixture dies (M1, native backend):

```
CFLAGS="-std=c99 -Wno-unused-function -Wno-unused-variable -O2 \
  -DKAI_HOST_FIBERS=0 -DKAI_HOST_STACK_GUARD=0 -DKAI_HOST_REACTOR=0 \
  -DKAI_HOST_SOCKETS=0 -DKAI_HOST_PROCESS=0 -DKAI_HOST_SIGNALS=0 \
  -DKAI_HOST_BACKTRACE=0 -DKAI_HOST_UNWIND=0" \
  bin/kai build --backend=native examples/runtime/host_minimal_handler.kai -o h && ./h
tail=42
kai: internal error: a resume frame pushed outside a handler clause
```

The reading, not verified further: `KaiFiber` holds a `ucontext_t` only when
`KAI_HOST_FIBERS` is 1, so the two halves place the fields after it at
different offsets (R). Nothing refuses the pairing at build time, and a check
that did would add a symbol to the default text of `runtime.h`, which PR 1
could not do. Generating the bitcode per target, already in the list above,
is what makes the two halves agree; until then `test-host-minimal` runs the C
backend only, and a guard that fails the link on a mismatch is an item of
this phase.

## 5. What WASI shares with the browser, and what is its own

**Shared, done once by phase 1**: the capability macros and the `wasm32`
guards in `runtime.h`; exception handling for non-local exits; the target
concept in the wrapper (compiler, flags, cache key); the target-aware check at
`main`; the parity harness and its ratchet; the single-thread rule.

**Its own**: no JavaScript host and no generated files beside the module;
`File`, `Env`, `Clock` and arguments are real, through preopened directories;
`kai run` and `kai test` need a runtime on the user's machine; sockets depend
on the WASI version chosen (preview 1 has none; wasi-sdk also carries
`wasip2` and `wasip3` sysroots (M)); and suspension has no JSPI to stand on.
For fibers the candidates there are the stack-switching proposal, which
wasmtime 49 names as an option and does not support on arm64 (M), or
Asyncify with the third option of 3.2. Until one is chosen, a WASI build is
what section 1 measured: everything except `spawn`, kept continuations and
the host facilities of gap 8. Phase 3 is go without fibers (3.0): the
effects that need them are refused at `main` on `wasm32-wasi`.

## Appendix: rebuilding the measurements

Everything lives outside the tree.

- **Toolchains**: wasi-sdk 34.0 unpacked from its release tarball; emsdk
  `install latest`; wasmtime, binaryen and wabt as installed.
- **The shim** (wasi-libc route): a header force-included with `-include`
  that declares what wasi-libc lacks (`ucontext_t` and its four functions,
  `backtrace`, `Dl_info` and `dladdr`, `struct addrinfo` and the socket calls,
  `struct sigaction` and the signal-mask calls, `fork` / `execvp` / `waitpid`
  / `kill`, `popen`, `mkdtemp`, `struct rlimit`); four empty headers
  (`execinfo.h`, `netdb.h`, `sys/wait.h`, `ucontext.h`); a C file defining
  each of those to fail (`errno = ENOSYS`), the four context functions to
  exit with a message, and `_Unwind_ForcedUnwind` to report failure; and a
  copy of `runtime.h` whose random-source gate accepts `__wasm__`.
- **The host**: the 16 imports listed in section 1. For the interop and
  suspension measurement, three more under `env`, one of them wrapped in
  `WebAssembly.Suspending`, and `_start` wrapped in `WebAssembly.promising`.
- **Pages**: one HTML file with the host and the module inlined as base64,
  because a `file://` page cannot fetch; run with
  `chrome --headless --dump-dom` and read back from the `<pre>`.
- **Engine features**: five hand-written modules (exnref, legacy exceptions,
  tail call, shared memory, typed continuations) validated and run in each
  engine, plus `typeof WebAssembly.Suspending`.
- **Corpus**: the entry points and skip rules of `tools/lib/corpus.sh`, each
  fixture built for the host and for `wasm32-wasip1`, both run with
  `KAI_THREADS=1`, output normalised by `kai_corpus_normalize_timestamps`.
- **Benchmark**: the two-part program of section 1, nine runs per build in
  alternation, minimum reported.
