# stdlib_across_deps — stdlib visible in every package of the chain

Three packages chained: `consumer → mid → leaf`. Each one
uses stdlib functions (string_concat, string_length, int_to_string,
list construction + pattern matching) directly, without
re-declaring them.

The point: the prelude / stdlib auto-loading reaches every
package in the resolved chain, not only the entry-point
package. If any layer had to import or re-declare stdlib pieces,
the cost of layered libraries (ahu, henua, kohau) would balloon.

Run:

```sh
bin/kai run examples/packages/stdlib_across_deps/consumer
```

Expected output:

```
leaf<a>[1] | leaf<bc>[2] | leaf<def>[3]
```
