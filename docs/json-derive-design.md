# JSON `#[derive(Json)]` — typed struct binding

**Status:** accepted and shipped. `#[derive(Json)]` generates the typed
binding between a record type and the JSON DOM — the Go-struct-tags side
of JSON, which kaikai previously lacked.

## What exists today

`stdlib/encoding/json.kai` is the **dynamic DOM** — Go's
`map[string]interface{}` side:

```kaikai
pub type JsonValue = JNull | JBool(Bool) | JNum(Int) | JReal(Real)
                   | JStr(String) | JArr([JsonValue]) | JObj([Pair[String, JsonValue]])
pub fn json_decode(s: String) : Option[JsonValue]
pub fn json_encode(v: JsonValue) : String
# navigators: get(v, key), at(v, i), as_string / as_int / as_real / as_bool → Option
```

`#[derive(Json)]` is the **typed** side over that same tree. Binding a
`JsonValue` to a domain type used to be a hand-written walk:

```kaikai
type Person = { name: String, age: Int, active: Bool }

fn person_of(v: JsonValue) : Person / Fail = {
  let name = match json.as_string(json.get(v, "name")) {
    Some(s) -> s ; None -> Fail.fail("missing/invalid 'name'") }
  # ... one such block per field
}
```

That walk is what the derive now generates:

```kaikai
#[derive(Json)]
type Person = { name: String, age: Int, active: Bool }

let doc : JsonValue = json_decode(src)?
let p : Result[Person, JsonError] = person_of_json(doc, "")
let s : String      = json_encode(to_json(p))
```

`JsonValue` and `JsonError` live in `stdlib/encoding/json_bind.kai`
alongside the runtime the derive lowers onto; only `protocol Json` sits
in `stdlib/protocols.kai`, where the compiler pins its protocol id.

The types deliberately stay out of the auto-loaded core: the C emitter
eager-seeds every user nullary constructor as an immortal singleton in
each binary's `main` (the direct-tag enum slot store needs it — a slot
read can precede any construction), so `JNull` in the core would offset
the RC trace baseline of every program that never touches JSON. A
protocol may name types it does not declare, so the core still compiles.
`encoding/json.kai` imports `json_bind`, so one `import encoding.json`
brings the DOM, the codec, and the derive runtime into scope together.

## Shape

```kaikai
protocol Json {
  to_json(self: Self) : JsonValue
  of_json(v: JsonValue, path: String) : Result[Self, JsonError]
}
```

`of_json` takes the path of the node it is decoding so a nested failure
can name its position. Single dispatch selects on the first argument,
which for a decode is the DOM node, so the derive also emits a
`<lower(T)>_of_json(v, path)` shim as the user-facing entry point — the
same trick BinSerialize and Layout use for `from_bytes`.

## The four binding policies

Binding JSON to a type forces four choices that are not mechanical.
These are decided once, in the derive, and are not per-type
configurable.

### 1. Field naming — verbatim

The kaikai field name goes to the wire exactly as written. There is no
automatic `snake_case` → `camelCase` conversion: a silent renaming rule
is a rule the reader of the type declaration cannot see, and the wire
format is not the place for a convention the compiler invents. What the
type says is what the document says.

A per-field `#[json(rename = "...")]` attribute is the natural extension
when a document's names cannot be spelled as kaikai identifiers; it is
not part of this landing.

### 2. Null and missing — `Option` accepts both, everything else is required

An `Option[T]` field accepts an explicit JSON `null` **and** a missing
key, both decoding to `None`. Any other field is required: a missing key
is a deserialization error, not a zero value. This is the serde/Go
model — `Option` is genuinely optional and everything else is not, so
the type declaration alone tells the reader which fields a document must
carry.

A field whose key is present but carries the wrong shape is an error
even under `Option`: `null` means absent, `7` does not mean absent.

### 3. Failure shape — `Result[T, JsonError]`, located

Decoding returns `Result[T, JsonError]`, where `JsonError` carries the
JSON path to the offending node:

```kaikai
pub type JsonError = { path: String, expected: String, found: String }
```

A failure three levels into a document reports `address.boxes[2].zip:
expected String, got Number` rather than one flat message about the
whole payload. That location is the point: a plain `String` error is
much less useful for nested documents, which is exactly where a typed
binding earns its keep. A record decode checks that its node is an
object *before* reading keys, so a wrong node reports as itself
(`address: expected Object, got Array`) rather than as every field
going missing at once.

### 4. Unknown fields — ignored

Keys the type does not declare are skipped, not rejected. This is
tolerant to schema evolution: a producer may add a field without
breaking every existing reader, which is serde-without-`deny_unknown_fields`
and Go's `encoding/json` default. Strict rejection is the rarer need and
would be an opt-in, not the default.

## Why derive and not a kind

JSON is derive-only and **never a kind**. Naming and null policy are
choices *about* a type rather than properties *of* one — unlike
`Layout`, where byte order genuinely must live in the type system
because two records with the same fields and different endianness are
different types. Two records with the same fields and different JSON
key-casing are the same type with different serializers. That
distinction is what keeps `Json` out of the kind system.

## Scope

- **In:** `#[derive(Json)]` over records, generating `to_json` and
  `of_json`; fields of type `String`, `Int`, `Real`, `Bool`, `[T]`,
  `Option[T]`, and nested records that themselves derive `Json`.
- **Out:** sum-type / enum encoding (tagged unions are a separate
  convention choice — Go punts on it too, and the derive rejects sum
  types rather than inventing one); `#[json(rename/default/skip)]`
  field overrides; streaming decode; schema validation beyond
  structural shape.

## Rejections the derive reports

A field with no JSON denotation is reported at the derive site naming
the field, rather than surfacing later as a missing `to_json` at the
first call site:

- a sum type — needs a tagged-union encoding the derive does not choose;
- `Option[Option[T]]` — one JSON `null` cannot denote two absences;
- a generic head (`Result[Int, String]`) — the emitted decode calls a
  monomorphic shim a parameterised type has none of;
- any other type with no denotation in the DOM.

## References

- `stdlib/protocols.kai` §Json — the protocol.
- `stdlib/encoding/json_bind.kai` — `JsonValue`, `JsonError`, and the
  runtime the derive lowers onto.
- `stdlib/encoding/json.kai` — the dynamic DOM this builds on.
- `stage2/compiler/json_derive.kai` — the impl builder.
- `examples/stdlib/json_derive_*.kai` — round-trip, absence, and
  error-path fixtures.
