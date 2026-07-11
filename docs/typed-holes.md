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
via `kai_core_panic("unfilled hole: ?name")`.

## Behaviour at check time

For each hole the compiler produces a **report**, not an error:

- **Expected type** — inferred from the surrounding context.
- **In-scope bindings** — every name reachable at that point, with
  its type.
- **Candidates** — expressions the compiler can synthesise that
  match the expected type. Synthesis considers: in-scope bindings,
  variant constructors, core functions of matching result type,
  `None` / `Ok(·)` / `Err(·)` wrappers, and 1-level applications.

If the file contains unfilled holes, compilation still **succeeds**
(warning, not error). The emitter substitutes a runtime panic so the
binary is runnable; the holes surface on execution, not at build
time, unless the user opts into `--strict-holes`.

## Output formats

Human-readable (default). `in scope (local)` lists the hole's LOCAL
bindings — parameters and lets — and summarises the surrounding
top-level scope (hundreds of stdlib entries) as a count:

```
foo.kai:4:3: type hole

  expected: String

  in scope (local):
    name    : String
    excited : String

  candidates that fit:
    excited
    name
    string_concat(name, excited)

  312 more bindings in scope (--holes-scope lists them)
```

`kai build --holes-scope` prints the same report with every reachable
binding listed instead of the count.

Machine-readable (`kai build --holes-json`): a single JSON array
where each element describes one hole. Stable schema:

```json
[
  {
    "file": "foo.kai",
    "line": 4, "col": 3,
    "kind": "hole",
    "name": null,
    "message": null,
    "expected_type": "String",
    "in_scope": [
      {"name": "name", "type": "String"},
      {"name": "excited", "type": "String"}
    ],
    "scope_elided": 312,
    "candidates": [
      {"expr": "excited", "kind": "local"},
      {"expr": "string_concat(name, excited)", "kind": "application"}
    ]
  }
]
```

`in_scope` carries the local bindings; `scope_elided` counts the
reachable bindings not listed. `kai build --holes-json-scope` lists
everything in `in_scope` (core included) with `scope_elided: 0`.

`kind` is `"hole"` for `?` / `?name` and `"todo"` for `todo!("msg")`,
which share the typed-hole pipeline (see *Implementation notes*).
`message` carries the `todo!` argument for `"todo"` kinds, `null`
otherwise. `name` is the `?name` identifier, or `null` for the
anonymous `?`. The schema is validated in CI by
`scripts/validate_holes_json.py` against `examples/holes/*.kai`.

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

Landed in stage 2 (milestone m10). The checker computes the expected
type at every AST node; the hole case is "stop, record, keep going".
Scope reporting piggybacks on the existing scope stack. Candidate
synthesis is a bounded enumeration (≤ one function application
deep); the bound keeps compilation fast, keeps candidate lists short
(*few forms, each with clear intent*), and leaves deeper reasoning
to tools.

The flags are `--holes` (human-readable report), `--holes-json`
(stable JSON schema, same contract as typed holes promises), and
their `--holes-scope` / `--holes-json-scope` variants that list the
full reachable scope instead of the local slice. They are exposed at
two levels: the underlying `kaic2 --holes` / `kaic2 --holes-json`
(no implicit stdlib core modules — used by `make test-holes`), and the
driver-level `kai build --holes` / `kai build --holes-json`
(auto-loads the same stdlib core modules as a normal `kai build` so the
in-scope dump matches what the user actually sees at compile time). Both surfaces print the
report and exit without producing a binary. Unfilled holes that
reach codegen compile to a runtime panic via the existing
`kai_core_panic` helper; no codegen impact beyond a stub for
unfilled positions. The JSON schema is validated by
`scripts/validate_holes_json.py`.
