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

Set `KAIKAI_HOME` to install somewhere other than `~/.kaikai`. Like
`kai upgrade`, the installer resolves the version from git tags and
honours `GITHUB_TOKEN`/`GH_TOKEN` if the unauthenticated API rate limit
gets in the way.

## Homebrew

```sh
brew install kaikailang-org/kaikai/kaikai
```

## Self-update

```sh
kai upgrade
```

`kai upgrade` resolves the newest version from the repository's git
tags and, if it is newer than the running version, downloads + verifies
+ replaces the binaries in place under `~/.kaikai/`. If already current,
it says so and exits.

Version discovery reads the GitHub *tags* API, not `releases/latest`.
Tags are created by every release bump, so a published GitHub Release is
not a prerequisite for `kai upgrade` (or `install.sh`) to see a new
version — only for the tarball assets it then downloads. The
unauthenticated GitHub API allows 60 requests/hour per IP; if you hit
that limit, `kai upgrade` says so and suggests retrying or exporting
`GITHUB_TOKEN` (or `GH_TOKEN`), which raises the limit to 5000/hour.

On a Homebrew install, `kai upgrade` does not touch the Cellar — it
prints a pointer to `brew upgrade kaikai` and exits. This is distinct
from `kai update`, which refreshes package dependencies in `kai.toml`.

## Build profiles

`kai build` takes a profile flag that trades compile speed, binary size,
and debuggability. All three use the native backend; the C backend ignores
them (it gets its opt level from `cc`).

```sh
kai build app.kai              # default: -O2, symbols kept, no DWARF
kai build --release app.kai    # -O2, stripped + smaller, no DWARF
kai build --debug   app.kai    # -O0, full DWARF line tables, source debug
```

- **default** (no flag) — `-O2` for fast runtime, symbols kept for cheap
  iteration. The baseline.
- **`--release`** — `-O2` and the binary is stripped, so it is smaller than
  the default. Ship this.
- **`--debug`** — `-O0` (fast compile, no inlining) and full DWARF keyed to
  the `.kai` source. `lldb`/`gdb` break on kaikai lines, and a panic prints
  a `<file>.kai:<line>` stack trace. On macOS the DWARF lands in a `.dSYM`
  bundle next to the binary; on Linux it stays in the executable.

## Platforms

This iteration ships darwin-arm64 only. On any other platform the
installer and `kai upgrade` exit with a clear message; build from
source (see the README) meanwhile. Linux and x86_64 are a later
iteration.
