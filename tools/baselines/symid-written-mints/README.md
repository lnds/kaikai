Where a reference by text (`EName`, `EModCall`, `mk_name`) may be built.

  file  fn  why        fn `*`: every function of the file

Before the resolver there is no id to carry, so a pass that runs there builds its references by
text and the resolver settles them. After it a reference is `ESym` / `ELocal` / `EOp`; the rows
below that are not "before the resolver" say what text they still stand on.
