# protocols

Single-dispatch protocols, Go/Clojure/Elixir-style.

## Description

kaikai has SINGLE-DISPATCH PROTOCOLS only. Lookup is `O(1)` over an
impl table — no constraint propagation, no HKT, no type families.
This is a Tier 1 #3 decision (fast compilation).

The dispatching parameter has type `Self` (capitalized). It is
conventionally named `x` (matching `stdlib/protocols.kai`), though
`self` or any other identifier is legal — there is no implicit
receiver. Op declarations inside `protocol { ... }` omit `fn`;
`impl` bodies use the full `fn name(...) : R = ...` form.

## Stdlib protocols

Show, Eq, Ord, Hash, Serialize, BinSerialize, Default, and the
arithmetic family Add[a] / Sub[a] / Mul[a] / Div[a] / Rem[a].

Most user code never declares a new protocol. Implementing the
stdlib ones for your types is the common case.

## Example

```kaikai
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

fn main() : Unit / Stdout = {
  Stdout.print(show(Red))
  Stdout.print(show(Blue))
  Stdout.print(if eq(Red, Red) { "eq" } else { "neq" })
}
```

## Derive

Some protocols can be derived. The annotation is `#[derive(...)]`
(square brackets — the bare `#derive(...)` form is rejected).

```kaikai
#[derive(Show, Eq)]
type Point = { x: Int, y: Int }

fn main() : Unit / Stdout = {
  let p = Point { x: 3, y: 4 }
  Stdout.print(show(p))
}
```

Derivable: Show, Eq, Ord, Hash, BinSerialize (partial — see
`docs/binserialize-collections-design.md`), Layout (records of
`U<N><be|le>` fields — `kai info kinds`), Json.

`#[derive(Json)]` binds a record to the JSON DOM: it generates
`to_json(self) : JsonValue` and a `<lower(T)>_of_json(v, path) :
Result[T, JsonError]` entry point.

```kaikai
import encoding.json.{json_decode, json_encode}

#[derive(Json)]
type Person = { name: String, age: Int, nick: Option[String] }

fn main() : Unit / Stdout = {
  let p = Person { name: "ana", age: 30, nick: None }
  Stdout.print(json_encode(to_json(p)))
  match json_decode("{\"name\":\"bo\",\"age\":7}") {
    None -> Stdout.print("bad json")
    Some(v) -> match person_of_json(v, "") {
      Ok(q)  -> Stdout.print(q.name)
      Err(e) -> Stdout.print(json_error_show(e))
    }
  }
}
```

Field names go to the wire verbatim — no case conversion. An
`Option[T]` field accepts an explicit `null` and a missing key alike,
both decoding to `None`; every other field is required. Keys the record
does not declare are ignored. A failure carries the JSON path to the
offending node (`address.boxes[2].zip: expected String, got Number`).
Sum types are not derivable: a tagged-union encoding is a convention
the derive does not pick for you. See `docs/json-derive-design.md`.

## NOT IN KAIKAI

- Type classes (Haskell). No constraint propagation, no HKT.
- Functional dependencies, type families, GADTs.
- Multi-parameter type classes.
- Multi-method / multiple dispatch (Clojure multimethods).
- Interface satisfaction by structural typing (Go duck typing on
  method sets). kaikai requires an explicit `impl`.
- Inheritance. Protocols compose by writing more impls.

## See also

`kai info syntax`, `kai info pipes` (pipe dispatch is by-convention,
NOT through protocols), `docs/protocols.md`
