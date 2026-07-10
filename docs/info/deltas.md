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
- No `throw` / `try` / `catch` — use the `Fail` effect or
  `Result[e, a]` with the `expr!` propagation postfix.
- No `for x in xs`, no C-style `for (;;)`, no `break` / `continue`.
  Iterate with pipes — `xs | (x => ...)` or `xs |> each(f)`. The
  `for` keyword appears only in `impl Proto for T`.
- No list comprehensions: `[x*2 for x in xs]` → `xs | (x => x * 2)`.
- The cons pattern is `[h, ...t]` — not `x:xs`, not `[h|t]`.
- Generics use brackets — `Foo[T]`, `[Int]` — never `Foo<T>`. Angle
  brackets belong to units of measure (`Real<m>`).
- `|` between values is the MAP pipe, not bitwise-or. There are no
  bitwise operators on `Int`; `import math/bits` gives `bit_and`,
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
- No `self` / `this` receiver. Methods take explicit parameters;
  `xs.map(f)` is UFCS sugar for `map(xs, f)`.
- No module header (`mod` / `package` / `module`). The file path is
  the package; bring code in with `import loop`.
- No `do { ... }` blocks, no `where` clauses — use a block body
  with `let`.
- Side effects are in the type: printing needs `/ Stdout` in the
  signature. An effectful call inside a pure signature is a type
  error, not a warning.

If a form is not in `kai info syntax`, it does not exist — check
there before inventing syntax. To look up one API, use
`kai doc <module>.<symbol>` instead of reading a topic page.

## See also

`kai info syntax` (NOT IN KAIKAI), `kai info llm`,
`kai info idiomatic`
