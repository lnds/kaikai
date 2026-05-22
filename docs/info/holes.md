# holes

Typed holes for incremental development.

## Description

A typed hole is a placeholder expression that elaborates to ANY
type. The checker reports holes with:

- the type the hole must have at that position,
- the bindings in scope and their types,
- and, for named holes, the name.

Holes let you sketch a function before knowing how to fill it. The
checker tells you what would type-check there.

## Example

```kaikai
fn maybe_parse(s: String) : Int = {
  let n = ?
  n
}

fn other() : String = {
  let host = ?host
  host
}

fn main() : Int = 0
```

```text
$ kai build --holes file.kai
file.kai:2:11: type hole ?
  expected: Int
  in scope:
    s : String
    main : () -> Int
    ...stdlib functions in scope...
```

## JSON output

`kai build --holes-json` emits a JSON array, one record per hole,
designed for LLM consumption. Record fields:

```text
file            string         source file path
line, col       int            position
kind            string         "hole" (vs other hole kinds)
name            string|null    null for `?`, name for `?ident`
message         string|null    extra context if any
expected_type   string         the type the hole must have
in_scope        array          [{name, type}, ...] of all bindings
```

See `docs/lsp.md` for the LSP hover format that wraps the same data.

## Where holes are legal

Three positions today:

- **Expressions** — `?` or `?name`. The compiler reports the inferred
  type and any in-scope bindings of that type.
- **Effect-row tails** — `fn f(x: Int) : Int / ?e = x` parses; the
  `--effect-holes-json` flag reports the inferred row variable.
- **Patterns** — `?` and `?name` are accepted in match-arm patterns
  (they match anything, like `_`). Pattern holes do not produce a
  typed-hole report today; the position is reserved for richer
  reporting in a follow-up.

Type-expression holes (`fn f(x: ?) : ? = x`) are not implemented.

## NOT IN KAIKAI

- Haskell `_` for type-level holes. kaikai's `?` is term-level
  only.
- Agda interactive refinement. Holes are one-shot; you edit and
  re-run.

## See also

`kai info syntax`, `docs/lsp.md`, `docs/library-mode.md`
