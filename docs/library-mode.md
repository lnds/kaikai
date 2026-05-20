# Library mode (issue #454)

`stage2/kaic2 --library-mode <file.kai>` runs the front-end (lex →
parse → resolve → infer) and answers position-keyed queries against
the typed AST instead of emitting C. It is the in-process query
surface that the LSP (#447), the stdlib cache (#452), and any
external tool (`kai check --json`, code-aware fmt, IDE plugins) need
to reason about kaikai source.

The default `compile_source` pipeline keeps working unchanged —
library mode is purely additive.

## Probes

A probe is a `# @probe <kind> <line>:<col>` comment somewhere in the
source. Probes are scanned by the comment lexer (everything after
`#` to end-of-line), so they sit anywhere a line comment is legal.
Six kinds are recognised today (the original three plus the three
the LSP wave added):

| Kind       | Question answered                                                              |
|------------|--------------------------------------------------------------------------------|
| `type`     | What is the inferred type of the innermost node at L:C?                        |
| `def`      | Where is the identifier at L:C declared?                                       |
| `enc`      | What is the innermost node enclosing L:C? (kind + start)                       |
| `complete` | Which top-level names (user fns + stdlib + prelude) are in scope at L:C?       |
| `sig`      | What is the callee type of the innermost enclosing call at L:C? (signature help) |
| `symbols`  | What top-level decls does this file declare? (documentSymbol outline)          |

Lines and columns are 1-indexed, matching every other diagnostic in
the compiler. `complete`, `sig`, and `symbols` shipped alongside
the LSP v3 wave (issue #447, v0.75.0 → v0.79.0).

## Output

`--library-mode` prints a single JSON object on stdout:

```json
{
  "file": "examples/library_mode/type_at_basic.kai",
  "probes": [
    { "kind": "type",
      "line": 4, "col": 11,
      "enclosing": { "node_kind": "EVar", "node_line": 4, "node_col": 11 },
      "type": "(Int, Int) -> Int" },
    { "kind": "def",
      "line": 8, "col": 11,
      "identifier": "double",
      "def": { "decl_kind": "DFn", "name": "double",
               "file": "...", "line": 5, "col": 1 } },
    { "kind": "enc",
      "line": 4, "col": 11,
      "enclosing": { "node_kind": "EBlock", "node_line": 2, "node_col": 11 } }
  ]
}
```

Field shapes (stable for v1):

- `enclosing.node_kind` ∈ the AST kind names emitted by
  `expr_kind_name` (e.g. `EVar`, `ECall`, `EBlock`, `EBinop`) plus
  the helpers `Pattern`, `Param`, `HClause`, `HReturn`, `SVar`,
  `SUse` and the `D*` decl names.
- `type` is the `ty_to_string` rendering or `null` when the node
  carries no inferred type (some intermediate nodes synthesised by
  desugar passes).
- `def.decl_kind` ∈ `DFn`, `DType`, `DConst`, `DEffect`,
  `DProtocol`, `DAxiom`, `DUnit` (top-level decls) plus `Param`,
  `PatBind`, `HReturn` for local binding sites introduced by
  function/lambda parameters, pattern matches, and the named
  binder of a `handle ... return` clause. `DConst` does not
  appear today — consts are lowered to nullary `DFn` before
  inference; the field reports the post-lowering shape, which is
  what an LSP consumer actually sees.

## In-process API

`compile_to_module(file: String, decls: [Decl]) : TypedModule` runs
inference and returns a queryable handle. Three entry points:

```kai
type_at(tm: TypedModule, line: Int, col: Int) : Option[Ty]
def_at (tm: TypedModule, line: Int, col: Int) : Option[DefLoc]
enclosing_node(tm: TypedModule, line: Int, col: Int) : Option[EncResult]
```

`TypedModule` retains the post-infer `[Decl]`. The walkers descend
the AST top-down and return the deepest node whose start is `≤` the
query position; positions are documented in `(line, col)` only — no
byte-offset machinery.

## v1 limitations

- `def_at` resolves top-level fn / type / const / effect /
  protocol / axiom / unit names, plus local bindings inside the
  same module (`let` binds, function and lambda parameters,
  pattern binds — `Some(x)`, `[h, ...t]`, `T { f: y }`, `(a, b)`,
  `as`-aliases, narrow binds, named holes — and closure captures
  resolving to outer scopes). Variant constructors in expression
  position (`MkPair(...)` as a builder) and cross-module
  declarations still return `null`; both are LSP v2 work (#447).
- The `def` answer's `file` field always reports the queried file's
  path, not the import origin. Cross-module *resolution* works
  (an `import helper_mod` followed by a `triple()` call resolves
  to `triple`'s declaration site in the imported file's source
  coordinates), but the `file` field still reports the entry point.
  Phase A (#452) attaches per-decl origin information through the
  cache loader.
- Innermost-enclosing uses `start ≤ pos` and document-order
  recursion. That is correct for non-overlapping AST regions,
  which kaikai's grammar guarantees, but it does *not* require
  end positions on every node. A position past every node returns
  the last one in document order — `null` is reserved for
  positions that precede every decl in the file.
- The query is O(n) per probe (a full AST walk). For interactive
  LSP load this is fine on files up to ~5 kLOC; the index ratchet
  (sorted (start, end, node_id) vector + binary search) is a
  follow-up when the LSP measurement justifies it.

## Why probe comments and not stdin

The CLI takes probes inline so a fixture is one file, not a
file-plus-protocol. Tier 1's `test-library-mode` diffs each
fixture's `.out.expected` byte-for-byte; the comment shape is the
single source of truth for what the test asks. An LSP consumer
that wants to issue probes programmatically uses the in-process
`compile_to_module` + `type_at/def_at/enclosing_node` instead.

## Selfhost waiver

This lane changes `stage2/compiler.kai` extensively (~700 LOC of
new code: walkers + JSON serialisation + CLI wiring). The standard
selfhost-byte-identical gate is **explicitly waived** for the lane;
what the gate verifies instead is that

- `kaic2 stage2/compiler.kai` produces a working C source that
  compiles cleanly and self-hosts to a fixed point (`stage3.c ==
  stage4.c`); and
- Tier 0 + Tier 1 + Tier 1-ASAN remain green.

Both properties are verified before the lane PR opens. The byte
diff vs. the pre-lane `kaic2` is expected and documented in the
PR body.

## Related issues

- #447 — LSP v1 (consumer of this surface)
- #452 — stdlib cache (uses the same `TypedModule` shape as the
  serialisation root)
- #348 — module graph (already feeds `expand_imports`, which this
  lane reuses for cross-module def-at)
- PR #448 — m11 v1 diagnostics shipped span tracking on the emit
  side; this lane formalises spans as a query-time property.
