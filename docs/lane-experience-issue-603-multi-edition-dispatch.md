# Lane retro — issue #603 multi-edition compiler dispatch

## Scope as planned

1. `bin/kai` reads `edition` from `kai.toml`, validates against
   `{tongariki, anga-roa}`, forwards `--edition <name>` to kaic2.
2. kaic2 accepts `--edition`, defaults to empty (= tongariki) if
   not provided.
3. At least one real branch point in the typer keyed off the
   edition — pipe dispatch (#594) gates user-type walk on
   `edition >= anga-roa`.
4. Cross-edition cache invalidation — cache path partitioned by
   edition under `preludes-v1/<edition>/`; header
   `cache_kaikai_version_hash` bumped to invalidate any pre-#603
   blob defensively.
5. Clear error for unknown edition values.

## Scope as shipped

All five planned items landed. The scope held: I did not thread the
edition parameter beyond the one branch point identified in the
brief.

The lane benefitted from #594 having already been merged into main
the same day. The `build_head_owner_map` function (introduced by
#594) was the natural gate site: under tongariki it returns only
the seeded `[HOE("List", ["list"])]` entry, which is byte-equivalent
to what the pre-#594 hardcoded `head_module_for` produced. Under
anga-roa, the full `from_decls` walk over user `pub type` heads
runs. The legacy `head_module_for` function was NOT reintroduced —
the seeded entry inside `build_head_owner_map` already discharged
that responsibility once #594 landed.

## Design decisions

**Cache partition by path, header bump as defense-in-depth.** The
brief suggested folding the edition string into the SHA used to
key cache files. I chose path-partition instead because the prelude
content itself does not vary by edition — only the compiler's
behaviour does. Path partition + `cache_kaikai_version_hash` bump
(from 1 to 2) gives the same effect (no cross-edition contamination)
without restructuring `cache_file_header` or `cache_read_header`.
A future lane that adds edition-specific stdlib will need to
revisit this if preludes themselves diverge.

**`edition_rank` instead of an enum.** Strings carry through CLI,
TOML, env vars, and serialised cache headers as strings. Wrapping
them in a `type Edition = ETongariki | EAngaRoa` would force every
caller to convert. The `edition_rank("tongariki") = 0`,
`edition_rank("anga-roa") = 1` mapping keeps the comparison
`>=` in one function and lets the rest of the typer pass the raw
string through. Adding Orongo is one new arm in `edition_rank` plus
a new entry in `known_editions()`.

**Empty edition string == tongariki at the typer.** Old harnesses
that bypass `bin/kai` (e.g. `examples/sugars/*` invoked directly
through kaic2) never pass `--edition`. The typer treats the empty
string as the oldest edition (rank 0) so they keep producing the
pre-#603 pipe dispatch behaviour. The validation rejection on
unknown values fires only for explicit non-empty values that fail
the known-set check, which is exactly the user-facing case.

**No CLI change to error text.** Following the precedent of
`--package-name` and `--unstable-optin`, an unknown `--edition`
value prints the diagnostic to stderr then `cli_with_mode(opts,
MHelp)` so the usage text follows. The `.err.expected` golden
captures only the first line; the usage tail is variable and is
not part of the contract.

## Structural surprises

**The driver's `prelude_cache_dir` is reached before the manifest
is resolved.** `stdlib_prelude_flags` is called from
`compile_to_binary` and `report_holes` at a point where the
manifest path is already known (or known-absent). I resolved the
edition value in those two call sites, then plumbed it as an
explicit argument through `stdlib_prelude_flags →
prelude_flag_for → prelude_cache_lookup / prelude_cache_build →
prelude_cache_dir`. Each function accepts the edition as an
optional second argument with a `read_edition` default so legacy
call sites (and the `$KAI_PRELUDE_CACHE_DIR` debug override) keep
working.

**CliOptions has 11 nested constructors.** The `cli_with_*` setter
pattern means every field added requires updating every setter.
The mechanical sweep (`unstable_optins: opts.unstable_optins }`
→ `unstable_optins: opts.unstable_optins, edition: opts.edition }`)
landed all nine call sites in one `replace_all`. The tenth and
eleventh constructors (`cli_push_unstable_optin` and `parse_cli`)
needed individual edits. A future lane that adds a 13th field
should think about whether the record-update sugar would simplify
this; it would have shaved ~30 lines off this lane.

## Fixtures

Five fixtures under `examples/editions/`:

- `edition_anga_roa_pipe/` — `pub type Box[a]` + `pub fn map` in
  `box.kai`; `main.kai` does `b | { x -> x + 1 } | { x -> x * 2 }`
  which routes through the convention dispatch. Output `42`.
- `edition_tongariki_pipe/` — `[1,2,3,4] | { x -> x * 10 } | { x ->
  x + 1 }` routes through the seeded `List → list` entry. Output
  `11 21 31 41`.
- `edition_unknown_error/` — `edition = "atlantis"` produces the
  load-bearing rejection text; `.err.expected` captures the first
  line.
- `edition_missing_field/` — `kai.toml` without an `edition` line
  defaults to the repo `EDITION` value (today: tongariki). Pipe
  dispatch over `[a]` still works.
- `edition_cache_invalidation/check.sh` — sets a private
  `$KAI_PRELUDE_CACHE_DIR`, compiles under each of `{tongariki,
  anga-roa}`, asserts the two subdirectories appear with non-empty
  cache contents.

`examples/editions/README.md` documents the matrix and runs the
fixtures from the repo root.

## Coverage gaps

- The unknown-edition fixture exercises the error message but does
  not pin behaviour for edge cases (empty string, leading/trailing
  whitespace, value with quotes). The TOML parser strips quotes; the
  awk parser tolerates whitespace; no fixture exercises that.
- The cache-invalidation script verifies the directory structure
  but does not verify hash-header rejection if a `.kab` is moved
  between edition directories manually.
- `examples/editions/*` fixtures are not wired into stage2/Makefile
  CI, matching the `examples/unstable/*` precedent. They are smoke-
  testable by hand. A follow-up to wire `examples/{editions,
  unstable}` into a `test-driver-manifests` target is filed.

## Real cost vs estimate

Brief estimate: 2–4 sessions, 600–900 LOC. Real cost: one session,
~370 LOC of code plus ~180 LOC of fixtures + docs. The 9 mechanical
record-update sites were the largest single block; everything else
was small targeted edits.

## Follow-ups deferred to Orongo

1. **`kai migrate --from <old> --to <new>`** — codemod that walks
   a package and rewrites the `kai.toml` edition field plus any
   surface affected by the bump. Pre-Orongo there are no breaking
   changes between editions that justify the tooling.
2. **Edition threading through every typer decision point.** The
   one branch point (#594 pipe dispatch) is enough for this lane.
   When Orongo introduces breaking surface changes (e.g. retiring
   a syntax form), the parser / resolver will need to consult the
   edition similarly.
3. **`stdlib_prelude_flags` could be table-driven.** The 27
   per-prelude `prelude_flag_for "$X" "$ed_arg"` lines are a
   shell-level repetition that an array iteration would shorten,
   but POSIX sh's array story is weak. A future bash-or-better
   rewrite is one option; another is generating bin/kai from a
   higher-level spec.
4. **`gh issue #585` cache-layer blocker.** The cache layer remains
   load-bearing; if #585 reopens, the edition partition still works
   but cache-roundtrip tests need a new fixture covering
   edition-sensitive blobs.
5. **kai-pkg paths segfaults when `[dependencies]` is absent.**
   Surfaces as a stderr `Segmentation fault: 11` line when running
   any fixture with a manifest-only-with-name kai.toml. The
   compilation completes successfully (the segfault is in a path-
   resolution helper that has no output to contribute), but the
   noise pollutes terminals. Filed as a separate issue; not in
   #603 scope.
