# transitive_privacy — regression guard for #565

Three packages chained: `consumer → mid → leaf`. `mid` imports
`leaf` and exposes a `pub fn middle_api` that wraps
`leaf.public_secret()` (a `pub` fn). The `consumer` calls only
`mid.middle_api`.

The bug guarded against (#565): the privacy check leaked across
module boundaries, so a private fn declared in `leaf` could end
up visible to `consumer` through the import chain. The fix made
privacy module-local: each `pub`/private decision belongs to the
declaring module, not the import chain.

Run:

```sh
bin/kai run examples/packages/transitive_privacy/consumer
```

Expected output: `mid wraps: leaf-public` (matches `consumer/main.out.expected`).
