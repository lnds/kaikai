# Lane retro: release.yml publish-source mirror pipeline fix

**Date:** 2026-05-16
**Branch:** `fix-mirror-pipeline`
**Trigger:** public mirror `kaikailang-org/kaikai` stuck at `VERSION=0.63.0` across 8 consecutive tag pushes (v0.63.0 → v0.68.1); integrator manually reset the mirror at 15:10 UTC and asked for a durable fix.

## Symptom

Every release since v0.63.0 produced a GitHub release on `kaikailang-org/kaikai` with darwin-arm64 tarballs (the `release` and `update-tap` jobs both succeeded), but the public source repo's HEAD never advanced past v0.63.0's commit. When the release workflow re-fired against the just-pushed tag on the public mirror, `actions/checkout@v4 with: ref: $tag` resolved `v0.68.1` to a commit whose `VERSION` file said `0.63.0`, and the `Read version` step failed:

```
##[error]tag v0.68.1 does not match VERSION 0.63.0
```

The integrator's interpretation (recorded in the brief) was that `git filter-repo` was either dropping bump-only commits or mis-anchoring tags. The hypothesis catalogue in the brief listed H1 (filter-repo re-anchors tags wrong), H2 (prune-empty collapses bump commits), and H3 (force-push race between main and tag pushes).

## Root cause

None of H1/H2/H3 fired. The actual root cause is upstream of all of them: the `Push filtered main to public` step was returning HTTP 403 on every run since the job was introduced (commit `7135b4e`, 2026-05-13). The mirror was never actually being written. The "stuck at v0.63.0" state is whatever the public repo contained when the manual `tools/publish.sh` script was last run by hand (before the automated job existed). The automation never wrote a single byte to the mirror in 8 attempted runs.

Evidence — from the failure log of run `25954150635` (v0.68.1):

```
Run git remote add public "https://x-access-token:${PUBLISH_TOKEN}@github.com/kaikailang-org/kaikai.git"
    git push public main:main --force
remote: Permission to kaikailang-org/kaikai.git denied to github-actions[bot].
fatal: unable to access 'https://github.com/kaikailang-org/kaikai.git/': The requested URL returned error: 403
##[error]Process completed with exit code 128.
```

The same 403, with the same `denied to github-actions[bot]` line, appears in the very first run after the job landed (run `25923686678`, v0.63.0). The job has a 100% failure rate. The local `git filter-repo` reproduction (script in the brief) ran cleanly against the work-repo clone — tags re-anchored correctly, `VERSION` at each tag matched its label, no empty-commit pruning fired. H1/H2/H3 are real hazards in principle but were not the bug here; the bug was 8 layers up.

**Why does the embedded PAT lose to `github-actions[bot]`?**

`actions/checkout@v4` defaults to `persist-credentials: true`. Among other things, this installs:

```
git config http.https://github.com/.extraheader "AUTHORIZATION: basic <github-actions[bot] token>"
```

That config is local to the repository checkout, but `extraheader` matches by URL host (`https://github.com/`), not by remote name. When the publish step later does:

```sh
git remote add public "https://x-access-token:${PUBLISH_TOKEN}@github.com/kaikailang-org/kaikai.git"
git push public main:main --force
```

git ignores the `x-access-token:${PUBLISH_TOKEN}@` user-info embedded in the URL and uses the `extraheader` Authorization instead — i.e. the `github-actions[bot]` token, which has no write permission on the org repo. The result: 403.

This is a well-documented hazard with `actions/checkout`; the fix is `persist-credentials: false` (or to wipe the extraheader explicitly). The `update-tap` job avoids the trap because it uses a separate `actions/checkout@v4` *with* the PAT as the token (it never touches the work-repo's checkout), so the extraheader it installs *is* the PAT and the subsequent push works.

## Why the workflow *looked* like it should work

The job reads as: clone → filter → overlay → push. The PAT is right there in the remote URL. The mental model the original author held — "the URL carries the credential" — is wrong in git's actual auth resolution order: `http.<url>.extraheader` outranks URL user-info on every push.

This is the kind of bug that escapes review because it's not a typo or a logic error; it's a default-value interaction between two unrelated tools (`actions/checkout` and `git`). Local repros pass because no `extraheader` is set on a personal machine. CI failure logs were never inspected after the first one because the *user-visible* artifact (the release page + tarballs) was correct — the `release` job's PAT-authenticated `gh` calls succeeded. The mirror failure was silent in every way that mattered to the integrator's workflow.

## Fix

Switched the job to **F3 (orphan snapshot)** from the brief, combined with the `persist-credentials: false` auth fix:

1. `actions/checkout@v4` now uses `ref: ${{ tag }}` (checks out the just-pushed tag, not `main`) and `persist-credentials: false` (no extraheader override).
2. New `Verify VERSION matches tag` step fails fast at the source if VERSION ever drifts inside the work repo — gives a clear error before any mirror state changes.
3. The overlay step now both copies the public CLAUDE.md/CONTRIBUTING.md *and* `rm -f`'s the internal-only paths from the worktree (instead of relying on `git filter-repo --invert-paths` to rewrite history).
4. The push step `git checkout --orphan public-snapshot`, commits the entire snapshot, and force-pushes that single commit as both `refs/heads/main` and `refs/tags/${tag}`.

Net effect: the public mirror is a sequence of orphan commits, one per release, where `main` always points at the most recent and the corresponding tag points at the same commit. `VERSION` at any tag is tautologically correct — the tag and `main` are *literally the same orphan commit*, which contains the snapshot of `lnds/kaikai` at that tag.

### Why F3 over F1

F1 (re-anchor tags onto post-mirror commit after filter-repo) preserves history, which has real value for users browsing the mirror. But:

- The `extraheader` auth bug also blocks F1; both fixes are needed in either path.
- After the integrator's manual reset, the mirror's history is already a single commit. F3 keeps the simpler state already on disk; F1 would require re-importing history from scratch on the next release.
- F1's complexity (compute pre-filter ↔ post-filter SHA mapping, re-tag, iterate old tags) re-introduces the surface area that has hidden bugs for 8 releases. F3 has no equivalent surface.
- Users who want full history have `lnds/kaikai`. The public mirror's job is "show people the source at the latest release"; binaries live on the Releases page either way.

### LOC

Net diff in `.github/workflows/release.yml`: −81 / +75 in the `publish-source` block. Within the brief's ~80-LOC budget; no other jobs touched.

## Verification

Local dry-run against `v0.68.1` (the just-published tag):

```
HEAD = 6144714b847dc0b94cdcb6a4fb936d712cadefcb
tag v0.68.1 = 6144714b847dc0b94cdcb6a4fb936d712cadefcb
VERSION at HEAD = 0.68.1
VERSION at tag = 0.68.1
CLAUDE.md size = 2180   # public version
CONTRIBUTING.md size = 7178   # added
lane retros tracked = 0
git log count = 1   # orphan
```

Idempotency: re-running the flow on a fresh clone produces an orphan commit with the **same tree SHA** (`2d50fc257489d9c51c984834b1dd89527344f913`). Commit SHA varies only by author timestamp, which doesn't affect the mirror's observable state because both `main` and the tag are force-pushed in lock-step.

Acceptance gates from the brief:

1. Diagnostic confirmed (auth bug, not H1/H2/H3) — ✓ above.
2. Workflow edited — ✓.
3. Dry-run validation — ✓ above.
4. Idempotency — ✓ (tree-identical).
5. No breakage of `build` / `release` / `update-tap` — ✓ (only `publish-source` changed).
6. This retro — ✓.

## Followups

- **Backfilling historical tags (v0.64.0 → v0.68.0) on the mirror**: skip. Those releases exist on the public Releases page with their darwin-arm64 tarballs (the artifacts users actually consume); the source at those tags is recoverable from `lnds/kaikai`. Bringing them onto the mirror as orphan commits would be a one-shot batch script and offers no user-visible value.
- **`tools/publish.sh`**: the script remains the "hand-run from a maintainer machine" fallback and is unchanged by this lane. It uses a different auth path (SSH to `git@github.com:...`) and a different model (filter-repo, preserves history). The CI job and the script have now diverged — the CI job is the canonical release-time path; the script is the recovery / one-shot tool. Worth a docstring update on the script noting that divergence, but not in this lane.
- **`extraheader` hazard is a general trap**: any future workflow that does `actions/checkout@v4` of the work repo and then pushes to a *different* repo on the same host (github.com) has the same risk. Worth a brief note in `CLAUDE.md`'s CI section. Deferred — not in this lane's scope.
- **Verify VERSION step is now redundant with the `build` job's identical check**: keeping it because it fires earlier in the `publish-source` flow (before any mirror state changes) and the duplication is 6 lines.

## What the brief expected vs. what the bug actually was

The brief was a thorough piece of forensic guidance — it pointed at filter-repo's empty-commit pruning, at the no-fail tag-push loop (`|| echo "::warning::..."`), and at the asymmetry between the post-filter mirror commit and the historical tags. All three are real hazards; if the auth had ever worked, at least one of them would probably have bitten next. But the actual bug was simpler and one layer up: the job never ran past line 1 of its first push. The hypothesis-driven local repro the brief asked for was the right discipline — it cleanly falsified H1 and H2 in minutes, and that falsification was the cue to stop trusting the hypothesis catalogue and read the CI failure log directly. The lesson worth carrying forward: when a brief lists three confident root causes and the local repro falsifies all of them, the next move is "read the actual error message," not "invent a fourth hypothesis."

## Cost

~30 minutes diagnosis (cloning + local filter-repo repro proved H1/H2 were wrong, log inspection found the 403, source of the 403 traced via `actions/checkout` docs + git auth resolution order). ~20 minutes implementation + local dry-run. ~25 minutes retro. Within the budget the brief implied for "diagnose first, fix second, retro third."
