# C calls kaikai: `extern "C"` with a body exports a function

**Status:** decided by the owner, 2026-10-22.

## The decision

`extern "C"` without a body imports a C function, as it always has. With a
body, it exports a kaikai function under the C ABI, so C code can call it and
take its address:

```
extern "C" fn cos(x: Real) : Real / Ffi                    # import
extern "C" fn on_save(id: String, req: String) : Unit / Stdout =
  Stdout.print("saving #{req}")                            # export
extern "C"("app_on_save") pub fn on_save2(req: String) : Unit / Log {
  Log.info("saving #{req}")                                # export, symbol named
}
```

- **Only top-level functions are exported, never closures.** A closure is a
  box with captures; handing one to C would need a lifetime C cannot express.
  A top-level function is static code with no box to free.
- **The row is closed.** C calls with no `handle` on the stack, so the body may
  perform only effects that have a default handler (`Stdout`, `Stderr`, `Log`,
  `Clock`, `Random`, …). An effect that needs a handler (`State`, `Abort`, a
  user effect) is a type error at the declaration. `Ffi` is not required: the
  body is kaikai code.
- **Boundary types** are the ones an import accepts: fixed-width scalars,
  `Real`, `Bool`, `String` (copied in, as a C string), declared `extern "C"`
  types and `opaque` handles. A function-typed parameter is still refused.
- **Context travels as a value.** A C API that takes a `void *userdata` gets a
  kaikai value through it (typically a `Pid` of the actor that owns the state),
  and the exported function receives it as a parameter. State lives in actors;
  the callback translates and forwards.
- **The symbol override names the exported symbol**, as it names the imported
  one.

## What the compiler and runtime guarantee

- The compiler emits a C adapter per exported function that converts the C
  arguments to kaikai values and the result back.
- A call can arrive on a thread that is not a kaikai worker (a GUI main
  thread). The adapter enters the runtime on that thread and leaves it after.
- A panic or an abandoned continuation inside the body never unwinds across C
  frames: the adapter stops it at the boundary and reports it.
- A call that arrives while kaikai is inside an FFI call (C calling back
  synchronously, as `qsort` does) is supported.

## Why

The desktop framework (a native kaikai backend with a system webview, Tauri
style) needs C to reach kaikai. Messages through a queue cover the window, but
APIs such as `qsort`, iterators of C libraries and webview bindings call a
function pointer. Exporting top-level functions with a closed row covers them
without the lifetime, row-inference and capture problems closures bring.
Reusing `extern` keeps one keyword for the C boundary in both directions, the
same shape Rust's `extern "C" fn` has.
