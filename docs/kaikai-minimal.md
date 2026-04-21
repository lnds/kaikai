# kaikai-minimal

The austere subset of kaikai compiled by **stage 0** (the C bootstrap compiler).

Goal: be just expressive enough to write a compiler for the full language in itself, and no more. Simplicity over power. Every feature here must earn its place by being necessary to build stage 1.

## What is **not** in kaikai-minimal

To make the scope explicit:

- No full effects system — only an implicit `Io` is available via a handful of built-in functions.
- No type inference beyond local propagation (the type of a `let` comes from its RHS; nothing else is inferred).
- No generics beyond a fixed set (`Option[T]`, `Result[E, T]`, `List[T]` — that's it).
- No fibers, no actors, no concurrency.
- No `|` pipe (map). Stage 0 only supports `|>` (apply).
- No `with` record update. Stage 0 builds records only via full-literal construction.
- No tuples (use records; or use sum-type variants with positional fields).
- No full Perceus — a simple reference counter is used.
- No string format beyond interpolation. No regex.

These all appear in **full kaikai** (the language stage 1 compiles), but stage 0 does not need them.

## Lexical structure

### Comments

```kai
# Single line comment. No multi-line comments in minimal.
```

### Identifiers

- `snake_case` for functions, variables, and fields: `read_file`, `x`, `employee_name`.
- `PascalCase` for types and sum-type constructors: `Int`, `Point`, `Some`, `Lit`.
- No other cases are accepted. The formatter enforces this.

### Keywords

```
and       as        assert    else      false
fn        if        import    let       match
not       or        pub       test      true
type
```

### Literals

```kai
# Integers
42    -7    0    1_000_000

# Reals
3.14    -0.5    1e10    2.5e-3

# Booleans
true    false

# Unit
()

# Chars
'a'    '\n'    '\t'    '\\'    '\''    '\u{2603}'

# Strings — double quotes, with #{expr} interpolation
"hello"
"Hello, #{name}!"
"line one\nline two"

# Triple-quote string (multi-line, preserves indentation relative to closing """)
"""
  First line
  Second line, still inside #{name}'s message
  """
```

Escape sequences in strings and chars: `\n`, `\t`, `\r`, `\"`, `\'`, `\\`, `\u{HHHH}`.

### Operators and punctuation

```
+  -  *  /  //  %          # arithmetic; // is integer division
==  !=  <  >  <=  >=       # comparison
and  or  not               # boolean (keywords, not symbols)
|>                         # pipe (apply)
=>                         # lambda arrow
->                         # function type arrow
=                          # binding / expression body
:                          # type annotation
,                          # separator
.                          # field access / placeholder
...                        # spread
..                         # range
{ } [ ] ( )                # grouping
```

Note: `!` is reserved (post-minimal uses for `Option`/`Result` propagation). It has no meaning in minimal. `|` is reserved for sum-type variants in type declarations only.

## Program structure

### Files and modules

One file = one module. The module name is derived from the file path relative to the project root:

- `math/vector.kai` → module `math.vector`.
- `main.kai` → module `main`.

There is no `module Foo` declaration.

### Visibility

Declarations are private by default. Prefix with `pub` to export:

```kai
pub fn api_function(x: Int) : Int = x * 2
fn internal_helper(x: Int) : Int = x + 1
pub type Point = { x: Int, y: Int }
type InternalState = { counter: Int }
```

### Imports

```kai
import math.vector                      # use qualified: vector.dot(a, b)
import math.vector as V                 # alias: V.dot(a, b)
import math.vector.{dot, cross}         # bring specific names into scope
```

No wildcard import.

### Entry point

The file passed to stage 0 must declare `fn main()`. Stage 0 compiles it as the program entry.

Full kaikai extends this with a `main.kai` convention and script mode (`kai run foo.kai`), but minimal only cares that a `main` exists.

## Declarations

### Functions

Two body forms, canonical for each case:

```kai
# Expression body — use `=`
fn square(x: Int) : Int = x * x
fn abs(x: Int) : Int = if x < 0 { -x } else { x }

# Block body — no `=`, last expression is the return value
fn classify(n: Int) : String {
  let abs_n = if n < 0 { -n } else { n }
  if abs_n == 0 { "zero" }
  else if abs_n < 10 { "small" }
  else { "big" }
}

# Unit return can be omitted from the signature
fn greet(name: String) {
  print("Hello, #{name}")
}

# `main` is always Unit-returning, signature is conventional
fn main() {
  print("Hello, kaikai")
}
```

Return-type annotations are mandatory except on `main`.

### Let bindings

```kai
let x = 42                        # type inferred from RHS: Int
let name: String = "kaikai"       # explicit annotation, optional
let sum = a + b                   # tipo from a + b
```

Bindings are immutable. There is no `var` in minimal.

### Type declarations

One keyword: `type`. Three shapes.

```kai
# Alias
type Age = Int
type Name = String

# Record (product)
type Point = { x: Int, y: Int }
type Employee = { name: String, age: Int, budget: Int }

# Sum type (tagged union) — constructors with positional fields
type Option[a] = Some(a) | None
type Result[e, a] = Err(e) | Ok(a)
type Expr = Lit(Int) | Var(String) | Add(Expr, Expr) | Mul(Expr, Expr)
```

Constructor names must be unique within their module.

## Expressions

Everything in a function body is an expression (except `let` bindings, which are statements that establish a name in scope).

### Blocks

`{ stmt; stmt; ...; final_expression }`. The last expression's value is the block's value. Statements are separated by newlines (or semicolons — both accepted; newlines are canonical).

### `if`

Always an expression. `else` is mandatory when the `if` produces a value; optional when it is `Unit`.

```kai
let abs = if x < 0 { -x } else { x }

# else-if chain
let label =
  if x == 0 { "zero" }
  else if x < 0 { "negative" }
  else { "positive" }

# Unit branch, else optional
if done { print("finished") }
```

### `match`

Pattern matching. No `case` keyword, no leading `|`.

```kai
match shape {
  Circle(r) -> pi * r * r
  Rect(w, h) -> w * h
  Triangle(b, h) -> b * h / 2
}

match xs {
  [] -> 0
  [head, ...tail] -> head + sum_of(tail)
}

match n {
  0 -> "zero"
  k if k > 0 -> "positive"
  _ -> "negative"
}
```

The compiler requires exhaustiveness (every inhabitant of the scrutinee's type must be matched) or a wildcard `_`.

### Lambdas

Two forms. Both yield the same thing semantically; they differ only in what fits syntactically.

```kai
# Named-argument arrow — for non-trivial bodies or multiple arguments
let square = x => x * x
let add = (a, b) => a + b
xs |> reduce(0, (acc, x) => acc + x)

# Placeholder — for trivial unary expressions only
xs |> filter(. < 5)
xs |> filter(. % 2 == 0)
xs |> map(.name)            # field access on the placeholder
```

Placeholder rules:

- `.` inside an expression passed where a function is expected refers to the current argument.
- Multiple `.` occurrences in the same expression refer to the **same** value (unary).
- `.` outside of lambda-expected context is a compile error.
- For multiple arguments or nested scopes, use named arrows.

### Pipes

Two operators with distinct semantics.

**`|>` — apply (Elixir-style)**: the left side becomes the first argument of the call on the right.

```kai
xs |> sum                       # sum(xs)
xs |> filter(. < 5)             # filter(xs, . < 5)
xs |> map(. * 2) |> sum         # sum(map(xs, . * 2))
```

In kaikai-minimal, `|>` is the only pipe. The map-style `|` operator arrives in full kaikai.

### Records: construction, access, destructuring

```kai
# Construction
let p = Point { x: 3, y: 4 }

# Field access
let x = p.x

# Destructuring in let
let Point { x, y } = p          # x = 3, y = 4

# Destructuring in match
match p {
  Point { x: 0, y: 0 } -> "origin"
  Point { x, y } -> "point(#{x}, #{y})"
}
```

Record update (`{ p with x = 10 }`) is **not** in minimal. Stage 1 introduces it.

### Lists and ranges

```kai
let xs = [1, 2, 3]
let ys = [1.0, 2.0, 3.0]
let empty = []

# Range literals (syntactic sugar producing a List)
let r1 = [1..10]        # [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]  inclusive
let r2 = [1..10..2]     # [1, 3, 5, 7, 9]                   with step
let r3 = [10..1..-1]    # [10, 9, ..., 1]                   descending

# Spread in construction
let zs = [0, ...xs, 99]

# Destructuring
let [h, ...t] = xs
match xs {
  [] -> "empty"
  [only] -> "one: #{only}"
  [first, second, ...rest] -> "at least two"
}

# Indexed access
let first = xs[0]
```

Lists are immutable. Cons is spelled with spread (`[h, ...t]`), never with `|`.

### String interpolation

```kai
let msg = "Hello, #{name}. You are #{age} years old."
```

Any expression fits inside `#{...}`. Its value is converted to `String` via a built-in conversion (each primitive type has one).

## Types

### Primitive types

`Int`, `Real`, `Bool`, `String`, `Char`, `Unit`.

### List type

`[T]` is the type of lists of `T`: `[Int]`, `[Point]`, `[[String]]` (list of lists).

### Function types

`(T1, T2) -> T3` is the type of a function from `T1, T2` to `T3`. Zero args: `() -> T`.

### User-defined types

As declared with `type` — aliases, records, sum types. Generic parameters are written `Option[Int]`, `Result[String, Int]`.

### Type annotations

Mandatory on:
- Function parameters.
- Function return types (except `main`).
- Public declarations.

Optional on `let` bindings (the RHS type is used).

## Built-in functions (minimal prelude)

These are provided by stage 0 directly; they cannot be redefined.

```kai
# IO
print(s: String) -> Unit              # write to stdout with a newline
eprint(s: String) -> Unit             # write to stderr with a newline
read_file(path: String) -> Result[String, String]
write_file(path: String, content: String) -> Result[Unit, String]
read_line() -> Result[String, String]

# Numeric conversions
int_to_string(n: Int) -> String
real_to_string(r: Real) -> String
string_to_int(s: String) -> Option[Int]
string_to_real(s: String) -> Option[Real]

# String operations
string_length(s: String) -> Int
string_concat(a: String, b: String) -> String
string_split(s: String, sep: String) -> [String]
string_contains(s: String, substr: String) -> Bool
char_at(s: String, i: Int) -> Option[Char]

# List operations — minimal set; full library is in stdlib for stage 1
list_length(xs: [a]) -> Int
list_append(xs: [a], ys: [a]) -> [a]
list_reverse(xs: [a]) -> [a]

# Program termination
exit(code: Int) -> a                  # never returns
panic(msg: String) -> a               # never returns, aborts with message
```

Higher-order operations like `map`, `filter`, `reduce` are provided as ordinary functions in the minimal prelude once stage 0 can parse them:

```kai
map(xs: [a], f: (a) -> b) -> [b]
filter(xs: [a], p: (a) -> Bool) -> [a]
reduce(xs: [a], init: b, f: (b, a) -> b) -> b
each(xs: [a], f: (a) -> Unit) -> Unit    # apply f for its effects; discards results
```

## Tests

Inline in the same file as the code under test.

```kai
pub fn square(x: Int) : Int = x * x

test "square of zero is zero" {
  assert square(0) == 0
}

test "square is non-negative" {
  assert square(-5) == 25
  assert square(7) == 49
}
```

`assert cond` or `assert cond, "message"`. Tests are excluded from non-test builds. `kai test` (or `stage0 --test file.kai` for minimal) runs all `test` blocks.

## Examples

### Hello, kaikai

```kai
fn main() {
  print("Hello, kaikai")
}
```

### Fizzbuzz

```kai
fn fizzbuzz(n: Int) : String {
  if n % 15 == 0 { "FizzBuzz" }
  else if n % 3 == 0 { "Fizz" }
  else if n % 5 == 0 { "Buzz" }
  else { int_to_string(n) }
}

fn main() {
  [1..100] |> map(fizzbuzz) |> each(print)
}

test "fizzbuzz boundary cases" {
  assert fizzbuzz(15) == "FizzBuzz"
  assert fizzbuzz(3) == "Fizz"
  assert fizzbuzz(5) == "Buzz"
  assert fizzbuzz(7) == "7"
}
```

### Quicksort

```kai
pub fn sort(xs: [Int]) : [Int] {
  match xs {
    [] -> []
    [pivot, ...rest] -> {
      let smaller = rest |> filter(. < pivot)
      let larger  = rest |> filter(. >= pivot)
      list_append(sort(smaller), list_append([pivot], sort(larger)))
    }
  }
}

test "sort works on small inputs" {
  assert sort([]) == []
  assert sort([3, 1, 4, 1, 5, 9, 2, 6, 5]) == [1, 1, 2, 3, 4, 5, 5, 6, 9]
}
```

### Tiny expression interpreter (dogfooding target)

Illustrates sum types for AST, pattern matching, recursion — the shape of a compiler written in kaikai-minimal.

```kai
type Expr
  = Lit(Int)
  | Add(Expr, Expr)
  | Mul(Expr, Expr)

pub fn eval(e: Expr) : Int {
  match e {
    Lit(n) -> n
    Add(l, r) -> eval(l) + eval(r)
    Mul(l, r) -> eval(l) * eval(r)
  }
}

test "interpreter evaluates basic arithmetic" {
  let e = Add(Lit(2), Mul(Lit(3), Lit(4)))
  assert eval(e) == 14
}
```

---

## Newlines and statement termination

kaikai is newline-sensitive but avoids requiring explicit semicolons in idiomatic code.

Rules:

- A newline **terminates** a statement when the preceding line is syntactically complete at that position.
- A newline is **whitespace** (does not terminate) when the parser is still expecting more — specifically when:
  - There is an unclosed `(`, `[`, or `{`.
  - The last token is a binary operator (`+`, `-`, `*`, `/`, `//`, `%`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `and`, `or`, `|>`, `|`).
  - The last token is an assignment or arrow (`=`, `->`, `=>`).
  - The last token is a separator inside a composite literal or parameter list (`,`, `:`).
- A semicolon `;` is always a valid statement terminator. Use it only when putting multiple statements on the same physical line, which is uncommon.

Multi-line constructs this enables:

```kai
# Pipe across lines
xs
  |> filter(. > 0)
  |> map(. * 2)
  |> sum

# If expression laid out vertically
let label =
  if x == 0 { "zero" }
  else if x < 0 { "negative" }
  else { "positive" }

# Type declaration with variants per line
type Expr
  = Lit(Int)
  | Add(Expr, Expr)
  | Mul(Expr, Expr)

# Record literal with trailing commas for vertical layout
let p = Point {
  x: 3,
  y: 4,
}
```

Style note: do not rely on `;` to cram statements; prefer one statement per line.

---

## Grammar (informal EBNF)

```
Program     ::= Import* Decl*

Import      ::= "import" ModulePath ("as" Ident | "." "{" IdentList "}")?
ModulePath  ::= Ident ("." Ident)*
IdentList   ::= Ident ("," Ident)*

Decl        ::= FnDecl | TypeDecl | TestDecl
FnDecl      ::= "pub"? "fn" Ident "(" ParamList? ")" (":" Type)? FnBody
FnBody      ::= "=" Expr | Block
ParamList   ::= Param ("," Param)*
Param       ::= Ident ":" Type

TypeDecl    ::= "pub"? "type" Ident TypeParams? "=" TypeBody
TypeParams  ::= "[" Ident ("," Ident)* "]"
TypeBody    ::= Type                                   (* alias *)
              | "{" FieldList "}"                      (* record *)
              | Variant ("|" Variant)*                 (* sum *)
Variant     ::= ConstructorName ("(" TypeList ")")?
FieldList   ::= (Ident ":" Type ("," Ident ":" Type)*)?

TestDecl    ::= "test" StringLit Block

Type        ::= Ident TypeArgs?
              | "[" Type "]"
              | "(" TypeList? ")" "->" Type

Block       ::= "{" Stmt* Expr "}"
Stmt        ::= "let" Pattern (":" Type)? "=" Expr
              | Expr

Expr        ::= Literal
              | Ident
              | Expr "(" ArgList? ")"                  (* call *)
              | Expr "." Ident                         (* field access *)
              | Expr "[" Expr "]"                      (* index *)
              | Expr BinOp Expr
              | UnaryOp Expr
              | IfExpr
              | MatchExpr
              | Lambda
              | RecordLit
              | ListLit
              | PipeExpr
              | Block
              | "(" Expr ")"
              | "."                                    (* placeholder *)

IfExpr      ::= "if" Expr Block ("else" "if" Expr Block)* ("else" Block)?
MatchExpr   ::= "match" Expr "{" MatchArm+ "}"
MatchArm    ::= Pattern ("if" Expr)? "->" (Expr | Block)
Lambda      ::= Ident "=>" Expr
              | "(" IdentList? ")" "=>" Expr
RecordLit   ::= TypeName "{" FieldInitList? "}"
ListLit     ::= "[" (ListBody)? "]"
ListBody    ::= Expr ("," Expr)*                       (* normal *)
              | Expr ".." Expr (".." Expr)?            (* range *)
              | SpreadOrExpr ("," SpreadOrExpr)*
SpreadOrExpr ::= "..." Expr | Expr
PipeExpr    ::= Expr "|>" Expr

Pattern     ::= "_"
              | Literal
              | Ident
              | "[" (Pattern ("," Pattern)* ("," "..." Ident)?)? "]"
              | TypeName "(" (Pattern ("," Pattern)*)? ")"
              | TypeName "{" (Ident (":" Pattern)?)* "}"
```

The grammar is LL(1) with minor bookkeeping. Stage 0's parser is a hand-written recursive descent — no parser generator.
