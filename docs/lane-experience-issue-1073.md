# Lane experience — issue #1073 (C-oracle ASAN-on-selfhost heap-buffer-overflow in typecheck_module)

## Scope as planned vs shipped

Planned: a localisation spike — reproduce the ASAN-on-selfhost
heap-buffer-overflow in the C oracle, pin the exact corrupt access, fix it.
The issue left two candidates open after its reuse bisect: TRMC/cctx spine
reuse, or a negative/misaligned slot walk in the typer over `expand_ta_decl`
output.

Shipped: the bug was **neither** candidate as literally stated — it is the
C emitter's *hoisted sub-pattern load* (`_hs<i>`), an unconditional read of
slot `i` emitted at the top of a match before any arm's tag test runs. The
fix guards the hoisted load with the shared ctor's tag (one `?:` in the
generated C). One emitter function split in two (`emit_hoist_decls` gate +
`emit_hoist_decls_loop` spelling), two comments corrected, one fixture, one
corpus line. The "slot walk in the typer" framing was close: the walk was in
typer *code*, but planted by the emitter for every match with a nested
sub-pattern — infer.kai itself has no bug.

## How the access was localised

1. **Reproduce first** (issue method): `kaic2 main.kai > kaic2b.c`, compile
   with `-fsanitize=address -DKAI_NO_CELL_POOL -O1 -g`, run self-compiling
   `main.kai`. Overflow reproduced identically on the first run — READ of
   size 8, 16 bytes before a live 64-byte `kai_variant_u_fast` region
   allocated by `expand_ta_decl`.
2. **Shadow-map arithmetic**: the buggy address sat in a 32-byte redzone
   *between* two 64-byte blocks. Read as `left_block + 80` it is slot 9 of a
   7-slot variant (blocks are `8 B header + n×8` — a 64-byte block holds 7
   slots; `expand_ta_decl`'s only 7-slot allocation is `DType`). That
   suggested "slot 9 of a DType read as if it were a DFn" before any
   disassembly.
3. **Statement-per-line resplit**: the crash line in `kaic2b.c` was one
   8,966-char generated statement — useless granularity. Re-splitting the
   generated function body on `"; "` and rebuilding under ASAN moved the
   report to a 2-line window: the inlined body of
   `collect_root_fn_names_loop` (its caller `typecheck_module` absorbed it
   at `-O1`, which is why the ASAN frame pointed at typecheck_module all
   along).
4. **Disassembly cross-check** (`llvm-objdump -d -l` at the crash PC,
   ASLR slide recovered from `nm` + the report's PC) confirmed the same
   line attribution.
5. The generated code at that line is the smoking gun:
   `KaiValue *_hs9 = kai_var_slots(_scr)[9].ptr;` emitted *before* the
   arm test `_scr->variant_tag == 194 (DFn) && ...`. The kaikai source is
   `match d { DFn(_, n, _, _, _, _, _, _, _, None) -> ...; _ -> ... }` —
   the `None` sub-pattern makes slot 9 a hoisted nested-deref slot, and the
   catch-all admits every other `Decl` ctor, including 7-slot `DType`.

## Root cause

`emit_hoist_decls` emitted the hoisted load unguarded, resting on the
comment's claim that any non-matching scrutinee is a nullary singleton of
`sizeof(KaiValue)` "so slot i is inside it, not OOB". That premise died when
variant blocks became arity-sized (`kai_alloc_var_nz(n)`, the 80 B → 48 B
Koka-packed shrink): a *payload* sibling ctor smaller than the hoisted slot
index flows through any catch-all arm (or a non-exhaustive match) and the
load reads past its block. A second latent hole in the same shape: a sibling
with *enough* slots but a **primitive** payload at slot i would hand the
hoisted local a raw i64 as a pointer, and the arm test's `_hs<i>->tag` deref
would chase it. The tag guard closes both at once; a bounds-only
(`var_n_args > i`) guard would have closed only the first.

## Why the cell pool masked it, and why byte-id never saw it

The junk value loaded from out-of-bounds is **never used**: every consumer
of `_hs<i>` sits behind the arm's `variant_tag == CTOR` test, which fails
for the smaller ctor. So the read is invisible to output comparison
(byte-id green), and with the cell pool the address lands in pool-owned,
mapped memory (no segfault). Only ASAN + `-DKAI_NO_CELL_POOL` puts a
redzone there. It is real UB regardless: the same read at the end of a heap
page would fault.

Reuse-independence (the issue's bisect) also follows: the load is planted
by the *match emitter*, not by any reuse/donation path — forcing all 212
donation sites to the fresh allocator could not move it.

## Fix shape

`_hs<i> = (_scr->tag == KAI_VARIANT && _scr->variant_tag == TAG) ? slots[i] : NULL`.
The tag constant is the shared ctor the hoist gate already requires, so the
guard compares against the same predicate every arm test starts with —
clang CSEs them, keeping the hoist's original purpose (sharing the
dependent load across arms, which clang cannot CSE across branches) intact.
The emitter resolves the tag via `evar_find_tag_opt` from the same
`cx.variants` table the hoist-slot gate already consulted, so a resolvable
ctor is an invariant, not a new failure mode.

Considered and rejected:
- **`var_n_args > i` dynamic bound** — no plumbing, but leaves the
  primitive-slot-as-pointer hole open.
- **Static per-sum rule** (hoist only when every sibling arity > i) —
  sound, but needs the sum table threaded into `EmitCtx` (~10 ctor copies
  to touch) and still needs the nullary-block special case; the dynamic
  guard is one comparison the arms pay anyway.

## Fixture

`examples/perceus/match_hoist_smaller_ctor_1073.kai` — a 10-slot ctor with
a nested `Option` sub-pattern at slot 9, a 7-slot sibling (mirroring the
64-byte DType geometry), and a catch-all. Validated both ways: the C
emitted by the pre-fix compiler aborts under ASAN/no-cell-pool with the
exact self-compile signature (READ of size 8, 16 bytes past a 64-byte
region, in the match fn); the post-fix emit runs clean with the correct
golden. Wired into `tools/rc-detector-corpus.txt`, so the rc-detector's
ASAN/no-cell-pool + golden pass covers it on both backends permanently.
The bounded repro was achievable here (unlike some prior self-compile-scale
bugs) because the trigger is purely shape-local: one match, one small
sibling ctor.

## Cost vs estimate

Localisation dominated, as the brief predicted, but converged fast thanks
to the shadow-map arithmetic + resplit trick (two ASAN self-compile builds
total, no `-O0` build needed). The fix itself is ~20 lines of emitter
change. The statement-resplit-then-rerun technique (sed `"; "` → newline on
the one suspect function, recompile, let ASAN name the exact statement) is
reusable for any future crash inside a megaline of generated C.

## Follow-ups left

- The native backend has no `_hs` hoist (checked `kir_lower*` /
  `emit_native*`), so nothing to mirror there.
- stage1's emitter never hoists (checked); stage0/1-built compilers are
  unaffected.
- The TRMC cctx comment in `stage2/runtime.h` ("holeptr points into the
  SEPARATE slots[] array") predates the packed layout — slots are inline
  now. Comment-only drift, left untouched here (out of lane scope, no
  behavioural claim relied on).
