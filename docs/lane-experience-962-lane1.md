# Lane experience — #962 Lane 1: root_fns isolation

## Scope as planned vs as shipped

**Planned (brief):** cut the `root_fns` user→core contamination path so
reusing a typechecked stdlib `TyEnv` is sound (batch-mode stdlib
typecheck, #962). Populate `root_fns` only in the target bucket, empty
for core. Leave a positive fixture. Do NOT close #962 (1 of 4 lanes).

**Shipped:** exactly that. One cut in `infer.kai:typecheck_module`, one
discriminant multi-module fixture, Makefile wiring. The fix is one
`match` on the bucket identity plus an empty-or-collect branch — ~10
lines of real change. No new module, no driver reshape.

## The contamination path, precisely

`root_fns` holds the bare names of root-file (`mo = None`) functions.
`try_bare_call_narrow` consults it: if a bare callee name is a root
shadow, it skips first-arg receiver-type narrowing so the user's decl
wins. That guard exists ONLY so user code can shadow a same-named
stdlib export.

Pre-cut, `root_fns` was computed once over `all_program_decls` and
injected into EVERY module's typing env in the fold — including
imported modules. So when an imported module's body did a bare call
with ≥2 candidates whose name matched a user root fn, the guard fired
*during the imported module's typecheck* and skipped narrowing there.
A user root fn could shift an imported module's typed AST. That is the
soundness hole: a cached/reused core typecheck is only valid if it is
independent of the user file.

The cut: seed `root_fns` only in the target bucket; `[]` for any
imported module. Directional visibility — an imported module never
observes the user.

## Design decisions and one real wrong turn

**The discriminant was inverted on the first attempt.** The brief and
my first read said "target = `mod.name == None`". That is wrong in the
common case. `partition_decls_by_home` + `bucket_append` give the
target bucket `name = Some(target_mod)`, NOT `None`. `None` only
appears in the single-bucket fallback (trivial programs with no
core/user split). My first cut seeded `[]` for the user's own bucket
and broke two existing shadowing fixtures (`length` resolved to
stdlib's `string.length` → 7 instead of the user's 42).

The correct discriminant: a bucket is the target iff `mod.name == None`
OR `Some(m)` with `m == inf_prelude_module_name(file)`. Everything else
is an imported module. Caught by running the existing
`examples/shadowing` corpus, not by reading — the partition's bucket
naming is subtle and the brief's line numbers/assumptions had drifted.

**asu validated the discriminant as sound** (no non-target bucket is
ever the user's; the asymmetry mirrors ML/Haskell module import, where
the importee typechecks in its own closed env). No hole.

## The fixture shape — why it is multi-module, not core-runtime

The brief implies a runtime fixture where core's behavior differs with
and without the cut. **That fixture cannot exist today:** no `core` body
performs a bare call that needs narrowing — core uses the primitives
(`string_length`, `list_length`) or unambiguous self-recursion. The
only core code with narrowing-eligible bare calls is the `test` blocks,
which don't run in a normal build. The contamination is real in the
typecheck but not observable via a normal build's stdout from core
alone. That is precisely WHY #962 needs Lane 3's differential oracle.

asu's resolution (option D): the cut protects EVERY imported module,
not just `core` — that core happens not to use narrowing is an accident
of its corpus, not a property of the cut. So the discriminant fixture
uses an imported *test library* that DOES depend on narrowing:

- `tag_str.tag(String)` + `tag_int.tag(Int)` — two narrowing candidates.
- `lib.driver` does bare `tag("hi")` — needs narrowing → `tag_str.tag`.
- `main` defines adversarial `fn tag(Int)` and calls `lib.driver`.

Verified empirically: revert the cut and the program fails to compile
(`tag("hi")` mis-resolves to `(Int) -> String`, type mismatch inside
`lib.driver`). With the cut: `STR:hi` (lib blind to user) + `ROOT`
(user shadow still wins in user code). Discriminant by construction.

`examples/shadowing/user_shadows_stdlib_list_fns.kai` stays as a
no-regression smoke ("user shadow still wins"); it does not prove the
cut and its own comment says so.

## Structural surprises the brief did not anticipate

- The brief's "target = None" was a real trap; the partition tags the
  target bucket `Some(target_mod)`. Trust the running code's bucket
  naming, not the brief's shorthand.
- `make kaic2` reports "up to date" and silently skips a native rebuild
  if the C binary's timestamps are newer; force native with
  `rm -f stage2/kaic2 stage2/build/{stage2.c,bundle.kai}` first.
- `test-multi-module` uses a HARDCODED case list (Makefile:55), not a
  glob — a new multi-module fixture must be added there or it never runs.

## Fixtures + coverage

- `examples/multi-module/issue-962-root-fns-isolation/` (4 files, `km`
  A++) — the discriminant. Wired into `test-multi-module` (abs+rel).
- Negative fixture: not applicable. The failure mode without the cut is
  a type-mismatch *inside an imported module*, which the positive
  fixture already exercises by being non-compilable when reverted; a
  standalone `.err` fixture would only re-test the broken state.

## Gates

- selfhost byte-identical (`kaic2b.c == kaic2c.c`).
- tier0 green; `examples/shadowing` 18/18; `test-multi-module` green.
- native-vs-c parity serial (`BACKEND_PARITY_JOBS=1`): pass=519 fail=0
  skip=64, ratchet OK (0 gaps).

## Follow-ups for next lanes

- **Lane 3** (barrier): the differential independence oracle —
  typecheck each core module with and without an adversarial user file,
  require byte-identical delta. That is the real proof both cuts
  (root_fns + lower_protocols) are user-independent. This lane's fixture
  is a targeted discriminant; the oracle is the general guarantee.
- **Lane 4**: batch mode — one typecheck, N files against the live
  TyEnv. Closes #962.
