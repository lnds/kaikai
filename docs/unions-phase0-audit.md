# Phase 0 audit — unified algebraic types (issue #187)

Status: **audit complete** (2026-05-03). Doc-only lane.

This audit informs Phase 1+ of the unified-algebraic-types
redesign tracked by issue #187. The redesign replaces the
additive-`union`-keyword approach proposed by `docs/unions-design.md`
(originally for issue #184): under the new model, `|` always
means "union of types" and there are no separate sum-type
variants — every name on the right of `|` is a type. Components
that don't already exist are auto-declared as nominal unit-like
or record types. See `gh issue view 187` for the full design
decisions D1–D6.

The audit measures the migration surface across `stdlib/`,
`examples/`, `demos/`, plus the typer in `stage2/compiler.kai`
and the bootstrap chain (`stage0/`, `stage1/compiler.kai`). It
does not propose Phase 1's implementation.

---

## §1 — Sum-type inventory

Walked every `type` declaration in `stdlib/`, `examples/`,
`demos/`. Tallies below count **distinct declarations**;
declaration shapes are:

- `unit-only`: `type T = A | B | C` — every variant nullary.
- `payload-only`: `type T = A(X) | B(Y)` — every variant has args.
- `mixed`: `type T = A | B(X)` — at least one of each.
- `record`: `type T = { ... }`.
- `alias`: `type T = U` (including `type T = Int where ...`
  refinement aliases and `type T = Eff1 + Eff2` effect aliases).

Total `type` declarations (excluding `.expected` golden files):
**~176** across `stdlib/` (37), `examples/` (~96), `demos/` (~30).

### Sum-type declarations only

Filtered to the shapes that this redesign affects (`unit-only`,
`payload-only`, `mixed`). Records and aliases are unaffected
modulo the `|` reinterpretation, so they don't appear below.

#### `stdlib/`

| File:line | Type | Shape | #variants | Variants |
|---|---|---|---|---|
| `actor.kai:27` | `Overflow` | unit-only | 3 | `DropOldest`, `DropNewest`, `BlockSender` |
| `actor.kai:29` | `MailboxPolicy` | mixed | 2 | `Unbounded`, `Bounded(Int, Overflow)` |
| `collections/map.kai:61` | `Tree[k,v]` | mixed | 2 | `TEmpty`, `TNode(k, v, Int, Tree[k,v], Tree[k,v])` |
| `crypto/hash.kai:88` | `SplitN` | payload-only | 2 | `SplitOk([Int],[Int])`, `SplitShort([Int])` |
| `encoding/json.kai:32` | `JsonValue` | mixed | 7 | `JNull`, `JBool(Bool)`, `JNum(Int)`, `JReal(Real)`, `JStr(String)`, `JArr([JsonValue])`, `JObj([Pair[String,JsonValue]])` |
| `regexp.kai:28` | `CharRange` | payload-only | 1 | `CR(Char, Char)` |
| `regexp.kai:32` | `Anchor` | unit-only | 2 | `AnchorStart`, `AnchorEnd` |
| `regexp.kai:52` | `RxAst` | mixed | 11 | `RxChar(Char)`, `RxAny`, `RxClass(...)`, `RxAnchor(Anchor)`, `RxConcat([RxAst])`, `RxAlt([RxAst])`, `RxStar(RxAst)`, `RxPlus(RxAst)`, `RxOpt(RxAst)`, `RxRepeat(...)`, `RxGroup(Int, RxAst)` |
| `regexp.kai:502` | `NfaTransition` | payload-only | 7 | `TChar`, `TClass`, `TAny`, `TEpsilon`, `TGroupOpen`, `TGroupClose`, `TAnchor` (each with args) |
| `regexp.kai:522` | `NfaFrag` | payload-only | 1 | `NF(Int, Int)` |
| `regexp.kai:624` | `NB_FragR` | payload-only | 1 | `NB_Frag(NfaBuilder, NfaFrag)` |
| `regexp.kai:1219` | `Regex` | payload-only | 1 | `R(Nfa)` |

12 sum types in `stdlib/` (single-variant wrappers and
multi-variant). Stdlib is **rigorously prefixed**: `Rx*`, `T*`,
`J*`, `NB_*`, `NF`, `R`, etc. — no global naming overlap.

#### `examples/`

| File:line | Type | Shape | #variants | Variants |
|---|---|---|---|---|
| `aspirational/audit_handler/audit_handler.kai:21` | `Event` | payload-only | 3 | `Login(String)`, `Logout(String)`, `Error(String)` |
| `aspirational/calculator/calculator.kai:32` | `BinOp` | unit-only | 4 | `Add`, `Sub`, `Mul`, `Div` |
| `aspirational/calculator/calculator.kai:34` | `Expr` | payload-only | 4 | `ENum(Decimal)`, `EVar(String)`, `EBin(BinOp,Expr,Expr)`, `ENeg(Expr)` |
| `aspirational/event_ledger/event_ledger.kai:26` | `Event` | payload-only | 3 | `Deposit(...)`, `Withdraw(...)`, `Transfer(...)` |
| `aspirational/event_logger/event_logger.kai:28` | `Event` | payload-only | 3 | `Login(String)`, `Logout(String)`, `Error(String)` |
| `effects/interp_recursive_walk.kai:24` | `Walked` | payload-only | 2 | `Leaf(Int)`, `Bin(Walked, Walked)` |
| `effects/m8_9_supervision_pattern.kai:28` | `WorkerStatus` | payload-only | 1 | `Done(Int)` |
| `effects/m8x_7_fiber_in_sum.kai:14` | `Boxed` | payload-only | 1 | `Wrap(Fiber[Int])` |
| `effects/m8x_7_pid_in_sum.kai:9` | `PidBox` | payload-only | 1 | `Hold(Pid[Nothing])` |
| `effects/m8x_7_recursive_sum_no_breach.kai:9` | `Tree[T]` | payload-only | 2 | `Leaf(T)`, `Node(Tree[T], Tree[T])` |
| `effects/m8x_9_nested_mailbox_under_trap_exit.kai:26` | `CellMsg` | unit-only | 2 | `Tick`, `Ack` |
| `minimal/interp.kai:1` | `Expr` | payload-only | 4 | `Lit(Int)`, `Add(Expr,Expr)`, `Mul(Expr,Expr)`, `Neg(Expr)` |
| `perceus/issue_82_leak_audit.kai:42` | `Pair` | payload-only | 1 | `MkPair(Int, Int)` |
| `portfolio/portfolio.kai:21` | `TxKind` | unit-only | 3 | `Deposit`, `Withdrawal`, `Fee` |
| `protocols/derive_ord_eq_consistency.kai:28` | `Maybe` | mixed | 2 | `Nothing`, `Just(Int)` |
| `protocols/derive_ord_sum_basic.kai:16` | `Tier` | unit-only | 4 | `Bronze`, `Silver`, `Gold`, `Platinum` |
| `protocols/derive_ord_sum_with_payload.kai:16` | `Shape` | mixed | 3 | `Circle(Int)`, `Square(Int)`, `Origin` |
| `protocols/derive_show_sum.kai:13` | `Shape` | mixed | 3 | `Circle(Int)`, `Square(Int)`, `None_` |
| `protocols/m12_8_compiler_shapes.kai:60` | `TokKind` | mixed | 5 | `TInt(Int)`, `TKwLet`, `TKwFn`, `TLParen`, `TRParen` |
| `protocols/m12_8_x_derive_eq_sum_basic.kai:21` | `MySum` | mixed | 3 | `Variant1(Int,Bool)`, `Variant2(String)`, `Variant3` |
| `protocols/m12_8_x_derive_eq_sum_nested.kai:18` | `Inner` | payload-only | 2 | `Leaf(Int)`, `Pair(Int,Bool)` |
| `protocols/m12_8_x_derive_eq_sum_nested.kai:24` | `Outer` | mixed | 3 | `Wrap(Inner)`, `Located(Point,Inner)`, `Empty_` |
| `protocols/m12_8_x_derive_eq_sum_no_impl.kai:17` | `Bad` | payload-only | 2 | `Plain(Int)`, `Wrapped(Payload)` |
| `protocols/m12_8_x_derive_hash_sum_basic.kai:19` | `MySum` | mixed | 3 | `Tag(Int,String)`, `Marker(Int)`, `Empty_` |
| `protocols/m12_8_x_derive_hash_sum_consistent.kai:31` | `Color` | mixed | 5 | `Red`, `Green`, `Blue`, `Custom(Int,Int,Int)`, `Named(Int)` |
| `protocols/m12_8_y_nested_derive.kai:17` | `Inner` | unit-only | 3 | `A`, `B`, `C` |
| `protocols/m12_8_y_postfix_method_call.kai:17` | `Inner` | unit-only | 2 | `A`, `B` |
| `quickstart/02_fizzbuzz.kai:15` | `Tag` | mixed | 4 | `Both`, `Fizz`, `Buzz`, `Other(Int)` |
| `quickstart/03_calculator.kai:10` | `Expr` | payload-only | 4 | `Lit(Int)`, `Add(Expr,Expr)`, `Mul(Expr,Expr)`, `Neg(Expr)` |
| `sugars/map_pipe_sum_type_intact.kai:10` | `Direction` | unit-only | 2 | `Up`, `Down` |
| `sugars/variants_basic.kai:8` | `Suit` | unit-only | 4 | `Hearts`, `Diamonds`, `Clubs`, `Spades` |
| `sugars/variants_payload.kai:5` | `Mixed` | mixed | 4 | `Foo`, `Bar(Int)`, `Baz`, `Qux(String)` |

32 sum-type decls in `examples/`.

#### `demos/`

| File:line | Type | Shape | #variants | Variants |
|---|---|---|---|---|
| `9d9l/huffman/main.kai:27` | `Tree` | payload-only | 2 | `Leaf(Int,Char)`, `Node(Int,Tree,Tree)` |
| `blackjack/main.kai:4` | `Suit` | unit-only | 4 | `Hearts`, `Diamonds`, `Clubs`, `Spades` |
| `blackjack/main.kai:7` | `Rank` | mixed | 5 | `Ace`, `Num(Int)`, `Jack`, `Queen`, `King` |
| `concurrent/main.kai:31` | `Status` | payload-only | 1 | `Done(Int)` |
| `forth/main.kai:4` | `Token` | mixed | 7 | `TNum(Int)`, `TPlus`, `TMinus`, `TStar`, `TDup`, `TDrop`, `TSwap` |
| `poker_dealer/main.kai:10` | `Pinta` | unit-only | 4 | `Picas`, `Corazones`, `Treboles`, `Diamantes` |
| `poker_dealer/main.kai:14` | `Mano` | mixed | 10 | `Invalida`, `CartaAlta(...)`, `Par(...)`, `DoblePar(...)`, `Trio(...)`, `Escala(Int)`, `Color(Int)`, `FullHouse(Int,Int)`, `Poker(Int,Int)`, `EscalaDeColor(Int)` |
| `poker/main.kai:13` | `Pinta` | unit-only | 4 | `Picas`, `Corazones`, `Treboles`, `Diamantes` |
| `poker/main.kai:17` | `Mano` | mixed | 10 | (same as `poker_dealer` Mano) |
| `vs/elixir/main.kai:35` | `CounterMsg` | mixed | 3 | `Inc`, `Dec`, `Get(Pid[Int])` |
| `vs/python/main.kai:27` | `Status` | unit-only | 3 | `Active`, `Inactive`, `Suspended` |
| `vs/rust/main.kai:38` | `WorkMsg` | mixed | 2 | `Push(Int)`, `Done` |

13 sum-type decls in `demos/`.

### Headline counts

| Bucket | Sum-type decls | Total variants |
|---|---|---|
| `stdlib/` | 12 | 38 |
| `examples/` | 32 | ~88 |
| `demos/` | 13 | ~52 |
| **Total** | **57** | **~178** |

Plus ~119 record / alias / refinement decls (unaffected by the
redesign at the variant-semantics level).

---

## §2 — Name-collision risk

D2 forbids the same name appearing in multiple `type`
declarations within the same scope (compilation unit / module).
kaikai today is one-file-one-module — `import` is the only way
two files share a namespace.

### Within-file collisions

**Zero.** No file declares two `type` decls that share a variant
name with each other. Stdlib is rigorously prefixed (`Rx*`,
`T*`, `J*`, `NB_*`); examples and demos each declare a small,
locally-disjoint set.

### Cross-file (cross-module) name overlap

These would only collide if both modules were imported into the
same compilation unit. None of them currently are — but they're
worth flagging because the redesign elevates them to *types*,
and types in two imported modules need disambiguation.

| Variant name | Declared in | Same intent? |
|---|---|---|
| `Done` | `examples/effects/m8_9` (`WorkerStatus`), `demos/concurrent` (`Status`), `demos/vs/rust` (`WorkMsg`) | Distinct. Each means "a worker finished" but with different payloads. Three independent demo files; no shared imports. |
| `Login`, `Logout`, `Error` | `examples/aspirational/audit_handler` AND `examples/aspirational/event_logger` (both as `Event`) | **Same intent** — `event_logger` is the renamed/successor demo. Collides if both are imported together (they aren't). Under redesign, pre-declare once and reference. |
| `Deposit` | `examples/aspirational/event_ledger/Event` (with payload), `examples/portfolio/TxKind` (unit) | Same domain word, different semantics. Distinct. |
| `Hearts`, `Diamonds`, `Clubs`, `Spades` | `examples/sugars/variants_basic/Suit`, `demos/blackjack/Suit` | **Same intent** — a card suit. Could be unified into a shared `Suit` type if either ever imported the other (they don't). |
| `Picas`, `Corazones`, `Treboles`, `Diamantes` | `demos/poker/Pinta`, `demos/poker_dealer/Pinta` | **Same intent** — Spanish-suited cards. `poker_dealer` is a fork of `poker`. Cross-import nonexistent. |
| `Invalida`, `CartaAlta`, `Par`, …, `EscalaDeColor` (10 names) | `demos/poker/Mano`, `demos/poker_dealer/Mano` | **Same intent**. Same fork situation. |
| `Leaf`, `Node` | `demos/9d9l/huffman/Tree`, `examples/effects/m8x_7_recursive_sum_no_breach/Tree[T]` | Same intent (binary tree node), different shapes. |
| `Pair` | `examples/protocols/m12_8_x_derive_eq_sum_nested/Inner` (variant) AND `stdlib/core/tuple.Pair` (record) AND record-`Pair` decls in `examples/sugars`, `examples/perceus`, `examples/protocols/derive_ord_eq_consistency` | The variant `Pair` in `Inner` is **distinct** from the stdlib record `Pair[a,b]`. Under redesign, the variant would auto-declare a *new* type `Pair` shadowing the stdlib alias if `examples/protocols/m12_8_x_derive_eq_sum_nested` ever imported `tuple` (it doesn't today). |
| `Inner` | reused as the parent **type name** (not variant) in 3 example files | These are type names in disjoint modules; no collision. |
| `MySum`, `Empty_` | `examples/protocols/m12_8_x_derive_eq_sum_basic`, `examples/protocols/m12_8_x_derive_hash_sum_basic` | Two protocol-derive fixture files. Distinct fixtures, never co-imported. |
| `Add`, `Mul`, `Neg`, `Lit` | `examples/minimal/interp/Expr`, `examples/quickstart/03_calculator/Expr`, plus `Add`/`Mul` in `aspirational/calculator/BinOp` | Two near-identical pedagogical interpreters. Same intent. |
| `Tag` | `examples/quickstart/02_fizzbuzz/Tag` (variant) and `examples/protocols/m12_8_x_derive_hash_sum_basic/MySum` (variant `Tag`) | Distinct. |

**Headline**: zero blocking collisions inside a single module.
A handful of cross-module same-intent pairs that *would* trip D2
if a future user imported both — but such co-imports do not
exist in the audited tree. Migration impact: zero immediate
breakage; minor renames or shared-type extraction if anyone
later co-imports a colliding pair.

This is consistent with kaikai's small surface and disciplined
naming. The migration is **not** non-trivial.

---

## §3 — Variant-as-type reliance

Searched for code where a current variant name is used in a
**type position** (function parameter type, return type, `let`
annotation, generic argument, list element type). Today such uses
are compile errors or treat the name as a free type variable;
under the redesign, the variant name resolves to an
auto-declared component type and would silently compile.

Method: built the full ~144-name variant set from §1, grepped
every occurrence of `: <Name>`, `[<Name>]`, `<<Name>>` across
`stdlib/`, `examples/`, `demos/`, then filtered out
constructor-call sites and same-file declarations.

Findings:

- **Genuine variant-as-type-position uses: zero.** Every grep
  hit was either:
  1. A constructor invocation in value position
     (`JStr("...")`, `Tx { kind: Deposit, ... }`,
     `Custom(255, 0, 0)`).
  2. The name of a *top-level type* (not a variant of another
     type) used legitimately as a type — e.g., `: Tree[k,v]`
     (the parent type), `: Pair[String, Int]` (the stdlib
     record type), `: Tag` (the parent of `Both | Fizz | Buzz |
     Other`), `: Walked` (parent), `: Wrap` (parent in
     `m12_8_y_postfix_method_call`), `: Color` (parent),
     `: Nothing` (the bottom type).
- **Equality-on-variant patterns** (`x == None`, `r.status ==
  Active`): widespread but unchanged in semantics. `None`,
  `Active`, etc., remain constructible values; the runtime
  representation is identical under Model-C-style lowering, and
  `Eq` derivation walks components either way.
- **Pattern-binding shapes**: every `match v { Foo -> ... }` for
  a unit variant `Foo` is benign — under the redesign it
  becomes a type-match instead of a label-match, but the
  observable behavior is identical (single-shape unit type,
  unique tag). No payload-binding pattern relied on label
  semantics.

**Headline count: 0 risky usages, 0 silent-behavior-change
sites.** This is the most important finding: the
syntactically-identical migration claim in issue #187 is
empirically supported by the audited tree.

---

## §4 — Typer surface area

Sites in `stage2/compiler.kai` that touch sum types and would
need work for Phase 1+. Lines reflect today's source.

### Type representation

- `stage2/compiler.kai:17948` — `type Ty = TyInt | TyReal | ... |
  TyCon(String, [Ty]) | ...`. **No `TySum` or `TyVariant` node**;
  user sum types fold into `TyCon(name, type_args)` and the
  list-of-variants is tracked separately in `TyEnv`.
  Redesign would add `TyUnion(components: [Ty])` (or reuse
  `TyCon` with an attached components list), plus
  associativity/commutativity/idempotence normalization helpers.

### Type-body parsing

- `stage2/compiler.kai:1476` — `type TypeBody = TBAlias |
  TBRecord | TBSum([Variant]) | TBEffectAlias`. `TBSum` survives
  but its components are now interpreted as types.
- `stage2/compiler.kai:1478` — `type Variant = Var(String,
  [TypeExpr])`. Stays syntactically; semantically each
  `Var(name, [arg_tys])` declares a new type (unit if `args`
  empty; record-with-positional-fields otherwise).
- `stage2/compiler.kai:7654` `parse_sum_body` and
  `7565`/`7586` `looks_like_sum_scan` / `looks_like_sum_after_nl`
  — parser shape unchanged. No work.

### Variant registration

- `stage2/compiler.kai:19387` — `scheme_of_variant` builds the
  constructor scheme `(args...) -> TyCon(parent, ...)`. Under
  redesign, **two** schemes: one for the component-as-type
  (constructor), one for the parent union (the parent
  resolves to `TyUnion([Component, ...])`).
- `stage2/compiler.kai:19402-19420` — `add_variants_of_decl` /
  `add_variants_loop`. Today registers each variant constructor
  in `TyEnv`. Under redesign also registers each component as a
  *type entry* in the type namespace. **D2 collision check
  lives here**: error if a component name already names a type.
- `stage2/compiler.kai:9456` `register_variants`,
  `stage2/compiler.kai:8932` `register_variants_env` — codegen-side
  variant table; same shape, just produces the new component
  types alongside the union.

### Pattern resolution + exhaustiveness

- `stage2/compiler.kai:23900` `variants_of_type` — lists
  variant-name strings reachable from a `TyCon` parent. Under
  redesign, lists *component types* instead.
- `stage2/compiler.kai:23912` `check_variants_nullary` — backs
  the `variants[T]()` builtin. Same idea, components instead of
  labels.
- `stage2/compiler.kai:23991` `check_exhaustiveness` — main
  exhaustiveness checker. Under redesign walks the union's
  components; for each component recurses into its variants if
  it is itself a sum-type-shaped component. Exhaustiveness rule
  in the redesign: "every variant of every component covered
  (possibly transitively)".

### Pattern matching codegen

- `stage2/compiler.kai:1305` `PVariant(String, [Pattern])` — AST
  node. Stays.
- `stage2/compiler.kai:4286-4326` `parse_variant_sub_patterns` —
  parser. Stays.
- `stage2/compiler.kai:10146` `emit_pat_test_variant`,
  `:10193`, `:10238` `emit_pat_test_variant_record`, `:10271`,
  `:10308` `emit_pat_binds_variant`, `:10336-10538`
  `emit_match_*` — codegen tests a tag and binds positional
  payloads. Tags continue to be the type ID (= today's variant
  string, plus the auto-declared component types). Codegen
  largely unchanged; the runtime representation is preserved
  by design (Model C in `docs/unions-design.md` §
  *Decision 2*).

### `#derive` walkers

- `stage2/compiler.kai:34806` `validate_derive_sum_protos`,
  `:34823` `validate_derive_sum_variants`,
  `:35001` `derive_show_sum_body`, `:35111` `derive_eq_sum_body`,
  `:35223` `derive_hash_sum_body`, `:35342`
  `derive_ord_sum_body`. Each walks `[Variant]`. Under redesign
  these walk the union's component list and, for each
  component, derive transitively (D4 in issue #187: walk each
  component, auto-derive on auto-declared unit components).

### Formatter + LLVM emit

- `stage2/compiler.kai:37448` `fmt_sum_variants` — purely
  syntactic, unchanged.
- `stage2/compiler.kai:31068` `llvm_emit_variants_of` — LLVM
  emission, unchanged because runtime representation is
  preserved.

### Stage 1 mirror

`stage1/compiler.kai` has the same shape with shorter names
(lines `1476`-equivalents around `:684–:692` for `Decl /
TypeBody / Variant`, `:8911 register_variants_env` etc.). Stage
1 lacks a typer (no exhaustiveness, no inference) — only the
parser + codegen sides change.

### Blast-radius estimate

**~40 named functions touched in `stage2`**, of which roughly:

- 5 representation/parser sites (small, additive).
- 8 variant-registration sites (where D2 lives).
- 6 exhaustiveness/pattern sites (small extensions).
- 12 `#derive`-walker sites (mechanical extensions).
- ~9 formatter / codegen / LLVM sites (mostly no-op).

The issue #187 estimate of ~250 lines for Phase 1 (introduce
`TyUnion`, lower current sum types to it, selfhost
byte-identical) is **realistic** given that the pattern-codegen
and runtime representation stay put. The bulk of work lands in
the typer (`add_variants_loop`, `scheme_of_variant`,
`check_exhaustive`, `variants_of_type`).

---

## §5 — stdlib protocol-impl interaction

Walked `stdlib/protocols.kai` (212 lines, 25 `impl` blocks).

| Impl target | Count |
|---|---|
| `Int` | 5 (`Show`, `Eq`, `Ord`, `Hash`, `Serialize`) |
| `Real` (incl. `Real<u>`) | 4 |
| `Bool` | 4 |
| `Char` | 4 |
| `String` | 5 |
| `Unit` | 2 |
| **User-defined sum type** | **0** |

Every `impl` in `stdlib/protocols.kai` targets a primitive type.
Zero impls walk a sum type by label, zero walk a sum type by
structure. Protocol dispatch in stdlib has **no hidden
assumptions about variants-as-labels**. The redesign cannot
break stdlib protocol impls because there are none against sum
types.

User-side `impl Show for Wrap` etc. live in `examples/protocols/`
and are syntactically `match self { Variant(...) -> ... }` —
identical syntax under the redesign because pattern syntax does
not change.

---

## §6 — Bootstrap-chain risk

### Stage 0 (`stage0/*.c`)

Stage 0 is the C bootstrap compiler (5,069 LoC). Variant
handling sites:

- `stage0/parser.c` parses `type X = A | B(args) | ...` into
  `N_TYPE_DECL` with body `N_TY_SUM` whose children are
  `N_VARIANT` nodes (string name + child arg types). **No
  semantic interpretation** — variant arg types are walked
  syntactically and discarded.
- `stage0/check.c:317-330` (`register_top_level`) registers the
  type name and each variant constructor name in the lexical
  scope so forward references resolve. Names only — no type
  info.
- `stage0/emit.c:1896-1910` (`register_user_variants`) tracks
  variant `(name, arity)` so calls and patterns know whether to
  emit a `kai_variant(...)` constructor or a regular function
  call. Output: `kai_variant(0, "Name", n_args, args)` — a
  string-tagged runtime value.
- `stage0/emit.c:508` `find_variant`, `567`/`590` ident
  emission, `837` `emit_variant_construction` — string-name
  dispatch, no type-level reasoning.

**Stage 0 is oblivious to variant semantics beyond string names
and arity.** The redesign does not change the syntactic shape
the parser sees, the names registered, or the runtime tag
representation. Stage 0 needs **zero changes** for Phase 1+,
modulo (later) Phase 2 if D2 collision-checks are mirrored
here — which is unnecessary because stage 0 is the *minimal*
compiler and the integrator can land D2 only in stage 2.

### Stage 1 (`stage1/compiler.kai`)

Stage 1 is the kaikai-minimal-language self-host. ~60 `type`
declarations, including the parser AST (`Expr`, `ExprKind`,
`Pattern`, `PatKind`, `TypeExpr`, `TyKind`, `Decl`, `TypeBody`,
`Variant`, …). All compiler-internal — none of these get
imported into user code.

Stage 1 mirrors stage 0's parser+codegen split: it parses sum
types and emits identical-shape C output, but does not do type
inference or exhaustiveness checking. Sum-type semantics that
*would* change under the redesign (the typer side) are absent
here. Stage 1 needs the same surface tweaks as stage 0
(none beyond the parser-already-accepts-the-syntax fact) plus
the new component-type registration if Phase 1 wants stage 1 to
*author* code that uses unions — but the audit bundle for #187
should keep stage 1 frozen until Phase 1+ proves the typer
changes.

### Stage 2 (`stage2/compiler.kai`)

Stage 2 self-hosts through ~289 internal `type` declarations.
None of the sum types from `stdlib/` (`JsonValue`, `RxAst`,
`NfaTransition`, etc.) appear in stage 2's selfhost critical
path — stage 2 is a single-file compiler that does not import
any stdlib module. The selfhost-critical sum types are
**internal to the compiler**: `TokKind`, `ExprKind`, `PatKind`,
`TyKind`, `Decl`, `Stmt`, `TypeBody`, `Variant`, `Ty`,
`TypeExpr`, `RowExpr`, `Pattern`, `EFn`, `EVar`, `IPart`, …

These migrate *transparently* under the redesign because the
compiler is its own first user — the moment the typer learns
the new model, the compiler's internal sum types continue to
parse and behave identically.

The risk is contained to Phase 1's typer changes, gated by `make
selfhost` (CI Tier 1).

### Headline

Stage 0 and stage 1 are **structurally insulated** from the
redesign — they treat variants as opaque string-named tagged
values. Stage 2 is the only compiler that gains real semantic
behavior. Bootstrap-chain risk is **low**. The redesign is
fundamentally safer than the additive-`union`-keyword path
because it requires no lexer change and no new keyword.

---

## §7 — Risk summary + recommendation

### What the audit found

- **57 sum-type declarations** (12 in `stdlib`, 32 in
  `examples`, 13 in `demos`) producing **~178 variant names**.
  Stdlib is rigorously prefixed; examples and demos are
  locally-disjoint.
- **Zero within-module name collisions.** D2 fires nowhere in
  the audited tree at Phase-2 enable time. A handful of
  cross-module same-intent pairs exist (`Suit`, `Pinta`/`Mano`,
  `Login`/`Logout`/`Error`, `Add`/`Mul`/`Lit`/`Neg`, `Done`)
  but no current file co-imports a colliding pair.
- **Zero variant-as-type reliance.** No code today uses a
  variant name in a type position that would silently change
  meaning under the redesign. The "syntactically-identical
  migration" claim from issue #187 is empirically supported.
- **Zero stdlib protocol impls** target sum types — all 25
  `impl` blocks in `stdlib/protocols.kai` are over primitives.
- **Stage 0 + stage 1 are insulated** — they treat variants as
  opaque string-named runtime tags. Only stage 2's typer needs
  semantic changes. Phase 1's ~250-line estimate is realistic
  given the localized blast radius (~40 fns in stage 2,
  concentrated around variant registration and exhaustiveness).
- **Runtime representation preserved** by design (Model C):
  codegen and LLVM emission paths need no rework.

### Recommendation

**Proceed as planned with issue #187 Phase 1.** The audit found
the redesign's risk profile is **lower** than the `union`
keyword path of #184:

- No lexer change (no new keyword).
- No fixture migration required (existing `type T = A | B`
  parses identically).
- No silent-semantics-change footgun (because §3 found zero
  reliance points).
- No stdlib protocol-impl rewrite required.

No `--legacy-sum-types` opt-in flag is justified by the data.
Phase 1's typer foundation can land directly with `make
selfhost` + `make selfhost-llvm` as the gate.

### Follow-up items for the integrator to consider filing

(Not opened by this audit, per lane discipline.)

1. **Cross-module same-intent variant duplication** — `Suit` in
   `sugars/variants_basic` vs `demos/blackjack`; `Pinta`/`Mano`
   in `demos/poker` vs `demos/poker_dealer`. Could be
   harmonized post-Phase 5 once unions are available.
2. **`event_logger` and `audit_handler` fixture overlap** —
   identical `type Event = Login | Logout | Error`.
   Documentation could clarify which one is canonical.
3. **`docs/unions-design.md` rewrite** — currently reflects the
   #184 additive-keyword design. Issue #187 design decisions
   (D1–D6) should land in that doc as part of Phase 5
   documentation, replacing the proposed-status text. Out of
   scope here (audit only).

---

*End of audit.*
