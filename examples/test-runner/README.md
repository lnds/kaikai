# examples/test-runner

Fixtures for `kai test` itself: files of `test` blocks with no `main`,
run through the test runner rather than built as programs. Each
`<name>.test.expected` holds the run's stdout followed by its stderr.

They live outside the program corpora on purpose: those sweeps build
every `.kai` as a program and pair it with `<name>.out.expected`.
