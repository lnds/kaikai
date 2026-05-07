# kaikai grammar

The single canonical reference for the surface syntax of full kaikai.

This document is the spec for tooling — tree-sitter authors,
formatters, alternative implementers — and a deeper reference for
language users who need precise answers about precedence, ambiguity,
and reserved tokens.

When this document and `stage2/compiler.kai` (the operational parser)
disagree, **`compiler.kai` wins**. This doc is regenerated against
the parser between waves; corrections to the doc are welcome but the
parser is the source of truth.

Out of scope: semantic rules (typing, effect inference, exhaustiveness,
unification) and AST representation. Stage 0 grammar is a strict
subset and lives in `docs/kaikai-minimal.md`. Sugars are spec'd
intentionally in `docs/syntax-sugars.md`; this doc carries only their
*grammar deltas*.

## Conventions

- `UPPERCASE` names denote tokens (terminals) produced by the lexer:
  `IDENT`, `INT_LIT`, `STRING_LIT`.
- `lowercase` names denote grammar productions (non-terminals):
  `expr`, `pattern`, `decl`.
- `'literal'` denotes a literal source string (a keyword or
  punctuation).
- `?` marks an optional element, `*` zero-or-more, `+` one-or-more,
  `|` alternatives. Parentheses group.
- Grammar shape only; operator grouping is resolved by the precedence
  table in §3, not by the productions in §2.

## §1 — Lexical structure

### 1.1 Whitespace and significant newlines

- Spaces (` `), tabs (`\t`), and carriage returns (`\r`) are
  whitespace and never carry meaning beyond token separation.
- Newlines (`\n`) **terminate statements** when the preceding tokens
  form a syntactically complete prefix at that position. They are
  *not* tokenised as whitespace; the lexer emits `NEWLINE` and the
  parser consumes or ignores it per the rule below.
- A newline is **whitespace** (does not terminate) when:
  - There is an unclosed `(`, `[`, or `{`;
  - The last token is a binary operator, an arrow (`->`, `=>`), an
    assignment (`=`, `:=`), or a separator (`,`, `:`).
- `;` is always a statement terminator. It is uncommon and reserved
  for putting multiple statements on one physical line.
- Pipe operators `|>`, `|`, `||` are accepted in **leading position**
  on a continuation line: the parser skips leading newlines before
  these operators only.

### 1.2 Comments

```
# line comment until end-of-line
```

There are no block comments. The sequence `#derive` at the start of a
token is *not* a comment — it produces the dedicated `DERIVE` token
(see 1.4).

### 1.3 Identifiers

Source identifiers must match `[A-Za-z_][A-Za-z0-9_]*`.

Naming conventions enforced by `kai fmt` and the resolver:

- `snake_case` — functions, variables, fields, parameters,
  module names, capability binders.
- `PascalCase` — type constructors, sum-type variants, protocols,
  effects, units of measure.
- `_` alone is the wildcard pattern token (`UNDERSCORE`), not an
  identifier.
- `kai_*` is reserved for the runtime / FFI shim layer.
- `Self` is reserved inside protocol declarations as the implicit
  self-type parameter.

### 1.4 Keywords and reserved tokens

Reserved words (recognised by the lexer as keyword tokens, never as
identifiers):

```
and       as        assert    axiom     bench     check
const     effect    else      ensures   extern    false
fn        for       handle    if        impl      import
let       match     not       or        protocol  pub
requires  test      true      type      unit      use
var       where     with
```

Plus the special tokens:

- `#derive`  — the `DERIVE` token, used as a prefix to a `type` decl.
- `todo!`    — the `TODOBANG` token, an explicit termination escape.
- `?`        — the `HOLE` token (bare typed hole).
- `?ident`   — the `HOLE_NAME` token (named typed hole).

`Self` is reserved inside protocol bodies but not lexed as a keyword
elsewhere.

### 1.5 Punctuation and operators

| Token | Source |
|-------|--------|
| `LPAREN`, `RPAREN`         | `(`  `)` |
| `LBRACKET`, `RBRACKET`     | `[`  `]` |
| `LBRACE`, `RBRACE`         | `{`  `}` |
| `COMMA`                    | `,`     |
| `COLON`                    | `:`     |
| `COLON_EQ`                 | `:=`    |
| `SEMI`                     | `;`     |
| `DOT`                      | `.`     |
| `DOTDOT`                   | `..`    |
| `ELLIPSIS`                 | `...`   |
| `EQ`                       | `=`     |
| `ARROW`                    | `->`    |
| `FAT_ARROW`                | `=>`    |
| `PIPE`                     | `|`     |
| `PIPE_APPLY`               | `|>`    |
| `BAR_BAR`                  | `||`    |
| `PLUS`, `MINUS`, `STAR`    | `+` `-` `*` |
| `SLASH`, `SLASH_SLASH`     | `/` `//` |
| `PERCENT`                  | `%`     |
| `PLUS_PLUS`                | `++`    |
| `EQ_EQ`, `NEQ`             | `==` `!=` |
| `LT`, `GT`, `LE`, `GE`     | `<` `>` `<=` `>=` |
| `BANG`                     | `!`     |
| `AT`                       | `@`     |
| `CARET`                    | `^`     |

The match between these names and `compiler.kai`'s `Tk*` enum is
1:1; differences in casing are presentation only.

### 1.6 Literals

#### Integer

```
INT_LIT     ::= DEC_LIT | HEX_LIT | BIN_LIT
DEC_LIT     ::= DEC_DIGIT ('_'? DEC_DIGIT)*
HEX_LIT     ::= '0' ('x' | 'X') HEX_DIGIT+
BIN_LIT     ::= '0' ('b' | 'B') BIN_DIGIT+
DEC_DIGIT   ::= [0-9]
HEX_DIGIT   ::= [0-9a-fA-F]
BIN_DIGIT   ::= [01]
```

Underscore separators are accepted only inside the decimal branch
(`1_000_000`). Hex / binary literals do not accept `_`.

#### Real

```
REAL_LIT    ::= DEC_DIGIT+ '.' DEC_DIGIT+ EXP_PART?
              | DEC_DIGIT+ EXP_PART
EXP_PART    ::= ('e' | 'E') ('+' | '-')? DEC_DIGIT+
```

#### Complex (suffix `i`)

```
COMPLEX_LIT ::= INT_LIT 'i' | REAL_LIT 'i'
```

The `i` is consumed only when adjacent to the numeric token (no
intervening whitespace). `3i`, `2.5i`, `1e10i` lex as `COMPLEX_LIT`;
`3 + i`, `3+i` keep `i` as an identifier.

#### Boolean and unit

```
BOOL_LIT    ::= 'true' | 'false'
UNIT_LIT    ::= '(' ')'
```

`UNIT_LIT` is parsed by the expression production, not by the lexer;
it appears here for completeness.

#### Character

```
CHAR_LIT    ::= '\'' (CHAR_BODY | ESCAPE) '\''
ESCAPE      ::= '\\' ('n' | 't' | 'r' | '\'' | '"' | '\\'
                     | 'u' '{' HEX_DIGIT+ '}')
```

#### String

```
STRING_LIT  ::= '"' STRING_BODY '"'
              | '"""' MULTI_BODY '"""'
STRING_BODY ::= (STRING_CHAR | ESCAPE | INTERPOLATION)*
INTERPOLATION ::= '#{' expr '}'
```

Triple-quoted strings preserve indentation relative to the closing
`"""`. Interpolation `#{expr}` accepts any expression; the value's
`Show` impl is invoked at compile time.

#### Regex sigil

```
REGEX_LIT   ::= '~r' '/' REGEX_BODY '/' REGEX_FLAGS?
```

The lexer also emits a bare `REGEX` token from the `/.../` form when
the previous non-newline token is the identifier `matches` (the
refinement-pure predicate). Outside that context, `/` keeps its
arithmetic meaning (`SLASH`).

#### Units of measure literal

```
UOM_LIT     ::= INT_LIT '<' unit_expr '>'
              | REAL_LIT '<' unit_expr '>'
unit_expr   ::= unit_atom (unit_op unit_atom)*
unit_atom   ::= IDENT
              | IDENT '^' INT_LIT
              | IDENT '^' '-' INT_LIT
              | INT_LIT                  (* '1' for dimensionless *)
              | '(' unit_expr ')'
unit_op     ::= '*' | '/' | <space>      (* implicit product *)
```

Inside `<...>`, `^` is the only place the caret token is legal as an
operator — outside UoM literals and types, `CARET` is a parse error.

#### Typed holes

```
HOLE_LIT      ::= '?'
HOLE_NAME_LIT ::= '?' IDENT
```

#### `todo!`

```
TODO_BANG   ::= 'todo' '!'           (* lexed as one token *)
```

The lexer recognises the identifier `todo` immediately followed by
`!` as a single token; arbitrary `ident!` is a parse error elsewhere.

## §2 — Grammar productions

### 2.1 Program and modules

```
program     ::= import* decl*
import      ::= 'import' module_path import_tail?
module_path ::= IDENT ('.' IDENT)*
import_tail ::= 'as' IDENT
              | '.' '{' ident_list '}'
ident_list  ::= IDENT (',' IDENT)*
use_decl    ::= 'use' module_path                    (* m7e §25 *)
```

There is no `module` declaration; a file's module name comes from its
path relative to the project root.

### 2.2 Top-level declarations

```
decl        ::= visibility? decl_inner
visibility  ::= 'pub'
decl_inner  ::= fn_decl
              | type_decl
              | effect_decl
              | protocol_decl
              | impl_decl
              | extern_decl
              | axiom_decl
              | const_decl
              | unit_decl
              | use_decl
              | test_decl
              | bench_decl
              | check_decl
```

#### Function declaration

```
fn_decl     ::= 'fn' IDENT type_params? '(' params? ')' return_spec? fn_body
fn_body     ::= '=' expr
              | block
type_params ::= '[' type_param (',' type_param)* ']'
type_param  ::= IDENT (':' kind_or_bound)?
kind_or_bound ::= 'Measure'
              | type_bound ('+' type_bound)*
type_bound  ::= IDENT                                  (* protocol name *)
params      ::= param (',' param)*
param       ::= IDENT ':' type
return_spec ::= ':' type effect_suffix?
              | effect_suffix                          (* return inferred *)
effect_suffix ::= '/' effect_row
```

`fn` body forms are *both* canonical: `=` for one-line expression
bodies, `{ ... }` (block) for multi-statement bodies. Two forms by
design (CLAUDE.md Tier 2 #4: few forms, each with clear intent).

#### Type declaration

```
type_decl   ::= derive_attr? 'type' IDENT type_params? '=' type_body
              | derive_attr? 'type' IDENT type_params (* sum, multiline *)
                  '=' variant ('|' variant)*
type_body   ::= type                                   (* alias *)
              | '{' field_list? '}'                    (* record *)
              | variant ('|' variant)*                 (* sum *)
variant     ::= IDENT ('(' type_list ')')?
field_list  ::= field (',' field)* ','?
field       ::= IDENT ':' type
type_list   ::= type (',' type)*
derive_attr ::= '#derive' '(' protocol_list ')'
protocol_list ::= IDENT (',' IDENT)*
```

#### Effect declaration

```
effect_decl ::= 'effect' IDENT type_params? '{' effect_op* '}'
effect_op   ::= IDENT '(' params? ')' return_spec
```

Operations are declared without `fn`, without a body; "paying the
effect" is implicit at the call site.

#### Protocol declaration

```
protocol_decl ::= 'protocol' IDENT type_params? '{' protocol_op+ '}'
protocol_op   ::= 'fn'? IDENT '(' params? ')' return_spec ('=' expr)?
```

`Self` is implicit. `protocol P[A]` (issue #180) introduces a single
type parameter `A` for parametrised single-dispatch protocols.

#### Implementation declaration

```
impl_decl   ::= 'impl' impl_type_params? IDENT type_args? 'for' type
                '{' impl_member* '}'
impl_type_params ::= '[' impl_type_param (',' impl_type_param)* ']'
impl_type_param  ::= IDENT (':' type_bound ('+' type_bound)*)?
impl_member ::= 'fn'? IDENT '(' params? ')' return_spec? fn_body
```

Bounded impl-site type parameters (`impl[T : Show + Eq] Show for [T]`,
issue #174) are the only place protocol bounds appear in the syntax;
ordinary `fn` declarations do not carry bounds.

#### `extern "C"` / `axiom`

```
extern_decl ::= 'extern' STRING_LIT 'fn' IDENT
                ('=' STRING_LIT)?                      (* C name override *)
                '(' params? ')' return_spec
axiom_decl  ::= 'axiom' IDENT type_params? '(' params? ')' return_spec
```

`extern "C" fn` (issues #260, #261) routes through `axiom_decl_inner`
so the rest of the pipeline keeps a single shape.

#### Constants and units

```
const_decl  ::= 'const' IDENT (':' type)? '=' expr
unit_decl   ::= 'unit' IDENT ('=' unit_expr)?
```

`unit` introduces a Measure-kind symbol; the optional `= unit_expr`
form declares a derived unit (`unit Newton = kg * m / sec^2`).

#### Test / bench / check blocks

```
test_decl   ::= 'test' STRING_LIT block
bench_decl  ::= 'bench' STRING_LIT block
check_decl  ::= 'check' STRING_LIT property_params? block
property_params ::= '(' params ')'
```

### 2.3 Types

```
type        ::= type_atom effect_suffix?
type_atom   ::= primitive_type
              | IDENT type_args?                       (* user type / ctor *)
              | module_path '.' IDENT type_args?       (* qualified ctor *)
              | '[' type ']'                           (* list of T *)
              | '(' type_list? ')' '->' type effect_suffix?  (* fn type *)
              | '(' type ')'                           (* grouping *)
              | '(' type ',' type (',' type)* ')'      (* n-tuple, arity 2..4 *)
              | type '<' unit_expr '>'                 (* UoM type *)
type_args   ::= '[' type (',' type)* ']'
primitive_type ::= 'Int' | 'Real' | 'Bool' | 'String' | 'Char'
                 | 'Unit' | 'Nothing'
```

### 2.4 Effect rows

```
effect_row  ::= effect_atom ('+' effect_atom)*
effect_atom ::= IDENT type_args?                       (* effect label *)
              | IDENT                                  (* row variable *)
```

Capitalisation disambiguates labels from row variables: `PascalCase`
identifier in row position is an effect label; lowercase is a row
variable bound by the enclosing type-parameter scope.

Examples:

```
T                          (* pure (no suffix) *)
T / Io                     (* singleton row *)
T / Io + Fail              (* closed two-element row *)
T / e                      (* row variable *)
T / Io + e                 (* open row: Io plus whatever e is *)
T / State[Int] + Io        (* parametrised effect label *)
```

### 2.5 Statements and blocks

```
block       ::= '{' (stmt stmt_sep)* expr? '}'
stmt_sep    ::= NEWLINE | ';'
stmt        ::= let_stmt
              | var_stmt
              | assign_stmt
              | assert_stmt
              | expr
let_stmt    ::= 'let' pattern (':' type)? '=' expr
var_stmt    ::= 'var' IDENT (':' type)? '=' expr
assign_stmt ::= IDENT ':=' expr                        (* capability set *)
              | index_lhs '[' expr ']' ':=' expr        (* indexed write *)
index_lhs   ::= IDENT ('.' IDENT)*
assert_stmt ::= 'assert' expr (',' expr)?
```

A block evaluates to the value of its trailing expression; if the
last form is a statement, the block's type is `Unit`.

### 2.6 Expressions

The expression grammar is layered to encode operator precedence
directly (see §3 for the table). Each `parse_<level>` function in
`compiler.kai` corresponds to one production below.

```
expr        ::= pipe_expr
pipe_expr   ::= or_expr (pipe_op or_expr)*
pipe_op     ::= '|>' | '|' | '||'
or_expr     ::= and_expr ('or' and_expr)*
and_expr   ::= cmp_expr ('and' cmp_expr)*
cmp_expr    ::= concat_expr (cmp_op concat_expr)?     (* non-associative *)
cmp_op      ::= '==' | '!=' | '<' | '>' | '<=' | '>='
concat_expr ::= add_expr ('++' add_expr)*              (* right-associative *)
add_expr    ::= mul_expr (('+' | '-') mul_expr)*
mul_expr    ::= unary_expr (('*' | '/' | '//' | '%') unary_expr)*
unary_expr  ::= ('-' | 'not' | '@') unary_expr
              | pow_expr
pow_expr    ::= postfix_expr ('^' postfix_expr)?       (* right-associative *)
postfix_expr ::= primary postfix*
postfix     ::= '.' IDENT                              (* field / method *)
              | '(' arg_list? ')' trailing_lambda?
                                  trailing_lambda?     (* call *)
              | '[' expr ']'                           (* index *)
              | '?'                                    (* postfix Option/Result *)
              | '!'                                    (* postfix unwrap *)
              | trailing_lambda                        (* paren-less call *)
              | trailing_lambda trailing_lambda
arg_list    ::= arg (',' arg)*
arg         ::= expr
              | '_'                                    (* placeholder *)
trailing_lambda ::= '{' (lambda_params '->')? block_body '}'
lambda_params ::= IDENT (',' IDENT)*
block_body  ::= (stmt stmt_sep)* expr?
```

#### Primary expressions

```
primary     ::= literal
              | IDENT                                  (* var / ctor *)
              | '_'                                    (* lambda placeholder *)
              | qualified_call
              | record_lit
              | list_lit
              | range_lit
              | tuple_or_paren
              | lambda_expr
              | block                                   (* block expression *)
              | if_expr
              | match_expr
              | handle_expr
              | hole_expr
              | 'todo!' ( '(' STRING_LIT ')' )?
              | '@' IDENT                              (* capability read *)
literal     ::= INT_LIT | REAL_LIT | COMPLEX_LIT
              | BOOL_LIT | CHAR_LIT | STRING_LIT
              | UOM_LIT  | REGEX_LIT
qualified_call ::= module_path '.' IDENT
hole_expr   ::= HOLE_LIT | HOLE_NAME_LIT
```

#### Lambda forms

```
lambda_expr ::= IDENT '=>' expr                        (* unary arrow *)
              | '(' params? ')' '=>' expr              (* multi-arg arrow *)
              | '{' lambda_params? '->' block_body '}'  (* lambda block *)
```

The lambda-block form `{ x -> ... }` is also produced by the
trailing-lambda postfix on a call. As a standalone primary, it is
disambiguated from a plain `block` by peeking past the `{`: an
identifier-list followed by `->` opens a lambda block; anything else
starts a block expression.

#### Record literals

```
record_lit  ::= IDENT ('{' field_init_list? '}')
field_init_list ::= named_inits | pun_inits | positional_inits
named_inits ::= named_init (',' named_init)* ','?
named_init  ::= IDENT ':' expr
pun_inits   ::= IDENT (',' IDENT)* ','?
positional_inits ::= expr (',' expr)*                  (* issue #266 *)
```

The parser inspects the first element after `T {`:

- `IDENT ':'`        — named-field literal.
- `IDENT (, | })`    — bare-ident punning (`{ x, y }` ≡ `{ x: x, y: y }`).
- otherwise          — positional list. A pre-typer pass rewrites
                       sentinels `__pos_<i>__` into the real field
                       names in declaration order.

Mixed positional-and-named is rejected at parse time.

#### Lists and ranges

```
list_lit    ::= '[' list_body? ']'
list_body   ::= range_body
              | spread_or_expr (',' spread_or_expr)*
range_body  ::= expr '..' expr ('..' expr)?            (* [start..end..step] *)
spread_or_expr ::= '...' expr | expr
```

#### Tuples / parens

```
tuple_or_paren ::= '(' ')'                             (* unit *)
                 | '(' expr ')'                        (* grouping *)
                 | '(' expr (',' expr){1,3} ')'        (* n-tuple, arity 2..4 *)
```

Single-paren `(e)` is grouping, never a 1-tuple. `()` is the unit
expression / zero-arg lambda head.

#### `if` and `match`

```
if_expr     ::= 'if' expr block ('else' if_expr | 'else' block)?
match_expr  ::= 'match' expr (',' expr)* '{' match_arm+ '}'
match_arm   ::= pattern_n ('if' expr)? '->' (expr | block)
pattern_n   ::= pattern (',' pattern)*                 (* matches each scrutinee *)
```

Multi-scrutinee match `match a, b { p1, p2 -> ... }` projects multiple
expressions onto a tuple of patterns per arm.

#### `handle` (effect handler)

```
handle_expr ::= 'handle' block 'with' IDENT ('as' IDENT)? '{'
                  (effect_clause | return_clause)*
                '}'
effect_clause ::= IDENT '(' clause_params ')' '->' (expr | block)
return_clause ::= 'return' '(' IDENT ')' '->' (expr | block)
clause_params ::= (IDENT (',' IDENT)*)? ',' 'resume'   (* resume always last *)
              | 'resume'
```

`handle` is a control-flow construct (in the family of `if`/`match`),
not a function call. Trailing-lambda sugar does not apply to it.

### 2.7 Patterns

```
pattern     ::= '_'                                    (* wildcard *)
              | literal_pattern
              | IDENT
              | IDENT '@' pattern                      (* @-bind *)
              | qualified_ctor type_args?
                  ('(' pattern_list ')')?              (* ctor pattern *)
              | qualified_ctor '{' field_pat_list '}'  (* record/variant pattern *)
              | '[' (pattern (',' pattern)* (',' list_spread)?)? ']'
              | '(' pattern (',' pattern){1,3} ')'     (* n-tuple pattern *)
              | INT_LIT '..' INT_LIT                   (* range pattern *)
qualified_ctor ::= IDENT
                 | module_path '.' IDENT
literal_pattern ::= INT_LIT | REAL_LIT | BOOL_LIT
                  | CHAR_LIT | STRING_LIT
pattern_list ::= pattern (',' pattern)*
field_pat_list ::= field_pat (',' field_pat)* ','?
field_pat   ::= IDENT (':' pattern)?                   (* punning if no ':' *)
list_spread ::= '...'                                  (* tail discarded — preferred *)
              | '...' '_'                              (* tail discarded — uniform-with-`_` *)
              | '...' IDENT                            (* tail bound to name *)
```

The bare `...` is the preferred spelling for "match any list with this
prefix, tail unused"; `..._` and `...IDENT` remain accepted for the
"uniform-with-`_`" reading and for binding the tail respectively
(issue #328).

### 2.8 Refinements

`m12.6.x` introduces `where` clauses and `requires` / `ensures`
annotations. They attach to function declarations and to `match` arms.

```
fn_decl     ::= ... fn_body refine_block?
match_arm   ::= ... refine_block?
refine_block ::= 'where' '{' refine_clause* '}'
refine_clause ::= 'requires' expr
              | 'ensures' expr
              | IDENT ':' type 'where' expr           (* per-binding refine *)
```

Refinement expressions are ordinary expressions restricted to the
refinement-pure predicate set (see `docs/refinements.md`).

## §3 — Operator precedence and associativity

From tightest (binds first) to loosest (binds last). Each level
resolves against the level above it.

| Level | Operators                                     | Associativity        | Parser fn          |
|------:|-----------------------------------------------|----------------------|--------------------|
|     1 | call `f(args)`, field `.`, index `[i]`, postfix `?` / `!`, trailing-lambda | Postfix              | `parse_postfix`    |
|     2 | `^` (power)                                   | Right                | `parse_pow`        |
|     3 | unary `-`, `not`, `@`                         | Prefix               | `parse_unary`      |
|     4 | `*`, `/`, `//`, `%`                           | Left                 | `parse_mul`        |
|     5 | `+`, `-` (binary)                             | Left                 | `parse_add`        |
|     6 | `++` (concat)                                 | Right                | `parse_concat`     |
|     7 | `==`, `!=`, `<`, `>`, `<=`, `>=`              | **Non-associative**  | `parse_cmp`        |
|     8 | `and`                                         | Left (short-circuit) | `parse_and`        |
|     9 | `or`                                          | Left (short-circuit) | `parse_or`         |
|    10 | `\|>`, `\|`, `\|\|`                            | Left                 | `parse_pipe`       |

Notes:

- Comparison is **non-associative**: `a < b < c` is a syntax error.
  Use `a < b and b < c`.
- Pipes are at the bottom: `a + b |> f` parses as `f(a + b)`.
- Postfix chains associate left: `a.b.c` ≡ `(a.b).c`,
  `f(x)(y)` ≡ `(f(x))(y)`.
- `^` (power) appears only at expression level when the base is
  numeric. Inside UoM literals / types `^` is the unit-power token
  with its own grammar (see §1.6 *UoM*). Outside both contexts
  `CARET` is rejected by the lexer's parse stage.
- `:=` is a **statement** form, not an operator. It does not appear
  in the precedence table.

## §4 — Sugars and their desugars

Each sugar lists the source form and the equivalent post-desugar AST
shape. The authoritative spec for intent and editorial rules is
`docs/syntax-sugars.md`; this section only summarises the grammar
delta and the desugar target.

### 4.1 N-tuples (issue #154)

```
(a, b)              ≡  Pair { fst: a, snd: b }
(a, b, c)           ≡  Triple { fst: a, snd: b, trd: c }
(a, b, c, d)        ≡  Quad { fst: a, snd: b, trd: c, frt: d }

(A, B)              ≡  Pair[A, B]                       (* type *)
(A, B, C)           ≡  Triple[A, B, C]
(A, B, C, D)        ≡  Quad[A, B, C, D]

let (a, b) = pair   ≡  let Pair { fst: a, snd: b } = pair  (* pattern *)
```

Cap arity 2..4. `(e)` is grouping, never a 1-tuple. `()` is unit.

### 4.2 Trailing lambdas (issue #244)

```
f(args) { body }       ≡  f(args, () => body)
f(args) { x -> body }  ≡  f(args, (x) => body)
f { body }             ≡  f(() => body)             (* paren-less *)
```

Same-line attachment only: a `NEWLINE` between the call and `{`
terminates the call; the `{` then starts a block expression.

### 4.3 Double trailing lambdas (issue #248)

```
f(a) { body1 } { body2 }  ≡  f(a, () => body1, () => body2)
```

Both braces must be on the same logical line as the call (or its
preceding `}` continuation).

### 4.4 Pipe forms

```
xs |> f                ≡  f(xs)                     (* apply *)
xs |> f(extra)         ≡  f(xs, extra)
xs | f                 ≡  map(xs, f)                (* map *)
xs || f                ≡  flat_map(xs, f)           (* flat-map *)
```

### 4.5 Lambda block as expression (issue #244)

```
let g = { x -> x * 2 }       ≡  let g = (x) => x * 2
let h = { -> 42 }            ≡  let h = () => 42
```

Disambiguated from a plain block by peeking past `{` for a parameter
list followed by `->`.

### 4.6 UFCS (issue #205)

```
r.f(args)               ≡  f(r, args)               (* when f is a free fn *)
```

Resolved by the head-module-of-`r` rule: if `r` has type `T` declared
in module `M`, `r.f(args)` rewrites to `M.f(r, args)` when `M.f`
exists.

### 4.7 Record punning (issue #266 family)

```
T { x, y }              ≡  T { x: x, y: y }         (* identifier punning *)
T { v1, v2 }            ≡  T { f1: v1, f2: v2 }     (* positional, #266 *)
```

Selected by first-element peek (see §2.6 *Record literals*).

### 4.8 Complex literal (issue #267 phase 1)

```
3i                      ≡  complex.mk(0.0, 3.0)
2.5i                    ≡  complex.mk(0.0, 2.5)
```

Lexer-only: `<digits>i` with no whitespace is one `COMPLEX_LIT` token.

### 4.9 Capability read / write (issue #275)

```
@cap                    ≡  Cap.get()
cap := v                ≡  Cap.set(v)
a[i] := v               ≡  Mutable.array_set(a, i, v)  (* indexed write *)
a[i]                    ≡  Mutable.array_get(a, i)     (* indexed read *)
var x = init            ≡  handle { ... } with State[T](init) { ... }
```

The `@` prefix and `:=` are exclusively for capability reads/writes;
`Ref[T]` uses ordinary `r.get()` / `r.set(v)`.

### 4.10 `unit` keyword (m12.5)

```
unit Newton = kg * m / sec^2
```

Introduces a Measure-kind symbol; has no value-level desugar but
extends the unit algebra accepted inside `<...>`.

### 4.11 `#derive` (m12.8)

```
#derive(Show, Eq) type Point = { x: Int, y: Int }
```

Expands at the type-decl boundary to synthetic `impl Show for Point`
and `impl Eq for Point` with the per-protocol auto-derivation rule
described in `docs/protocols.md` §*Auto-derivation*.

## §5 — Ambiguity resolution

The grammar is LL(1) with three documented bookkeeping rules. Each
rule below is checked by `compiler.kai`'s parser at the point named.

### 5.1 Trailing-lambda vs block expression

When a `{` follows a callable, attach as a `trailing_lambda` only
when the `{` is on the **same line** as the callable. A `NEWLINE`
between the previous token and `{` terminates the call; the `{` then
starts a new statement whose first token is `{` (a block expression).
Implemented by a one-bit "saw newline since previous token" flag
maintained by the lexer-to-parser bridge. Same rule extends to the
second trailing lambda (§4.3).

### 5.2 Lambda-block vs block expression

In any expression position where `{` may begin either a
parameterised lambda block (`{ x -> ... }`) or a plain block
expression (`{ stmt; expr }`), the parser peeks past the `{`:

- `{` `->`              → zero-arity lambda block.
- `{` IDENT (`,` IDENT)* `->`  → parameterised lambda block.
- otherwise             → block expression.

This is bounded lookahead — one identifier-list scan terminating at
`->` or any non-`,`/non-`IDENT` token. Implemented as a checkpoint
on the lexer with rewind on miss (`parse_lambda_block_body`).

### 5.3 `EField(EVar(n), m)` — projection vs module call vs UFCS

`a.b(args)` has three readings; the resolver (not the parser) picks
between them in the order:

1. If `a` is bound as a module alias (`import math as a`), parse as
   `EModCall(a, b, args)`.
2. Else, if `a` resolves to a value of type `T` declared in module
   `M` and `M.b` exists, parse as UFCS: `EModCall(M, b, [a, ...args])`.
3. Else, parse as field projection followed by call:
   `ECall(EField(a, b), args)`.

The parser produces the syntactic form `ECall(EField(EVar("a"), "b"), args)`
unconditionally; resolution rewrites later.

### 5.4 `a < b > c` — comparison vs generic instantiation vs UoM

Three readings disambiguated by **position**:

- In **type position** after an identifier: `IDENT '<' ...` opens
  type arguments (`Map<K, V>` does not occur in kaikai — generics use
  `[]` — but `Real<m>` is a UoM literal/type because `Real` is a
  primitive type and `m` is a unit identifier).
- In **expression position** after a numeric literal: `INT_LIT '<' ...`
  opens a UoM literal, recognised by the lexer/parser pair
  (`100<USD>`).
- In **expression position** after any other expression: `<` is the
  comparison operator (level 7).

Comparison is non-associative, so `a < b < c` cannot occur; this
removes the residual ambiguity that other languages must resolve at
parse time.

### 5.5 Record literal vs block expression

`IDENT '{' ... '}'` is a record literal when:

- The IDENT is `PascalCase` (a type-constructor name), and
- The `{` is on the same line as the IDENT, and
- The contents are field-init / pun / positional shapes that pass the
  first-element peek (§2.6).

Otherwise the `{` opens a block. The same-line rule prevents

```
let f = SomeFn
{ ... }       # this is a block, not a record literal
```

from accidentally parsing `SomeFn { ... }`.

### 5.6 `:=` and indexed write

`IDENT ':=' expr` and `index_lhs '[' expr ']' ':=' expr` attach
unambiguously because `COLON_EQ` is a distinct token that no other
production starts with. Indexed read (`a[i]`) goes through the
ordinary postfix `[expr]` and stays unchanged.

## §6 — Reserved tokens (post-MVP)

The following tokens are reserved by the lexer / parser and are not
yet part of any production. Using them outside the contexts described
below is a parse error.

- `&`, `&mut` — reserved for a possible future borrow-style analysis.
  Not produced by the current lexer; mentioned here so external
  tooling does not assume them as identifier-class.
- `^` outside UoM annotations and the expression-level power operator
  is a parse error. The parser only accepts `^` in two contexts:
  inside `<...>` unit expressions and in numeric `pow_expr` (§3
  level 2).
- `0o` (octal prefix) — not in the lexer. Hex (`0x`) covers the same
  use cases.
- Underscore digit separators in hex / binary literals (`0xFF_FF`) —
  parse error; reserved for a possible follow-up.

The keywords below are reserved but currently have no productions
besides those described in §2:

- `for`  — reserved for a possible future iteration form; today only
  appears inside `impl ... for ...` declarations.
- `as`   — appears in `import ... as ...` and `handle ... with E as
  cap` only.
- `with` — appears in `handle ... with ...` only.

## Cross-references

- `docs/kaikai-minimal.md` — stage 0 grammar, a strict subset of this
  document.
- `docs/syntax-sugars.md` — authoritative spec of each sugar's intent
  and editorial rules; this document carries only the grammar delta.
- `docs/effects.md`, `docs/effects-stdlib.md` — semantics of effect
  rows; this document covers only the row syntax.
- `docs/protocols.md` — protocol semantics; this document covers
  `protocol` / `impl` shapes only.
- `docs/units-of-measure.md` — UoM kind system; this document covers
  the literal and type-position syntax only.
- `docs/refinements.md` — refinement semantics; this document covers
  the `where` / `requires` / `ensures` shapes only.
- `stage2/compiler.kai` — operational source of truth. When in
  doubt, the parser wins.
