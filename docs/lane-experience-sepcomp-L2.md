# Lane experience — sep-comp L2: shared `home_tu` partitioner + `KFn.mo`

Lane 2 of 3 in the Modelo A modular-compilation chain (refs #963, #989).
Small infra lane, prerequisite of L3 (the native KIR partition). Base:
`main` after L1 (#1045 runtime owner).

## Scope as planned vs as shipped

Planned (from the sep-comp plan memo): extract `home_tu(d: Decl) : ModId`
to a shared AST-level module as the single source of truth for the
decl-to-TU partition; add an `mo` field to `KFn`, populated in lowering;
redirect the C `emit_program_modular` consumer to `home_tu`; leave both
ready for L3 without implementing the native partition.

Shipped exactly that, in three blocks:

1. `compiler/home_tu.kai` (new, A++) + redirect `distinct_module_origins`
   / `decls_of_origin` in `emit_c.kai` + fixture with a 6-case partition
   assertion (`test-home-tu-partition`).
2. `KFn.mo` field + populate in `kir_lower_fns.kai` (4 constructors) via
   `home_tu(d)`; native-only, invisible to the C byte-id.

No L3. No native partition.

## Design decision: physical TU, not logical module (Option A)

The plan memo suggested `home_tu` should *canonicalise* the root-file
`mo` inconsistency (a root `pub fn` is tagged `Some(target_mod)`, a
private fn / `main` stays `None`), collapsing `Some(target_mod)` to root
so the whole root file lands in one TU.

Empirically, that is wrong for a partition function and would break the
byte-id gate. Today a root file already splits into TWO TUs: `pub fn`
bodies mangle to `kai_<file>__f` and live in `<file>.c`; private fns and
`main` live in `__root__.c`. That split is *correct* — the mangled
symbol commits the TU, and collapsing it would desync symbol from TU.

asu confirmed Option A (preserve): `home_tu` returns the *physical*
emission unit, aligned to the mangled symbol, NOT the *logical* module
identity a protocol-op shadow check compares. The `root_file_mo_
inconsistent` canonicalisation belongs to that other consumer (the #893
shadow guard), not to the partition. The `#[doc]` says so, so a future
consumer does not reuse `home_tu` for module-identity and reintroduce
the shadow bug.

"Spec homes to the defining generic's module" needs no special case:
monomorph already stamps the spec DFn with the generic's `mo`, so
`home_tu(spec) = mo` gives it for free. The fixture pins it
(`kai_widget__wrap__mono__Int` lands in `widget.c`, instantiated from
`main.kai`).

## Structural surprises the brief did not anticipate

**Restricting `home_tu` to DFn/DType is load-bearing for byte-id.** The
first draft mapped DConst / DEffect / wrapper nodes to their `mo` too (a
"more complete" `home_tu`). That spawned an EMPTY `concurrent.c` TU in
the portfolio stream — a DEffect from module `concurrent` that
`decl_origin` sent to root now claimed its own TU, with no body (only
DFn emits a per-TU body). One extra stem = byte-id regression. The gate
caught it. Fix: `home_tu` reproduces `decl_origin` exactly (DFn/DType ->
mo, else None); the `#[doc]` explains why the bodyless kinds ride root.
Lesson: a partition function must match the emitter's body-emission set,
not the set of decls that merely carry an `mo`.

**kaic1 chokes on a file-top `#[doc("""...""")]`.** `home_tu.kai` first
led with a module-level triple-string doc-attr on line 1. The bundle
build failed with `unterminated char literal` pointing ~1900 lines LATER
(a legitimate `']'` in a regex module) — the bootstrap lexer left the
triple-string in a bad state and mis-lexed the rest of the concatenated
bundle. Not apostrophes, not non-ASCII (a working bundle triple-string
in `kir_pat_alias.kai` has both): the trigger is `#[doc("""` as the
FIRST line of a bundled compiler module. Fix: lead with a `#` module
comment (the pattern every other compiler module already follows) and
keep `#[doc]` on the `pub` symbol only. Worth a memory for the next
compiler-module author.

## Fixtures + coverage

`examples/multi-module/home-tu-partition` (main + `lib/widget.kai`)
covers all six cases in one c-modular split: root pub fn -> main.c, root
private fn / main / lambda -> __root__.c, imported pub fn -> widget.c,
cross-module spec -> widget.c. `test-home-tu-partition` asserts each
symbol's TU from the stream, then links + runs the whole-program build
to pin the partition link-correct. The cross-module-spec case overlaps
`test-issue-963-cross-module-mono` (which already exercised spec home-TU
attribution); the new fixture makes the whole partition explicit in one
place.

## Gate

The real gate is byte-id: single-TU C, c-modular stream, and
`test-modular-selfhost` (the compiler compiles a program byte-identically
whether built single-TU or modular). All held across both blocks.
`KFn.mo` is native-only — `kir_dump` does not print it and the C backend
never reads KIR — so KIR goldens and emitted C are unchanged. Local:
tier0 + selfhost byte-id + test-modular-selfhost + test-kir +
test-home-tu-partition, all green. Heavy tests (tier1, serial parity,
ASAN-Linux) to CI.

## Follow-ups for L3

- L3 (#989) consumes `home_tu` + `KFn.mo`: partition the KProgram by
  `KFn.mo` -> N LLVM modules -> N content-addressed objects -> link with
  the runtime owner. `home_tu` gives it the same symbol->TU map the C
  path uses, so cross-TU links agree by construction.
- If L3 ever needs a home for a decl kind that grows a real emitted body
  (a new bodied top-level form), add its arm to `home_tu` AND confirm the
  emitter emits its body per-TU, or it will spawn an empty stem again.
