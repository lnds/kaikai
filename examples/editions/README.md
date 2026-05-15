# Multi-edition dispatch fixtures (issue #603)

These fixtures exercise the load-bearing `edition` field in
`kai.toml` introduced by #603. Each is runnable from the repo root
with `bin/kai run <fixture>/main.kai` (or `bin/kai build` for the
negative case).

| Fixture                       | Edition       | Exercises                                                      |
|-------------------------------|---------------|----------------------------------------------------------------|
| `edition_hanga_roa_pipe/`      | `hanga-roa`    | #594 convention-based pipe dispatch over a user `pub type Box` |
| `edition_tongariki_pipe/`     | `tongariki`   | Legacy seeded `List → list` mapping, no user-type walk         |
| `edition_unknown_error/`      | `atlantis`    | Unknown-edition diagnostic with the known set named            |
| `edition_missing_field/`      | (omitted)     | Manifest with no `edition =` → repo `EDITION` fallback         |
| `edition_cache_invalidation/` | both          | `check.sh` builds under each edition into a private cache root |

The cache-invalidation fixture sets `$KAI_PRELUDE_CACHE_DIR` to a
temporary directory so the system cache is untouched, and asserts
that compiling under `tongariki` and `hanga-roa` produces disjoint
subdirectories.
