# Lane experience — issue #1279: `validate_derive_layout`

## Scope as planned vs as shipped

Planned: mirror `validate_derive_binserialize` so `#[derive(Layout)]` on a
non-Layout-bearing record fails at the derive site naming the offending field,
instead of silently emitting no impl and surfacing as "no method `to_bytes`" at
the first call site.

Shipped exactly that, plus the issue's explicit sub-requirement that the three
failure modes stay distinct rather than collapsing into one generic message.
No change to the impl builder: when a record *is* Layout-bearing the emitted
bytes are unchanged, confirmed by the three pre-existing positive fixtures
matching their goldens byte-for-byte.

## What was reused from `validate_derive_binserialize`

The decl-walk skeleton transferred almost verbatim:

- The `DDerive(names, inner, _, _)` → `DType(_, tname, _, body, line, col, _)`
  match, guarded by `list_has(names, "<Proto>")`.
- The wrapper-transparent recursion (`DUnstable`, `DProtoLaws`, `DImplAxiom`,
  `DConstructor` re-push their inner decl onto the work list) — the derive can
  sit under any of them, and skipping this is a silent miss, not a crash.
- The `errs: Int` accumulator threaded through the fold and summed into
  `derive_errs`, which the driver turns into a short-circuit via `proto_errs > 0`.
- The message shape ``cannot `#[derive(P)]` for `T`: field `f` of type `U`…``.

What did **not** transfer: BinSerialize validates against a *provider set*
(`collect_binser_providers`) because "is this type serialisable" is an open,
whole-compilation-unit question. Layout's rule is closed and purely syntactic —
`U8..U64` carrying `<be>`/`<le>` — so there is no provider collection pass and
no whitelist for the self-recursive case. That halves the code.

## Structural surprise: the classification had to be rebuilt, not reused

`layout_fields_of` already decides Layout-bearing-ness, so the obvious move was
to call it and report on `None`. That gives the *right* accept/reject set but
cannot produce the message the issue asks for: `layout_field_of` funnels a
missing habitant, a signed head, and a non-fixed-width type into one shared
`None`. The reason is lost at the point it is computed.

So `layout_validate.kai` re-walks the fields with a `LayoutReject` sum that
keeps the reason. This is deliberate duplication of a *predicate*, not of
logic-with-consequences: synthesis needs a yes/no, diagnostics need a why. The
alternative — widening `layout_field_of` to return `Result`-shaped data — would
have pushed diagnostic concerns into the synthesiser that every non-diagnostic
caller then has to discard. The trap to note for a future lane: the two walks
must stay in agreement, and the fixture is what pins that. If `layout_width_of`
ever grows a head, `layout_is_unsigned_head` grows with it or a valid record
starts being rejected with a confusing message.

## The three cases, and why they are not one

Kept separate because the fixes differ and the issue calls this out:

- `id: U32` — no habitant. Fix: add `<be>`/`<le>`. There is intentionally no
  host-endian default, so the message must say which to add.
- `delta: I32<be>` — signed head. Sign extension is a declared follow-up, so
  the message says Layout covers the unsigned heads rather than implying the
  field is malformed.
- `name: String` — no fixed width. Not expressible in the shipped cut.

Two extra rejections fell out of the walk that the issue did not enumerate: an
empty record, and `#[derive(Layout)]` on a sum type. Both previously produced
the same silent nothing.

## Fixtures

- `examples/sugars/kinds_layout_derive_nonbearing_err.kai` +
  `.err.expected` — one record per failure mode, so a regression that collapses
  the three messages into one generic string fails the golden.
- Regression cover for the positive path is the three pre-existing fixtures
  (`kinds_layout_derive`, `kinds_layout_derive_short`, `kinds_layout_fields`),
  which must keep matching their `.out.expected` unchanged.

Harness trap worth recording: `test-sugars` matches each golden line with
`grep -q "$line"` inside a Makefile `while read` loop, so the expected text is a
**regex** run through two layers of shell quoting. Backticks are fine as `.`,
but a `;` in an expected line is eaten before grep sees it and the failure
reports as `missing diagnostic:` while printing the very line it claims is
missing — which reads as a compiler bug and is not one. Existing goldens are
short plain prose for exactly this reason; the new golden follows suit and pins
each case by its identifying line plus its distinct remedy fragment.

## Cost vs estimate

Close to estimate. The build/gate cycle dominated; the code is ~90 LOC in a new
module plus ~85 LOC of decl walk in `protos.kai` and three one-line wirings
(import, `BUNDLE_SRCS`, the `derive_errs` sum).

## Follow-ups left

- Signed Layout heads (`I8..I64`) with sign extension on decode. The diagnostic
  now names this as the reason rather than a malformed field, so the message
  comes off cleanly when that lands.
- `layout_head_name` renders `TyList`/`TyFn` structurally (`List`, `Fn`) and
  falls back to `?` for the short-lived `TyRow`/other shapes. A field of those
  shapes is rejected correctly; only the rendering is coarse.
