# Lane experience — variant-slot FAM C99 portability (Linux clang CI block)

## Scope as planned vs. as shipped

**Planned:** the session task was to close the 4 CI-red bugs from the
2026-06-03 handoff, then watch CI. After all 4 landed and were pushed,
CI was *still* red on every commit — tier1, tier1-asan, and
tier1-backend-parity all failed at the same point.

**Shipped:** diagnosis showed the CI red was a **pre-existing**
portability bug, not from any of the 4 fixes. `stage0/runtime.h` had a
flexible array member (FAM) in a configuration C99 forbids; Apple clang
accepts it as a silent GNU extension (local builds passed), Linux clang
(CI) rejects it, breaking the stage1 bootstrap before any test ran. This
lane is that fix: drop the FAM from the union, access variant slots via
a `kai_var_slots(v)` cast macro.

## Diagnosis — it was not one of the 4 fixes

- `gh run list` showed `failure` on all 3 workflows for all 4 of my
  commits AND would have failed identically without them.
- The log pinned `stage1/build/stage1.c` failing to compile:
  `stage0/runtime.h:364: error: flexible array member 'var_slots' not
  allowed in otherwise empty struct`.
- `git merge-base --is-ancestor e02b185 962f818` confirmed the offending
  commit (`e02b185`, "wrap variant FAM in a struct") was already in
  `origin/main` before my session base. My session inherited a red main.

## Root cause — two stacked C99 violations

The Koka-packed node representation (#83f5705, #741) stores variant
payload slots INLINE, overlapping the union `as`, so a 5-slot node is
8B header + 5×8 = 48B (Koka's exact size — the whole point, for Perceus
in-place reuse and rb-tree perf). To express "slots at the union
offset", the slots were declared as a flexible array member.

- **First attempt** (`83f5705`): `KaiVarSlot var_slots[]` as a direct
  union member. C99 §6.7.2.1 forbids a FAM in a union → Linux clang:
  "flexible array member in a union is not allowed".
- **Second attempt** (`e02b185`): wrap it — `struct { KaiVarSlot
  var_slots[]; } var;` inside the union. But a struct whose *only*
  member is a FAM is ALSO illegal C99 → Linux clang: "flexible array
  member in otherwise empty struct is a GNU extension". `e02b185` traded
  one Linux-clang error for another, and again passed local (Apple clang
  accepts both as silent GNU extensions).

Reproduced locally with `clang -std=c99 -pedantic-errors`:
`-Wgnu-empty-struct` fires on the exact construct.

## Fix (Linus's verdict; asu concurred)

There is no way to keep the `as.var.var_slots` spelling without a GNU
extension or a dummy member that breaks the 48B offset. The clean,
portable fix:

1. **Drop the `var` member from the union entirely.** The union keeps
   its other members (`rec`/`arr` at 24B), so its size is unchanged.
2. **Access slots via a macro:** `#define kai_var_slots(v)
   ((KaiVarSlot *)(&(v)->as))`. Since slots overlap the union at offset
   0, `&v->as` cast to `KaiVarSlot *` is the slot array — same address,
   same layout, zero size cost.
3. **Replace `kai_var_block_size`'s bogus-pointer trick**
   (`(char*)&((KaiValue*)1)->...var_slots[0] - (char*)1`, itself a
   UBSan misaligned-pointer hazard) with `offsetof(KaiValue, as)` —
   what stage2 already did. Added `#include <stddef.h>` to stage0's
   header for `offsetof` (stage2 already had it).

### Alternatives rejected

- **struct hack `var_slots[1]`** (keeps the spelling): real UB. C99
  §6.5.6p8 bounds pointer arithmetic to the declared array; indexing
  past `[0]` is out-of-bounds. `-fsanitize=address,undefined` (the ASAN
  tier) fires on it. Not folklore — it is caught in practice.
- **C11 anonymous-struct FAM-in-union**: C11 lifts the union ban but the
  "otherwise empty struct" ban survives; a dummy pad member breaks the
  offset. Rejected.
- **Pull the FAM out as the last member of the outer KaiValue**: slots
  would land after the whole union (offset 8+24), breaking the 48B node.
  Rejected.

## Blast radius

The `as.var.var_slots` spelling appears as both real C expressions
(runtime) and emitted-text string literals (the 3 emitters):

- `stage0/runtime.h` — struct member dropped, macro added, ~26 access
  sites rewritten, `kai_var_block_size` → `offsetof`, `<stddef.h>` added.
- `stage2/runtime.h` — same, ~28 sites, `kai_var_block_size` → `offsetof`.
- `stage0/runtime_llvm.c` — 1 site.
- `stage0/emit.c` — 2 `snprintf` format strings.
- `stage1/compiler.kai` — 2 emitted-text literals.
- `stage2/compiler/emit_c.kai` — ~16 emitted-text literals (varied
  suffixes: `.ptr`, `.i64`, `.r`, `[i] = slots[i]`, `&...`, bare `[i]`).

Mechanical search-and-replace; the receiver in every site is a simple
identifier or a literal-embedded name, so a captured-identifier perl
substitution handled them cleanly.

## Verification — layout-identical, perf unchanged

- **C99 portability:** `clang -std=c99 -Werror=gnu-empty-struct
  -Werror=flexible-array-extensions` on the regenerated stage1.c → zero
  FAM/empty-struct errors (the exact diagnostics CI Linux rejected).
- **Layout identical:** a standalone test confirmed
  `offsetof(old.as.var.var_slots) == offsetof(new.as) == 8` and
  `sizeof` unchanged; `kai_var_slots(p) - p == offsetof(as) == 8`.
- **Bootstrap + selfhost:** clean stage0→stage1→stage2 rebuild;
  `kaic2b.c == kaic2c.c` deterministic. Emitted C uses `kai_var_slots`
  (7593 sites), zero `as.var.var_slots`.
- **rb-tree perf (the load-bearing concern):** 1M-insert bench, C
  backend -O2, 5 runs each. Pre-fix median 0.454s (min 0.440), post-fix
  median 0.463s (min 0.440). Ranges overlap, mins identical — no
  regression. Expected: the macro inlines to the same pointer arithmetic
  as the old member access under -O2. size=1M height=29 in both
  (Perceus reuse-in-place still fires; correctness identical).

## Fixtures

No new fixture — the regression surface is the CI bootstrap itself
(stage1 compiling under Linux clang `-std=c99`). The pre-existing
tier1/tier1-asan/parity workflows ARE the guard; they were red because
of this bug and go green with the fix. The `-Werror=gnu-empty-struct`
recompile is the local proxy for the Linux-clang gate.

## Follow-ups

- The honesty note in `e02b185`'s message ("verified ... compiles under
  the exact CI flags") was wrong — it verified against Apple clang,
  which accepts the GNU extension. This lane's local proxy
  (`-Werror=gnu-empty-struct` / `clang -std=c99 -pedantic-errors`)
  should be the standard check for any runtime-header change that could
  hit a GNU extension, since the mac dev environment silently accepts
  them. Worth a note in the testing-tiers doc.
