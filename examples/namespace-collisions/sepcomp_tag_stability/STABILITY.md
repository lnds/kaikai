Tag stability under partial recompilation: adding a constructor to one
module must not renumber the tags already baked into another module's
emitted code, so constructor tags are never a globally dense index.

Inserting `AAAFirst` into `sb.kai` — a name that sorts before `Alpha1` —
leaves `Alpha1`'s tag at 1. `tests/namespace_matrix.tsv` runs this
fixture on both backends.
