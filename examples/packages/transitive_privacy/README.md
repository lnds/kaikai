# transitive_privacy

Three packages chained: `consumer → mid → leaf`. `mid` imports
`leaf` and exposes a `pub fn middle_api` that wraps
`leaf.public_secret()` (a `pub` fn). The `consumer` calls only
`mid.middle_api`.

Privacy is module-local: each `pub`/private decision belongs to the
declaring module, not the import chain, so a private fn declared in
`leaf` never becomes visible to `consumer` through `mid`.

Run:

```sh
bin/kai run examples/packages/transitive_privacy/consumer
```

Expected output: `mid wraps: leaf-public` (matches `consumer/main.out.expected`).
