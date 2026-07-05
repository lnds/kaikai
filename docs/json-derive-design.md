# JSON `#[derive(Json)]` — typed struct binding (design proposal)

**Status:** proposal, not accepted. A lateral discovery: the gap between kaikai's
JSON today and Go's struct binding. Not a kind, not a theory — plain
derive-over-`Type`, the same mechanism as `#[derive(Eq)]`.

## What exists today (verified, runs)

`stdlib/encoding/json.kai` is a **dynamic DOM** — Go's `map[string]interface{}`
side, not Go's struct-tags side:

```kaikai
pub type JsonValue = JNull | JBool(Bool) | JNum(Int) | JReal(Real)
                   | JStr(String) | JArr([JsonValue]) | JObj([Pair[String, JsonValue]])
pub fn json_decode(s: String) : Option[JsonValue]
pub fn json_encode(v: JsonValue) : String
# navigators: get(v, key), at(v, i), as_string / as_int / as_real / as_bool → Option
```

Binding a `JsonValue` to a domain type is **manual** today — you write the walk by
hand (verified working, `/tmp/json_demo.kai`):

```kaikai
type Person = { name: String, age: Int, active: Bool }

fn person_of(v: JsonValue) : Person / Fail = {
  let name = match json.as_string(json.get(v, "name")) {
    Some(s) -> s ; None -> Fail.fail("missing/invalid 'name'") }
  let age  = match json.as_int(json.get(v, "age")) {
    Some(n) -> n ; None -> Fail.fail("missing/invalid 'age'") }
  let active = match json.as_bool(json.get(v, "active")) {
    Some(b) -> b ; None -> Fail.fail("missing/invalid 'active'") }
  Person { name: name, age: age, active: active }
}
```

That 15-line walk is exactly what Go's reflection generates from struct tags — for
free. kaikai writes it by hand. That is the gap.

## The gap: Go's struct binding

Go has two JSON APIs; kaikai has only the first:

```go
// 1. dynamic — kaikai HAS this (JsonValue)
var v interface{}; json.Unmarshal(data, &v)

// 2. struct binding by reflection — kaikai LACKS this
type Person struct {
    Name string `json:"name"`
    Age  int    `json:"age"`
}
json.Unmarshal(data, &p)   // fills the struct automatically
```

## Proposal: `#[derive(Json)]`, not a kind

Binding JSON to a type is **derive dirigido por la forma del tipo** — the same
compile-time codegen as `#[derive(Eq)]`, driven by the record's fields. It is NOT
a theory or a kind: JSON is a name-keyed tree (commutative in keys —
`{a,b} == {b,a}`), which is ordinary structural typing (kind `Type`), and the
binding is codegen over that structure.

```kaikai
#[derive(Json)]
type Person = { name: String, age: Int, active: Bool }

let p : Person = json.from_json(src)?      # generated; replaces person_of by hand
let s : String = json.to_json(p)           # generated
```

The compiler walks `Person`'s fields and generates the `get`/`as_*`/`match` chain
you write by hand today. `from_json` / `to_json` derive from the field structure.

### Field overrides — attributes, NOT Go's magic-string tags

Go's `json:"name"` is an **untyped string** the reflection parses at runtime — a
`jsom:"name"` typo fails silently at runtime. kaikai's overrides are **attributes**
the parser validates:

```kaikai
#[derive(Json)]
type Config = {
  #[json(rename = "user_name")]  host: String,   # Go's tag, but compiler-checked
  #[json(default = 30)]          retries: Int,    # missing key → default
  #[json(skip)]                  cache: [Int],    # excluded from JSON
}
```

`#[json(rename/default/skip)]` covers Go's tag surface — but a `#[jsom(...)]` typo
is a **compile error**, not a silent runtime miss, and the override is structured
(`rename`, `default`, `skip` are fields with shape) not text re-parsed by
reflection. Same mechanism as `#[derive]` / `#[doc]`; zero new surface. Backticks
were considered and **rejected** — copying Go's magic string is copying its worst
part; the backtick is for embedded grammars (bit-scope), not field metadata.

## Where kaikai beats Go

**Failure is in the type.** `from_json : Person / Fail` (or `: Option[Person]`) —
a malformed document is a typed failure the caller must handle. Go returns an
`error` that can be ignored, and a wrong tag fails silently. kaikai's derive puts
the parse failure in the signature and the field override under compiler check. The
whole-session theme (types don't lie) applied to JSON.

## Scope

- **In:** `#[derive(Json)]` generating `from_json : T / Fail` and `to_json : T ->
  String` over records; `#[json(rename/default/skip)]` field overrides.
- **Out:** sum-type / enum encoding strategy (tagged unions — a real question, Go
  punts on it too); streaming decode (that would ride effects, `/ Read`); schema
  validation beyond structural shape.

## Open questions

- Sum types: how does `type Shape = Circle(Real) | Square(Real)` encode? Tagged
  (`{"tag":"Circle","value":1.0}`) vs other conventions — needs a decision, out of
  the record-first scope.
- Does `from_json` return `Option[T]` or `/ Fail`? `/ Fail` carries a message
  (which field, why); `Option` is simpler. Lean `/ Fail` for diagnostics.
- Reuse `Serialize` protocol (`stdlib/protocols.kai`) or a dedicated `Json`
  derive? `Serialize` is whole-string round-trip; JSON needs per-field structure.
  Likely a dedicated derive that leans on `Serialize` for leaves.

## References

- `stdlib/encoding/json.kai` — the dynamic DOM this builds on.
- `stdlib/protocols.kai` §Serialize — the leaf-serialization protocol.
- `/tmp/json_demo.kai` — the hand-written binding this would generate (verified
  running: `name=ana age=30 active=true`).
- CLAUDE.md — `#[derive]` / `#[doc]` attribute mechanism this reuses.
