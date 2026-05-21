PROTOCOLS(7)                    kaikai                    PROTOCOLS(7)

NAME
  protocols — single-dispatch interfaces, Go/Clojure/Elixir-style

SYNOPSIS
  protocol Show {
    show(x: Self) : String
  }

  impl Show for Int {
    fn show(x: Int) : String = int_to_string(x)
  }

DESCRIPTION
  kaikai has SINGLE-DISPATCH PROTOCOLS only. Lookup is `O(1)` over an
  impl table — no constraint propagation, no HKT, no type families.
  This is a Tier 1 #3 decision (fast compilation).

  The dispatching parameter has type `Self` (capitalized). It is
  conventionally named `x` (matching stdlib protocols.kai), though
  `self` or any other identifier is legal — there is no implicit
  receiver. Op declarations inside `protocol { ... }` omit `fn`;
  `impl` bodies use the full `fn name(...) : R = ...` form.

STDLIB PROTOCOLS
  Show, Eq, Ord, Hash, Serialize, BinSerialize, Default, and the
  arithmetic family Add[a] / Sub[a] / Mul[a] / Div[a] / Rem[a].

  Most user code never declares a new protocol. Implementing the
  stdlib ones for your types is the common case.

EXAMPLE

  type Color = Red | Green | Blue

  impl Show for Color {
    fn show(c: Color) : String = match c {
      Red   -> "red"
      Green -> "green"
      Blue  -> "blue"
    }
  }

  impl Eq for Color {
    fn eq(a: Color, b: Color) : Bool = match (a, b) {
      (Red, Red)     -> true
      (Green, Green) -> true
      (Blue, Blue)   -> true
      _              -> false
    }
  }

DERIVE
  Some protocols can be derived:

    #[derive(Show, Eq)]
    type Point = { x: Int, y: Int }

  Derivable: Show, Eq, Ord, Hash, BinSerialize (partial — see
  `docs/binserialize-collections-design.md`).

NOT IN KAIKAI
  - Type classes (Haskell). No constraint propagation, no HKT.
  - Functional dependencies, type families, GADTs.
  - Multi-parameter type classes.
  - Multi-method / multiple dispatch (Clojure multimethods).
  - Interface satisfaction by structural typing (Go duck typing on
    method sets). kaikai requires an explicit `impl`.
  - Inheritance. Protocols compose by writing more impls.

SEE ALSO
  kai info syntax, kai info pipes (pipe dispatch is by-convention,
  NOT through protocols), docs/protocols.md
