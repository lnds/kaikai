# Systems programming in kaikai — the frame, safe binary matching, and the raw layer (design proposal)

**Status:** proposal, not accepted. Written for later evaluation. Nothing here has
landed beyond the arena runtime primitive noted below (#120 P0).

**Reading order.** This doc has three layers, most-load-bearing first:

1. **What kaikai cedes to Rust vs what it only lacks** (below) — the strategic
   frame. One thing is a design renunciation; everything else is a roadmap.
2. **Safe binary pattern matching** — the primary mechanism for binary formats
   (parsers, protocols). Elixir-lineage, safe, no `Unsafe`. This closes the
   motivating use cases (buffers, reinterpreting bytes, framebuffers).
3. **The raw layer (`region` + raw memory + `Unsafe`)** — the rest of the doc.
   Reduced to genuinely raw cases after layers 1–2 absorb the common ones. The
   `Ptr[T]` framing it originally carried was found wrong in design review (see
   "Why `Ptr[T]` is the wrong shape") and is superseded by opaque block access.

## What Rust gives in systems that kaikai does not

Split into the one renunciation and the rest, because they are not the same kind
of gap.

### The one irreducible renunciation

**Rust proves at compile time that shared mutable aliasing cannot corrupt memory,
at zero runtime cost. That proof is the borrow checker, and it is the only systems
capability kaikai gave up by design** (CLAUDE.md "Do not introduce a Rust-style
borrow checker"). A mutable graph, a self-referential cache, two threads writing
disjoint regions of one buffer — Rust verifies these statically. kaikai does not,
and will not.

kaikai replaces the borrow checker with three separate mechanisms that together
cover most of what people *use* Rust's shared mutable aliasing to achieve:

- **in-place mutation** → `Mutable` effect + Perceus reuse (exclusivity is
  inferred from refcount, not proven),
- **shared identity** → `Ref[T]`, a runtime-counted cell,
- **race-free concurrency** → fiber isolation (the problem disappears — no shared
  memory to prove safe — rather than being proven safe).

What this does **not** cover is the one case needing all three at once at proven
zero cost: a mutable, concurrent, RC-free graph. That is Rust's home turf, ceded
on purpose. The "Zig with effects" identity *is* accepting this cession — Zig
proves no aliasing either, and writes systems fine.

### Everything else is a roadmap, not a wall

These Rust gives and kaikai *can* give without violating any principle. They are
debt or unbuilt surface, not renunciation:

| Capability | Rust | kaikai path |
|---|---|---|
| Flat layout, no per-element RC (`Vec<Point>`) | `Copy`/`repr(C)`, 16B inline | **#1053** — connect the dead typed-slot path (bug, not limit) |
| Type layout control (`repr(C)`, packed, align) | attributes | new `#[repr(...)]`-style attribute; same mold as `#[derive]`/`extern type` |
| Raw memory with indexed access | `unsafe` + pointers | `Array[T]` in a `region` + bounds-check + the four nets |
| Binary parsing | `unsafe` transmute / `nom` / `zerocopy` | **safe binary pattern matching** (below) — *safer* than Rust here |

And a genuine grey zone — present in Rust, absent in kaikai today, reachable in
part: SIMD, inline asm, intrinsics, atomics, `volatile` (MMIO). Atomics and
`volatile` are reachable via FFI or future intrinsics; SIMD is a large project.
Plus `no_std` / bare-metal: kaikai has a runtime (RC, fibers, effect scheduler),
so it does not run on bare metal without it — a real renunciation for OS/driver/
firmware work, less discussed than the borrow checker but equally load-bearing.

**Consequence for the motivating cases** (buffer management, reinterpreting bytes,
framebuffers): all fall in the roadmap column, not the renunciation. What you want
is *indexed access to a block whose extent is known* — which is exactly what makes
it safe, and exactly what a raw C pointer with free arithmetic lacks. C's
insecurity (stack corruption, uninitialised reads) comes from the pointer not
knowing the block's bounds, not from addressing memory. Give the block its extent
and the danger is netted:

| C error | Cause | kaikai net |
|---|---|---|
| stack/heap corruption | write past block bounds | **bounds-check** — block knows its size |
| uninitialised read | read before write | **zeroed alloc / typer requires set-before-get** |
| use-after-free | handle outlives its block | **region-escape check** (compile-time) |
| data race on memory | handle shared across threads | **fiber-escape check** (compile-time) |
| aliasing an RC value | handle into a counted value's interior | **POD restriction** — raw memory holds plain-data only |

## Safe binary pattern matching (the primary mechanism)

The motivating request — parse a binary format, read a protocol header,
reinterpret bytes — is **not** what the raw `Ptr`/`Unsafe` layer is for. It is
what binary pattern matching solves, safely, with no `Unsafe` and no raw pointer.
This is the Elixir/Erlang bit-syntax lineage, and it fits kaikai's existing
`match` rather than adding a memory subsystem.

### Shape — layout lives in the *type*, via a `Layout` kind (not a new delimiter)

An earlier draft delimited binary patterns `bytes< … >`. **Rejected:** `bytes` is
already a stdlib function (`random_secure.bytes`, all of `stdlib/bin.kai`), so the
token would carry two meanings by position — the exact `Unit`/`Unit` collision #253
had to untangle. The right shape came from a sharper observation: the byte specs
(`u32-be`, `i64-le`) are not ad-hoc pattern syntax — they are **a small type system
of physical layouts**, with their own grammar and composition. That is a *kind*.

`Layout` passes all three kind tests (see `docs/kinds.md`), the same way `Measure`
does: it classifies types by physical representation (orthogonal to what they are);
it has its own algebra (a struct's layout composes its fields' layouts; size sums);
and it enables polymorphism `Type` cannot — `fn parse[l: Layout](bytes) : …`, a
parser generic over layouts. It differs from `Measure` in one honest way: `Measure`
is phantom (zero runtime), a `Layout` inhabitant *executes* (it decodes bits). It
would be the first kind whose inhabitants run — a real distinction to resolve, not
a blocker.

With `Layout` a kind, the syntax **falls out of UoM** — angle brackets on a numeric
head, exactly as `Real<m/s>` — with **no new delimiter**:

```kaikai
# UoM precedent:        a unit over a numeric
let g : Real<m/s^2> = 9.81<m/s^2>

# Layout, same shape:   a representation over a numeric
type Header = {
  len:  U32<be>,        # a U32 laid out big-endian; <be> inhabits kind Layout
  port: U16<be>,
}
```

The binary "pattern" then needs no special form at all — it is ordinary record
destructuring of a type whose fields carry layout, using the `{ len, port }`
pattern that **already exists**:

```kaikai
fn parse_header(packet: Array[U8]) : Option[Header] = decode[Header](packet)
```

If the bytes do not suffice, `decode` returns `None` — no garbage read, no
corruption. That failure-instead-of-UB is the whole safety story, and it is free.

### Specs declare layout — the two things `reinterpret` cannot do

```kaikai
type Packet = {
  version: U8,
  flags:   U8,
  length:  U32<be>,         # endianness EXPLICIT (network order), arch-independent
  id:      I64<le>,
  # payload: dynamic size (depends on `length`) — the one case still needing
  # a value-dependent form, since a field length is a runtime value, not a Layout
}
```

1. **Endianness is declared** (`<be>`/`<le>`), so decoding is correct regardless of
   host architecture. `reinterpret[T]` assumed host memory layout — a portability
   bug waiting to happen.
2. **Data-dependent size** (a `payload` field as long as a prior `length` field
   said) is *impossible* with `reinterpret[T]`, whose `T` has a fixed compile-time
   size. Tag-length-value is the shape of every real binary format. This is also
   the one case the pure `Layout`-on-fields form does not cover — a length that is
   a *runtime value* is not a `Layout`. It needs either a value-dependent decode
   combinator or a residual pattern form; left open, it is the boundary of the
   declarative model, not its centre.

### Note on bitwise operators (blocks legible binary code today)

Binary code is bit-twiddling, and kaikai's bitwise ops are **intrinsic functions,
not operators**: `bit_and`, `bit_or`, `bit_xor`, `bit_not`, `bit_shl`, `bit_shr`,
`bit_ushr` (wired in `infer.kai`/`emit_c.kai`). So a line of SHA-256 in
`stdlib/crypto/hash.kai` reads:

```kaikai
bit_or(bit_or(bit_shl(b0, 24), bit_shl(b1, 16)), bit_or(bit_shl(b2, 8), b3))
```

— which everywhere else is `(b0 << 24) | (b1 << 16) | (b2 << 8) | b3`. The
function-nesting form is unreadable for dense bit expressions, and `crypto/hash.kai`
is among the hardest stdlib files to read *for this reason alone*. Bitwise-as-
functions was tolerable while bit-twiddling was rare; the moment kaikai targets
binary/systems work it stops being tolerable. Modelling bit ops as functions is the
same category error as modelling `+` as `add(a, b)`. If systems is a real
direction, dedicated infix operators (Elixir's triples `<<<` `>>>` `&&&` `|||`
`^^^`, chosen not to collide with the logical pair) are the fix — Elixir validated
exactly these in a same-family language. Independent of the `Layout` work; tracked
here because both surfaced together and both gate legible binary code.

### What this leaves for the raw layer

With binary matching taking parsers, and `Array[U8]`-in-region taking buffers, the
raw `Ptr`/`Unsafe` layer shrinks to what genuinely reinterprets *the same bytes as
two types* with no structured decode — a `union`-like overlay, `mmap` of a file
too large to copy, an allocator handing out typed cells. Rare in application code,
and with no measured case in the repo. The rest of this doc describes that layer;
read it as "the exotic remainder", not "the systems story".

## Goal, stated honestly

Let kaikai write low-level data structures — byte ring buffers, binary-protocol
parsers, bump allocators — **in kaikai itself**, without dropping to a C shim,
while keeping the language's load-bearing invariants intact.

The target identity is **"Zig with effects"**, NOT "compete with Rust in
systems". That distinction is the whole design:

- We beat Go outright: layout control + first-class effects + race-free isolated
  fibers, none of which Go offers.
- We compete with Rust **where Rust hurts** (effects, isolated fibers, writing
  graphs without `Rc<RefCell>` — cases 1/4/5 of the `kaikai-vs-rust` repo), NOT
  on Rust's home turf.
- We do **not** match Rust's zero-cost compile-time aliasing discipline. That
  power *is* the borrow checker, and kaikai has ruled the borrow checker out
  (CLAUDE.md "Do not introduce a Rust-style borrow checker"). Our safety net for
  raw memory is therefore **runtime (debug bounds-checks, Zig-style) plus three
  compile-time checks**, never Rust-style compile-time aliasing.

Claiming "as powerful as Rust in systems" under these constraints is impossible
by construction and would require reopening the borrow-checker decision. This
proposal deliberately does not.

## The insight that reframes the request

The motivating benchmark is case 6 of `lnds/kaikai-vs-rust` (`flat-layout-no-rc`,
the one clean Rust win). Its measurements split the problem in two:

1. **Contiguous layout** — *already solved*. `Array[T]` is a single allocation
   (`alloc_total ~15` for 1M elements). Layout is not the gap.
2. **Per-element / per-field boxing** — the real cost. This is already tracked
   as **#1053 (open, labels `perceus`/`compiler`/`unboxing`)**, whose diagnosis
   is sharper than the case README's. The README frames it as "RC per access";
   #1053 shows the deeper native-backend bug: every primitive field of a
   `Point{x,y}` is constructed as a *separate boxed heap object* via
   `kaix_variant` (mask==0) — 4.17× more allocs than the C backend on the same
   `.kai`, a 3.6× rb-tree wall gap. Rust wins because `Point` is `Copy`/`repr(C)`:
   16 bytes inline, no box, no count. The typed-slot fix (`kaix_variant_masked`,
   `kaix_variant_arg_i64`) already exists in the runtime (shipped for #747) but
   no native codegen path emits it — "road built, not connected". Family:
   #440 (variant field unboxing), #383 (param/return unboxing), #861 (raw call
   re-box).

So the request "I want a byte ring buffer with controlled layout" decomposes:

| What you want | What is actually needed | Needs `Ptr[T]` + `Unsafe`? |
|---|---|---|
| Byte ring buffer | `Array[U8]` storing flat bytes | **No** — `Array` unboxing |
| `Vec[Point]` flat, no RC | POD value-types stored inline in `Array` | **No** — flat-layout analysis |
| Allocator handing out cells that hold live kaikai values | pointer-to-RC-value inside raw memory | **Yes** — this is where Perceus breaks |
| Binary parser reading a structured format | ~~raw pointer + reinterpret~~ **binary pattern matching** | **No** — see layer 2 above |
| Overlaying the same bytes as two types (`union`, `mmap`) | raw block + reinterpret | **Yes** |

Only the allocator and the same-bytes-two-types rows justify the `Unsafe` effect.
Everything else — the ring buffer, flat `Vec[Point]`, and *binary parsing itself*
— is closed by unboxing (#1053) or safe binary matching, with no new effect and no
touch to Perceus. Binary parsing moved out of the `Unsafe` column in review: it is
a pattern-match, not a reinterpret.

## Why `Ptr[T]` is the wrong shape (superseded)

The original proposal exposed a raw handle as `Ptr[T]`. Design review rejected the
shape, in stages, each rejection sharper than the last:

- **`Ptr[T:POD]` breaks Tier 1 #3.** A bound on a *type constructor* is constraint
  propagation — the exact thing "no HKT, no constraint propagation, no type
  families" forbids. A `fn f[T](p: Ptr[T])` would have to thread `T:POD` to every
  caller. Not allowed.
- **`#[pod]` + a formation rule fixes soundness but not the smell.** Marking types
  POD (an assertion the typer verifies, `#[derive]`-style) and making `Ptr[T]`
  well-formed only for POD `T` avoids the bound. But it patches a deeper problem
  rather than removing it.
- **The deeper problem: `Ptr[T]` wears the shape of a generic without being one.**
  `Ref[T]` earns `[T]` — it is parametric, uniform over every `T`, because RC
  counts anything. `Ptr[T]` is *not* uniform: only POD, because the absence of
  counting is only safe for plain-data. The `[T]` form lies. Contrast: `Ref[T]` is
  a *counted* indirection (Perceus tracks it, any `T`, safe by the count);
  `Ptr[T]` would be an *uncounted* raw address (Perceus-invisible, POD-only, unsafe
  by the absence of the count). They are opposites, and `Ptr` borrows `Ref`'s
  notation without earning it.
- **The `T` was never in the pointer.** A pointer is an address; bytes have no
  type. The `T` is a promise about *how to read*, and that lives at the read site,
  not in a persistent type. So the handle is opaque — **`Ptr`, no `[T]`** — and
  the type appears only in the typed operations over it.

**Resolved shape:** raw memory is `Array[U8]`/`Array[T]` inside a `region`; the
handle, if one is even exposed, is an opaque `Ptr` (a region-owned address with
known extent), Zig-style, never a C `*mut T` with free arithmetic. Types live in
the *operations* — bounds-checked `get`/`set` (safe, no `Unsafe`), and
`reinterpret[T]` (`/Unsafe`, POD-restricted) — not in the handle. `Ptr[T]` as a
type does not exist in the resolved design.

## Where the analogy to Rust's `unsafe` holds and breaks

`unsafe` in Rust marks an unverifiable precondition and **propagates it to the
caller** (`unsafe fn` / `unsafe` block). That propagation is exactly what an
effect row does. So `unsafe` *is* capturable as an effect, and `unsafe { }` is
the handler that discharges the row.

Where it breaks: in Rust `unsafe` **disables the borrow checker** — that is its
job. kaikai has no borrow checker to disable. What a raw `Ptr[T]` would break
instead is the pair of invariants that give kaikai safety *without* a checker:

- **(a) Perceus discipline** — exposing a raw pointer to the interior of an
  RC-managed value creates an alias Perceus does not count → UAF / double-free,
  RC stops being sound.
- **(b) Fiber isolation** — a pointer crossing a `spawn` boundary (fibers copy
  messages BEAM-style today) reintroduces shared memory and races.

`Unsafe` in kaikai therefore does not relax a nonexistent checker; it opens a
crack in the two invariants that replace the checker. The design's job is to keep
that crack closed by construction.

## The model: `region` owns, `Ptr[T]` handles, `Unsafe` audits

Raw memory lives **inside a `region { }` block** (the bump-arena from #120: `rc =
INT32_MAX`, Perceus-invisible, bulk-freed at block close). That region is the
"allocator" in the Zig sense — every raw block is owned by a region that knows
its extent.

```
region {
  let buf = alloc_bytes(1024)        # Ptr[U8] into raw memory INSIDE the region
  ring_push(buf, head, tail, 0x41)   # store: bounds-checked (safe) or /Unsafe
  ...
}                                     # buf dies here, in bulk, no per-value walk
```

The decision that makes a safety net possible: **`Ptr[T]` always originates from
a region/allocator the runtime knows** (Zig-style), never an arbitrary address
(`*mut T`, C-style). A bounds-check is only possible when the runtime knows the
block's extent. Arbitrary-address pointers make every net impossible, which would
be strictly worse than Rust (Rust's net lives *outside* `unsafe`, in the borrow
checker we rejected).

This choice yields three invariants for free:

1. **Perceus intact.** The region is already `INT32_MAX`; Perceus counts nothing
   inside it. `Ptr[T]` points at raw bytes in that region, not at the interior of
   a live RC value. Zero crack in the count — *conditional on the POD rule below*.
2. **Fiber-safe by construction.** A `Ptr[T]` cannot cross a `spawn`; the typer
   rejects it, reusing the cell-escape mechanism from #997 (cells are fiber-local,
   compile-rejected on escape). No shared raw memory between fibers; BEAM model
   preserved. Compile-time, zero runtime cost.
3. **Bulk-free, not individual free.** The region frees everything at close. No
   out-of-order `free(ptr)`. This is *more restrictive* than Zig (which has
   `allocator.free(ptr)`) but *safer* and matches the existing runtime. Scratch
   structures (ring buffers, parsers) are born and die with the region.

## Operations and which carry `/Unsafe`

| Op | Signature | Effect | Net |
|---|---|---|---|
| `alloc_bytes(n)` | `Ptr[U8]` (inside region) | none | region knows the size |
| `get(p, i)` bounds-checked | `U8` | **none — safe** | check in debug, elided in release |
| `set(p, i, v)` bounds-checked | `Unit / Mutable` | `Mutable` | check + mutation visible in row |
| `get_unchecked(p, i)` | `U8 / Unsafe` | **`Unsafe`** | none — hot path, you promise |
| `offset(p, n)` pointer arith | `Ptr[U8] / Unsafe` | **`Unsafe`** | none |
| `reinterpret[T](p)` cast | `Ptr[T] / Unsafe` | **`Unsafe`** | none |

The fine point that makes this tolerable: **~90% of a systems structure uses the
safe, bounds-checked `get`/`set`** (no `/Unsafe`). You drop to `get_unchecked`
only where you *measured* the bounds-check to be the bottleneck — and that exact
line is typed `/Unsafe`, visible in the signature, greppable. The net is a
gradient, not all-or-nothing, and the effect marks precisely where you left the
safe zone.

`Unsafe` itself is the twin of `Ffi`: `effect Unsafe {}` — a no-operation
boundary marker whose only job is to appear in the row (see `docs/effects-stdlib.md`
on `Ffi`, "declared `effect Ffi {}` — its sole job is to appear in the row").
Trivial to add; the row-typer already handles marker effects.

## Why this is better than Zig, not just "Zig with functional syntax"

Two things Zig does not have, gained for free by coming from the effects side:

1. **The unsafe frontier is in the TYPE, not the block.** `/Unsafe` propagates
   upward. A public function that internally does a `reinterpret` declares it in
   its public signature. `grep '/ Unsafe'` is the complete audit trail — exactly
   as `grep '/ Ffi'` is today for C crossings. Zig has no such marker; every
   deref is equally "normal". kaikai marks the frontier and the row-typer verifies
   it. Also strictly finer than Rust: Rust's `unsafe` block wraps a whole region
   and loses per-op granularity; here each raw op is individually typed.

2. **Raw memory cannot race.** By the fiber-escape check, a `Ptr[T]` never crosses
   a `spawn`. Zig has no isolation — a pointer shared across threads is your
   problem. In kaikai it is a compile error. The BEAM model gives data-race-freedom
   over raw memory, which neither Zig nor C has and Rust achieves only with the
   borrow checker we rejected. This is the real differentiator, and it falls out
   of a decision already made.

## The three edges that still bite (honesty)

**Edge 1 — the POD restriction is the load-bearing beam.** If `T` in `Ptr[T]` /
`reinterpret[T]` were a type containing a `KaiValue*` (a record with a `String`
field), a `set` would write a pointer-to-RC-value into raw memory and Perceus
would stop counting correctly. The net here is a **type restriction**: `Ptr[T]`
and `reinterpret[T]` accept **only POD `T`** — `U8..U128`, `F32/F64`, `I8..I128`,
and structs whose fields are all POD. The typer rejects `Ptr[Record-with-String]`.
This single rule is what keeps invariant (a) intact. Without it, all the safety
collapses. Compile-time checkable, zero runtime cost. It is also the same
"what is POD" analysis the case-6 unboxing needs — see sequencing.

**Edge 2 — use-after-region-close.** If a `Ptr[T]` escapes its region (stored in
something outliving the block), it dangles. #120's policy is deep-copy-out at the
border, but a raw pointer cannot be deep-copied meaningfully, so here it must be a
**hard escape error**: the typer rejects a `Ptr[T]` crossing its region's close,
via the same side-table flow analysis as #120 P2a. Without it, silent UAF.

**Edge 3 — the strategic cost.** This adds surface to Tier 1 (the `Unsafe`
effect, `Ptr[T]`, POD-analysis, pointer escape-check) for a benefit that has **no
measured use case in the repo today**. Case 6 — the only net Rust win — is closed
by the field-unboxing work already tracked in #1053 (+ family #440/#383/#861),
NOT by this. Building the general power *before* the structure
that demands it is exactly the anti-pattern linus flagged in #120 ("permanent
type-system complexity for a gain below measurement noise"). This proposal is
honest that the `Unsafe`/`Ptr` layer is speculative until a real `reinterpret`-
demanding structure (a binary parser, an allocator) exists.

## Component status

| Piece | What it is | Status in repo |
|---|---|---|
| Arena runtime | bump-alloc, `INT32_MAX`, bulk-free, Perceus-invisible | **exists** (#120 P0, `kai_arena_alloc`/`kai_arena_free` in `runtime.h`) |
| `region { }` surface | lexical block, same mold as `nursery` | missing (P2a deferred, benchmark-gated) |
| Escape-check side-table | `Ptr`/region does not cross block close or a `spawn` | missing (#997 pattern reusable) |
| `Ptr[T]` with `T:POD` | handle to offset; typer rejects `T` containing `KaiValue*` | new |
| `Unsafe` marker effect | twin of `Ffi` (`effect Unsafe {}`) | new, trivial |
| safe `get`/`set` layer | bounds-checked, no `/Unsafe`, `set` via `Mutable` | new stdlib |

Load-bearing safety reduces to **three compile-time checks**: POD, region-escape,
fiber-escape. Everything else (debug bounds-checks, `/Unsafe` only on the unchecked
ops, `set` via `Mutable`) is net gradient, not soundness.

## Recommended sequencing

The path to "Zig with effects" runs **through closing case 6 first**, not through
the `Unsafe` effect:

1. **Field/element unboxing — already tracked as #1053** (connect the dead
   `kaix_variant_masked` / `kaix_variant_arg_i64` typed-slot path in native
   codegen so POD fields stay inline, matching the C backend). Closes the bulk of
   case 6 — the only measured Rust win — with no new effect, no Perceus change.
   The "what is POD" analysis it needs is the same beam Edge 1 relies on.
   Smaller, measurable, a connect-the-cable fix rather than new design. Note this
   closes the *native-vs-C* gap; any residual *C-vs-Rust* per-access RC cost on
   `Array[Point]` is a separate, finer question to re-measure only after #1053.
2. **`region { }` surface + lowering to arena** (unblocks #120, useful alone).
3. **`Ptr[T:POD]` + `alloc_bytes` + safe `get`/`set`** inside a region. The byte
   ring buffer and ~80% of systems programming land here, **without touching
   `Unsafe`**.
4. **`Unsafe` + `get_unchecked`/`offset`/`reinterpret`** — only when a real
   structure measures the safe layer as insufficient (binary parser, allocator).

Steps 1–3 give the ring buffer and most of the systems story with the `Unsafe`
effect never appearing. The effect is step 4: the hot path, the minority, opt-in
and marked. That is the intended shape — the net is the default, raw power is
opt-in and typed.

## Open questions for evaluation

- Is the "Zig with effects" identity the accepted target, or is closing case 6
  (via #1053, no `Unsafe`/`Ptr` layer at all) enough on its own?
- Does a concrete structure requiring `reinterpret` / pointer arithmetic exist
  yet? If not, steps 1–3 are the whole scope and step 4 waits for a real case.
- `region { }` was benchmark-gated in #120 (closed). Reviving its surface for
  systems use (not just parser scratch) changes its justification from
  "self-compile alloc traffic" to "raw-memory ownership" — worth re-deciding on
  the new grounds.

## References

- `docs/issue-120-regions-design.md` — the arena/region design and the
  block-not-effect decision (asu + linus, unanimous). The arena runtime (P0)
  shipped; the surface (P2a) and escape analysis are deferred.
- `docs/ffi.md` — FFI v2 (#417): fixed-width `U8..F32` are boundary-only today
  (no kaikai-land arithmetic); `opaque` handles; the `Ffi` marker-effect model.
- `docs/effects-stdlib.md` — the `Ffi` marker effect (`effect Ffi {}`, "sole job
  is to appear in the row"), the twin `Unsafe` would follow.
- `lnds/kaikai-vs-rust` case 6 (`flat-layout-no-rc`) — the measured Rust win and
  the layout-vs-per-element-RC decomposition.
- #1053 (open) — native backend boxes every variant field (mask==0), 4.17× more
  allocs than C; the typed-slot path exists but is unemitted. Family: #440
  (variant field unboxing), #383 (param/return unboxing), #861 (raw call re-box).
  This is the issue that closes the bulk of case 6.
- #997 — fiber-local cell escape (compile-reject + runtime floor), the mechanism
  the pointer fiber-escape check reuses.
