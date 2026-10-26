# Names stay out of identity

The ledgers behind `tools/symid-names-gate.sh`. After the resolver a
declaration, a type, an effect, a constructor or a local is its id; text
is read only where a name is resolved, shown, or spelled into output.

| file | holds |
|------|-------|
| `gate.py` | the source checks; its header documents every ledger |
| `extract.py` | finds each string literal that name text is tested against |
| `sources.py` | the sources as both read them: comments gone, literals masked |
| `../baselines/symid-dual-slots/` | types with a text slot beside an id slot, and what the text is |
| `../baselines/symid-written-readers/` | functions calling `written_text`, with a reason per call |
| `../baselines/symid-allow/` | literal tests on name text, with a reason per literal |
| `../baselines/symid-written-mints/` | where a reference by text may be built |
| `written-eq.txt` | functions that may compare a value holding a written name |
| `pending-written.txt` | sites that still decide by text after the resolver |
| `probe/main.kai` | what `--check-written-eq` must report, and must not |

Reasons: `R` resolution, `D` diagnostic or dump, `E` emitted text,
`L` an operation or field label inside a declaration known by its id.

When the gate fails on new code, convert the site to an id. If the text
is legitimate (a diagnostic, a spelled symbol, a pass before the
resolver), add or delete its entry file with the reason (`tools/lib/entries.py put`, see `tools/baselines/README.md`). A site that decides by
text after the resolver goes in no ledger but `pending-written.txt`.
