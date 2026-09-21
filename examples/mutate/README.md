# examples/mutate

Fixtures for `kai mutate`.

`operators.kai` carries one construct per mutation operator;
`operators.sites.expected` is the site catalogue it must produce.

`negate_shapes.kai` carries one `if` per condition shape — atom,
comparison, connective, call. A site catalogue proves an operator
finds its construct, not that the mutant it writes compiles, and
`negate` is the operator where the two come apart: `not` binds tighter
than every binary operator, so a bare prefix is well-formed only on an
atom. `make -C stage2 test-mutate-negate-shapes` pins the catalogue and
`--check`s every `negate` mutant.

`site_data.kai` pins the `--mutate-list-json` catalogue in
`site_data.sites.json.expected`: every operator, an arm inside an impl
method, a const, and a repeated `(operator, original)` pair.
`make -C stage2 test-mutate-site-data` diffs it and checks that each
`--mutate-apply` mutant is the source with the reported span replaced.
Regenerate it from `stage2/` so the `file` field matches:

```sh
cd stage2 && ./kaic2 --mutate-list-json ../examples/mutate/site_data.kai --path ../stdlib \
  > ../examples/mutate/site_data.sites.json.expected
```

Regenerate the golden after an intentional operator change:

```sh
./stage2/kaic2 --mutate-list examples/mutate/operators.kai --path stdlib \
  > examples/mutate/operators.sites.expected
```

Check it:

```sh
diff <(./stage2/kaic2 --mutate-list examples/mutate/operators.kai --path stdlib) \
     examples/mutate/operators.sites.expected
```

Two invariants the golden pins, both of which a passing diff proves:

- **Every operator finds its construct.** A site list missing `arm`,
  `compare`, `connect`, `negate`, `literal` or `call` means an operator
  stopped matching the shape it targets.
- **The last arm of a `match` is never a site.** Dropping it makes the
  match non-exhaustive, so the mutant is a compile error rather than a
  question about the suite. `classify`'s `_ -> "many"` must not appear.

Beyond the catalogue, every site must apply and reparse:

```sh
n=$(wc -l < examples/mutate/operators.sites.expected)
i=0; while [ "$i" -lt "$n" ]; do
  ./stage2/kaic2 --mutate-apply "$i" examples/mutate/operators.kai --path stdlib \
    > /dev/null || echo "site $i does not resolve"
  i=$((i + 1))
done
```

Not wired into a test tier: `kai mutate` is a diagnostic run at
discretion, not a gate.
