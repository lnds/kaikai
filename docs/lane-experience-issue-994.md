# Lane experience — issue #994: rustup-style installer + `kai upgrade`

## Scope as planned vs. as shipped

Planned: a `curl | sh` `install.sh`, a `kai upgrade` self-update
subcommand, Homebrew coexistence, macOS arm64 only.

Shipped exactly that, in four blocks:

1. `install.sh` at repo root — detect platform, fetch latest release,
   verify SHA-256, extract to `~/.kaikai/`, idempotent PATH wiring.
2. `kai upgrade` in `bin/kai` — version compare, download + verify +
   in-place replace; brew refusal; distinct from `kai update`.
3. Symlink-following in the wrapper's own path resolution (needed for
   the brew check; folded into block 2).
4. Docs — README Install section + `docs/info/install.md`.

No scope drift. Linux/x86_64/darwin-x86_64 stayed out, with a clear
exit message rather than a half-implementation.

## Design decisions

- **No `gh` dependency in install.sh or `kai upgrade`.** An end user
  has `curl`, not `gh`. Both resolve the latest tag via the GitHub
  REST API (`releases/latest`) and a one-line `sed` over the JSON. The
  repo's own tooling uses `gh`, but the user-facing path must not.

- **Consume the existing release asset, build nothing.** `release.yml`
  already publishes `kaikai-v<ver>-darwin-arm64.tar.gz` + `.sha256` on
  each `cz bump`. The installer just downloads and lays it out. The
  tarball already ships `docs/info/*` as `share/kaikai/info/`, so
  `kai info install` travels with no packaging change.

- **brew detection by resolved path under a Cellar.** `kai upgrade`
  resolves its own wrapper path (following symlinks) and refuses if it
  sits under `*/Cellar/*`. This is robust to both `/opt/homebrew`
  (arm64) and `/usr/local` (Intel) prefixes without hardcoding either.

- **Symlink-following in the startup path resolution.** The wrapper
  resolved `$0` to an absolute path but did NOT follow symlinks, so a
  brew binstub (`/opt/homebrew/bin/kai` → `../Cellar/.../bin/kai`)
  would compute `$ROOT` as the bin dir, not the Cellar root, and fail
  layout detection. The brew-coexistence requirement forced fixing
  this — a real latent bug for any symlinked install, not just upgrade.

- **In-place swap replaces `bin`/`libexec`/`share` wholesale** rather
  than merging file-by-file, so a file removed in a newer release does
  not linger. `install.sh` goes further and `rm -rf "$PREFIX"` first
  (a fresh install should be pristine); `kai upgrade` keeps the prefix
  and swaps the three dirs (it must not delete a prefix the user may
  have customized around).

## Structural surprises

- The wrapper's path resolution was symlink-naive. Caught only because
  the brew test invoked `kai` through a symlink and layout detection
  failed before reaching `cmd_upgrade`. A test that called the Cellar
  binary directly would have masked it.

- `releases/latest` flipped from v0.94.0 to v0.95.0 mid-lane (an
  automated release fired). This validated that nothing hardcodes a
  version: the final verify followed the new tag automatically.

## Verification (all on this macOS arm64 host, real downloads)

- Fresh `install.sh`: downloads, verifies checksum, extracts, wires
  PATH; installed `kai --version` runs and a real `.kai` program
  compiles + executes.
- Idempotency: second `install.sh` run leaves exactly one PATH line.
- Tampered checksum: rejected, exit 1, no partial install left behind.
- Unsupported platform (faked Linux / x86_64 `uname`): clear exit 1.
- `kai upgrade` older→latest: in-place replace, version reflects it,
  still compiles afterward.
- `kai upgrade` up-to-date: reports and exits 0.
- `kai upgrade` on a brew-style symlinked Cellar: refuses, points to
  `brew upgrade`, exits 0, binary untouched (byte-identical).
- `tier0` green (selfhost deterministic) after the wrapper change.

## Coverage gaps / follow-ups

- No automated harness exercises `install.sh` / `kai upgrade` (the
  flow needs network + a real release). Verification is manual,
  documented above. A CI smoke that installs a pinned tag into a temp
  prefix is a candidate if the wrapper grows further.
- Linux + x86_64 + darwin-x86_64 are the obvious next iteration: add
  the platform matrix to `release.yml`'s tarball build, then drop the
  platform guards in `install.sh` / `cmd_upgrade`.
- `kai self uninstall` was explicitly out of scope; still unbuilt.
