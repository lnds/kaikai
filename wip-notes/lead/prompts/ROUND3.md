You are working on one PR of the kaikai stage-2 compiler: "identity by declaration id only, names confined to the resolver and diagnostics" (tracking issue #2340). The AST no longer holds a name beside an id (`ESym(SymId, inst)`, `ELocal(LocalId)`, `EOp(OpRef)`, `TyCon(SymId, args)`, declarations with ids only, text only as `Written` behind `written_text`). The repaired compiler type-checks, builds and self-hosts. You close ONE remaining item, in your own git worktree; the lead merges it.

Below, `<S>` is `/private/tmp/claude-501/-Users-ediaz--herdr-worktrees-kaikai-symid-names-by-type/8228d0c8-8150-407f-935b-2dc3cdac79d2/scratchpad`.

SETUP
1. Work ONLY in the git worktree you were started in. `git reset --hard BASE` where BASE is the commit named in your task (moves the worktree to the lane; discards nothing). Check with `git log --oneline -1`.
2. Background (read what you need, not all of it): `<S>/REPAIR-RULES.md` (the new shapes, where names come from, silent traps), `<S>/repair/STATUS.md` (what each area's repair reported), `<S>/repair/API.md` (signature log; grep it). Project rules: `CLAUDE.md` at the repo root (comments brief/timeless/or absent; English only; `kai info <topic>` before writing kaikai; new files score `km score` A- or better).
3. Type-check: `sh <S>/errs.sh "$(git rev-parse --show-toplevel)" [files…]` (about a minute; `0 errors in 0 files` for the whole compiler is the bar).
4. Build your worktree's compiler: `sh <S>/bslot.sh "$(git rev-parse --show-toplevel)" <S>/<your-name>-kaic2` (6-10 minutes, at most 2 agent builds run at once, so plan to build a few times, not after each edit). It leaves the new compiler at `stage2/kaic2` of your worktree; then `KAI_BACKEND=c ./bin/kai run <file.kai>` / `./bin/kai build <file.kai> -o <out>` use it. Use the C backend (`KAI_BACKEND=c`); the native backend is checked by CI.
5. Never: `git stash`, `pkill -f`, pushing, opening issues or PRs, editing `CHANGELOG.md` / `VERSION`, running the full test corpus or `make tier1` (CI does that). Running individual fixtures and `make tier0` is fine.

RULES OF THE PR (binding)
- No decision by comparing names after the resolver: identity is the id. Text is read only where a name is being RESOLVED (before / at the resolver, or type-directed resolution of a still-written name in the typer), shown in a diagnostic or dump, or spelled by an emitter.
- No new `String` slot beside an id. No `sym_none()` / empty table fabricated to satisfy a type.
- Every fix carries a regression fixture under `examples/` (positive: `.out.expected`; negative: `.err.expected`), placed where the closest precedent lives and wired the way that directory's fixtures are run (many directories are globbed; some need a row in a list or matrix — look at how a neighbour is run in `stage2/Makefile` / `stage2/test-lists/` / `tests/`).
- Commit: Conventional Commits, English, body, last line `Part of #2340`, NO other trailer (no session links, no Co-Authored-By). Tree clean at the end.

REPORT (final message): commit hash, files, what changed in substance, what you measured (state the backend) and how to rerun it, fixtures added, anything you could not do, anything that needs a design decision, anything that looked wrong.
