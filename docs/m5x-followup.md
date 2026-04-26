# m5.x — Perceus + memory follow-up

Tracks every item the m5 lane (rounds 1-3, 2026-04-25/26) deferred
to a future milestone. The common thread is that **m5 v1 ships
scaffolding without flipping the runtime to consume args linearly**
— the dup/drop infrastructure is in tree but inert until stage 1
also has a perceus pass and the runtime can be flipped atomically
across both stages.

The single high-impact deliverable that did land in m5 v1 is **m5 #7**
(constant pool for nullary primitives): self-compile alloc count
dropped from ~131M to ~29.5M, a 77.4% reduction with no emitter
changes. The remaining ~29M still leak — that is the absence of a
Perceus discipline, not a Perceus inefficiency, and what the items
below address.

## Deferred items

### 1. m5 #9 step 3/4 — runtime primitive linear consumption *(BLOCKED on stage 1)*

Modify `kai_lt` / `gt` / `le` / `ge` / `eq_v` / `ne_v` / `add` / `sub` /
`mul` / `div` / `idiv` / `mod` / `neg` / `boolnot` / `truthy` / `field`
to decref their args linearly. Tried during m5 round 3 and reverted
because `stage1/compiler.kai` has no `perceus_pass`: stage 1 emits C
in a *loose* discipline where primitives are called without dup, so a
linear-consuming runtime produces use-after-free in stage 1 selfhost.

**Path to unblock**:

1. Port `perceus_pass` to `stage1/compiler.kai` (~550 LOC mirror of
   the stage 2 implementation, simpler because no parametric-effect
   complications).
2. Atomic flip of the runtime covering both stages in a single
   commit. Both kaic1 and kaic2 must agree on linear-consumption
   semantics from that commit on.
3. Re-measure `live_peak` and leak rate. Expect a step change beyond
   the current 77.4%.

**Estimate**: ~1-2 days.

Documented in `perceus-basic.md` §"What unblocks step 3+".

### 2. m5 #6 (doc grain) — `kai_closure` incref of captures *(DEFERRED)*

When a lambda is constructed, its captured values need an incref to
survive the call site. Today they do not. This is a latent bug: the
loose-consumption runtime hides it because nothing decrefs aggressively
mid-flight, but **once step 3 above flips the runtime, this becomes a
crash bug** as soon as a captured value is consumed by a primitive
inside the lambda.

**Cost**: coordinated change in runtime + emitter. ~1 day.

Order of operations: ship this **before** step 3 lands, so the flip
does not reveal the bomb.

### 3. m4c retrofit — `clause_fn_name` should be fn-name aware *(BLOCKED)*

`_kai_clause_<line>_<col>_<op>` mints C symbol names by source position
without awareness of the enclosing function. Duplicating polymorphic
function bodies that contain `EHandle` produces colliding symbol
names, which the C linker rejects.

**Fix path**: thread the enclosing fn name through `collect_decls` /
`lc_new` / clause-info plumbing. Once done, the **real m4c
specialisation** (post-monomorph specialisation per call-site type
tuple) can re-land — currently m4c #1/#2 ship only an identity pass.

**Cost**: ~1-2 days.

Order of operations: lands when m4c real specialisation is back on
the critical path, which is post-stage-1-perceus.

### 4. LLVM emit mirror of m5 #3 *(PENDING)*

`llvm_emit_stmt` does not insert the `kai_decref` drops that m5 #3
adds for unused let-bindings whose RHS is a fresh allocation. Programs
compiled with `--emit=llvm` therefore do not benefit from the
unused-let-drop pass; the C backend gets the optimisation, the LLVM
backend stays loose.

**Cost**: ~½ day. Mechanical mirror of the stage 2 C-emitter changes
in m5 #3 into the LLVM emitter.

Order of operations: anytime — independent of the stage-1 path.

### 5. Full Perceus (post-m5 milestone)

The m5 lane scope was *basic* Perceus: walker scaffold + last-use
analysis on fn parameters + drop unused fresh allocations + dup/drop
infrastructure. The full optimisation surface stays for a future
milestone:

- **Reuse-in-place** (Koka style): a constructor reuses the
  consumed cell in place of `free` + `alloc` when the type system
  can prove no live aliasing remains.
- **Drop specialisation**: decref chains generated per type and
  inlined, instead of going through the runtime's dispatch.
- **Unboxing**: `Int` / `Bool` / `Char` / `Real` in native machine
  registers inside each fiber. Heap boxing only for compound
  immutable values. Messages across fiber boundaries copy.
- **Opt-in regions**: for intermediate buffers (parser scratch,
  lexer state) where reference counting demonstrably costs more
  than an arena.

**Cost**: weeks, not days. Lands as its own multi-milestone block
post-m12 selfhost checkpoint. Doc placeholder in `stage2-design.md`
§*Full Perceus*.

## Follow-ups inherited from m5 #0 baseline

These are tracked in `perceus-basic.md` §*Follow-ups* and surface
again here for index visibility:

1. **Runtime symbol shadowing**: kaikai functions named `add` /
   `sub` / `neg` / etc. collide with the runtime's `kai_add` / `sub`
   / `neg`. Three fix options with different costs documented in
   `perceus-basic.md`.
2. **m14 nominal migration coupling**: when m14 migrates `list_take`
   → `list.take`, the 16 m14-pre functions need to migrate together.
   Currently the flat-prefix style sits in `stdlib/core.kai`; the
   migration is mechanical but coordinated.
3. **kaic2 typer rejects polymorphic prelude calls from foreign
   files**: blocks several tests under `examples/` from running
   against `kaic2` directly. Workaround in place; root cause stays
   in m7b parametric-effect plumbing.
4. **Per-process double `KAI_TRACE_RC` report when run via
   `bin/kai`**: the env var is inherited by the child binary, so
   the wrapper's report and the program's report both fire. Cheap
   fix: unset `KAI_TRACE_RC` before exec'ing the program, or have
   the wrapper opt out.

## What does NOT belong here

- The **stage-1 perceus port** itself (item 1 step 1) is its own
  milestone, not an m5.x deferral. It is the *prerequisite* that
  unblocks the m5.x items.
- Any m12.5 (UoM), m12.6 (refinements + contracts), m12.7 (axiom),
  m13 (bench + bit ops) work — those are independent milestones,
  not Perceus follow-ups.
