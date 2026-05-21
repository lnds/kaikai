HOLES(7)                        kaikai                        HOLES(7)

NAME
  holes — typed holes for incremental development

SYNOPSIS
  ?                                # anonymous hole
  ?name                            # named hole

  kai build --holes <spec>         # report holes as warnings, continue
  kai build --holes-json <spec>    # same, structured JSON output

DESCRIPTION
  A typed hole is a placeholder expression that elaborates to ANY
  type. The checker reports holes with:

  - the type the hole must have at that position,
  - the bindings in scope and their types,
  - the effects in scope (the row at that point),
  - and, for named holes, the name.

  Holes let you sketch a function before knowing how to fill it. The
  checker tells you what would type-check there.

EXAMPLE

  fn maybe_parse(s: String) : Int = {
    let n = ?
    n
  }

  fn other() : String = {
    let host = ?host
    host
  }

  $ kai build --holes file.kai
    file.kai:2:11: type hole ?
      expected: Int
      in scope:
        s : String
        main : () -> Int
        ...stdlib functions in scope...

JSON OUTPUT
  `--holes-json` emits a JSON array, one record per hole, designed
  for LLM consumption. Record fields:

    file            string         source file path
    line, col       int            position
    kind            string         "hole" (vs other holes kinds)
    name            string|null    null for `?`, name for `?ident`
    message         string|null    extra context if any
    expected_type   string         the type the hole must have
    in_scope        array          [{name, type}, ...] of all bindings

  See `docs/lsp.md` for the LSP hover format that wraps the same
  data.

WHERE HOLES ARE LEGAL
  Anywhere an expression is legal. Patterns and types do not (yet)
  accept holes.

NOT IN KAIKAI
  - Haskell `_` for type-level holes. kaikai's `?` is term-level
    only.
  - Agda interactive refinement. holes are one-shot; you edit and
    re-run.

SEE ALSO
  kai info syntax, docs/lsp.md, docs/library-mode.md
