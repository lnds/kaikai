Functions that call `written_text`, the one way characters leave a `Written`, and why.

  file  fn  reads        reads: how many calls for which reason, e.g. `R=2 D=1`

  R  resolution: a pass before the resolver, the resolver's own tables, or the typer (and the
     stamps right after it) settling a name the resolver left written
  D  a diagnostic, a dump, tool output, a test's assertion
  E  an emitter or generator spelling a symbol, a label, a key
  L  an operation or field label looked up inside a declaration known by its id

A call that decides something by the text after the resolver is not listed here: it is a `read`
row of pending-written.txt until it is converted.
