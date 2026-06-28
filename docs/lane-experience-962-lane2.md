# Lane experience — #962 Lane 2 (proto-ops directional-visibility cut)

Lane 2 of the four-lane sequence behind #962 (batch-mode stdlib
typecheck: typecheck core once, compile N files against the live
TyEnv). For reusing a once-typed core to be sound, two user→core
contamination paths must be cut, both under one invariant — **core
never sees the user (directional import visibility)**. This lane cuts
Route 1: a user `protocol`+`impl` rewriting a core body via
`lower_protocols`. It does NOT close #962 (`refs #962`); it leaves the
cut + its fixture running on native.

## Scope as planned vs as shipped

**Planned** (issue #962, Route 1): filter the op set fed to the
per-module proto-call rename so a core module never sees ops declared
in a user module that imports it. The named site was
`protos.kai:2961` (`lower_protocols` + `op_arities_from_ops`). Design
by the `asu` architect: Option A binary — exclude from a core body's
rewrite set every op whose declaring protocol has origin `None` (root
file = user). The discriminant is the PROTOCOL's origin, never the
impl provider, so legitimate polymorphic dispatch (a core body calls a
core op, the user supplies the impl) is untouched.

**Shipped**: the cut as planned, PLUS a root-cause fix to a
pre-existing bug the cut exposed and depended on — the DProtocol
home-module tagging in `collect_proto_decls` was wrong, which made
Option A's `origin == None` discriminant unreliable.

## The cut

`shadow_for_mo` (the memoised per-module shadow resolver inside
`rename_proto_calls_decls_mo`) already routes each decl to a rewrite
op-set keyed by its canonical home module. The cut adds a second
projection of the registry's ops, `core_origin_op_arities`, keeping
only ops whose declaring protocol resolves to a named home (core).
`shadow_for_mo` now uses that core-only set for any decl homed in a
named module (`dmo = Some(_)`) and the full set for root-file decls
(`dmo = None`). asu's implementation trap held: the `OpAr` projection
drops the `proto` field, so the origin filter must run on the `POR`
list *before* projecting — hence a dedicated `core_origin_op_arities`
rather than a post-hoc `[OpAr]` filter.

## The structural surprise the brief did not anticipate

The brief framed the cut as self-contained in `protos.kai`. It is —
but the cut is the first consumer that needs a protocol's REAL origin,
not a self-consistent one, and that exposed a latent bug.

`DProtocol` carries no `module_origin` field (v1 of #510 only added it
to DFn). `collect_proto_decls` recovered each protocol's home by
streaming `cur_mod` forward through the merged `prelude ++ user`
stream. That mis-tags any DProtocol that precedes the first anchored
DFn of its file: every protocol inherits the home of the PRECEDING
module. Measured on the adversarial fixture:

```
Spaceable -> array      (user protocol; should be None)
Show      -> complex     (should be protocols)
Numeric   -> string      (should be numeric)
```

All shifted by one. It "worked" before because the dispatcher symbol
(`c_sym` folds `mo` into the C name) AND its call sites both resolve
through the same broken `proto_origin_lookup` — self-consistent
garbage. Definition and use shift together, so symbols link, stdout is
identical, and `make selfhost` (a fixed-point determinism check, not a
correctness check) stays green. The off-by-one never produced a
corrupt program because it was symmetric; it was latent debt waiting
for the first consumer to read the value rather than compare it with
itself. The cut is that consumer: a user protocol mis-tagged
`Some("array")` instead of `None` inverts the visibility decision.

**Fix (root cause, not workaround):** the compiler already had the
correct algorithm — `imports_home_anchors` (used by
`validate_pub_access`), which assigns one home per decl in source
order with *back-propagation*: when the first anchored DFn/DType of a
file lands, it retroactively corrects every preceding non-anchored
decl. `collect_proto_decls` now pre-computes proto homes via that
analysis and looks them up by name, instead of inferring from the
running `cur_mod`. No AST change (DProtocol gains no field — the 34
pattern-match sites stay untouched); the fix is contained to
`protos.kai`.

Considered and rejected: (A1) adding `module_origin` to `DProtocol` —
asu's first preference, but it touches 34 `DProtocol(...)` sites in
stage2 plus the cache codec / emit / fmt, far outside a Route-1-cut
lane. (B) routing the prelude/user boundary from the driver — leaves
the tagging bug for Lane 3 with less context. The chosen fix reuses
the proven back-propagation and fixes the root cause for both
consumers (the cut AND the cosmetic dispatcher `mo`).

## Liveness — the cut is not dead code

The cut is byte-transparent for every collision a *natural-named* op
can construct: the per-module shadow guard already collects all DFns
(pub and priv) of a module, so any identifier defined in module M
protects M's bodies; core ops sharing `(name, arity)` with a user op
share the same `__proto_*` dispatcher; arity matching covers the rest.
Four adversarial differential tests (with-cut vs no-cut C emit) all
diffed zero.

The one gap the cut closes structurally: a core body in module M
calling a bare identifier NOT defined in M and not a core op.
`core/string.kai`'s `trim`/`lines` call `is_space(c)` bare;
`is_space` is a `pub fn` in `core/char.kai`, with no local shadow in
string.kai and not a core protocol op. A user
`protocol Spaceable { is_space(x: Self): Bool }` mints a new
`__proto_is_space`. Pre-cut, `lower_protocols` rewrote string.kai's
core `is_space(c)` to that user dispatcher and the typer rejected the
core body with `no impl of Spaceable for type Char` (×3). The fixture
`examples/protocols/user_proto_op_no_contaminate_core.kai` is that
witness: it does not compile without the cut, and runs clean with it
(`[hi]` / `3` / `tok-space` / `tok-nospace`). End-to-end, honest, on
native.

## Fixtures

`examples/protocols/user_proto_op_no_contaminate_core.kai` (+
`.out.expected`), auto-globbed by `stage2/Makefile`'s `test-protocols`
(C now; LLVM when native is built). Positive case: user protocol op
collides with a bare cross-module call inside a core body; core stays
intact, user dispatch still works. Doubles as the liveness witness.

## Verification

- `examples/protocols` corpus: 57 OK, no DIFF (C); the two `--test`
  block fixtures pass via their dedicated Makefile targets.
- `test-stdlib-modules`: 60/60 — the tagging fix corrects core
  protocol homes without breaking any multi-module stdlib typecheck.
- `make selfhost`: deterministic fixed point on both C and native
  builds (kaic2b.c == kaic2c.c).
- `make tier0`: green (demos baseline 36, arena, heap ceiling).
- Backend parity (native vs C oracle), SERIAL (`BACKEND_PARITY_JOBS=1`):
  green.
- Fixture verified on the DEFAULT (native) backend, not just C.

## Cost vs estimate

The cut itself was ~40 lines. The tagging bug — unanticipated by the
brief — was the bulk of the lane: finding it (the cut looked correct
but the adversarial fixture still failed), diagnosing the off-by-one
via debug instrumentation of `reg.origins`, and choosing the
contained back-propagation fix over the invasive AST change.

## Follow-ups for next lanes

- **Lane 1** (root_fns cut, `infer.kai`) is the disjoint sibling under
  #962; it stays untouched here (collision zone).
- **Lane 3** (independence oracle) can now assume core proto-origin
  tagging is correct — the differential-independence gate no longer
  has to work around a self-consistent-but-wrong dispatcher `mo`.
- The other `is_space`-shaped bare cross-module calls in core
  (`char_at`, `int_to_char`, …) are now all protected by the same cut;
  no per-site work needed.
