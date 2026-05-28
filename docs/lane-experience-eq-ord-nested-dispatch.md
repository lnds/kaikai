# Lane retro: nested `==` / `<` dispatch to custom Eq/Ord (dense user head tags)

## Why

`==` `!=` `<` `>` `<=` `>=` are sugar for `Eq.eq` / `Ord.cmp` and must honour
a user `impl`. The ROOT case (the compared value's own type carries the
impl) shipped earlier via the resolver (`633a255`): with concrete types in
hand, the resolver rewrites `a == b` to `__pimpl_Eq_T_eq(a, b)`. But the
resolver only sees the type of the ROOT operand, not of the fields reached
through a container. So `W(Pt) == W(Pt)` and `[Pt] == [Pt]` (with
`impl Eq for Pt`, no impl on `Wrap`/`List`) fell back to structural
comparison and ignored `Pt.eq`.

This lane closes the NESTED case for variants and lists. Records nested as
fields stay a follow-up (see "Limitation").

## Scope as planned

The handoff laid out four steps:

- **PASO 0** — runtime dispatch hooks in `kai_op_eq` (VARIANT/RECORD) and
  `kai_op_lt`/`kai_op_gt`, dispatching via `kai_lookup_impl(PROTO,
  kai_head_tag(a))` before the structural fallback.
- **PASO 1-3** — give every user record/sum type a dense runtime head tag
  (`>= 20` = `KAI_USER_HEAD_TAG_BASE`), threaded through `variant_head_tag_for`
  (emit_shared), `stdlib_head_tag_c` (emit_c), `stdlib_head_tag_int`
  (emit_llvm), so `kai_lookup_impl` matches the impl registered under that tag.

The premise: once user types carry real head tags, the already-written
runtime hooks fire on nested fields automatically, because `kai_op_eq`
recurses per-field and re-checks the impl table at every level.

## Scope as shipped

PASO 0-3 landed as planned. But the handoff under-weighted the actual
blocker, and there were two index bugs to fix before nested dispatch fired:

1. **`collect_variant_to_head` was indexed by LIST POSITION, the runtime
   indexes by TAG.** `register_variants` head-conses user variants to the
   front of the EVar list (tags 11+), so the list order is *not* tag order.
   The old `collect_v2h_loop` emitted one head per list entry in list order,
   so slot `i` held the head of the i-th *list* entry, not of the
   constructor whose `variant_tag == i`. This was harmless while every user
   variant mapped to `KAI_HEAD_ANON` (0) — any slot read gave 0. Assigning
   real dense tags is what exposed the latent misalignment: `P` (tag 11)
   read `kai_variant_to_head[11]` and got a *builtin* head. Fix
   (asu-validated): rewrite `collect_variant_to_head` to build the array
   slot-by-slot in tag order `0..N-1` via a new `evar_name_for_tag`. Variant
   tags are dense/contiguous (`vs_to_evars_count` only consumes a tag when it
   emits an EVar; re-emitted builtins recurse without advancing), so
   `N == list_length(variants)` with no gaps.

2. **`user_ctor_type_head_tag` computed the index off the recursion tail.**
   My first cut walked `decls` and, on finding the sum owning the ctor,
   called `user_type_head_tag(dn, decls)` — but inside the recursive loop
   `decls` had already been narrowed to the tail starting at that sum, so the
   dense index restarted at 0 and every ctor resolved to 20. Fixed by
   threading the full decl list (`all`) separately from the recursion
   remainder (`rem`).

The LLVM backend had the *same* position-vs-tag bug by a different route
(`llvm_emit_variant_register_calls` used its loop counter `i` as the
variant tag). The tag-ordered array fixed both backends with no
backend-specific change — but it needed a fixture run through LLVM to
confirm, since selfhost compiles via C and never exercises that path.

## Design decisions

- **Dispatch lives in `kai_op_eq`, not in a separate value wrapper.**
  `kai_op_eq` is the non-consuming C-int equality used both by the consuming
  `kai_op_eq_v` wrapper and recursively for CONS/VARIANT/RECORD fields.
  Hooking it there means a nested field of a dispatchable type fires its impl
  on the recursive descent for free — one hook covers root and nested.

- **RC discipline (asu).** `kai_op_eq` is non-consuming, so the hook
  `incref`s `a`/`b` before the impl (which consumes) and does NOT decref.
  `kai_op_lt`/`gt` are consuming, so they incref-balance AND decref at the
  end. Validated with `KAI_TRACE_RC`: a single dispatching comparison and a
  single structural comparison both report `alloc_total=3 free_total=2
  leaked=1` — the hook adds zero leak; the 1 is the pre-existing emitter
  leak documented in the emitter-no-RC-discipline note.

- **Give EVERY user type a head tag, impl or not.** Sound because
  `kai_lookup_impl` returns NULL for a tagless-but-no-impl type, and the
  structural fallback recurses per field, so a nested field that DOES carry
  an impl still fires. Presence of an impl is `lookup != NULL`, never
  `tag >= 20`.

## Structural surprises

The handoff said the runtime hooks were "in place, only registration is
missing." They were not in place — `runtime.h` had zero `kai_lookup_impl`
callsites; PASO 0 wrote them. And the load-bearing fix turned out to be
neither the hooks nor the dense tags but the v2h table's indexing scheme,
which the handoff mentioned only as an invariant footnote ("largo ==
total variants"). The real defect was *order*, not *length*.

## Fixtures added

Extended `examples/protocols/eq_ord_sugar_custom.kai` (was root-only) with:
- `Wrap = W(Pt)` — variant-in-variant; `W(P(7,1)) == W(P(7,2))` is TRUE
  (Pt.eq ignores y).
- list of variants — `[P(1,0),P(2,5)] == [P(1,9),P(2,9)]` is TRUE.
- `Two = MkOne(Int) | MkTwo(Int)` with its own `impl Eq` — collision check:
  `MkTwo(1) == MkTwo(2)` TRUE (not hijacked by Pt.eq), `MkOne(5) != MkTwo(5)`.

`test-protocols` runs the fixture through BOTH C and LLVM and diffs the
golden, so the LLVM path is covered despite selfhost being C-only.

## Gates

`make kaic2`, `make selfhost` (C byte-identical), `make selfhost-llvm`
(byte-identical), `make tier0` (selfhost + 34 demos), `make test-protocols`
(80 OK, C + LLVM), ASAN clean on the fixture in both backends.

## Cost vs estimate

The four PASOs were mechanical (~1h). The position-vs-tag debugging — adding
a temporary debug emit to discover that `user_type_head_tag("Pt")` returned
30 while `variant_head_tag_for("P")` returned 20 with the same `decls` —
took the bulk of the lane and required an asu consult to confirm the
tag-ordered rewrite was the simplest sound fix rather than a larger refactor.

## Follow-ups left

- **Nested RECORD field.** A user record with a custom impl reached as a
  nested field still does not dispatch. `kai_head_tag` of a record reads
  `as.rec.head_type_tag`, stamped at construction (`kai_record_h`'s 4th arg);
  the runtime never learns the record's type name, so there is no
  registration path. Closing it would mean stamping the type tag at every
  record-literal emit site (~the whole record emit walker). Records never had
  nominal dispatch (not for Show either). Root records work via the resolver.
