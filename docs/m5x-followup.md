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

### 1. m5 #9 step 3/4 — runtime primitive linear consumption *(STEP 1 LANDED, STEP 2 BLOCKED)*

Modify `kai_lt` / `gt` / `le` / `ge` / `eq_v` / `ne_v` / `add` / `sub` /
`mul` / `div` / `idiv` / `mod` / `neg` / `boolnot` / `truthy` / `field`
to decref their args linearly.

**Step 1 — stage 1 perceus port**: LANDED (`f327d34`, 2026-04-26).
`stage1/compiler.kai` now has its own `perceus_pass`, with magic-name
emit for `__perceus_dup` / `__perceus_drop`. Inert under the loose
runtime; ready to balance the runtime once the flip lands.

**Step 2 — atomic runtime flip**: BLOCKED. Attempted in the m5x-1-2
lane (2026-04-26) and reverted. Three compounding issues surfaced
during self-host:

1. C does not specify the evaluation order of function-call arguments.
   The lexically-last "transferred raw" read can fire before the
   dup-wrapped earlier reads (clang on AArch64 evaluates right-to-
   left), freeing the local before the dups execute. A conservative
   variant (any binding with ≥ 2 uses dups every read) is sound but
   not sufficient on its own — the other two issues remain.
2. `stage0/emit.c`'s `emit_pat_test` / `emit_pat_binds` for record
   patterns chains multiple `kai_field(_scr, ...)` on the same
   scrutinee. Under linear consumption the first field-test consumes
   `_scr`; subsequent tests UAF.
3. The kaic1 binary's machine code is whatever stage 0 emits, and
   stage 0 has no perceus pass. An eager-dup retrofit at every local
   read works for simple programs but interacts with closure capture
   construction and match-pattern aliasing in ways that need a
   substantial stage 0 audit.

Detail in `docs/perceus-basic.md` §"Step 3+ outcome (m5.x-1-2 lane)"
and `docs/lane-experience-m5x-1-2.md`.

### 2. m5 #6 (doc grain) — `kai_closure` incref of captures *(LANDED)*

Landed (`80b0015`, 2026-04-26). `kai_closure` now increfs each
captured value at construction; the symmetric decref already lived
in `kai_free_value`'s KAI_CLOSURE branch. Inert under the loose
runtime — nothing decrefs primitive args mid-flight — and ready to
balance the closure-capture rc once step 1 above lands.

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
