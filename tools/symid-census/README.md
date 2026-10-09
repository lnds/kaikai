# Name reads in identity data

`census-main-1b21b356.tsv` lists every place the stage-2 compiler read a
name out of identity-bearing data at `1b21b356`, found by `extract.py`
(pattern arms on the name slot, literal-name mints, name accessors,
literal-name and `__`-prefix compares) and classified by hand.

| cat | meaning | stays? |
|-----|---------|--------|
| R | resolver: written text turned into an id | yes |
| D | diagnostic, dump, JSON, docs, test assertion | yes |
| S | surface tooling: parse, fmt, migrate, lsp, surface lowering | yes |
| L | local binder compared against local binders | yes |
| E | text spelled into a C/LLVM symbol or runtime string, never compared | yes |
| P | node rebuilt with the name copied, nothing decided | disappears with the slot |
| X | identity decided by a name | no |
| M | compiler marker spelled as a string | no, becomes a typed node |
