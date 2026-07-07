# Lane experience — kind/theory surface v1 (the cosmetic first cut)

Outcome: the reserved-but-unused `taxon`/`taxology` keywords are gone,
replaced by the real `kind`/`theory` surface. `theory` is a hard
keyword; `kind` is a contextual keyword recognised by shape only (the
`region` trick). A stdlib catalog file declares the built-in `Measure`
kind through the new surface. This cut adds **no new user capability**
— its whole value is (a) killing `taxon`/`taxology`, (b) proving `kind`
as a contextual keyword survives self-host byte-identity despite ~1055
internal `.kind` field uses, and (c) mapping the plumbing traps before
v2 (user kinds + generalized engine).

## Scope as planned vs shipped

Planned (the five v1 blocks) and shipped, all of it:

- **Lexer** (`lex.kai`): `TkTaxon`/`TkTaxology` removed from the enum,
  `tk_name`, and `keyword_kind`; `TkTheory` added as a hard keyword.
  `kind` stays `TkIdent` (contextual). `tk_is` (`parse.kai`) exhaustive
  match updated in the same shape.
- **AST** (`ast.kai`): `DTheory(String, [String], Int, Int)` and
  `DKind(Bool, String, String, String, Int, Int)` added to `Decl`. The
  `Kind = KType | KUnit` enum and the `#Unit` string hack are
  untouched.
- **Parser** (`parse.kai`): the two `taxon`/`taxology` "not yet
  supported" stub arms are replaced. `theory` dispatches by `TkTheory`;
  `kind` is recognised contextually by `decl_head_is_kind` (head ident
  is `kind` AND next is `IDENT` AND the token after is `:`). Property
  list and `with unit` tail parsed. Barrier: an unknown theory name
  errors at parse.
- **Stdlib catalog** (`stdlib/core/kinds.kai`): file-top module
  `#[doc]`, `theory AbelianGroup = { assoc, commut, inverse, identity }`,
  `kind Measure : AbelianGroup with unit`. NOT auto-loaded (see below).
- **Fixtures**: a positive sugars fixture proving `kind`/`taxon`/
  `taxology` are identifiers again + `.kind` field access; a positive
  fixture proving `theory`/`kind` parse and `unit`/`Real<m>` are
  unchanged; a negative golden for the unknown-theory barrier; the six
  stale `taxon`/`taxology` reservation fixtures deleted.

## The KUnit wiring decision (reported per the brief)

v1 keeps `Measure` on its existing hardcoded path. The
`kind Measure : AbelianGroup with unit` declaration is **parsed and
validated, then stripped** before the typer — it does not feed KUnit.
`Measure` as a tparam kind keeps working through the same `#Unit`
suffix encoding it always used; `unit m` / `Real<m>` are byte-for-byte
unchanged. Wiring the declaration to *produce* KUnit was explicitly
allowed to be deferred, and it is: the catalog is a recognised,
validated surface, not yet the source of truth for the kind engine.

## The strip lifecycle

`DTheory`/`DKind` are dropped in `dsc_loop` (`doc_attr.kai`), the same
single post-parse walk that strips `DDoc`/`DModuleDoc`. After that
pass no pipeline stage past the parser sees a catalog decl. This kept
the blast radius to the parser, the strip point, and the formatter —
everything else (typer, mono, perceus, emit, cache) never receives one.

## Structural surprises / plumbing traps found

- **`theory` as a pattern binder is the trap the bundle hides.** The
  first self-host attempt failed with `expected pattern` at
  `driver.kai:666` — I had written `DKind(is_pub, name, theory, intro,
  ...)` in a `match` arm, using the freshly-minted **hard keyword**
  `theory` as an ordinary binder. The bundled `kaic2` compiled fine
  (concatenated source), but the per-file self-host (`import
  compiler.driver`) rejected it — exactly the "bundle-concat hides
  self-host catches" trap. Fix: rename every `theory` binder/param/var
  to `thy` (five sites across `driver`, `fmt_decl`, `emit_c`, `parse`).
  **Lesson: reserving a common English word as a hard keyword bites
  the compiler's own source; the self-host per-file gate is what
  catches it, not the bundle build.** This is precisely why `kind`
  had to stay contextual — a hard `kind` keyword would have forced
  renaming ~1055 `.kind` uses.

- **Twelve exhaustive `Decl` matches, no wildcard.** kaikai's `match`
  has no mandatory `_`, so adding two `Decl` variants makes every
  wildcard-less match over a `Decl` non-exhaustive. Twelve sites needed
  arms: `cache_decl_to_hex`, `dump_decl`, `find_enc_decl`,
  `expand_ta_decl`, `dump_decl_type`, `collect_decl`, `fmt_decl`,
  `decl_line`, `expand_unit_aliases_decl`, `qualtype_decl`, `chk_decl`,
  `register_one`. Because the decls are stripped post-parse, the
  post-strip sites (cache, resolve, infer, modules, emit) got defensive
  arms — `cache_decl_to_hex` follows the `DDoc` precedent and `panic`s
  loudly if an unstripped catalog decl reaches the serializer. Only
  `fmt_decl` (which formats the raw stream, pre-strip) got real
  formatting; `--fmt` round-trips both decls byte-identically.

- **`#[doc]` above a contextual-keyword decl is a real gap.** A
  per-decl `#[doc("...")]` on a `kind`/`theory` declaration would be
  mis-resolved as a *module* doc, because `doc_head_starts_decl` keys
  off the raw `TokKind` and neither `TkTheory` nor a contextual `kind`
  ident is in its decl-head set. v1 sidesteps this: the catalog uses a
  single file-top module `#[doc]` plus internal `#` notes, no per-decl
  doc. Wiring per-decl docs for `kind`/`theory` (add `TkTheory` to
  `doc_head_starts_decl`; teach it the contextual `kind` shape, which
  needs lookahead it does not currently receive) is a v2 follow-up.

- **The `--ast` dump does not show the catalog decls, `--fmt` does.**
  Consistent with the strip: `--ast` runs after `doc_strip_collect`
  (decls already gone), `--fmt` formats the pre-strip stream. Both are
  correct; noted so a future reader is not surprised.

## Deferred to v2 (noted, not implemented)

- **The hard module barrier.** `theory X = {…}` outside
  `stdlib/core/kinds.kai` is NOT an error in v1 — a user file may write
  `theory` decls today. The parser/resolver has no current-module-path
  access, so the "theory declarations only allowed in the catalog"
  barrier is deferred. For v1, `theory` is a hard keyword the catalog
  uses by convention; it is not documented as a user form.
- **The catalog is not auto-loaded.** `stdlib/core/kinds.kai` is not
  in the driver's core-module set. It parses clean as a standalone file
  (proven by fixture and `--fmt`), but it is not loaded into every
  compilation — v1 has no consumer that reads the catalog (Measure is
  hardcoded), so auto-loading would only widen the blast radius.
- **`unit` as a registered introducer (the north star).** `unit` stays
  `TkUnitKw`, a hard keyword. The intended v2+ direction is for `unit`
  to become an introducer *registered by the kind declaration* (like
  `metric`/`currency` will be), not a hard keyword — `with unit` today
  just recognises the existing token. The per-kind stateful introducer
  parser (a table populated by `kind … with intro`) is the delicate
  piece v2 must build.
- **User kinds + per-kind isolation.** Metric/Imperial user kinds, the
  `kind`-tag on `TyDimT` that makes `meter + foot` an error, and the
  generalized abelian engine are all v2. v1 is Measure-only;
  `unify_unit`/`TyDimT` are untouched.
- **Replacing the `#Unit` string hack with real kind data** — deferred;
  v1 adds no kinds, so nothing breaks.

## Verification

- `make KAI_LLVM=1 kaic2` (C and native) — compiles clean.
- Self-host byte-identical (C and native): `kaic2b.c == kaic2c.c`.
- New fixtures: contextual-ident positive (`67`), theory/kind surface
  positive (`kind surface ok`), unknown-theory negative golden.
- Behavioural smoke: `let kind`/`let taxon`/`.kind`/`{kind: 7}` compile
  and run; `kind X : NoSuchTheory` → `error: unknown theory
  ` + backtick-quoted name; `theory`/`kind` decls `--fmt` round-trip.
