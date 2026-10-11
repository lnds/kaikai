# Lead's own work at the merge of round 1 (ast.kai and cross-cutting)
1. `python3 repair/effop.py <root>`: EffectOp ctor `EOp(6 fields)` → `EffOp`; rename the declaration in ast.kai too.
2. ast.kai `Label` gains `cell: LocalId` (API.md ~l.235, infer-B): every `Label { sym, ty_args }` literal adds
   `cell: local_none()`; readers `label_in_cell`, `alias_cell` are in infer part B.
3. ast.kai `TrmcStep` gains `ctor: SymId` (API.md ~l.890, kir): builder fills from the ctor call's ESym.
4. `DispHead` must be visible in impl_index.kai (declare where both can import; API.md ~l.1069).
5. Interpolation `#{x}` show: mint `ESym(core Show dispatcher id)` at the sugar so no user `show` captures it
   (API.md ~l.1074, protos-B OPEN).
6. Generated-symbol spelling of homonymous types (`ref_text` in protos.kai) and run-time spelling of contested
   effects by id (emit_c-C was told to publish `## effect run-time spelling`).
7. DUse/SUse(String) → DeclRef (front-load-B design 2).
8. Delete TEMPORARY shims: ast.kai tail block, sym_read.kai, es_note/es_lookup/ESp, any `TEMPORARY` grep hit.
9. Ledgers: dual-slots.txt (EO, ESp gone…), written-readers.txt (parse.kai…), regen allow.
QUEUED AGENT: emit_c-C (prompt repair/prompts/emit_c-C.md + the extra note about effect run-time spelling).
10. ast.kai `CtorIntSlots = CIS(SymId, [Bool])`, `RawArmBinder = RAB(LocalId, Int, Int)` (ownership).
11. ast.kai `FBR` (fn bounds record) carries the SymId instead of (name, mo) (protos-sat design 2).
12. Round 2 driver reconciliation: bounds_by_sym no tab; gen_law_checks(decls, PScope, reg, file); etc. (STATUS).
13. ENTRY-POINT PORT (round 2 agent, on the repaired tree; start from 79eb9c89's diff) — OWNER DECISION via
    integrador, replaces "grouped kai test drops each test root's main":
    * Do NOT drop any `main` (owner rule: no regressions). Not (a) build alone, not (b) pre-resolver name scan.
    * Two notions, two predicates by id in entry.kai: `is_root_module(m)` (a file the user passed: its `main`
      keeps root rules — implicit row allowed, no "imported here" note) and `is_entry(d)` (what the binary
      starts). Grouped test run: every test file is a root module, the runner is the entry; each file's `main`
      stays an ordinary callable fn with its root-inferred row, just not the entry.
    * Every former `== "main"` site picks the one it means: evidence / frames / reach / live = entry;
      row inference / lint "imported here" = root.
    * (1) a test calling its own file's `main()` must work grouped; (2) a test root importing another test
      root's `main` must behave exactly as solo (b_test imported by a_test is non-root there too) — make that
      the check. Fixtures for (1) and (2) in grouped mode. Remove `rs_drop_entry` / `mt_unexport_entry`.
    * No `kai migrate` row writing (accepted).
14. PR body: intended behaviour changes list accepted by integrador (nullary qualified fast path; clause
    resumes via own continuation binder; purity by exact id; closure_spec by local id + guards deleted;
    NCMethod class).
