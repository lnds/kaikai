# examples/test-runner

Fixtures for `kai test` itself: files of `test` blocks run through the
test runner rather than built as programs. Each
`<name>.test.expected` holds the run's stdout followed by its stderr.

They live outside the program corpora on purpose: those sweeps build
every `.kai` as a program and pair it with `<name>.out.expected`.

`coverage_report/` is a package, not a file: `kai test --json --coverage`
reports the package lines each of its blocks ran, pinned by
`coverage_report.json.expected` and, under `--only`,
`coverage_report.only.expected`.
`coverage_prune/` is a package whose library the build prunes; the same
gate (`make -C stage2 test-coverage-report`) pins its report, cold and
warm, in `coverage_prune.json.expected`. `coverage_const/` and its
`coverage_const.json.expected` are not run by any target.
