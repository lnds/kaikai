# pub_leak — negative companion to transitive_privacy (#565 + #2)

A consumer tries to call `leaf.private_value()` (a non-`pub` fn).
The build **must** be rejected with a diagnostic naming
`private_value`. If the build succeeds, privacy is broken.

The `.err.expected` golden contains a single substring
(`private_value`) that the runner greps for in the compiler's
stderr — exact wording can vary across diagnostic revisions but
the function name must appear.

Run:

```sh
bin/kai build examples/packages/pub_leak/consumer 2>&1 | grep private_value
```

This is the regression guard for the negative case of #565.
