#!/bin/sh
# install.sh — rustup-style standalone installer for kaikai.
#
#   curl -fsSL https://raw.githubusercontent.com/kaikailang-org/kaikai/main/install.sh | sh
#
# Downloads the latest release tarball from kaikailang-org/kaikai,
# verifies its SHA-256, extracts it to ~/.kaikai/, and adds
# ~/.kaikai/bin to PATH. Independent of Homebrew; the two coexist.
#
# This iteration supports macOS arm64 (darwin-arm64) only. Other
# platforms exit with a clear message — Linux/x86_64 are a later
# iteration. The binary is self-contained (libLLVM linked), so no
# system LLVM is required.

set -eu

REPO="kaikailang-org/kaikai"
PREFIX="${KAIKAI_HOME:-$HOME/.kaikai}"
BIN_DIR="$PREFIX/bin"

err() { printf 'install.sh: error: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*" >&2; }

need() {
  command -v "$1" >/dev/null 2>&1 || err "required tool '$1' not found in PATH"
}

# ---- platform detection ----------------------------------------------------
# Only darwin-arm64 is published today. Anything else gets a clear
# exit, not a half-working install.
detect_platform() {
  os="$(uname -s)"
  arch="$(uname -m)"
  case "$os" in
    Darwin) os_name="darwin" ;;
    *) err "unsupported OS '$os'. Only macOS (darwin-arm64) is supported in this iteration." ;;
  esac
  case "$arch" in
    arm64|aarch64) arch_name="arm64" ;;
    *) err "unsupported architecture '$arch'. Only arm64 is supported in this iteration (Linux/x86_64 come later)." ;;
  esac
  PLATFORM="$os_name-$arch_name"
}

# ---- release discovery -----------------------------------------------------
# Resolve the newest release tag via the GitHub tags API, with no `gh`
# dependency — `curl` is all an end user has. Tags are created by every
# `cz bump`, so they always exist, unlike releases/latest which 404s
# until a Release is published. Honours GITHUB_TOKEN/GH_TOKEN to lift
# the 60/h unauthenticated rate limit. On failure, writes the reason
# (network|ratelimit|notag) to the file named by $2 so the caller can
# diagnose across the command-substitution subshell.
latest_tag() {
  hdr="$1"
  err_file="$2"
  url="https://api.github.com/repos/$REPO/tags?per_page=100"
  token="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
  if [ -n "$token" ]; then
    body="$(curl -sSL -H "Authorization: Bearer $token" -D "$hdr" "$url" 2>/dev/null)" || { printf 'network' >"$err_file"; return 1; }
  else
    body="$(curl -sSL -D "$hdr" "$url" 2>/dev/null)" || { printf 'network' >"$err_file"; return 1; }
  fi

  status="$(sed -n 's/^HTTP\/[0-9.]* \([0-9][0-9][0-9]\).*/\1/p' "$hdr" | tail -n1)"
  remaining="$(sed -n 's/^[Xx]-[Rr]ate[Ll]imit-[Rr]emaining:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$hdr" | tail -n1)"
  if [ "$status" = "403" ] || [ "$status" = "429" ] || [ "$remaining" = "0" ] \
     || printf '%s' "$body" | grep -qi 'rate limit'; then
    printf 'ratelimit' >"$err_file"
    return 1
  fi

  # tr splits objects onto their own lines first: sed's greedy `.*`
  # would otherwise grab the *last* tag when the JSON is minified.
  tag="$(printf '%s' "$body" | tr ',' '\n' \
    | sed -n 's/.*"name"[[:space:]]*:[[:space:]]*"\(v[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)".*/\1/p' \
    | head -n1)"
  if [ -z "$tag" ]; then
    printf 'notag' >"$err_file"
    return 1
  fi
  printf '%s\n' "$tag"
}

# ---- checksum verification -------------------------------------------------
# The .sha256 file is GNU-style "<hash>  <filename>"; we compare only
# the hash so a renamed download still verifies.
sha256_of() {
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | awk '{print $1}'
  elif command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    err "no sha256 tool (shasum or sha256sum) found"
  fi
}

main() {
  need curl
  need tar
  need uname

  detect_platform

  tmp="$(mktemp -d "${TMPDIR:-/tmp}/kaikai-install.XXXXXX")"
  trap 'rm -rf "$tmp"' EXIT INT TERM

  info "==> resolving latest kaikai release"
  tag="$(latest_tag "$tmp/tags.hdr" "$tmp/tags.err")" || true
  if [ -z "$tag" ]; then
    case "$(cat "$tmp/tags.err" 2>/dev/null)" in
      network)   err "cannot reach github.com — check your network connection" ;;
      ratelimit) err "GitHub API rate limit reached (60/h unauthenticated). Retry in a few minutes, or set GITHUB_TOKEN." ;;
      *)         err "could not find a release tag matching v<major>.<minor>.<patch> on GitHub" ;;
    esac
  fi

  tarball="kaikai-$tag-$PLATFORM.tar.gz"
  base="https://github.com/$REPO/releases/download/$tag"
  info "==> downloading $tarball"

  curl -fsSL -o "$tmp/$tarball" "$base/$tarball" \
    || err "failed to download $tarball"
  curl -fsSL -o "$tmp/$tarball.sha256" "$base/$tarball.sha256" \
    || err "failed to download $tarball.sha256"

  info "==> verifying checksum"
  want="$(awk '{print $1}' "$tmp/$tarball.sha256")"
  got="$(sha256_of "$tmp/$tarball")"
  if [ "$want" != "$got" ]; then
    err "checksum mismatch for $tarball
  expected: $want
  actual:   $got
The download is corrupt or tampered — aborting."
  fi
  info "    ok ($got)"

  info "==> extracting to $PREFIX"
  # The tarball holds a single top-level dir (kaikai-<tag>-<platform>/);
  # unpack to a scratch dir, then atomically swap its contents into
  # $PREFIX so a re-run replaces a prior install cleanly.
  mkdir -p "$tmp/unpack"
  tar -xzf "$tmp/$tarball" -C "$tmp/unpack"
  top="$tmp/unpack/kaikai-$tag-$PLATFORM"
  [ -d "$top" ] || top="$(find "$tmp/unpack" -mindepth 1 -maxdepth 1 -type d | head -n1)"
  [ -d "$top" ] || err "unexpected tarball layout: no top-level directory"
  [ -x "$top/bin/kai" ] || err "tarball missing bin/kai"

  rm -rf "$PREFIX"
  mkdir -p "$PREFIX"
  # Move every entry (including dotfiles) from the unpacked top dir.
  (cd "$top" && tar -cf - .) | (cd "$PREFIX" && tar -xf -)

  [ -x "$BIN_DIR/kai" ] || err "install failed: $BIN_DIR/kai not executable"

  add_to_path
  info "==> installed kaikai $tag to $PREFIX"
  "$BIN_DIR/kai" --version >&2 || true

  info ""
  info "kaikai is installed. To use it in this shell now:"
  info "    export PATH=\"$BIN_DIR:\$PATH\""
  info "Open a new terminal (or source your profile) for it to persist."
  info "Upgrade later with: kai upgrade"
}

# ---- PATH wiring -----------------------------------------------------------
# Append an idempotent PATH line to the user's shell profile. We mark
# the block with a sentinel comment so a re-run never duplicates it.
# If we cannot identify a profile, we print the line for the user.
add_to_path() {
  line="export PATH=\"$BIN_DIR:\$PATH\""
  marker="# added by kaikai install.sh"

  profile=""
  shell_name="$(basename "${SHELL:-}")"
  case "$shell_name" in
    zsh)  profile="${ZDOTDIR:-$HOME}/.zprofile" ;;
    bash)
      if [ -f "$HOME/.bash_profile" ]; then profile="$HOME/.bash_profile";
      else profile="$HOME/.profile"; fi ;;
    *)    profile="$HOME/.profile" ;;
  esac

  if [ -z "$profile" ]; then
    info "Could not determine a shell profile. Add this line manually:"
    info "    $line"
    return 0
  fi

  if [ -f "$profile" ] && grep -Fq "$marker" "$profile" 2>/dev/null; then
    info "    PATH already configured in $profile"
    return 0
  fi

  {
    printf '\n%s\n' "$marker"
    printf '%s\n' "$line"
  } >> "$profile"
  info "    added $BIN_DIR to PATH in $profile"
}

main "$@"
