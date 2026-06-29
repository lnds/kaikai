# doc_stdlib_import

Regression fixture for `kai doc` on a **package** module that imports a
**stdlib** module.

`kai doc` extracts docs through `kaic2 --doc-json`. For a package module
that resolution must see the stdlib search root, exactly like `kai check`
/ `kai build` do — otherwise `import spawn` fails to resolve, `--doc-json`
exits non-zero, and `kai doc` prints nothing.

- `pkg/clean.kai` — no imports; documents fine even without the stdlib path.
- `pkg/withimport.kai` — `import spawn`; before the fix `kai doc
  pkg/withimport` returned empty with exit 1 while `kai check` passed.

Exercised by `tools/test-doc.sh` (tier1 `test-doc`): both modules must
document with exit 0.
