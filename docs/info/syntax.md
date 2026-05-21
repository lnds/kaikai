SYNTAX(7)                       kaikai                       SYNTAX(7)

NAME
  syntax — one-page reference of the forms kaikai actually has

SYNOPSIS
  See FORMS below. Every form on this page is real. Forms NOT on this
  page do not exist in kaikai. See `NOT IN KAIKAI` at the bottom.

DESCRIPTION
  kaikai is statically typed with HM extended by effect rows. Day-to-day
  syntax stays close to Python/Elixir/JS; advanced surface (effects,
  handlers, fibers, holes) is novel by design.

  Comments start with `#`. Files have no module header — the path is
  the package name (see `kai info packages`).

FORMS

  Declarations
    fn name(p1: T1, p2: T2) : R = expr               # short body
    fn name(p1: T1, p2: T2) : R { stmts; expr }      # block body
    fn name(p1: T1, p2: T2) : R / Eff1 + Eff2 = ...  # effect annotation
    pub fn name(...) : R = ...                       # exported
    type Color = Red | Green | Blue                  # sum type
    type Point = { x: Int, y: Int }                  # record type
    effect Logger { log(s: String) : Unit }          # effect decl
    unit C                                           # unit of measure
    extern "C" fn name(p: T) : R                     # FFI

  Bindings
    let x = expr                                     # immutable binding
    let x: T = expr                                  # with annotation
    var x = expr                                     # local mutable cell
                                                     # (sugar for State[T]).
                                                     # Read with @x;
                                                     # write with `x := ...`.
                                                     # Bare `x` is rejected.

  Functions and lambdas
    (x) => body                                      # lambda expression
    (a, b) => body                                   # multi-arg lambda
    .                                                # placeholder lambda
    { x -> body }                                    # block lambda (in
                                                     # trailing position)
    f(a, b) { x -> body }                            # trailing lambda
    f { body }                                       # paren-free single
                                                     # trailing lambda

  Control flow
    if cond { then_block } else { else_block }
    if cond { then_block }                            # else is optional
    match expr {
      Pat1            -> arm1
      Pat2 if guard   -> arm2
      _               -> arm3
    }
    while { cond } { body }                          # stdlib helper
    until { cond } { body }                          # stdlib helper
    if_then_else(p) { a } { b }                      # stdlib helper
                                                     # (NO `for x in xs`;
                                                     # use `xs | (x => ...)`
                                                     # or `xs |> each(f)`)

  Pipes
    x |> f(args)                                     # apply: f(x, args)
    xs | f                                           # map (head-type dispatch)
    xs || f                                          # flat-map (head-type)
    xs |? p                                          # filter (head-type)

  Effects
    handle { body } with Eff {
      op(args, resume) -> expr
      return(x)        -> expr                       # optional
    }
    handle { body } with Eff as name { ... }         # rebind capability
    Eff.op(args)                                     # call op via capability

  Effect rows in types
    : T / Eff1 + Eff2                                # row of two effects
    : T                                              # pure (empty row)

  Literals and operators
    42  0xFF  0b1010  3.14  3.14e-2                  # numbers
    'a'  '\n'  'A'                                   # chars
    "hello"  "x = #{expr}"                           # strings + interp
    [1, 2, 3]   [head, ...tail]                      # lists
    [1..10]     [1..10..2]                           # ranges (incl. step)
    {x: 1, y: 2}                                     # records
    {x, y}                                           # record punning
                                                     # (≡ {x: x, y: y})
    Some(42)    None                                 # variants
    (a, b)      (a, b, c)      (a, b, c, d)          # n-tuples (sugar for
                                                     # Pair/Triple/Quad records;
                                                     # access via .fst/.snd/.trd/.frt)
    a[i]        a[i] := v                            # array index / set
    @cap        cap := v                             # capability read/write
    expr!                                            # Option/Result propagation
                                                     # (early-return None/Err)
    + - * / %   == != < <= > >=   and or not         # operators
    ++                                               # list/string concat

  Pipe placeholder
    x |> f(a, _, b)                                  # `_` puts x at the given
                                                     # position; without `_`,
                                                     # x lands first.

  Match patterns (see `kai info match` for the full list)
    name @ pattern                                   # as-pattern: bind whole
                                                     # to `name`, also destructure

  Units of measure
    Real<m>     Real<m/s>     Real<m^2>              # in types
    9.81<m/s^2>                                      # in literals

  Holes (typed)
    ?           ?name                                # ask the type-checker

  UFCS (method-style dispatch)
    receiver.fn(args)                                # ≡ fn(receiver, args),
                                                     # dispatched by receiver
                                                     # type. Chains left-assoc:
                                                     # r.f(x).g(y) parses as
                                                     # (r.f(x)).g(y).

NOT IN KAIKAI
  These look plausible but DO NOT EXIST. Do not write them:

    \x -> body                  # Haskell lambda. Use (x) => body.
    fn x -> body                # ML lambda. Use (x) => body.
    (+ 1)   (* 2)               # operator sections (Haskell). Don't.
    [x*2 for x in xs]           # Python comprehension. Use `xs | (x => x*2)`.
    do { ... }                  # Haskell do-notation. Use block body.
    where x = ...               # Haskell `where`. Use `let` inside block.
    class Foo where ...         # type classes / HKT. kaikai has neither.
                                # Single-dispatch protocols only (see
                                # `kai info protocols`).
    type Foo<T> = ...           # angle generics. kaikai uses [T]: Foo[T].
                                # Angle brackets are reserved for units of
                                # measure (`Real<m>`).
    null  undefined  nil        # there is no null. Use Option[T].
    throw  try/catch  raise     # exceptions. Use Fail effect or Result[e, a]
                                # (and `expr!` for propagation).
    async / await keywords      # concurrency is via fibers + Spawn effect.
                                # See `kai info fibers`.
    interface I { ... }         # no interfaces. See `protocol` instead.
    impl Trait for T { ... }    # close — kaikai writes `impl Proto for T`.
    return expr                 # no `return` statement. Last expression
                                # is the value. (Early-return is via `expr!`
                                # for Option/Result, match, or structure.)
    self.x  this.x              # no implicit self/this. Method receivers
                                # are explicit parameters in `impl` blocks.

SEE ALSO
  kai info effects, kai info fibers, kai info match, kai info pipes,
  kai info protocols, kai info units, kai info packages,
  kai info testing, kai info holes
