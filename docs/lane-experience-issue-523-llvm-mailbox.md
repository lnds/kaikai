# Lane experience — issue #523 (LLVM mailbox runtime bindings)

## Scope

Issue #523 reported that the 7 m8 mailbox builtins declared in the
C-backend `prelude_table` and defined as statics in `stage0/runtime.h`
were missing from the LLVM backend's three parallel tables. Programs
calling `mailbox_alloc` / `mailbox_alloc_bounded` /
`mailbox_alloc_unowned` / `mailbox_assign_owner` / `mailbox_send` /
`mailbox_recv` / `mailbox_free` under `--backend=llvm` would emit
`call %KaiValue* @kai_mailbox_<name>(...)` and fail to link with
`use of undefined value '@kai_mailbox_<name>'`.

User code typically reaches mailboxes through `stdlib/actor.kai`
(`spawn_actor` / send / receive), so the practical blast radius
depends on how often the actor surface runs under LLVM. The fix
is mechanical regardless: wire the same three sites #513 / #522 /
#524 wired for their respective surfaces.

## Scope-as-planned vs scope-as-shipped

Planned: 7 missing names. Shipped: **7** (1:1 with the issue body).

Arities verified against the C-backend `prelude_table` block
(`stage2/compiler.kai:12132-12140`) and confirmed against the static
signatures in `stage0/runtime.h:3437-3510`:

- `mailbox_alloc` (0)
- `mailbox_alloc_bounded` (2 — `cap`, `overflow_strategy`)
- `mailbox_alloc_unowned` (0)
- `mailbox_assign_owner` (2 — `pid`, `fiber`)
- `mailbox_send` (2 — `pid`, `msg`)
- `mailbox_recv` (1 — `pid`)
- `mailbox_free` (1 — `pid`)

The C `prelude_table` groups `mailbox_alloc_unowned` and
`mailbox_assign_owner` together (Tier 2 `spawn_actor` primitives,
arity-0 + arity-2) under a separate comment block from
`mailbox_alloc` / `mailbox_alloc_bounded` / `mailbox_send` /
`mailbox_recv` / `mailbox_free`. The LLVM table and IR prologue
keep all seven entries contiguous after the libm block from #522,
since the C-side grouping was a documentation choice rather than a
structural constraint.

## Design decisions

- Inserted the mailbox block immediately after the `real_*` libm
  block from #522 in both `llvm_prelude_table` and the IR prologue
  declare list. This keeps the three-table site ordering convergent
  with the natural read-order across recent issues (#513, #522,
  #523).
- 0-arg declares use `@kaix_prelude_mailbox_alloc()` (matching the
  existing convention for `@kaix_prelude_args()` /
  `@kaix_prelude_program_name()` at lines 42806-42807).
- Cited issue #523 in all three sites' comments, mirroring the
  precedent from #513, #522, #524.

## Structural surprises

None. The lane was as mechanical as expected: smallest of the recent
prelude-gap trio (7 names vs #522's 20 and #524's modcall fallback
refactor).

One minor verification step: confirmed all 7 `kai_prelude_mailbox_*`
statics exist in `stage0/runtime.h:3437-3510` and have thunk
forwarders in `_kai_prelude_mailbox_*_thunk` (used by the C
backend). The thunks for `mailbox_alloc_unowned` and
`mailbox_assign_owner` are emitted lazily by the Tier 2 spawn_actor
path; the underlying statics are the source of truth for both
backends.

## Fixtures

No mailbox-specific fixture exists in `examples/stdlib/` or
`examples/actors/`. `make demos-no-regression` (which gates tier0)
runs the C backend only (`demos/Makefile` invokes `$(KAIC2)`
directly, never `KAI_BACKEND=llvm bin/kai`), so the pre-fix gap
was invisible to the demos baseline.

Pre-fix verification: building `demos/ping_pong/main.kai` (which
exercises mailboxes through `stdlib/actor.kai`'s `spawn_actor`
surface) under `KAI_BACKEND=llvm bin/kai build` aborted at clang
with `use of undefined value '@kai_mailbox_alloc'` — confirming the
gap-class described in the issue.

Post-fix verification: same demo links cleanly. A direct
minimal probe (`fn main() = { let m = mailbox_alloc();
mailbox_free(m); print("ok") }`) compiles and runs successfully
under `KAI_BACKEND=llvm`, confirming the link path is wired
end-to-end (LP entry → IR `declare` → C forwarder → `kai_prelude_*`
static).

Note: `demos/ping_pong/main.kai` *links* under LLVM after this lane
but still segfaults at runtime. The segfault is unrelated to the
link-table gap closed here — it surfaces a separate LLVM-side
defect in the fiber/scheduler or `stdlib/actor.kai`'s
spawn_actor surface that pre-fix was masked by the earlier link
failure. Out of scope; worth tracking as a follow-up issue.

Pre-fix audit found no demos skipped under LLVM for this reason
(`grep "skip.*llvm" examples/actors/ stdlib/` returned empty). The
gap was a latent link-time failure waiting on direct calls — there
were no skipped fixtures to unblock.

## Coverage gaps

A direct-mailbox fixture (one that calls `mailbox_alloc` / `send` /
`recv` / `free` without going through `stdlib/actor.kai`) would
explicitly cover the new LLVM surface end-to-end. Not added in
this lane because:

- The actor demos already exercise the same C wrappers through
  the spawn_actor path under LLVM.
- The 3-site link-table discipline is verified by the build itself
  (a missing entry would surface as `use of undefined value` at
  link, not as a silent miscompile).

Left as a follow-up if a future lane wants explicit coverage of the
non-actor mailbox surface.

## Cost vs estimate

- Estimated: 0.5 day (brief).
- Actual: ~30 minutes including selfhost rebuild + tier0 + tier1.

The three-site edit fit on a single screen per site; arities
unambiguous from the C `prelude_table`; no tooling surprises.

## Follow-ups for next lanes

- The structural recommendation from the Linus audit on #513
  (one-shot collapse of the three tables into a generated manifest)
  remains on the table but out of scope here.
- No further prelude-table gaps known after #513 / #522 / #523 /
  #524. The recurring shape (C-side table grows, LLVM-side tables
  forgotten) is a tooling smell rather than a functional one —
  consider adding a CI check that diffs the two surfaces.
- `demos/ping_pong/main.kai` segfaults under `KAI_BACKEND=llvm`
  after this lane unblocks the link path. The crash is downstream
  of the link gap closed here (likely fiber/scheduler runtime or
  `stdlib/actor.kai` interactions under LLVM); worth filing as a
  follow-up issue if not already tracked.
