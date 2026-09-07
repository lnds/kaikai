# examples/mutate

Fixtures for `kai mutate`.

`operators.kai` carries one construct per mutation operator;
`operators.sites.expected` is the site catalogue it must produce.

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
