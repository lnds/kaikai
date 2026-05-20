# sibling_examples_tests — package with examples/ and tests/

Layout:

```
sibling_examples_tests/
├── kai.toml            name = "mathlib"
├── main.kai            entry — imports mathlib.adder
├── mathlib/
│   └── adder.kai       pub fn add, pub fn double
├── examples/demo/
│   ├── main.kai        imports mathlib.adder
│   └── main.out.expected
└── tests/
    └── test_adder.kai  `test "..." { assert ... }` blocks
```

The package's own `mathlib.adder` resolves from sibling
directories — `examples/` and `tests/` — without a self-dep
entry in `kai.toml`. This is the regression guard for the part
of #567 that ensures `import <pkgname>.<module>` works from any
directory under the manifest.

Run:

```sh
bin/kai run examples/packages/sibling_examples_tests
bin/kai run examples/packages/sibling_examples_tests/examples/demo/main.kai
```

Expected: `7` from the entry, `42` from the demo.
