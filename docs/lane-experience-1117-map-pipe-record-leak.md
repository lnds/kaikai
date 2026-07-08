# Lane experience — #1117: map pipe over records leaks the input records

## Scope

Fix the leak where `xs | (p => p.v)` — a map pipe whose closure projects a
field of a record element — drops every input record box without freeing it
(≈N leaked, linear in the input). The leak reproduces on BOTH backends and is
distinct from the native-only #1112 (discard-decref).

## The bug, and the brief's inverted diagnosis

The brief pointed at `map_loop`'s caller side: `pcs_lu_consuming_kind`
conservatively assuming the closure `f` MAY retain `h`, so it omits `h`'s drop
after `f(h)`. That framing is wrong, and the C evidence corrected it.

The real cause is on the **callee** side. Reading the generated C for
`map_loop` (`[h,...t] -> map_loop(t, f, [f(h), ...acc])`):

```c
KaiValue *kai_h = kai_incref(_scr->as.cons.head);              // birth ref
kai_cons(kai_apply(dup(f), 1, (KaiValue *[]){kai_h}), dup(acc)) // h handed RAW
```

`h` is handed **raw (owned)** to `kai_apply`, whose contract (#298) is that
every arg is OWNED by the call — so `map_loop` correctly transfers `h`'s birth
ref into the call. The caller is sound. The hole is in the **lifted closure
thunk**: a closure body only ever DUPs its param reads. `_kai_lam(p => p.v)`:

```c
KaiValue *kai_p = args[0];
return ({ KaiValue *_b = dup(kai_p); field_at(_b, 0); decref(_b); }); // net-zero on args[0]
```

The birth ref that `kai_apply` "handed" the callee via `args[0]` is never
consumed → one box leaks per call. This was already flagged as deferred debt
in `perceus.kai` (the ELambda rewrite comment, residual of #817).

### Why the callee always leaks its param — the structural invariant

`pcs_is_non_last` has `else if in_lam { true }` **before** any last-use
analysis: inside a closure body every `EVar` read is unconditionally wrapped in
`__perceus_dup`. So a closure body **never moves a param raw** — every use
dups. Verified empirically across shapes: field-read, ctor-of-its-fields,
bare-pass-to-fn, identity `p => p`, unused `p => 42`, twice-use `p.v + p.w` —
all leak one box per call. Even nested-closure capture does not consume the
param: `emit_one_closure_cap` emits `kai_p` raw, but `kai_closure` INCREFs each
capture (`runtime.h` — "incref each captured value so the closure owns its own
reference"), so `args[0]` stays intact and also leaks.

The differential vs #817 (correctly green): `filter_loop` stores `h` into the
`[h, ...acc]` cons — the ctor RETAINS `h`, no drop needed. `map_loop` stores
`f(h)` — `f` does not retain `h`.

## The fix

Because a closure body never consumes its param, **every closure param needs
exactly one unconditional drop** at the end of the lifted thunk — no
borrow-vs-consume analysis, no risk of double-free. Two sites, one per backend:

- **C** (`emit_c.kai` `emit_lam_helper`): capture the body result in `_lam_ret`,
  `kai_decref` each param, return `_lam_ret`. Mirror of `ffi_emit_arg_decrefs`.
  Order matters: the drops fire AFTER the body (which used dups) and BEFORE the
  return.
- **Native** (`kir_lower_fns.kai` `lower_lam_fn`): emit a `KDrop(KVar(param))`
  per param into the KIR stream after the body's `val` register is computed and
  before `KRet`. All lambda params are `SBoxed` (`lower_params`), so every drop
  is register-safe.

Captures are NOT dropped in the thunk: the closure owns them (donated once at
construction, released in the `KAI_CLOSURE` destructor). A per-call drop would
UAF after the first of N invocations (a combinator runs the thunk once per
element). This is why the fix touches params only.

## Soundness: why no over-drop / UAF

The one failure mode is dropping a param whose single use actually consumed it
(double-free). It cannot happen: the `in_lam → dup-always` invariant means the
param is never consumed by the body. Verified with ASAN on the full shape
matrix INCLUDING the maximal-danger retaining case
`foldl(ps, [], (acc, x) => [x, ...acc])` (the lambda stores `x` into the cons)
— ASAN clean, correct result, because the store dups `x` and the param's birth
ref is a separate ref my drop releases.

## What shipped vs planned

- Planned: close the leak on both backends without a UAF. Shipped exactly that.
- The fix is simpler than the brief anticipated (unconditional param drop, not a
  refined `pcs_lu_consuming_kind`) because the empirical convention removed the
  need for a borrow-vs-consume gate.

## Residual (reported honestly, per brief)

A **nested-closure capture** leaks the CAPTURE (not the param) on the NATIVE
backend only: `p => apply0(() => p.v)` leaves ≈N leaked native (0 on C). This
is a **preexisting** native capture-lifecycle leak — identical pre-fix and
post-fix (measured by stashing the fix and rebuilding), independent of #1117.
The #1117 canonical shape (`ps | (p => p.v)`, field projection, no nested
capture) closes cleanly on native (leaked 1004→4). The capture-lifecycle
residual is a separate concern (sibling of #1112's native RC work), not
in-scope here — closing it would need work in the native closure destructor /
capture donation, not the param-drop path.

## Fixtures

- `examples/perceus/map_pipe_record_leak_1117.kai` (+ `.out.expected`): maps a
  field-projecting closure over 1000 heap records, 20 times. With the leak
  `leaked` grows ~1000/iter (≈20000); fixed it is a small constant (10 C / 4
  native).
- `test-perceus-1117-map-pipe-record-leak` (C, TEST_LIGHT_TARGETS + tier1).
- `test-perceus-1117-native-map-pipe-record-leak` (native, tier1-native.yml).

## Gates

kaic2 builds (C + native); `ps | (p => p.v)` leaked→~0 both backends; full
perceus corpus + ASAN variants green; #817 stays green; C-vs-native correctness
parity on the whole shape matrix; retaining-closure smoke ASAN-clean; selfhost
byte-identical C + native.

## Follow-ups

- Native nested-closure capture-lifecycle leak (the residual above) — worth a
  separate issue in the #1112 native-RC family if it bites a real workload.
