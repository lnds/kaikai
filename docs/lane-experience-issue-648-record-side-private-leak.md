# Lane experience report — issue #648 record-side private-type leak

Best-effort retrospective by the implementing agent.

## Goal

Close issue #648 — *typer: record-side private-type shadow (rec_find
lacks arity discriminator)*. PR #646 made the variant-table lookup
arity-aware (issue #643); the record-table equivalent was left
intact, so a user `type Thread = { id: Int, name: String }`
colliding with `stdlib/regexp.kai:982`'s private
`type Thread = { state_id: Int, captures: [Capture] }` got the
prelude's field set surfaced on the user's value:

```
error: no field 'name' on Thread
  --> repro:6:24
```

## Path taken — arity discriminator + field-set tie-breaker

The brief recommended **Path 1 (arity-aware `rec_find`)** as a
mechanical mirror of PR #646's variant-side fix. Path 1 alone
closes the diff-arity case but does not separate two arity-0
records sharing a name — and `Thread` is exactly that shape: both
declarations have arity 0. The variant-side equivalent (`SplitN`,
`NB_FragR`) sits in the audit's skip-list as a documented
limitation; the goal here was the same shape NOT remaining
broken.

The way out was a second discriminator the variant side did not
need: the **field-set**. Two same-(name, arity) records differ
in their `[FieldDecl]`, and the typer's access sites already know
the field name they are reading or writing. The fix introduces
`rec_find_with_field(rs, target, arity, field)` which prefers a
`RI` whose declared field set includes `field`. Wired through:

- `synth_field` (`stage2/compiler.kai:32340`) — reads the
  scrutinee's `TyCon` arity and the requested field name.
- `record_has_field` (`stage2/compiler.kai:31839`) — same shape;
  used by `try_ufcs_call` to honour field-wins-on-ties.
- `synth_record_lit` (`stage2/compiler.kai:34253`) — probes by
  the first `FieldInit`'s name from the construction site.
- `dpr_record_spread` (`stage2/compiler.kai:19984`) — probes by
  the first override's field name. The positional-record desugar
  (`dpr_record_lit`) keeps the bare-name lookup because its fields
  are still `__pos_<i>__` markers at that point; the disambiguator
  takes over downstream in `synth_record_lit` after the rewrite.
- `check_variant_record_pattern` (`stage2/compiler.kai:35047`) and
  `check_record_pattern` (`stage2/compiler.kai:35061`) — probe by
  the first `PField`'s name.

The arity-only `rec_find_arity` remains the fallback for callers
that do not have a useful field name (record literals with zero
fields, scrutinee types where the access is a partial application).

## Newest-first ordering

`collect_records_loop` now keeps the cons-front accumulator as
newest-first (the `list_reverse` at the end is gone). The user's
later redeclaration ends up at the head of `recs`, so bare
`rec_find` returns it before the prelude's same-name entry.
`rec_find_with_field` does not care about ordering (it prefers
field-set matches regardless of position), and `rec_find_arity`
deterministically picks the matching-arity entry. Selfhost stays
byte-identical with this layout — no consumer of `recs`
introspects ordering directly.

## What shipped

### Compiler (stage2/compiler.kai)

- `rec_find_arity_loop` + `rec_find_arity` — arity-aware walker.
  `arity = -1` reproduces the legacy bare-name semantics.
- `rec_find_with_field_loop` + `rec_find_with_field` — the
  field-set tie-breaker. First arity-matched record whose declared
  fields include `field` wins outright; otherwise the first
  arity-matched record acts as fallback.
- `field_decl_list_has` / `field_init_list_has` — small helpers
  for the field probes.
- `collect_records_loop` — newest-first accumulator (no
  `list_reverse`).
- Six call sites rewired (`synth_field`, `record_has_field`,
  `synth_record_lit`, `dpr_record_spread`,
  `check_variant_record_pattern`, `check_record_pattern`).

### Fixture

`examples/shadowing/user_thread_does_not_collide_with_private_regexp_thread.kai`
— the canonical case. User redeclares `Thread { id, name }`,
constructs and accesses both fields, and compiles alongside
`stdlib/regexp.kai`'s `Thread { state_id, captures }` without
either side surfacing the other's field set.

Wired into `test-shadowing` via the existing glob.

### Audit script

`tools/audit-prelude-private-records.sh` — sibling to
`tools/audit-prelude-private-types.sh`. Walks every non-`pub
type X = { ... }` in `stdlib/`, synthesizes a user redeclaration
with two non-overlapping field names, and verifies the program
compiles. Wired into `Makefile`'s `tier1` chain as
`test-private-record-shadow-audit`.

The first run audits 9 records and they all pass:

```
$ tools/audit-prelude-private-records.sh
audit-prelude-private-records: pass=9 fail=0 skip=0
```

The skip-list is empty by design — the field-set tie-breaker
handles every existing same-(name, arity) collision without
needing per-name carve-outs.

## Why a different shape from #643

Variant-side same-arity collisions (#643's `SplitN` /
`NB_FragR`) sit in a skip-list because the variant table is
indexed by ctor name, not by "field set". Two same-(name, arity)
sum types share the discriminator namespace and the typer cannot
tell which declaration a ctor belongs to without knowing the
caller's scope.

Records are different: every field access carries the field name
inline. `t.name` at a use site names exactly one field, which
exactly one of the two collided records declares (if any). So
the disambiguator is local to the access site — no boundary
sentinel, no caller-scope plumbing, no module mangling.

Whether the analogous fix exists for variants is an open
question for a follow-up: ctor patterns and constructions also
carry sub-pattern arity / payload-type information that could
in principle distinguish two same-named ctors. Not in scope here.

## Selfhost

```
$ make -C stage2 selfhost
self-hosting fixed point: OK
```

Byte-identical with the post-#646 baseline.

## Real cost vs estimate

| Phase                                  | Estimate | Actual |
|----------------------------------------|----------|--------|
| Localize bug + read #643 retro         | 15 min   | 25 min |
| First-cut fix (arity-only)             | 20 min   | 15 min |
| Selfhost gate                          | 5 min    | 5 min  |
| Notice same-arity gap (`Thread`)       | —        | 20 min |
| Boundary-sentinel design (rejected)    | —        | 25 min |
| Field-set tie-breaker design + impl    | 30 min   | 35 min |
| Fixture                                | 10 min   | 10 min |
| Audit script + Makefile wiring         | 15 min   | 20 min |
| Lane retro                             | 15 min   | 20 min |

Total ~3 hours. The boundary-sentinel detour mirrored #643's
approach faithfully but produced the wrong outcome for `Thread`
(prelude code lost access to its own private fields); discarding
it for the field-set tie-breaker resolved the same-arity case
that #643 left documented.

## Decisions made

- **Field-set tie-breaker instead of caller-scope plumbing.** A
  boundary sentinel in `[RecInfo]` (mirroring
  `ty_env_add_decl_marker`) closes the user-side query but does
  not help the prelude-side query for its own private record.
  The field-set discriminator is local to each access site —
  cleaner, less state, no marker entries in the lookup table.

- **Newest-first `[RecInfo]` accumulator without
  `list_reverse`.** Bare `rec_find` (used by callers that do not
  know the access's field name) needs the user's redeclaration
  to shadow the prelude's same-name record. The pre-#648
  oldest-first order made bare `rec_find` return the prelude's
  entry, which broke positional desugar (`dpr_record_lit`)
  before `synth_record_lit` had a chance to apply the field-set
  tie-breaker.

- **Two audit scripts instead of one.** Sum-type and record-type
  shadow audits diagnose different walkers; keeping them as
  siblings keeps the failure message specific. Lifted both into
  the `Makefile`'s `tier1` chain.

- **No skip-list in the new record audit.** Unlike #643's audit
  (which carries `SplitN` + `NB_FragR` as documented limitations),
  every existing stdlib record passes the field-name discriminator.
  A future stdlib record whose name collides with a user
  redeclaration AND whose field names overlap would need a
  skip-list entry — none exist today.

## Cross-references

- Issue #643 — variant-side analog.
- PR #646 — closing PR for #643 with the arity-aware variant walker
  that this lane mirrors and extends.
- `docs/lane-experience-issue-643-private-type-leak.md` §Limitations
  and follow-ups item 2 — the exact gap this lane closes.
- `feedback_kaikai_param_args_shadow.md` — the prelude-`args`
  shadow that bit #643; not encountered here (no slot named
  `args`).

## Limitations and follow-ups

1. **Same-name, same-arity, overlapping-field-set collisions.**
   If the user's redeclaration and the prelude's private record
   share BOTH name and at least one field name, the field-set
   tie-breaker is ambiguous on that field. No such pair exists
   in stdlib today. The clean long-term fix is per-module name
   scopes for non-`pub` records (the variant-side follow-up from
   #643's retro applies here too).

2. **Variant-side same-arity is still skip-listed.** This lane
   did not touch the variant walker; `SplitN` / `NB_FragR`
   remain documented limitations. A follow-up could apply an
   analogous payload-shape tie-breaker to ctors, but the
   variant table's structure differs enough that the design
   would not be a literal mirror.

3. **Empty-field records.** `synth_record_lit` with zero
   `FieldInit`s falls back to bare `rec_find`. No such literal
   appears in stdlib today; if one is introduced for a
   colliding name, the bare-name walker takes the newest-first
   match (the user's), which is still the correct user-side
   outcome but means the prelude-side query would surface the
   user's entry. Negligible risk today.

## Verification commands

```sh
# Repro from the issue body (passes after #648)
cat > /tmp/repro_648.kai <<'EOF'
type Thread = { id: Int, name: String }
fn make_thread(i: Int, n: String) : Thread = Thread { id: i, name: n }
fn main() : Unit / Stdout = {
  let t = make_thread(42, "alice")
  println("name: #{t.name}")
}
EOF
bin/kai run /tmp/repro_648.kai        # prints "name: alice"

# New fixture
bin/kai run examples/shadowing/user_thread_does_not_collide_with_private_regexp_thread.kai
# id=42 name=alice-renamed

# Audit script
make test-private-record-shadow-audit  # pass=9 fail=0 skip=0

# Selfhost
make -C stage2 selfhost                 # self-hosting fixed point: OK
```
