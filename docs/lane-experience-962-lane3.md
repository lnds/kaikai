# Lane experience — #962 Lane 3: the differential independence oracle

Lane 3 of the four-lane sequence behind #962 (batch-mode stdlib
typecheck). The two contamination cuts (Lane 1 root_fns isolation in
`infer.kai`, Lane 2 `lower_protocols` proto-op filter in `protos.kai`)
are in main. This lane builds the standing proof those cuts hold: a
gate that typechecks core with and without an adversarial user file and
requires a byte-identical core typed AST. It is the BARRIER before Lane
4 (batch mode). It does NOT close #962 (`refs #962`, Lane 3).

## Scope as planned vs as shipped

**Planned (issue #962 soundness gate):** typecheck each core module
with and without an adversarial user file, require a byte-identical
delta; RED if any core module diverges. Wire to CI. Adversarial
fixtures exercising both cut routes. Prove RED by reverting each cut.

**Shipped:** exactly that. One new compiler flag (`--dump-typed-core`,
~40 LOC reusing the existing `dump_typed_*` walker), four adversarial
fixtures (a benign/adversary pair per lane), one runner
(`tools/independence-oracle.sh`), one Makefile target wired into
`tier1` + `tier1-shard-3`. No typer behaviour change — the flag is a
read-only dumper; the cuts it watches were already in main.

## The one design question that shaped everything

**The contamination is NOT observable via the program's stdout.** Lane
1's retro is explicit: core has no bare call that needs narrowing, so a
root-fn shadow shifts an *imported module's* typed AST without changing
any user-visible output. An oracle that compared program output would
be green on contaminated core — useless. So the oracle MUST compare the
**typer's output** (the resolved type of every core AST node), not
execution.

The compiler already had `--dump-typed`, which prints `tag_of_kind +
ty_tag` per node — exactly the typer output wanted. But it filters to
the root file (`module_origin = None`), hiding core. The oracle needs
the *opposite*: dump core (`Some(mod)`), hide the user. `--dump-typed-core`
is that one inverted predicate over the same walker.

## The shape (asu's ruling, verified empirically)

Two candidate granularities: (a) whole-program typed dump, core decls
only — run `infer_program` on `core ++ user` and dump the `Some(mod)`
decls; (b) per-core-module isolation. asu ruled (a), with the load-
bearing correction: **never typecheck "core alone".** Both sides are
`core ++ user`; the *only* variable is the user file (benign vs
adversary). That dissolves the "is core-alone a valid typecheck unit?"
trap entirely — core resolves its cross-module refs against the real
joined stream (`driver.kai` `merged_raw = prelude ++ user`) exactly as
in a normal build. Per-module isolation (b) is *less* faithful: it
tests a regime the real compiler never runs.

The benign and adversary user files differ by **exactly one atom** —
the contaminating identifier:

- **Lane 1:** root fn named `tag` (collides with the bare `tag(...)`
  inside imported `lib.driver`) vs `tagx` (collides with nothing).
- **Lane 2:** protocol op `is_space` (collides with core
  `string.kai`'s bare `is_space(c)`) vs `is_spacey` (collides with
  nothing).

So a divergence in the core dump can only come from the contaminating
atom reaching into core — never from incidental differences.

## The RED rule

A lane is contaminated iff `(benign exit) != (adversary exit)` OR
`(both exit 0 AND the two core dumps byte-differ)`. The two cut shapes
produce *different* RED signals and the rule covers both:

- **Lane 2 reverted** → `lower_protocols` rewrites core `string.kai`'s
  bare `is_space(c)` to the user dispatcher. Measured: both sides still
  exit 0, but the core dump diverges — `ident is_space : (Char) -> Bool`
  (benign) vs `ident __proto_is_space : ?` (adversary), at three sites
  (`trim`/`lines`). So the **byte-diff** arm catches it, silently: the
  user op reached into a core body and shifted its resolved types
  without rejecting the program. (The asymmetric-exit arm exists for
  the stronger case where the rewritten core body fails to typecheck.)
- **Lane 1 reverted** → the root fn leaks into `lib`'s env,
  `lib.driver`'s `tag("hi")` mis-resolves to `(Int) -> String`, which
  in this fixture also surfaces as a type mismatch *inside the imported
  module* → **exit asymmetry** (benign=0 adversary=1). The byte-diff
  arm is the general guarantee for any silent `ty_tag` shift that does
  not also error.

Between the two reverts, BOTH arms of the RED rule fire on real
contamination — exit asymmetry (Lane 1) and silent byte-diff (Lane 2) —
which is why neither arm is dead.

Self-vs-self, no committed golden: the reference is the benign run of
the same compiler, so a legitimate stdlib edit that changes core's
types moves both sides together and the oracle stays green. It only
fires on user→core asymmetry.

## Verification — RED on revert, GREEN restored (the honest proof)

The oracle is worthless if it cannot detect real contamination, so both
reverts were run, not argued:

- **Cuts in place:** `oracle OK lane1` (4661 lines byte-identical),
  `oracle OK lane2` (4651 lines) → GREEN.
- **Lane 1 cut reverted** (`is_target_bucket` forced true): `oracle RED
  lane1 — exit asymmetry (benign=0 adversary=1)`, the adversary's
  `lib.driver` `tag("hi")` mis-resolving to `(Int) -> String`; lane2
  still OK. RED, on the right lane.
- **Lane 2 cut reverted** (`core_ops` → `raw_ops` in `shadow_for_mo`):
  `oracle RED lane2` ; lane1 still OK. RED, on the right lane.
- **Both cuts restored:** GREEN again, byte-for-byte the original dump.

So the oracle is discriminant by construction: it goes RED iff a cut is
absent, on exactly the lane whose cut is absent.

## Structural surprises

- `--dump-typed`'s module-origin filter is the whole mechanism, and it
  was *backwards* for the oracle's purpose by design (it focuses the
  user-facing dump on what the user wrote). Inverting it is trivial,
  but recognising that the existing dump hides the very thing the
  oracle must see — and that no userland-observable proxy is sound
  (Lane 1's contamination never reaches user-visible types) — is the
  load-bearing insight. A pure-shell oracle over `--dump-typed` would
  silently under-test.
- The dispatch already gates every dump on `check_program` first, so
  the Lane-2 "core rejected" shape falls out for free as a non-zero
  exit — no special-casing in the runner.

## Fixtures + coverage

- `examples/oracle/lane1/` — `benign.kai`, `adversary.kai`, and a
  shared probe library `lib/{lib,tag_str,tag_int}.kai` both import
  (so its modules are imported `Some(mod)` and appear in the core dump).
- `examples/oracle/lane2/` — `benign.kai`, `adversary.kai` (the
  collision is against core `string.kai`'s own bare call; no extra
  library).
- `tools/independence-oracle.sh` — the runner (RED rule above).
- `make test-independence-oracle` (root + stage2), in `tier1` and
  `tier1-shard-3`.

No `.out.expected` golden: the oracle is self-vs-self, so there is no
static file to drift.

## Gates

- tier0 green; selfhost byte-identical (kaic2b.c == kaic2c.c) — the
  flag is a pure dumper, no codegen change.
- `make test-independence-oracle`: GREEN with cuts; RED with either
  revert (verified, then restored).
- Backend parity serial (`BACKEND_PARITY_JOBS=1`): green.

## Follow-ups for next lanes

- **Lane 4** (batch mode, closes #962): one typecheck, N files against
  the live `TyEnv`. The oracle is now the standing guarantee that
  reusing the once-typed core is sound — if batch mode ever
  reintroduces a user→core leak, this gate goes RED before it lands.
- The oracle currently exercises the two known routes. A future
  contamination path (a third leak a later typer refactor introduces)
  needs its own benign/adversary pair added under `examples/oracle/`;
  the runner globs nothing, so a new lane is a one-line `run_lane`.
