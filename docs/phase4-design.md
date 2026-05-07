# phase 4 design

Basic stdlib and MVP tooling. Closes the MVP as defined in `docs/design.md`: a
user with only `cc` on their machine can build the toolchain and run
non-trivial kaikai programs. Phase 3 got us through the self-hosting
checkpoint; this phase turns the compiler into something usable.

## Non-goals

- **Full module resolution with cross-file imports**. Stage 1 is still a
  single-file compiler; real import plumbing arrives with stage 2 or when
  stage 1 gains it incrementally. Phase 4 provides a *prelude* mechanism
  instead: a single stdlib file is prepended to every compilation.
- **`kai fmt`, `kai repl`, `kai lsp`, `kai doc`, `kai new`, `kai add`**.
  Post-MVP tooling listed in the original design but not required to
  execute programs.
- **Native binary distribution, incremental compilation**. Stage 2 work.

## What Phase 4 adds

1. **`kai` command** — a thin shell driver that orchestrates kaic1 + cc.
   Subcommands:
   - `kai build <file.kai> [-o <out>]` — compile to a native binary
     (stdlib is prepended automatically, `cc` links the runtime).
   - `kai run <file.kai> [args...]` — build then execute the resulting
     binary; forward any arguments.
   - `kai test <file.kai>` — compile the file with `--test` and run it,
     propagating the pass/fail exit code. Only `test "..." { ... }`
     blocks defined in `<file.kai>` (and any user-imported modules)
     run; `test` blocks defined inside the auto-loaded stdlib preludes
     are dropped at load time so the count reflects only the user's
     tests (issue #318). Pass `--include-prelude-tests` to opt into
     the legacy behaviour where every prelude test is also counted.
   - `kai --version`, `kai help` — informational.
   The driver lives at `bin/kai`. On first call it ensures `stage0/kaic0`
   and `stage1/kaic1` are built.

2. **`--prelude <file>` flag on kaic1** — when given, parse and include
   every declaration of `<file>` before the input module's own decls.
   The prelude's decls land in the same global scope, available for
   name resolution and emission. Makes a stdlib usable without imports.
   `test`/`bench`/`check` blocks from prelude files are dropped at load
   time so they do not contaminate `kai test`/`kai bench`/`kai check`
   on the user's file (issue #318). `--include-prelude-tests` keeps
   them — used by stdlib developers running prelude-side test suites
   and by the kaikai project's own integration gates that need the
   full pre-fix behaviour.

3. **`stdlib/core/`** — a small set of files (one per topic) with
   convenience wrappers over the builtin prelude. Kept small and
   written in kaikai-minimal so they compile with both kaic0 and
   kaic1. Originally a single `stdlib/core.kai` monolith; split
   into per-topic files (`list.kai`, `string.kai`, `option.kai`,
   `result.kai`, `char.kai`, `tuple.kai`, `io.kai`) on 2026-04-27
   per `docs/stdlib-layout.md`. Contents:
   - **List**: `list_map_indexed`, `list_find`, `list_take`, `list_drop`,
     `list_any`, `list_all`, `list_sum`, `list_index_of`, `list_nth`,
     `list_is_empty`, `list_concat_list`.
   - **String**: `string_starts_with`, `string_ends_with`, `string_trim`,
     `string_repeat`, `string_join`.
   - **Option**: `opt_map`, `opt_unwrap_or`, `opt_and_then`, `opt_is_some`,
     `opt_is_none`.
   - **Result**: `result_map`, `result_and_then`, `result_is_ok`,
     `result_is_err`, `result_unwrap_or`.
   - **IO**: `println` alias over `print`, `io_args` alias over `args`.
   - **Char**: `ch_is_digit`, `ch_is_alpha`, `ch_is_space`, `ch_to_lower`,
     `ch_to_upper` (ASCII).

4. **Ported demos** — a small set of existing `demos/*.kai` rewritten
   against kaikai-minimal + stdlib. Initial targets: `hello`, `fizzbuzz`,
   `collatz`, `euler1`, `quicksort`. The old pre-redesign demos under
   `demos/` are kept for reference but not built; new ones live at
   `examples/phase4/*.kai` until they supersede the old ones.

5. **README + install note** — short section describing `make` from the
   repo root that builds stage 0 → stage 1 → `kai`, plus a
   one-liner on running a program.

## Milestones within phase 4

Each validated and committed before moving on:

1. **Design doc** (this file).
2. **`kai` shell driver** (`bin/kai`) with build/run/test subcommands
   and a top-level `Makefile` target that produces it. No stdlib yet —
   the driver just wraps kaic1+cc.
3. **`--prelude` flag** on kaic1; accepts a second `.kai` file whose
   decls are parsed, checked, and emitted before the input file.
4. **`stdlib/core/*.kai`** with the list/string/option/result/io/char
   helpers listed above, and a `kai` default that prepends every
   file under `stdlib/core/` (one `--prelude` per file).
5. **Ported demos** under `examples/phase4/`, each running via
   `kai run`. Tests added to the repo-root Makefile.
6. **README update** documenting the three-line bootstrap and a
   `kai run hello.kai` example.

## What phase 4 still cannot do

These all land in stage 2 — see `docs/stage2-design.md`
§Milestones for the full plan:

- **Cross-file imports** — stage 2 m6 (Module resolution).
- **Effects system and inference** — stage 2 m7a (mechanics)
  and m7b (ergonomic sugars). Pinned across `docs/effects.md`,
  `docs/effects-stdlib.md`, `docs/effects-impl.md`,
  `docs/syntax-sugars.md`.
- **Fibers, actors, structured concurrency** — stage 2 m8.
  See `docs/structured-concurrency.md` and `docs/actors.md`.
- **LLVM backend** — stage 2 m3.
- **Formatter, REPL, LSP, doc generator** — stage 2 m15 / m16
  / m17.
- **WASM, Windows, more architectures, package manager** —
  post-stage-2.

The point of phase 4 is to make the MVP *usable* without
extending the language itself. Everything above is deliberately
deferred to stage 2 (or beyond).
