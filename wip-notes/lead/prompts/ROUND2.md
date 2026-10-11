You are repairing the kaikai stage-2 compiler after a deliberate one-move change of its AST node types (identity by id; no name string beside an id). Round 1 (26 agents, one batch of files each) is merged: every name resolves and the whole compiler reaches the type checker with about 114 errors in 21 files. Almost all are CROSS-FILE: a call site written against a signature its callee did not land with. Round 2 closes them.

Below, `<S>` is `/private/tmp/claude-501/-Users-ediaz--herdr-worktrees-kaikai-symid-names-by-type/8228d0c8-8150-407f-935b-2dc3cdac79d2/scratchpad`.

SETUP
1. Work ONLY in the git worktree you were started in. `git reset --hard 1902ecf7` (moves it to the merged lane; discards nothing). Check with `git log --oneline -1`.
2. Read `<S>/REPAIR-RULES.md` fully (binding: shapes, where names come from, silent traps, how to work). `<S>/repair/API.md` is the log of every signature the round-1 agents changed (2,000+ lines: grep it by function name, do not read it whole). `<S>/repair/STATUS.md` summarises each batch's report, including the caller fixes each one asked for ("Round 2 …" lines).
3. Check with `sh <S>/errs.sh "$(git rev-parse --show-toplevel)" <files…>` (about a minute per run, at most 8 run at once across agents; with no file it prints the per-file counts of the whole compiler).

HOW TO RECONCILE
- The CALLEE as it stands in the tree is the truth: read its definition and fix the call. Change a callee only when it is itself wrong (it cannot do its job with what every caller can give it), and say so in the report.
- A caller that lacks a value the callee needs (a `SymTab`, `SpecNames`, `LocalNames`, `ProtoIds`, `CtorIx`, the entry id, a file's scope view) gets it threaded in from where it exists; never fabricate one (no empty table, no `sym_none()` to satisfy a type) unless the callee documents that as the "absent" case.
- `effect not handled: Mutable`: the typer's environment holds the symbol table (arrays), so a function that receives a table as a parameter or field and hands it to the typer needs `+ Mutable` on its own row; a function that BUILDS the table in a local `let` masks it and keeps its row. Add `+ Mutable` to the row of the reported function and follow it up its callers until it is masked. It must not reach `main`.
- Fixing one error can uncover the next in the same function: iterate until YOUR files show `0 errors`.
- Keep behaviour. No builds, no tests. Do not delete the TEMPORARY shims (the lead does that after round 2), but do rewrite any use of one you touch.
- New errors appearing in files that are not yours because of a signature you changed: fix those call sites too and list them.

FINISH: commit (`refactor(compiler): <area> agrees with the signatures that landed`, body + `Part of #2340`, no other trailer), tree clean, and report: commit, files, errors before/after for your files and for the whole compiler, every callee you changed and why, anything that needs a design decision, anything that looked wrong.
