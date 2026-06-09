# Lane experience — KIR list-pattern match lowering

Branch off `kir-native-walk`. This lane fixes a frontend bug in the
AST→KIR lowering: a `match` on **list patterns** (`[]`, `[h, ...t]`,
`[a, b, ...rest]`) lowered to a malformed KIR — a `KSwitch` with EMPTY
cases routing to a default block that referenced head/tail bindings
(`h`, `m`) that were never emitted. The C-direct backend (which emits
from the AST, never the KIR) was unaffected; the bug was confined to the
`--emit=kir` / `--emit=native` path.

## Scope as planned vs as shipped

Planned (brief): make list-pattern arms lower like the C-direct emitter —
discriminate cons/nil, bind head/tail via `KProj`, handle exact-length,
the `...rest` binder, and the wildcard catch-all. Add a list-match
regression fixture wired into a parity harness. Keep `km` A−/B+.

Shipped: exactly that, via a dedicated fail-chain decision tree (not the
variant tag-switch), in a new A++ module + a thin driver in the Expr-SCC.
Two corrections to the brief's premises surfaced (see "Surprises").

## Root cause (confirmed)

`lower_match` lowered every match the same way: read `KTagOf(scrv)`, build
one `KSwitch` case per arm keyed on the constructor tag (`plan_arms` /
`arm_tag`), bind variant sub-patterns by slot (`bind_pattern_fields`).

A list pattern is none of those: `arm_tag` returned `-1` for `PList`, so
`plan_arms` gave it no switch case (→ empty switch), and `bind_pattern_
fields` only handled `PVariant`, so the head/tail binders were never
emitted. A `[] -> 0` arm fell to `first_default_arm` (it is "non-variant",
so `arm_is_default` said true), and a `[h, ...t] -> h` arm vanished
entirely along with `h`. Result: `switch tN [] default L; L: t <- h` with
`h` unbound.

The deeper reason it could not be patched into the existing switch: list
arms of different lengths (`[]`, `[x]`, `[a, b]`, `[h, ...t]`) all share
the runtime `KAI_CONS` tag, so a single tag-switch cannot tell them apart.
They need a **length-aware decision tree** — exactly the fail-chain the
C-direct (`emit_pat_test_list`) and LLVM (`llvm_emit_pat_list`) emitters
already build. This lane mirrors that, as KIR nodes.

## Design

A list match (any arm is a `PList`) routes to `lower_list_match` instead
of the variant tag-switch. Each arm gets a body label and a fail label
(= the next arm's test block), and emits a chain of `KCondBr` on a
boxed-Bool cons/nil predicate (`kai_is_cons_b` / `kai_is_nil_b`) — the
same condbr shape `lower_if` produces. On a cons cell it binds the head
from slot 0 and advances the cursor to the tail (slot 1) via `KProj`:

- `[]` (no rest) → `is_nil cur`.
- N fixed heads + rest → N `is_cons cur` tests, each binding its head and
  projecting the tail; then bind the rest = the final tail.
- N fixed heads, no rest → as above, then require `is_nil` of the final
  tail (exact length).
- catch-all (`_`, whole-list binder, `@`-pattern) → unconditional body,
  ends the chain (later arms are dead).
- no catch-all → the final fail label is `KUnreachable` (matches are
  exhaustive, per the existing Linus-review note).

The **cons head/tail share `KProj` with variant slots** — in the runtime
union, `as.cons.head` overlaps variant slot 0 and `as.cons.tail` slot 1
(`stage2/runtime.h` ~line 412 + 469). So `KProj(cur, 0)` = head and
`KProj(cur, 1)` = tail with no new node — `kaix_variant_arg(consCell, i)`
reads the right slot by construction.

### Why a boxed predicate, NOT `KTagOf` (the trap the rebase exposed)

The first cut used `KTagOf(cur) == KAI_CONS/KAI_NIL`. That is correct by
the C-direct oracle (`->tag` compare) but the **native walk consumes
`KTagOf` as `kaix_variant_tag_of` — it reads `v->variant_tag`**, which is
meaningless for a cons/nil cell (`kai_cons` never sets it; the nil
singleton leaves it uninitialised). So the native object built, but
SEGFAULTED at runtime — a silent miscompile, worse than a clean
"unsupported" abort. The fix reads the runtime `->tag` instead, via two
boxed-Bool predicates `kaix_is_cons_b` / `kaix_is_nil_b` added to
`runtime_llvm.c` (the native link target). They ride the native walk's
EXISTING uniform boxed-prim path (`kai_*` → `kaix_*`, a
`%KaiValue* (%KaiValue*)` call) and the existing `KCondBr` → `kaix_truthy`
lowering — so the native walk needed NO logic change, only a runtime
helper. The bool singleton is immortal, so the ephemeral test value needs
no drop (same as the `==` prim's result).

### Where the code lives (module-DAG constraint)

The pure test+bind emission (cons/nil discrimination + head/tail `KProj`,
no `lower_expr` recursion) is a NEW module `compiler/kir_lower_match.kai`
(A++ 98.5, 67 LOC). The **driver** that interleaves arm-body lowering must
stay in the Expr-SCC `kir_lower_walk.kai` because it calls the private
`lower_branch_into → lower_expr` recursion; moving it out would make the
module DAG cyclic (`kir_lower_match → kir_lower_walk → kir_lower_match`).
The walk's own docstring already names `list` as part of this SCC, so the
~25-LOC driver belongs there by charter.

## Soundness decisions

- **RC via KIR `KLet` aliasing, not C-style incref.** The head/tail/rest
  binders are bound with plain `KLet` (the same convention as the variant
  `bind_sub_binders`); KIR `KLet` never increfs — Perceus's already-placed
  `KRC` nodes do all refcounting. Because the rest binder is in the same
  `pat_ubinds` set Perceus walked, the dup/drop it inserted stays balanced.
  Binding every PBind/rest unconditionally (including `_`-prefixed and the
  `__list_rest_*__` spread sentinel) matches that set exactly.
- **Exhaustiveness gives the final `KUnreachable`.** The typer guarantees
  exhaustiveness, so the chain never falls off the end; a wrong index or
  missed nil/cons surfaces as a wrong value or crash (not silently), with
  C-direct as the parity oracle.
- **The variant path is byte-identical.** The `__pcs_scr` / join preamble
  was extracted to a shared `match_preamble` used by both paths; the
  `control_flow` KIR golden (a variant match) is unchanged, proving the
  refactor is behaviour-preserving.

## Fixtures

- `examples/kir/list_match.kai` (+ `.kir.fns` + `.kir.expected`), wired
  into `test-kir` (in `make test` → tier1; also `test-fast`). Covers nil,
  head-bind + rest, multi-head + tail-cons discrimination, exact-length
  (`[x]`, `[a, b]`), and a **named** rest binder used in the body
  (`drop_two`). The golden is the validated KIR; `test-kir` also diffs two
  runs for determinism. C-direct exec returns 24.
- `examples/native/list_match.kai`, auto-globbed by
  `tools/test-native-parity.sh` (the `tier1-native` path-gated CI). Builds
  the native object via the in-process libLLVM walk, links against
  `stage0/runtime_llvm.c`, runs, and diffs exit code + stdout vs the
  C-direct oracle. Exit code 25; native == C-direct end-to-end.

## Surprises the brief did not anticipate

1. **The native walk landed on `main` while the lane branch was open.**
   The brief was written against a base where `build_native_object` was
   the hardcoded `main -> 42` spine. By the time the lane rebased, the
   generic KIR native walk (PR #780-series) + native effect lowering had
   merged, so the native path now consumes arbitrary KIR — making the
   brief's native-vs-C-direct parity gate genuinely testable. The rebase
   conflict was real: `main` switched the variant tag register to `SInt32`
   (so the native backend emits a real LLVM `switch`); the resolution
   keeps that AND adds the list-match dispatch.
2. **`KTagOf` is variant-only — the native trap.** See the Design section:
   `KTagOf` → `kaix_variant_tag_of` reads `variant_tag`, garbage for a
   cons/nil cell, so the first cut silently segfaulted on native. The fix
   discriminates on `->tag` via boxed predicates. This is why the lane
   touches `runtime_llvm.c` (two thin helpers) despite the brief's "do not
   touch the native walk" — the walk's *logic* is untouched; the helpers
   ride its existing boxed-prim + condbr lowering.
3. **"cons tag 12 / nil tag 11" in the brief was a misread.** The buggy
   dump's `[11 -> ..., 12 -> ...]` were the OUTER variant arms (A=11,
   B=12, user-variant tags from `docs/variant-tags.md`), not cons/nil.
   Lists discriminate on the runtime `KaiTag` enum — `KAI_NIL = 6`,
   `KAI_CONS = 7` — which is what C-direct compares and the predicates read.

## Gates

- `--emit=kir` on the reproducer + all list-pattern perceus fixtures:
  0 empty-switches (was non-zero); proper `condbr is_cons/is_nil` chains
  with head/tail `proj` binds.
- `test-kir` green (new fixture + determinism + the unchanged variant
  golden `control_flow`).
- `test-match` green (9/9, incl. `list_spread_*`).
- `tools/test-native-parity.sh` green 6/6, incl. `list_match (rc=25)` —
  native == C-direct end-to-end.
- `make tier0` green (selfhost byte-id `kaic2b.c == kaic2c.c`, demos
  baseline, arena gate).
- C↔LLVM backend parity 413/0 (untouched — the C/LLVM emitters are
  byte-for-byte the same; the fix is AST→KIR + a runtime helper).
- `km`: new module A++ (98.5), cogcom avg 1.9 / max 4.

## Follow-ups for next lanes

- **Nested destructuring inside a list head** (`[Some(x), ...t]`, a `PLit`
  head) is not yet handled — the head bind covers plain `PBind`/`PWild`;
  a nested PVariant/PLit head would need recursion into the variant test
  (and, on native, the matching variant-tag discrimination). None of the
  in-scope fixtures (perceus + the compiler's own sources) use that shape.
- **Literal-discriminated matches** (`match n { 0 -> ...; 1 -> ... }`) and
  **record-pattern discrimination** still emit empty switches in the KIR
  (5 perceus fixtures: `match_pbind_catchall_raw`, `unbox_phase2_match`,
  etc.). These are SEPARATE, pre-existing gaps the code already flags
  (the `lower_default_arm` comment: "exact literal dispatch is a later
  refinement"); out of scope for this list-pattern lane.

## Cost note

The edited `kir_lower_walk.kai` slipped A− (87.4) → B+ (85.4): the driver
adds operators to a file whose weak dimension is already Halstead Effort
(F) — the Expr-SCC is inherently operator-dense. Above the B hard floor,
and the shared-preamble extraction removed a near-duplicate the change
would otherwise have introduced. Restoring A− would require moving the
driver out, which the module-DAG cycle forbids. The new code itself is
A++; the slip is the F-monolith's pre-existing weakness, not new debt.
