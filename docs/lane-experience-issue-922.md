# Lane experience — issue #922: module const captured in a closure

## Scope as planned vs as shipped

**Planned.** Make a module-level `const` captured inside a lambda compile
and run identically to a direct read, on BOTH backends (native default +
C oracle). Pre-fix the free-var collector listed the captured const as a
closure capture: the C emitter wrote a `kai_PUERTO` capture with no
definition (`use of undeclared identifier`), and the native KIR lowering
hit an `unbound register PUERTO` and refused to emit the module.

**Shipped.** Exactly that, via a single shared fix at the capture-set
source — no parallel backend patches. Both backends fall out from one
change to `build_globals`.

## Where the capture set is computed, and why a const was mis-listed

The lambda capture set is one value computed in one place and consumed by
both backends:

- `collect_lambda` (`emit_shared.kai`, the `ELambda` arm of `collect_expr`)
  computes `caps = fv_expr(body, own, effective_globals, [])` and stores it
  in the `LamInfo` record. `effective_globals = list_minus(st.globals,
  st.local_scope)` — the program's global-name set minus any name the
  current local scope shadows (the issue #285 rule that keeps a
  locally-shadowed global capturable).
- `fv_expr`'s `EVar(name)` arm treats `name` as a free variable to capture
  unless it is in `own` (lambda params), `globals`, or already collected.

`build_globals` is what seeds `st.globals`, and it returned prelude names +
user fn names + variant ctor names + effect names + qualified op names —
**but not module const names**. So `PUERTO` was neither a param nor a
"global", and `fv_expr` dutifully added it to `caps`.

Both backends then consume that `caps`:

- C: `emit_lam_helper` makes `caps` part of the lambda body's `lcs`
  (`body_lcs = … caps`) and `emit_lam_cap_reads` emits
  `kai_PUERTO = self->as.clo.captures[i]`; the install site emits
  `kai_closure(…, (KaiValue *[]){kai_PUERTO})`. Neither `kai_PUERTO`
  definition exists, because a const is normally inlined, not emitted as a
  global.
- Native: `lower_lam_fn` seeds `LowerSt.locals` with `param_local_names(
  params, caps)`, so `lower_var("PUERTO")` resolves via `ls_is_local` to a
  `KVar` register read — a register the closure call site never binds, i.e.
  `unbound register PUERTO`.

## Why a shared fix, not a split

A module const is a named literal: every NON-captured read already
materialises its body inline — `emit_c.kai`'s `const_body_of(cx.consts,
name) → emit_expr(body)` and `kir_lower_walk.kai`'s `kir_const_body(
st.consts, name) → lower_expr(body)`, two mirrors of the same rule. The
only thing wrong was that the capture machinery did not know a const is a
global, not a capturable local. Adding const names to the `globals` set
makes `fv_expr` exclude them from `caps`, so on BOTH backends the captured
read stops being a capture and falls through to the existing const-inline
path — the C emitter inlines `8080`, the native lowering inlines `8080` —
identical to a direct read. One cause, one fix, at the shared source the
brief predicted.

Concretely: a new shared helper `const_names(decls) : [String]` in
`emit_shared.kai`, a new `consts: [String]` parameter on `build_globals`,
and the three call sites (`emit_program`, `emit_program_modular`,
`collect_lams`) pass `const_names(decls)`. stage1's separate two-arg
`build_globals` is untouched — bootstrap never compiles a captured const,
and the C selfhost stays byte-identical, confirming it.

The `effective_globals = list_minus(st.globals, st.local_scope)` line means
a `let PORT = …` shadowing the const is still captured correctly (the
shadow wins, same as a shadowed fn name) — the fix does not regress
shadowing.

## RC handling for captured heap consts

Because a captured const is no longer a capture, the heap-String case
needs no special RC work: the const is materialised inline at each read by
the same path a direct read uses, so the existing Perceus discipline for a
const literal applies unchanged. The equality fixture reads a `String`
const twice inside one lambda (`string_concat(HOST, HOST)`); ASAN + UBSan
on the C-direct emit are clean (exit 0, no reports), and the native-vs-C
diff agrees byte-for-byte on stdout and exit code. No capture register, no
incref/decref imbalance to manage.

## How inside-vs-outside equality was tested

Two positive fixtures in `examples/native/` — auto-globbed by
`tools/test-native-parity.sh`, which compiles each on BOTH backends and
diffs stdout + exit code against the C-direct oracle (the real
both-backends gate):

- `const_capture_int.kai` — the 8-line repro shape (`PUERTO + 1` captured
  in a lambda), prints `8081`.
- `const_capture_equality.kai` — reads an Int const and a `String` const
  both directly and through a lambda, prints `int_eq` / `str_eq` from the
  `==` comparison, and reads the `String` const twice inside one lambda
  body (`twice=localhostlocalhost`).

The native-parity harness's oracle IS the C backend, so a passing fixture
proves the two backends produce identical output — exactly the "captured
read equals direct read on both backends" claim.

## Validation

- 8-line repro: `8081` on `--backend=native` AND `--backend=c`.
- `tools/test-native-parity.sh`: 22/22 pass incl. both new fixtures.
- ASAN + UBSan clean on the captured-const heap-String path (mac, no LSAN —
  Linux tier1-asan covers leaks).
- `make tier0`: selfhost byte-id (`kaic2b.c == kaic2c.c`), demos
  no-regression, arena, heap-limit all green.
- Serial backend parity (`BACKEND_PARITY_JOBS=1`) clean.

## Follow-ups

None. The fix is the minimal correct one; no scope was deferred.
