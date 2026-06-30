# install

Install and self-update the kaikai compiler.

## Description

Two ways to get a prebuilt `kai` on macOS arm64 (Apple Silicon). The
binary is self-contained — no system LLVM needed. The two methods
coexist; pick whichever fits your setup.

## curl | sh

A rustup-style installer that needs no Homebrew:

```sh
curl -fsSL https://raw.githubusercontent.com/kaikailang-org/kaikai/main/install.sh | sh
```

It downloads the latest release tarball, verifies its SHA-256 (and
aborts on any mismatch), extracts it to `~/.kaikai/`, and appends
`~/.kaikai/bin` to your shell profile's `PATH` (idempotently — re-running
is safe). Then:

```sh
export PATH="$HOME/.kaikai/bin:$PATH"   # this shell now; profile persists it
kai --version
```

Set `KAIKAI_HOME` to install somewhere other than `~/.kaikai`.

## Homebrew

```sh
brew install kaikailang-org/kaikai/kaikai
```

## Self-update

```sh
kai upgrade
```

`kai upgrade` queries the latest release and, if it is newer than the
running version, downloads + verifies + replaces the binaries in place
under `~/.kaikai/`. If already current, it says so and exits.

On a Homebrew install, `kai upgrade` does not touch the Cellar — it
prints a pointer to `brew upgrade kaikai` and exits. This is distinct
from `kai update`, which refreshes package dependencies in `kai.toml`.

## Platforms

This iteration ships darwin-arm64 only. On any other platform the
installer and `kai upgrade` exit with a clear message; build from
source (see the README) meanwhile. Linux and x86_64 are a later
iteration.
