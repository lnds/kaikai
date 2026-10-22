# wasm

Build a program for a browser page with `--target=wasm32-browser`.

## Description

`kai build --target=wasm32-browser` compiles through the C backend with a
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
ship exception handling with exnref: Chrome and Edge 137, Firefox 131,
Safari 18.4. A program that spawns a fiber or keeps a continuation also
needs JSPI (Chrome and Edge 137, Firefox 153, Safari 27); the host refuses
to load it on an engine without.

Not available on the page: sockets, processes, signals and the reactor
(sleeps, `File`, `Stdin`). Non-tail recursion is limited by the engine's stack, about 6 000
frames in Chrome.

## Flags

- `--target=wasm32-browser` is accepted by `kai build` only, and requires
  the C backend (`--backend=native` is refused).
- `-o out/app` names the files; a trailing `.wasm` is dropped.
