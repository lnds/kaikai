# auto_install — regression guard for #512

A manifest declares a git-source dep, no `kai.lock` exists, no
cache directory exists. The first `kai run` invocation must:

1. Detect the missing lockfile via `needs_auto_install`.
2. Trigger `kai-pkg install` automatically before invoking
   `kaic2`.
3. Materialize `kai.lock` and the cache.
4. Build + run the program end-to-end.

`check.sh` enforces all four:
- Removes `kai.lock` and isolates the cache via `KAIKAI_CACHE`.
- Runs `kai run`; checks exit code, output, and lockfile creation.

Setup the bare repo (one-time):

```sh
tests/fixtures/git-fixtures/setup.sh
examples/packages/render-fixtures.sh
```

Then:

```sh
examples/packages/auto_install/check.sh
```
