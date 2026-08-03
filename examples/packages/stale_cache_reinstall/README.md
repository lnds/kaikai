# stale_cache_reinstall — regression guard for #1557

The sibling `auto_install` fixture covers a *missing lockfile*.
This one covers the other half of `needs_auto_install`: the lock
exists and pins a git dep, but the dep's cache directory is gone —
the state a user lands in after clearing `~/Library/Caches/kai`
(macOS) or `~/.cache/kai` (Linux).

The wrapper must:

1. Walk `kai.lock`, mirroring `kai-pkg`'s slug logic to derive each
   dep's cache path.
2. Notice the path no longer exists and re-run `kai-pkg install`.
3. Build + run end-to-end.

`check.sh` primes the cache, prunes it while keeping the lock, and
asserts both halves of the bug this guards:

- **No `awk:` diagnostics in the build output.** The detector
  mirrors the slug logic in an awk regex literal. An unescaped `/`
  inside its bracket expression parses under gawk but not under BSD
  awk (the macOS default), where awk exits non-zero with no output.
- **The cache is actually repopulated.** When the detector aborts,
  its consumer sees no lines, concludes "fresh" unconditionally, and
  the build fails downstream with a confusing `cannot open module`
  instead of re-resolving. A silent detector is worse than a noisy
  one, so passing assertion 1 alone is not enough.

Setup the bare repo (one-time):

```sh
tests/fixtures/git-fixtures/setup.sh
examples/packages/render-fixtures.sh
```

Then:

```sh
examples/packages/stale_cache_reinstall/check.sh
```
