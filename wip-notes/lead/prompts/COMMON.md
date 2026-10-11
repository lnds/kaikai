You are one of ~27 agents repairing the kaikai stage-2 compiler after a deliberate one-move change of its AST node types (identity by id; no name string beside an id). The compiler does not type-check now (about 5,400 errors in 257 files). Each agent repairs one batch of files in its own git worktree; the lead merges the branches. Work ONLY in the git worktree you were started in.

Below, `<S>` is `/private/tmp/claude-501/-Users-ediaz--herdr-worktrees-kaikai-symid-names-by-type/8228d0c8-8150-407f-935b-2dc3cdac79d2/scratchpad`.

SETUP
1. `git reset --hard baf56156` (your worktree starts from the default branch; this moves it to the lane's base and discards nothing). Check with `git log --oneline -1`.
2. Read fully and follow `<S>/REPAIR-RULES.md` — binding: the old→new shapes, where names come from now, the silent traps, how to work, the shared log `<S>/repair/API.md`, what to report. Other agents started before you: API.md already holds their signature changes — read it before you write a cross-file call.
3. Read the change itself: `git show baf56156 -- stage2/compiler/ast.kai`, then `stage2/compiler/written.kai`, `local_names.kai`, `spec_names.kai`, `decl_text.kai`.

The check script is `sh <S>/errs.sh "$(git rev-parse --show-toplevel)" <files…>` (about a minute per run; run it after a group of edits, not after each one).

FINISH: commit when your batch shows `0 errors` (or only cross-file mismatches you recorded in API.md) and hand back the report the rules ask for. Do not try to make the whole compiler type-check.
