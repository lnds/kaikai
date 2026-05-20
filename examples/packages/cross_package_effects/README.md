# cross_package_effects — effect crosses package boundaries

Three packages chained: `consumer → mid → leaf`.

- `leaf` declares `pub effect Counter { bump() : Int }`.
- `mid` re-uses `Counter` in a public fn `tick_three` whose
  effect row carries `/ Counter`.
- `consumer` installs a `handle ... with Counter` handler that
  threads state via a `var`, satisfying the effect row.

The package-mode build must keep the effect row attribution
correct across the chain: `tick_three`'s row references the same
`Counter` effect that `consumer` handles, even though it was
declared in `leaf`.

Run:

```sh
bin/kai run examples/packages/cross_package_effects/consumer
```

Expected output: `6` (1 + 2 + 3).
