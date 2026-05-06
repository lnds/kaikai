# Lane: op-rem-protocol (Wave Op-1)

Promote the `%` operator from hardcoded `Int`-only to the `Rem`
single-dispatch protocol. Mirrors the #246 recipe that promoted
`+ - * /` to `Add / Sub / Mul / Div`. First of three Wave Op
lanes; Op-2 is `-x` unary → `Neg`, Op-3 is `++` → `Concat`.

## Objective metrics

- **Wall**: ~14 min from `git checkout op-rem-protocol` to all
  gates green (start `2026-05-06T17:23:26`, end
  `2026-05-06T17:36:54`).
- **Source delta** (excluding fixtures + docs):
  - `stage2/compiler.kai`: +21 lines (split `//` from `%` in
    `synth_binop`, add `%` → `"rem"` in `binop_proto_method`,
    add `%` → `"Rem"` in `binop_proto_name`).
  - `stdlib/protocols.kai`: +14 lines (`Rem[a]` declaration +
    `impl Rem for Int`).
- **Fixture delta**: 2 new positive fixtures
  (`rem_basic.kai`, `rem_user_record.kai`) + matching
  `.out.expected` goldens.
- **Doc delta**: `docs/protocols.md` gains a "Wave Op series"
  table summarising the operator → protocol map and pinning
  the `Real % Real` deferral.
- **Gates**:
  - `make tier0`: OK (selfhost byte-identical, demos baseline 26)
  - `make tier1`: OK
  - `make tier1-asan`: OK
  - `make selfhost` (C backend): byte-identical
  - `cd stage2 && make selfhost-llvm`: byte-identical

No new regressions; the existing #246 fixtures (`add_user_record`,
`poly_impl_multi_bound`, `proto_pa_basic`, etc.) all still pass
on both backends after the typer change.

## Protocol declaration

`stdlib/protocols.kai` gets one new declaration plus a single
primitive impl:

```kaikai
protocol Rem[a] {
  rem(self: Self, rhs: a) : Self
}

impl Rem for Int {
  fn rem(self: Int, rhs: Int) : Int = self % rhs
}
```

`Real` is omitted because the stage 0 runtime has no `fmod`
binding (verified with `grep fmod stage0/runtime.h`). Adding it
would require either an FFI extern or a dedicated runtime helper;
neither felt in-scope for a strictly incremental lane. The
existing primitive-mismatch diagnostic still fires for
`Real % Real`, so users get a clean error rather than silently
wrong arithmetic.

## Typer change (`//` vs `%` split)

The pre-lane code lumped `//` and `%` together — both were pinned
to `Int + Int → Int` with a shared diagnostic message. The split
preserves `//` exactly and lets `%` fall through to the same
protocol-dispatch shape that `*` / `/` already use:

```kaikai
} else if op == "//" {
  # Integer division stays Int-only; no protocol overload in v1.
  let s1 = st_unify(rr.st, rl.ty, TyInt, "integer arithmetic", line, col)
  let s2 = st_unify(s1, rr.ty, TyInt, "integer arithmetic", line, col)
  mk_inferred(s2, TyInt, binop_root(op, rl.expr, rr.expr, line, col, TyInt))
} else if op == "%" {
  let lty_now = apply_ty(rr.st.sub, rl.ty)
  let rty_now = apply_ty(rr.st.sub, rr.ty)
  if binop_should_dispatch_proto(lty_now, rty_now) {
    synth_binop_proto(rr.st, op, rl.expr, rr.expr, line, col)
  } else if binop_dispatch_proto_heterogeneous(rr.st, op, lty_now, rty_now) {
    synth_binop_proto(rr.st, op, rl.expr, rr.expr, line, col)
  } else {
    let s1 = st_unify(rr.st, rl.ty, TyInt, "integer arithmetic", line, col)
    let s2 = st_unify(s1, rr.ty, TyInt, "integer arithmetic", line, col)
    mk_inferred(s2, TyInt, binop_root(op, rl.expr, rr.expr, line, col, TyInt))
  }
}
```

The two helpers (`binop_proto_method`, `binop_proto_name`) gain
exactly one branch each. `binop_should_dispatch_proto` already
gates on "both operands concrete non-primitive" — no change
needed.

The decision to keep `//` Int-only is deliberate: integer division
on user types overlaps too heavily with `Div.div`, and rationals
(the most common motivator) would want `Real`-style `/` instead.
Modulo, by contrast, is the natural protocol case — modular
integers, residue rings, time-of-day arithmetic, and the
`ModInt` / `Clock` records in the new fixtures all want a custom
`%`.

## Empirical verification

Both fixtures compile and run byte-identical on C and LLVM
backends:

- `examples/protocols/rem_basic.kai` — exercises both paths in
  one program: `10 % 3` and `17 % 5` use the primitive `Int %
  Int` fast path; `ModInt { value: 23, modulus: 7 } % ModInt {
  value: 5, modulus: 7 }` routes through `__proto_rem`. Output
  `1\n2\n3\n7\n` matches on both backends.
- `examples/protocols/rem_user_record.kai` — exercises a
  non-trivial `impl Rem` body that wraps negative residues
  (`-5 % 12 → 7`). The `if raw < 0` branch inside the impl
  proves the protocol method dispatches the user-supplied
  reduction, not a hidden runtime shortcut. Output `3\n7\n`
  matches on both backends.

Both fixtures are picked up automatically by `test-protocols`
(it globs `examples/protocols/*.kai`).

## Friction points

- **kaic2 caching gotcha**: the first `make tier0` reported
  `kaic2 is up to date` despite the freshly edited
  `compiler.kai`. The Makefile's dependency chain reaches
  `kaic2` only through specific paths; touching the source
  forced a rebuild. Worth a follow-up: stamp `kaic2` against
  `compiler.kai`'s mtime so the chain is automatic.
- **Selfhost-llvm target lives under `stage2/`**: not at repo
  root. Briefing referenced `make selfhost-llvm`; correct invocation
  is `cd stage2 && make selfhost-llvm`. Aligning the names
  (`make selfhost-llvm` at root delegates to stage2) would
  shorten future briefings.
- **No fmod in runtime**: caught at protocol-declaration time
  before writing fixtures. The brief flagged this as a risk;
  the verification took one grep. Cheap to check, cheap to
  defer.

## Subjective summary

Maximally incremental. The #246 lane already laid every piece of
machinery (`synth_binop_proto`, `binop_should_dispatch_proto`,
`binop_dispatch_proto_heterogeneous`, the post-inference rewrite
in `lower_protocols`); this lane is three localised string adds
plus one branch split. The hardest decision was whether to ship
`Real % Real` — and the answer was determined by a 5-second grep
on `stage0/runtime.h`. No design choices, no open questions.

The pattern compounds: with `Add / Sub / Mul / Div / Rem` all
sharing the same lowering, the next two Wave Op lanes (`Neg`,
`Concat`) can lean on the same shape. `Neg` is a unary version of
the same predicate; `Concat` is the binary-with-list-special-case
version.

## Limitations

- `Real % Real` is rejected — no `fmod` shim in stage 0 runtime.
  Add the FFI binding plus `impl Rem for Real` together when a
  user needs it.
- `Rem[a]` parametrised heterogeneous impls (`impl Rem[Int] for
  Money`) are syntactically accepted but not exercised by a
  fixture in this lane; the `synth_binop` `op == "%"` branch
  delegates to `binop_dispatch_proto_heterogeneous`, which is
  the same code path #180 validated for `*` / `/`. A targeted
  fixture is a cheap follow-up if needed.
- Wave Op-2 (`-x` → `Neg`) and Op-3 (`++` → `Concat`) follow
  this lane sequentially.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-06T17:26:51-04:00	tier0	OK	-
2026-05-06T17:27:51-04:00	tier0	done	-
2026-05-06T17:33:58-04:00	tier1	done	-
2026-05-06T17:35:00-04:00	tier1-asan	done	-
2026-05-06T17:35:41-04:00	selfhost	done	-
2026-05-06T17:36:47-04:00	selfhost-llvm	done	-
```
