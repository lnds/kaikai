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

  fn parse_url(s: String) : Url / Fail = {
    let scheme = ?
    let host = ?host
    { scheme: scheme, host: host, port: 0 }
  }

  $ kai build --holes parse_url.kai
    parse_url.kai:2  hole: expected String
      in scope:  s: String
    parse_url.kai:3  hole 'host': expected String
      in scope:  s: String, scheme: String

JSON OUTPUT
  `--holes-json` emits a stable JSON document (one record per hole)
  designed for LLM consumption — that is the Tier 3 #8 strategic bet.
  Field names: `file`, `line`, `col`, `name`, `expected_type`,
  `scope`, `effects`.

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
