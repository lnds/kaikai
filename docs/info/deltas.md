# deltas

Where kaikai deliberately differs from Rust/Go/Python/JS/Haskell
habits — one cheap read that prevents the classic prior collisions.

## The card

Each line is a habit from another language and the kaikai form.
Every entry is verified against the compiler; when in doubt,
`kai info syntax` (its NOT IN KAIKAI section) is ground truth.

- Comments are `#`, not `//`. Doc comments are `#[doc("...")]` above
  the declaration, not `///`.
- `Int / Int` truncates toward zero (`7 / 2 == 3`, `-7 / 2 == -3`);
  it never promotes to `Real`. `%` is the remainder.
- No `i++` / `i--`. `++` exists but it is CONCAT — `[1] ++ [2]`,
  `"a" ++ "b"` — not increment.
- `let` never reassigns. Mutation is a cell: `var i := 0`, then
  `i := i + 1` — the assignment operator is `:=`, not `=`.
- `"a" + "b"` is a type error. Build strings with interpolation
  (`"#{expr}"`) or `++`.
- Lambdas are `(x) => x + 1`. Not `\x -> ...`, not `fn x -> ...`,
  not `|x| ...`.
- No `return` — the last expression of a block is the value.
- No `null` / `nil` / `undefined` — `Option[T]` (`Some(x)` / `None`).
- Option default is the infix `o ?? d` (Swift/JS `??`, Rust `unwrap_or` /
  `unwrap_or_else`): lazy, `Option` only. A `Result` needs `ok(r) ?? d`.
- No `throw` / `try` / `catch` — use `Result[a, e]` with the `expr!`
  propagation postfix, or an effect whose op returns `Nothing`.
- No `for x in xs`, no C-style `for (;;)`, no `break` / `continue`.
  Iterate with pipes — `xs |! f` for an effect, `xs | (x => ...)` for a
  new list. The `for` keyword appears only in `impl Proto for T`.
- Drop a result on purpose. A non-`Unit` value used as a
  statement is a `kai lint` warning; close it with `|> ignore` or bind
  it with `let _ =`.
- A list always ends. A range with no end is a `Seq`, written
  `lazy [0..]`; a bare `[0..]` is an error.
- No list comprehensions: `[x*2 for x in xs]` → `xs | (x => x * 2)`.
- The cons pattern is `[h, ...t]` — not `x:xs`, not `[h|t]`.
- Generics use brackets — `Foo[T]`, `[Int]` — never `Foo<T>`. Angle
  brackets belong to units of measure (`Real<m>`).
- `|` between values is the MAP pipe, not bitwise-or. There are no
  bitwise operators on `Int`; `import math.bits` gives `bit_and`,
  `bit_or`, `bit_shl`, ….
- Attributes are `#[derive(Eq, Show)]` / `#[doc(...)]` — not
  `#derive(...)` — and they come BEFORE `pub`.
- No multi-clause function heads (`fn fib(0) = 0`). Use one
  clause-block body: `fn fib(n: Int) : Int { case 0 -> 0 ... }`.
  Its guard keyword is `when`; a plain `match` guard is `if`.
- No type classes, interfaces, or traits — single-dispatch
  `protocol P { ... }` + `impl P for T`.
- No `async` / `await` — structured concurrency is the `Spawn`
  effect (`kai info fibers`).
- No `self` / `this` receiver and no methods. Functions take explicit
  parameters; `xs.map(f)` does not exist — write `xs | f` or
  `xs |> map(f)`.
- No module header (`mod` / `package` / `module`). The file path is
  the package; bring code in with `import loop`.
- No `do { ... }` blocks, and no Haskell-style `where` clause for
  trailing local bindings — use a block body with `let`. (`where`
  *does* exist, but for refinement predicates: `Int where self >= 0`.)
- Preconditions and postconditions are `requires` / `ensures` on the
  function, and value constraints are `type T = Base where P`
  refinement types — both checked, both panic-on-violation. Not
  assertions, not comments. See `kai info contracts`.
- Side effects are in the type: printing needs `/ Stdout` in the
  signature. An effectful call inside a pure signature is a type
  error, not a warning.
- How many elements? `length` on the ordered/indexable ones (`list`,
  `string`, `vec`, `string_builder`); `size` on the unordered ones
  whose abstraction is cardinality (`map`, `set`, `hashmap`, `hashset`,
  `queue`, `stack`). Not `len` everywhere: the name tells you whether
  positions exist. `list.count(p)` is count-*if* (how many satisfy `p`),
  and `stream.count` drains a lazy stream — neither is a length.
- Left fold is `foldl` on every collection (`list`, `vec`, `stream`,
  `map`); `list` also has `foldr`. `fold` exists only on `stream` and
  `map`, as a deprecated alias for `foldl` kept until the Orongo
  edition (`kai migrate` rewrites it); `list` and `vec` have no `fold`.
  `reduce` is the list-builtin sibling of `map`/`filter`/`each` and
  stays.

If a form is not in `kai info syntax`, it does not exist — check
there before inventing syntax. To look up one API, use
`kai doc <module>.<symbol>` instead of reading a topic page.

## See also

`kai info syntax` (NOT IN KAIKAI), `kai info llm`,
`kai info idiomatic`
