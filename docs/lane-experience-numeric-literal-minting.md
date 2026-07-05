# Lane experience: numeric-literal minting

Goal: generalize the bidirectional literal-minting the typer already had
(gated narrowly to `Decimal<u>` + let-annotation) so a numeric literal
coins to the concrete carrier resolved from context — `let a: Decimal =
0.2` is born exact, `let x: BigInt = 5` works — plus two mechanical
suffixes (`u8`, `d`).

## Scope as planned vs as shipped

Planned: generalize the existing `try_lower_decimal_uom_lit` match, add
`u8`/`d` suffixes, keep the source lexeme on the Real token.

Shipped: the mechanism turned out MORE fragmented than the brief framed
it. There were two disjoint narrow lanes, not one:
- Decimal-with-unit: `try_lower_decimal_uom_lit` gated on
  `TyDimT(Decimal, u)` + `ELitUnit`, entered post-synth via
  `maybe_lower_decimal_lit` in SLet/field-init — and LOSSY, because
  `real_to_dec_parts(r: Real)` re-stringified the f64 and ignored the
  source span already sitting in `EReal(_, span)`.
- Byte-in-list: `check_elem_against` retagged `EInt(n) -> TyByte` with a
  0..255 check, entered via the EList arm of `check_expr_against`.

The generalization is a new `compiler/lit_mint.kai` (A, km 92.8) holding
the whole coinage lattice as pure functions, plus a `maybe_mint_literal`
that replaced `maybe_lower_decimal_lit` at the two post-synth sites.

## Key design decisions

1. Where the minting arm lives — post-synth `maybe_mint_literal`, NOT a
   new arm in `check_expr_against`. A scalar literal is a leaf; the
   bidirectional dispatcher adds nothing, and the Decimal lane already
   validated the post-synth pattern.

2. The concrete-vs-variable gate — mint only when `apply_ty(st.sub,
   expected)` is a concrete carrier; a type variable falls through to the
   shape default and unifies. This is the line that keeps the design out
   of Haskell territory: no "coinable-from-integer" obligation ever
   travels, so `f(5)` where `fn f[a](x:a)` defaults `5` to Int and
   unifies `a := Int`. mint-then-unify-confirms: the unifier stays the
   authority.

3. past-i64 integers — chose a new textual node `EIntLit(String)` over
   widening `EInt(Int)` to `EInt(Int, String)`. The arity change touched
   ~113 sites (49 `EInt(_)`, ~40 binders, ~24 constructions) across the
   backend — coupling a slot only two sites use, exactly what byte-id
   selfhost and the quality bar punish. `EIntLit` is additive: the
   parser emits it in place of the past-i64 error, the typer dissolves
   it (invariant: never survives inference), and the ~25 backend walkers
   get a leaf arm. The `d` suffix got a twin `EDecimalLit(String)` for
   the same reason — Decimal has an Int128 ceiling, so past-carrier is a
   fold-time error only the typer can raise (a stdlib-call desugar like
   `5n` can't, because it degrades the static error to a runtime None).

## Structural surprises the brief did not anticipate

- The brief said "add the source lexeme to the Real token." It was
  already there — `EReal(Real, String)` slot 1, `parse.kai:1612`. The
  lossy path ignored it; the fix was feeding the span to a string-based
  decoder, not a lexer/token change.

- Minting runs AFTER the resolver, so a minted `bigint.from_literal`
  callee had to be an already-resolved `EModCall`, not the
  `EField(EVar("bigint"), _)` the parser's `5n` desugar uses (the parser
  runs before the resolver). First rebuild caught this as `use of
  undeclared identifier 'kai_bigint'`.

- A textual literal RHS under annotation is destroyed by `synth` before
  the post-synth mint sees it (synth errors on a context-free
  `EIntLit`). Fix: a pre-synth `synth_let_rhs_or_mint` intercepts
  textual literals in the annotated-SLet path and mints against the
  resolved annotation directly.

- `const NAME : String = ...` at module level does not parse through
  kaic1 (no other compiler module uses `const`). Reverted to a `fn`
  returning the constant, the precedent (`i64_max_digits()`).

- non-ASCII em-dashes in NEW comments were a red herring at first (the
  files already had hundreds of pre-existing ones and compiled fine);
  the real first-rebuild failure was the `const`.

## The exhaustive-walker sweep — and the trap in it

Adding two ExprKind variants means every exhaustive `match ExprKind`
needs the two arms. The bulk was ~49 `EReal(_, _) ->` arms across ~19
files, each getting an `EIntLit`/`EDecimalLit` arm copying the neighbor.
Mechanical, delegated, done in one pass.

The trap: a `grep "EReal(_, _)"` sweep MISSES the walkers that bind the
Real's fields — `EReal(r, _) ->`, `EReal(r, span) ->`. Six such sites
survived the sweep (dump_expr, fmt_expr, cache serialize, two kir
lowering fns, emit_expr_boxed, emit_kind_raw). They split by phase:
pre-typer dumpers/formatter re-emit the source lexeme (`EIntLit(s) ->
emit(s)`, `EDecimalLit(s) -> emit(s ++ "d")`); post-typer emit/cache/kir
sites take a loud `panic(...)` — honest to the invariant that these
nodes never survive inference. `kaic1` builds the bundle without
exhaustiveness checks, so it compiled fine and hid all six; only the
`kaic2` self-host surfaced them as `non-exhaustive match on ExprKind`.
Lesson: after the byte-id `make kaic2`, the real gate for a new AST
variant is the self-host, not the bundle build — and the sweep pattern
must cover both `EReal(_, _)` and `EReal(<binder>, _)`.

A second reporting wrinkle: kaic2 labels every self-host error location
`main.kai:N` (the import-resolved buffer name) but keeps the module's
PHYSICAL line number. So `main.kai:6327` was literally `emit_c.kai:6327`
— map by finding the compiler file long enough to own that line.

## Fixtures added

Positives (examples/numeric/): literal_mint (Decimal 0.2/0.20 exact,
DecimalBig 30-digit exact, BigInt 5 + 2^128, Decimal 5), literal_suffix
(200u8, 19.99d, 42d), literal_principality (id(5)/pair(2,3) default to
Int — the principality guard). Negatives: mint_int_from_point (0.2:Int),
mint_int_from_whole_point (3.0:Int), mint_byte_overflow (256),
mint_u8_suffix_overflow (256u8), mint_decimal_overflow (past-Int128 d).
Wired into test-numeric-literal-mint (C backend, tier1 + test-fast).

## Follow-ups left

- The `real_to_dec_parts` lossy decoder in the OLD Decimal-with-unit
  path (infer.kai) is now superseded by lit_mint's lexeme decoder for
  the bare-Decimal case, but `Decimal<u>` still routes through the old
  path — a later lane could unify them.
- f32 remains out (F32 collapses to TyReal); Rational stays out of the
  lattice by design.
