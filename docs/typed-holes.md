# typed holes

Adopted for stage 2. Fills a gap in the "LLM-friendly" principle by
letting the compiler *answer* with structured information when the
programmer (or an assistant) has not yet decided what to write.

## Syntax

Two forms:

```kai
?              # anonymous hole
?name          # named hole (persists across error messages, survives until filled)
```

A hole is a legal expression **and** a legal pattern. It has no
runtime representation: reaching an unfilled hole at runtime aborts
via `kai_prelude_panic("unfilled hole: ?name")`.

## Behaviour at check time

For each hole the compiler produces a **report**, not an error:

- **Expected type** — inferred from the surrounding context.
- **In-scope bindings** — every name reachable at that point, with
  its type.
- **Candidates** — expressions the compiler can synthesise that
  match the expected type. Synthesis considers: in-scope bindings,
  variant constructors, prelude functions of matching result type,
  `None` / `Ok(·)` / `Err(·)` wrappers, and 1-level applications.

If the file contains unfilled holes, compilation still **succeeds**
(warning, not error). The emitter substitutes a runtime panic so the
binary is runnable; the holes surface on execution, not at build
time, unless the user opts into `--strict-holes`.

## Output formats

Human-readable (default):

```
foo.kai:4:3: type hole

  expected: String

  in scope:
    name    : String
    excited : String

  candidates that fit:
    excited
    name
    string_concat(name, excited)

  replace `?` with one of the candidates or a literal String.
```

Machine-readable (`kai build --holes-json`): a single JSON array
where each element describes one hole. Stable schema:

```json
[
  {
    "file": "foo.kai",
    "line": 4, "col": 3,
    "name": null,
    "expected_type": "String",
    "in_scope": [
      {"name": "name", "type": "String"},
      {"name": "excited", "type": "String"}
    ],
    "candidates": [
      {"expr": "excited", "kind": "local"},
      {"expr": "string_concat(name, excited)", "kind": "application"}
    ]
  }
]
```

The JSON path is the integration point for LLM agents.

## Patterns

A hole in pattern position gets the same treatment, plus
**exhaustiveness information**:

```
tail.kai:4:5: pattern hole

  expected: a pattern matching [Int]

  remaining inhabitants (exhaustiveness):
    [_, ...rest]

  candidates:
    [_, ...rest]
    [only]
    [a, b, ...rest]
```

Binding names introduced by a pattern candidate are previewed in the
subsequent expression-hole's scope.

## Typing rules

- Anonymous `?` has the type expected by its context. If the context
  does not fix the type (e.g. the hole is the whole function body),
  the compiler picks the most general type variable and reports it.
- Named `?name` instances with the same name **share a type** within
  the same declaration. This lets a user write
  `if ... { ?same } else { ?same }` and know both arms will unify.
- Holes propagate through type inference like unification variables,
  but they never leak beyond a top-level decl: each decl is checked
  in isolation so a hole elsewhere doesn't fix a hole here.

## Non-goals

- Full program synthesis. We list candidates, not complete
  implementations. Deeper synthesis is what the LLM on the other side
  of the JSON does.
- Proof obligations / refinement checks. Holes give types, not proofs.

## Implementation notes

Stage 2 target. The checker already computes the expected type at
every AST node; the hole case is "stop, record, keep going". Scope
reporting piggybacks on the existing scope stack. Candidate synthesis
is a bounded enumeration (≤ one function application deep); the
bound keeps compilation fast, matches the "one canonical form"
principle, and leaves deeper reasoning to tools.

Rough complexity: a few hundred lines on top of the existing
checker; no runtime support beyond the panic helper (which already
exists as `kai_prelude_panic`). No impact on codegen except a stub
for unfilled positions.
