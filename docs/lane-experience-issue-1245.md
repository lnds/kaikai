# Lane experience — issue #1245: consolidate Layout serialization under `#[derive(Layout)]`

## Scope as planned vs. as shipped

Planned, and shipped as planned: remove the two magic global names
(`encode`/`decode`), remove by-shape auto-synthesis, add `derive_layout_impl`
reusing the existing codegen, migrate fixtures, update the design doc.

Shipped beyond the brief, both forced by the change rather than chosen:

- **`Layout` had to become a real protocol** in `stdlib/protocols.kai`, with a
  protocol ID (`KAI_PROTO_LAYOUT 13`) in both `runtime.h` copies. The brief said
  "emit an `impl`"; an `impl` needs a `protocol` to implement.
- **A latent bug in the unit walk** (`unit_walk.kai`) had to be fixed for the
  derive to work at all. See *Structural surprises*.

## What of `layout_synth`'s codegen was reused vs. rewritten

**Reused wholesale — the classification.** `layout_fields_of`, `layout_field_of`,
`layout_order_of`, `layout_width_of`, and the `LayoutField` accessors are
untouched and now serve the derive. This is the part with real subtlety (a record
is Layout-bearing only if *every* field is; the `be`/`le` habitant may arrive bare
or kind-qualified) and it had no reason to change. The three `lf_*` accessors went
`pub` — the bundle build hid the privacy violation, modular selfhost caught it.

**Reused as a shape, rewritten as code — the byte fold.** The encoder is still a
left fold of `bin_put_uint_be/le(acc, __detach_unit(field), width)` over fields in
declaration order onto `array_make(0, bin_byte(0))`, and the decoder still reads
each field and re-brands via `__attach_unit`. The bytes are identical — verified
by `diff` against output captured from the pre-change compiler (`CA FE BA BE 1F 90
01 02` for the canonical `Header`).

The code is new rather than shared because every AST node changed in ways that do
not factor cleanly:

- Source of the value: `v.field` (a fn param) → `self.field` (the impl receiver).
- Position: the old builders hardcoded line/col `0` (injected pre-typer, no source
  position); the derive threads the *type declaration's* real line/col, so
  diagnostics point at the user's `#[derive]`.
- Decoder offsets: absolute (`0`, `4`, `6`) → relative to `pos` (`pos + 0`,
  `pos + 4`), because `from_bytes` decodes at a cursor.
- Failure shape: `Option`/`None` → `Result`/`Err(String)`, and the bounds check
  moved from per-field to a single up-front width check.

Sharing a parameterised builder across those four axes would have cost more than
the ~60 lines of fold it saved. The classification — the part where a bug would be
subtle — *is* shared, which is where sharing pays.

**Deleted.** `layout_rewrite.kai` (173 lines) in full: it existed only to match
`EVar("encode")`/`EVar("decode")` post-typer. Also the auto-synthesis half of
`layout_synth.kai` (~205 lines: the encode/decode fn builders, `synthesize_layout_fns`,
`layout_type_names`), leaving an 85-line classification module. Net compiler lines
are roughly flat, but the mechanism count drops by one.

## Why Layout and BinSerialize share a signature but not an impl

They answer the same question — "give me these bytes, take these bytes back" — so
the *shape* is identical: `to_bytes(self) : Array[Byte]`,
`from_bytes(buf, pos) : Result[BinCursor[Self], String]`, plus a
`<lower(T)>_from_bytes` shim (single dispatch selects on the first argument, which
is the buffer, so neither can expose `from_bytes` directly).

They differ in **who controls the format**, which is not expressible in a
signature:

- `BinSerialize` — kaikai owns the encoding. Int is 8-byte LE, String is
  length-prefixed, and round-tripping kaikai↔kaikai is the contract. The user does
  not know or care about exact bytes.
- `Layout` — an external spec owns the encoding. `U32<be>` means those 4 bytes in
  that order because a TCP header says so. Byte-exactness *is* the feature.

A shared impl would have to be parameterised by format policy, which is the same
mistake as one derive with a mode flag: the user picks by intent
(`#[derive(Layout)]` vs `#[derive(BinSerialize)]`), and each derive emits the code
that intent implies. Two derives, one protocol shape, no shared body.

Worth noting they are not *interchangeable* despite the shared signature: deriving
both on one type would produce two impls of two protocols, and a generic function
bounded on one does not accept the other. That is correct — a function that wants
byte-exact framing must say so.

## Structural surprises the brief did not anticipate

**`map_units_decl` did not descend into `DDerive`.** The first working build
failed with `expected: Int<be>, found: Int<be>` — identical rendering on both
sides, which is the signature of a mismatch in something the printer elides. The
cause: `unit_walk.kai` returned `DDerive` unchanged, so a derived record's field
units never got kind-qualified (`be` → `Layout$be`) while the user's literal did.

This was latent before this lane, not introduced by it: any `#[derive(...)]` on a
record with dimensioned fields — `#[derive(Show)] type Money = { amt: Real<USD> }`
— hits it. It went unnoticed because auto-synthesis ran *before* kind resolution
and never needed the qualification, and because deriving on a dimensioned record
was not otherwise exercised. The fix is a one-line recursion matching how
`bkc_aliases` and `decl_unit_locals` already handle `DDerive`. Sibling wrappers
(`DUnstable`, `DConstructor`) plausibly have the same gap; not touched, since none
is reachable with a dimensioned record today. **Worth a follow-up sweep.**

**Bundle order forced a duplicated lowercaser.** `layout_derive` must precede
`protos` (which calls it), but `string_to_lower_ascii` lives in `protos`. Rather
than reorder modules — `protos` sits late for real dependency reasons — the derive
carries its own 8-line `ld_lower`. Mild duplication, deliberately chosen over a
risky reordering; the alternative is hoisting the lowercaser into `util`, which is
the better long-term home if a third caller appears.

**`EStr` carries the raw source span, quotes included.** The error-message
constructor needs `"\"...\""`, not `"..."`. Cost one build cycle.

## Fixtures

- `examples/sugars/kinds_layout_derive.kai` (+`.out.expected`) — renamed from
  `kinds_layout_encode_decode.*`, migrated to `#[derive(Layout)]` + `to_bytes` /
  `header_from_bytes`. Asserts the same byte offsets as before (b0/b3 big-endian,
  b6/b7 little-endian) plus the cursor `pos` past the record.
- `examples/sugars/kinds_layout_derive_short.kai` (+`.out.expected`) — **new
  negative fixture**: short buffer, empty buffer, and a valid buffer read at an
  offset that leaves a partial record. All three yield `Err`, none reads garbage.
- `examples/minimal/encode_decode_user_fn.kai` — kept as a C/native parity
  regression guard; comment rewritten, since "shadows the Layout builtin" now
  describes machinery that no longer exists. It passes for a stronger reason than
  before: there is nothing to shadow.
- `tools/test-native-selfhost-gate.sh` — retargeted at the renamed fixture; still
  asserts byte-identical C between the native-built and oracle compilers for a
  Layout program.

Coverage gap: nothing exercises `#[derive(Layout)]` on a record that is *not*
Layout-bearing. The derive emits no impl, so the user gets "no method `to_bytes`"
rather than "this record cannot derive Layout" — correct but unhelpful. A
`validate_derive_layout` mirroring `validate_derive_binserialize` would fix the
message; deliberately out of scope here.

## Cost vs. estimate

Close to estimate, with one unbudgeted detour. The removals and the derive were
mechanical — the BinSerialize pattern is a good template and the codegen was
already correct. The `DDerive` unit-walk bug consumed the single largest block of
time and was invisible from the brief; the "expected X, found X" diagnostic gives
no hint that the difference is a kind qualifier, and the reflex is to suspect
one's own freshly-written codegen. Capturing the golden bytes from the *pre-change*
compiler before touching anything paid for itself: byte-exactness became a `diff`
instead of a judgement call.

## Follow-ups left for next lanes

1. **`DDerive` sibling wrappers in `unit_walk`** — `DUnstable`, `DProtoLaws`,
   `DImplAxiom`, `DConstructor` pass through unchanged. Same latent bug, not
   currently reachable. A sweep should either fix all or document why not.
2. **`validate_derive_layout`** — reject `#[derive(Layout)]` on a non-Layout-bearing
   record with a message naming the offending field, mirroring
   `validate_derive_binserialize`.
3. **`#[derive(Json)]` / `#[derive(Xml)]`** — now unblocked;
   `docs/json-derive-design.md` is the design and `derive_layout_impl` is the
   second worked example of the pattern.
4. **Hoist `string_to_lower_ascii` to `util`** if a third derive needs it.
5. **TLV / nested Layout records / signed heads** — unchanged from before this
   lane; still rejected up front rather than mis-encoded.

## Breaking change

`encode(v)` / `decode(b)` no longer exist. Code using them must add
`#[derive(Layout)]` and call `v.to_bytes()` / `<lower(T)>_from_bytes(buf, pos)`,
and handle `Result[BinCursor[T], String]` where it previously handled `Option[T]`.
Accepted pre-1.0 per the issue. The upside is that `encode` and `decode` are now
ordinary identifiers users may bind freely — previously a user `fn encode` would
silently break Layout serialization with no diagnostic.
