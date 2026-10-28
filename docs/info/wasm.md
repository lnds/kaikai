# wasm

Build a program for a browser page with `--target=wasm32-browser`, or for a WASI runtime with `--target=wasm32-wasi`.

## Description

`kai build --target=wasm32-browser` compiles with a
pinned wasi-sdk and writes three files beside each other: the module, the
page that loads it, and the host that runs it.

```sh
kai build --target=wasm32-browser app.kai -o out/app
# out/app.wasm   the module
# out/app.mjs    runs app.wasm on the host
# out/app.html   a <pre id="kai-stdout"> and one module script
# out/kai_host.mjs
```

Serve the directory and open `app.html`: what the program prints is in the
element and in the browser console. `kai_host.mjs` also runs under node:
`node out/app.mjs`.

## Toolchain

The first build for the target downloads wasi-sdk 34 into the user cache
(`$XDG_CACHE_HOME/kai/wasi-sdk`, else the platform cache directory) and
verifies its SHA-256 against the value pinned in `kai`. A failed download
or a checksum that does not match stops the build; `kai` never falls back
to a compiler on `PATH`.

- `KAI_WASI_SDK=<dir>` uses the wasi-sdk 34 installed at `<dir>` instead.
- `KAI_WASI_SDK_URL=<url>` fetches the archive from a mirror; the checksum
  is still checked.

## What runs

Programs that run to completion on one thread: pure code, records, sum
types, handlers (exception handling, exnref encoding), `Seq`, fibers
(`spawn`, `nursery`), actors and handlers that keep `resume`. The engine must
ship exception handling with exnref and tail calls: Chrome and Edge 137, Firefox 131,
Safari 18.4. A program that spawns a fiber or keeps a continuation also
needs JSPI (Chrome and Edge 137, Firefox 153, Safari 27); the host refuses
to load it on an engine without.

Sleeps and `receive_timeout` park on the page's event loop, so the page stays
responsive; a sleep of one frame or less wakes on the next animation frame.
Not available on the page: sockets, processes, signals and the reactor's
poll (`File`, `Stdin`). Non-tail recursion is limited by the engine's stack, about 6 000
frames in Chrome.

## The page stdlib

`web.run { … }` (from `import web.run`) answers four effects with the page:
`Dom` (`web.dom`), `Timer` (`web.timer`), `Fetch` (`web.fetch`) and `Storage`
(`web.storage`). Events are values: `Dom.listen(node, "click")` asks the host
to queue them, and `Dom.next_event()` parks the fiber until one arrives.

```kaikai-fragment
import web.dom
import web.run

fn count(label: Node, n: Int) : Unit / Dom {
  let e = Dom.next_event()
  if e.kind == "click" {
    Dom.set_text(label, "clicks: #{n + 1}")
    count(label, n + 1)
  } else {
    count(label, n)
  }
}

fn main() : Unit / Js + Ffi + Clock =
  run.run(() => {
    let button = Dom.create("button")
    Dom.append(Dom.query("body") ?? button, button)
    Dom.listen(button, "click")
    count(button, 0)
  })
```

The row needs `Js`, so the program builds only for this target. A `main` that
returns a nonzero status is reported on the console of the page
(`kai: exited with status N`). The `[js]` modules of the entry package and of its
dependencies are loaded; the entry package's wins a shared name.

## Flags

- `--target=wasm32-browser` is accepted by `kai build` only, with
  either backend (see "Native backend" below); the native one is the default.
- `-o out/app` names the files; a trailing `.wasm` is dropped.

## WASI: `--target=wasm32-wasi`

`kai build --target=wasm32-wasi app.kai -o app.wasm` writes one module and
nothing beside it: no host, no page. The toolchain is the same pinned wasi-sdk
(`KAI_WASI_SDK`, `KAI_WASI_SDK_URL`).

`kai run --target=wasm32-wasi app.kai [args...]` builds in a scratch directory
and runs the module on `wasmtime`, found on `PATH` (the command stops with a
message naming it when it is missing). The arguments go to the program, the
whole environment is inherited, and the preopened directories are the current
directory; `KAI_WASI_DIRS=<dir>[:<dir>...]` replaces them, and a path outside
them is not reachable (`File` returns an `Err`). The program's exit status is
`kai`'s. `kai test` has no `--target`: it builds and runs tests on the host.

What runs: pure code, records, sum types, handlers, `Seq`, `File`, `Env`,
`Stdin`, `Stdout`, `Clock` (a sleep blocks the instance), `Random`,
`SecureRandom` (`random_get`) and `Process.exit`. Every other `Process`
operation stops at run time.

There are no fibers. `main` is refused, with a diagnostic naming the target,
when its final row holds `Spawn`, `Cancel`, `Link`, `Monitor`, `Actor`,
`Signal`, `NetTcp`, `NetUdp`, `NetUnix`, `NetDns` or `Js`; preview 1 has no
sockets, and fibers wait for the stack-switching proposal in wasmtime.
Handle the effect inside `main`, or remove it from the row.

A handler that keeps `resume` (a generator, a non-tail resume) cannot be seen from
`main`: it stops at run time with `kai: fibers and kept continuations are not
available on wasm32-wasi`.

## Native backend

`kai build --backend=native --target=wasm32-wasi|wasm32-browser` emits the module
from the in-process LLVM: no C is generated and no user code goes through a C
compiler. The pinned wasi-sdk still supplies `wasm-ld`, wasi-libc and the
runtime owner object, which is built once per target and cached. The output
files are the same as the C backend writes.

The native backend is the default for these targets when this kaic2 has the
WebAssembly LLVM target, and the C backend otherwise; `--backend=c|native` or
`KAI_BACKEND` chooses. An explicit `--backend=native` on a kaic2 without the
target stops with "this kai was built without the WebAssembly LLVM target".
`extern "js"` declarations are WebAssembly imports of module `kai_js`, named
`<param letters>:<return letter>:<path>` as on the C backend, so the same host
resolves them; they are refused on the native backend for any other target.
Struct-by-value `extern "C"` parameters are refused on the native backend for
both WebAssembly targets; the C backend takes them.

The runtime bitcode is generated per target (`runtime_llvm.wasm32-wasi.bc`,
`runtime_llvm.wasm32-browser.bc` and their `runtime_inline` twins) where a wasi-sdk
sysroot is available, with the macros each target is built with. A `CFLAGS` that
sets other `-DKAI_*` macros stops the build, because the bitcode and the owner
object would disagree on struct layout.
