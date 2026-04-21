# proposed extensions

Ideas in the same family as `docs/typed-holes.md`: the compiler emits
structured, machine-consumable information that an LLM (or LSP) can
act on. None of these are adopted yet. They are catalogued here so
that when typed holes land and we learn what the LLM integration
point actually needs, we have a short list of coherent next moves —
not a pile of ad-hoc additions.

Each entry states what it buys, what it costs, and what it depends
on. All entries share the same output contract as typed holes: human-
readable diagnostic text plus a stable JSON schema.

## Status summary

| Extension                                  | Status   | Depends on              |
|--------------------------------------------|----------|-------------------------|
| `todo!(msg) : T`                           | proposed | typed holes             |
| `kai type <pos> --json`                    | proposed | stage-2 type checker    |
| Counterexample JSON for exhaustiveness     | proposed | match exhaustiveness    |
| `axiom name : T`                           | proposed | stage-2 type checker    |
| `kai effects <target> --json`              | proposed | effect inference        |
| `?e` — effect holes                        | proposed | typed holes + effects   |
| `import ?name` — dependency holes          | proposed | module resolution       |
| `kai lint --json` — canonical-form rules   | proposed | canonical style guide   |

## 1. `todo!(msg) : T` — principled unimplemented

```kai
fn parse_expr(tokens: [Token]) : Expr = todo!("pending binary ops")
```

`todo!(msg)` is an expression of any type. Structurally a sibling
of `?`:

- Type-checks as the expected type at its position.
- At runtime, aborts via `kai_prelude_panic("todo: #{msg}")`.
- Reported in `--holes-json` with `"kind": "todo"` and the message.

The distinction with `?` is intent. `?` means *I don't know yet*;
`todo!` means *I know, not yet*. Persists through reformats and is
grep-able, replacing informal `// TODO` comments with a typed marker
that the checker tracks.

**Cost**: low. One new token, reuses the hole runtime.
**Depends on**: typed holes.

## 2. `kai type <pos> --json` — queryable type-at-position

```
$ kai type foo.kai:10:5 --json
{
  "file": "foo.kai", "line": 10, "col": 5,
  "expression": "xs |> filter(. > 0)",
  "type": "[Int]",
  "effects": [],
  "in_scope": [
    { "name": "xs", "type": "[Int]" }
  ]
}
```

Exposes what the checker already computes at every AST node. Covers
any position, not just holes. The schema deliberately overlaps with
`--holes-json` so tools learn one format.

**Cost**: low. The checker annotates every node with its inferred
type; the query walks to the cursor position.
**Depends on**: stage-2 type checker.

## 3. Counterexample JSON for exhaustiveness

When a `match` is non-exhaustive, the compiler already knows which
patterns are missing. Expose them:

```json
{
  "kind": "non_exhaustive_match",
  "at": "foo.kai:14:3",
  "counterexamples": [
    { "pattern": "Rect(0.0, _)", "reason": "not covered" },
    { "pattern": "Triangle(_, _, _)", "reason": "not covered" }
  ],
  "suggested_arms": [
    "Rect(0.0, h) -> ?",
    "Triangle(a, b, c) -> ?"
  ]
}
```

The suggested arms contain `?` holes, so the LLM can paste the
completion and receive hole reports for the bodies. This closes
the loop: error → concrete fix with holes → LLM fills → compile.

**Cost**: medium. The match-check already computes missing inhabitants
for its current error text; formalising the output is a refactor
plus a schema.
**Depends on**: pattern-match exhaustiveness check (already planned).

## 4. `axiom name : T` — postulated symbols

```kai
axiom unsafe_cast[A, B] : (A) -> B
axiom db_layer_exists : Database / Io
```

An `axiom` declares a symbol with a type but no body. The compiler
accepts it, type-checks its uses, and lists it in `--axioms-json`.

- Unlike `todo!`, axioms live at top level and can be called from
  anywhere. They declare *this will never have an implementation
  here* (FFI boundary, architectural placeholder, library seam).
- By default, calling an axiom at runtime aborts via
  `kai_prelude_panic`. Axioms can be bound to an external symbol
  via `Ffi` to receive a real implementation at link time.
- Every binary carries a manifest of which axioms it was built
  against, so an auditor (human or LLM) can see the trust surface.

**Cost**: low to medium. One new top-level form, a side list, a
manifest pass.
**Depends on**: stage-2 type checker.

## 5. `kai effects <target> --json` — effect graph as data

```
$ kai effects src/ --json
[
  { "fn": "greet",
    "effects": ["Io"],
    "handled_in": null },
  { "fn": "main",
    "effects": [],
    "handlers_installed": [
      { "effect": "Io", "at": "main.kai:9" }
    ] }
]
```

Summary of which effects each function performs and where handlers
are installed across the call graph. Complements `kai type` for
effect-row-level queries.

**Cost**: low once effect inference is working — it walks the
inferencer's output.
**Depends on**: stage-2 effect inference.

## 6. `?e` — effect holes

```kai
fn run() : Int / ?e {
  perform Io.read_line() |> string_to_int |> unwrap
}
```

The compiler infers `?e` and reports it like a regular hole: `?e`
resolves to `Io + Fail`. Useful when the user does not yet know which
effect row a function belongs to — common during exploratory work
and when an LLM is drafting a signature.

**Cost**: low. Effect rows are already unification variables during
inference; this exposes one as a named hole.
**Depends on**: typed holes + effects.

## 7. `import ?name` — dependency holes

```kai
import ?parse_expr
```

When the user knows the symbol they need but not the module, the
compiler searches the stdlib and project modules and reports
candidates:

```
foo.kai:1:8: import hole

  looking for a module that exports `parse_expr`:
    - syntax.parser           (pub fn parse_expr(...))
    - experimental.pratt      (pub fn parse_expr(...))

  replace `?parse_expr` with one of those paths.
```

**Cost**: low. Needs a symbol index over the loaded modules; the
resolver already visits them.
**Depends on**: module resolution (stage 2).

## 8. `kai lint --json` — canonical style as data

```json
[
  {
    "at": "foo.kai:3:5",
    "kind": "pipe_simplification",
    "message": "this could be a pipe chain",
    "suggestion": "xs |> filter(. > 0) |> map(. * 2)"
  }
]
```

A small, hand-curated set of canonical rewrites emitted by the
compiler — not a pluggable linter. The rules enforce the principle
*one canonical form per construct*; they are the cultural scaffold
for that principle, not an optional plugin.

**Cost**: medium. The rules must be defined and maintained. The
output pipeline is trivial once the rules exist.
**Depends on**: a canonical-form style guide (cultural design work,
not technical).

## Deliberately not on this list

These were considered and rejected for the same reasons they are
rejected elsewhere in the design:

- **Macros / reflection**: break the *regular, predictable syntax*
  and *fast compilation* principles.
- **Refinement holes** (`?x : { n: Int | n > 0 }`): require a
  constraint solver. That violates the decidable-and-predictable
  commitment of the type system.
- **Gradual-typing holes** (`?: Dyn`): introduce dynamic typing into
  a language whose central promise is that typed effects cannot
  escape unhandled.

## Adoption criteria

Each extension lands only when:

1. Typed holes have shipped and been used in anger. They are the
   prototype for this family; the others inherit their shape
   (expected type, in-scope bindings, candidates, text + JSON, stable
   schema).
2. A concrete need has shown up in practice. *LLMs might like this*
   is not enough. A concrete interaction with an LLM or LSP that
   currently fails or is awkward is.
3. The feature fits in ≤500 lines on top of the stage-2 checker.
   Anything larger gets its own design doc first.

The goal is to keep the surface small. A handful of orthogonal,
well-integrated extensions is worth more than a pile of clever
features.
