# Lane experience — issue-279-bin-kai-stdlib-path

## Objective metrics

- Branch: `issue-279-bin-kai-stdlib-path`
- Base: `main` at `10d80e1` (`bump: version 0.39.0 → 0.40.0`)
- Start: `2026-05-05T08:46:49-04:00`
- End: `2026-05-05T08:59:42-04:00`
- Wall: ~13 min (vs 60 min calibrated budget; well under).
- LOC touched: `bin/kai` 1 line, `Makefile` 1 phony list + 1 dep + 21-line target, 2 new files under `examples/imports/`.

## Diagnosis

Issue #279: `bin/kai` line 469 invoked `kaic2` with `--path "$ROOT/stdlib"`
literally, ignoring the already-resolved `$STDLIB_ROOT`. In the
dev layout this happened to work (line 61 sets
`STDLIB_ROOT="$ROOT/stdlib"` so the two paths coincide). In the
installed (brew) layout `$ROOT` is `<prefix>/libexec/` and the real
stdlib lives under `<prefix>/libexec/share/kaikai/stdlib/` (line 68).
Result: any source using a non-prelude stdlib import — `import loop`,
`import reader`, etc. — failed with `cannot open module ...` on a
fresh `brew install lnds/kaikai/kaikai`.

`KAI_STDLIB` does redirect `STDLIB_ROOT` (line 77), but the bug on
line 469 ignored that redirection too, so the env-var workaround
documented in the wrapper header was also broken for non-prelude
imports. Prelude paths worked because they go through dedicated
`--prelude` flags built from `$STDLIB_ROOT` correctly.

The bug shipped in 0.40.0 because the pre-release smoke tests
exercised only prelude imports. No CI fixture invoked the wrapper
in the installed layout against an `import` statement.

## Fix

```diff
-  (unset KAI_TRACE_RC; "$KAIC2" --path "$ROOT/stdlib" --path "$src_dir" $prelude_flags "$@" "$src") > "$tmp/out.c"
+  (unset KAI_TRACE_RC; "$KAIC2" --path "$STDLIB_ROOT" --path "$src_dir" $prelude_flags "$@" "$src") > "$tmp/out.c"
```

Single-line change at `bin/kai:469`. Dev layout unchanged because
`STDLIB_ROOT` evaluates to the same string. Installed layout now
resolves the stdlib correctly, and `KAI_STDLIB` overrides become
honoured for `import` resolution as well.

## Regression fixture shape

`examples/imports/import_loop_basic.kai` mirrors the issue repro
exactly: `import loop` + `until { @i == 0 } { ... }`. Output golden
is `3\n2\n1\n`.

Wired in `Makefile` as `test-import-stdlib`, modelled on
`test-multi-module` because both targets test driver-level (not
kaic2-level) behaviour by invoking `bin/kai run` directly. The
target is added to top-level `test`, so it runs in `tier1`.

The fixture exercises the wrapper's `--path` argument construction
end-to-end on every CI run, preventing a regression that bypasses
kaic2's own tests.

## Brew smoke test

```sh
./scripts/build-release.sh /tmp/lane-279-dist
mkdir /tmp/lane-279-extract
tar -xzf /tmp/lane-279-dist/kaikai-v0.40.0-darwin-arm64.tar.gz \
  -C /tmp/lane-279-extract
/tmp/lane-279-extract/kaikai-v0.40.0-darwin-arm64/bin/kai \
  run /tmp/lane-279-test.kai
```

Output:

```
3
2
1
```

The extracted layout has no `stdlib/` at root; only
`share/kaikai/stdlib/loop.kai`. Pre-fix `bin/kai` would resolve
`import loop` against the non-existent `$ROOT/stdlib/` and fail.
Post-fix it walks `$STDLIB_ROOT=$ROOT/share/kaikai/stdlib` and
resolves correctly.

## Friction points

None of substance. One minor hiccup: I drafted the regression
fixture using `//`-style comments and Unicode em-dash `—`, both of
which the kaikai lexer rejects. Rewrote with `#` comments and ASCII
punctuation (consistent with `examples/loop/main.kai`). No code
change implication — comment style was the only issue.

A second small note: `selfhost-llvm` is reachable only via
`make -C stage2 selfhost-llvm`, not as a top-level target. First
attempt with `gmake` failed (not installed on this host); switched
to `make -C stage2`. Recorded as a 0s FAIL in the build TSV before
the successful retry.

## Subjective summary

A textbook one-line shipping fix: clear repro from #279, mechanical
diff, layout invariant preserved by construction (dev layout is
unchanged because `STDLIB_ROOT` already resolves to `$ROOT/stdlib`
there). The interesting design moment was deciding where to put
the regression fixture: an existing `examples/loop/main.kai` ran
through stage2 directly (`stage2/Makefile test-loop`), but the
bug was in the wrapper, not kaic2 — so a stage2-level fixture
would not have caught it. Modelling the new target on
`test-multi-module` keeps the test at the wrapper level where the
bug actually lives.

All gates green:

- tier0: 48s OK
- tier1: 319s OK (includes new `test-import-stdlib`)
- tier1-asan: 52s OK
- selfhost: 30s OK
- selfhost-llvm: 41s OK (byte-identical fixed point both backends)
- brew layout smoke test: output `3 2 1` from staged tarball

## Limitations

Post-merge, the orchestrator must:

1. Cut a `0.40.1` patch tag via `cz bump --increment PATCH --yes`.
2. Push tag to fire the release workflow that produces the
   `v0.40.1` tarball.
3. Update `lnds/homebrew-kaikai/Formula/kaikai.rb` with the new
   version + sha256 of the v0.40.1 tarball.
4. Verify with `brew upgrade kaikai` that the fix lands in the
   tap.

This lane does NOT touch the brew formula or VERSION/CHANGELOG —
those are integrator-side after merge per CLAUDE.md.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-05T08:50:22-04:00	tier0	OK	48
2026-05-05T08:55:47-04:00	tier1	OK	319
2026-05-05T08:56:51-04:00	tier1-asan	OK	52
2026-05-05T08:57:27-04:00	selfhost	OK	30
2026-05-05T08:57:39-04:00	selfhost-llvm	FAIL	0
2026-05-05T08:58:26-04:00	selfhost-llvm	OK	41
```

(The `selfhost-llvm FAIL 0` line is the bookkeeping entry for the
attempted `gmake` invocation — `gmake` not installed on macOS host;
retried with `make -C stage2 selfhost-llvm` which succeeded in
41s. Kept in the TSV for honesty.)
