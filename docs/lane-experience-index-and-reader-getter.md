# Lane experience — §1.3 index dispatch + §1.4 Reader.get→ask (smell audit)

Date: 2026-06-04
Scope: two name-keyed hardcodes from the smell audit — `obj[k]` index dispatch
keyed on the type name "Map" (§1.3), and the `Reader.get → ask` op aliasing
duplicated across infer + emit (§1.4).

## Scope as planned vs as shipped

Planned: "generalize" both. Shipped: neither was generalized — §1.4 was
consolidated (DRY), §1.3 was reclassified as intrinsic and documented in place.
The audit's framing ("should be a protocol / should be general") was wrong for
both, for different reasons. This is the second audit item this session where
the recommendation did not survive contact with the code (the first was §1.1's
false "blocked on a missing mechanism").

## §1.4 — the near-miss

Initial read (mine): `grep 'Reader.get'` across stdlib/examples/demos/tests
returned **zero** literal uses — everyone writes `Reader.ask()`. I concluded
the alias was orphaned dead code and the right move was to *delete* the two
lines.

That was wrong, and the architecture review caught it. The consumer of the
alias is not the literal `Reader.get` — it is the `@cap` sugar. The parser
desugars every `@name` read to `name.get()` uniformly (desugar.kai ~2155). For
a `State` capability the read op *is* `get`, so it works directly; for a
`Reader` capability the read op is `ask`, so `get` must be remapped or the
op-call lookup fails. Deleting the alias would have broken `@x` on any
`Reader`-bound capability — covered by `examples/sugars/m7b_4_at_reader.kai`,
whose comment states the path explicitly. The grep was the wrong question: the
alias is keyed by *effect*, exercised by the *sugar*, never written literally.

Lesson (a standing one): "no literal callers" ≠ "dead code" when a desugar
synthesizes the call. Check what lowers *to* the form, not just what is written
*as* the form.

Action taken instead: the remap was triplicated in spirit (infer.kai alias
path + emit_shared.kai rewrite_alias_kind, plus the conceptual coupling).
Extracted to one `pub fn canonical_cap_getter(eff_name, op_name)` in
emit_shared.kai; both sites call it. Zero behaviour change, one source of truth,
and the next effect whose read op isn't `get` has exactly one place to extend.
Cross-module forward reference (infer.kai at bundle pos 15 calls a fn defined at
pos 22) works because kaikai resolves top-level names globally post-parse —
`c_sym`/`fv_expr` already cross the same way.

## §1.3 — why a protocol is the wrong tool

The audit wanted an `Index` protocol so `[]` dispatches by impl-table instead of
`if name == "Map"`. Three reasons it is over-engineering, all confirmed in code:

1. **It moves the hardcode, doesn't remove it.** A protocol adds the name
   "Index" + two builtin impls (Array, Map) spread across more sites — same
   count of hardcoded names, more surface, plus every `[]` now routes through
   proto resolution at runtime. With exactly two closed cases, the 3-line
   typer switch is simpler, compiles faster, and is what Tier 1 #3 prefers
   (it *bans* type-class-style resolution).
2. **It degrades the error.** The Array fallback for an unresolved type var is
   deliberate (comment at infer.kai ~9893) — it makes the unifier emit
   "expected `Array[T]`". A protocol over `?t` can't pick an impl, so it emits
   a generic "no `Index` impl" or re-hardcodes the Array bias inside the
   resolver. Worse message, no gain.
3. **Not parallel to §1.4.** A table beats inline only with N≥2 consumers of
   the same mapping (§1.4: three sites). §1.3's dispatch is one site; a
   `type→module` table would add an indirection and a lookup site for the same
   two cases — ceremony, not cleanup.

Action: no code change beyond a design-note comment on `synth_index` recording
why the closed 2-case dispatch is correct and when to revisit (a third
indexable container with genuine index semantics). Issue #128 is CLOSED, so the
rationale lives in the code, not a reopened issue.

## Fixtures

None added. §1.4's path is already pinned by `m7b_4_at_reader` (Reader → ask),
`m7b_4_at_state` (State → get, no remap), and `m7b_4_assign_on_reader`
(negative), all in `test-sugars` and all green post-change. §1.3 is unchanged
behaviour. Adding a fixture for a pure refactor with existing coverage would be
noise.

## Cost vs estimate

§1.4: ~20 minutes, almost all of it in the review that reversed the delete
decision — the actual edit is one helper + two call-site swaps. §1.3: a comment.
The expensive part was thinking, not typing, which is the correct ratio for
"is this a smell or a boundary" work.

## Follow-ups

Smell audit §1 is now fully triaged: §1.1 fixed (separate lane), §1.2/§1.3
reclassified as intrinsic, §1.4 consolidated. Remaining §1.5/§1.6/§1.7 are LOW
and not yet assessed.
