# Lane experience — issue #668: list.map over large list overflows fiber stack

## Scope as planned vs. as shipped

**Reported:** `list.map(xs, f)` over >~3K elements inside a fiber
(64 KiB stack) overflows; the same call from `main` (8 MiB) succeeds.
The issue suspected the `kai_apply` HOF closure chain (3 C frames per
element, none TCO'd at -O0).

**Shipped:** the suspicion was wrong. The overflow is the plain
non-tail recursion of `list.map` itself. Rewrote `map`, `filter`, and
`flat_map` in `stdlib/core/list.kai` tail-recursive + `reverse`, which
the backend TCOs into a goto-loop (O(1) C stack). Regression fixture +
`make test-issue-668`.

## Root cause (correcting the issue's hypothesis)

`list.map` was `[f(h), ...map(t, f)]`. The recursive `map(t, f)` is NOT
in tail position — it sits inside the cons constructor — so each
element pushes one C frame of `kai_list__map` that does not return
until the whole list is built. 40K elements = 40K nested frames =
overflow of the 64 KiB fiber stack. From `main` the 8 MiB OS stack
absorbed it, which is why it only showed inside fibers.

The issue author tried "rewriting list.map tail-recursive + reverse:
same overflow" and concluded the recursion was not the issue, blaming
`kai_apply`. That conclusion was wrong. Verified directly:

- The emitted C for the repro shows `kai_list__map(kai_t, ...)` called
  *inside* building the cons — the non-tail recursion, not an
  `kai_apply` loop. (`kai_prelude_map` in the runtime IS already an
  iterative `while` loop, O(1) stack — but `list.map(xs, f)` as a
  direct call resolves to the stdlib kaikai `map`, not that primitive;
  the primitive only backs the pipe operators.)
- A hand-written tail-recursive `map` + `reverse` with `f` applied as a
  **closure** (exactly `list.map`'s shape) runs 40K elements in a fiber
  with zero overflow. So `kai_apply` per element is fine — each `f(h)`
  returns before the next tail-recursive step; it never stacks.

The author's failed experiment most likely didn't actually reach a
tail-position self-call (the backend's `__kai_tcrec` rewrite only fires
on a genuine self-tail-call). The fix here is a textbook
accumulator-then-reverse, which the rewrite does TCO.

## Fix

`stdlib/core/list.kai`:
- `map` → `reverse(map_loop(xs, f, []))`, `map_loop` tail-recursive.
- `filter` → `reverse(filter_loop(xs, p, []))`.
- `flat_map` → `reverse(flat_map_loop(xs, f, []))`, where each `f(h)`
  is pushed onto the accumulator reversed via the existing
  `reverse_loop` helper, so one final `reverse` restores order.

Effect order is preserved (the issue's `map` doc promises left-to-right
`f` invocation): the accumulator prepends in input order and the final
`reverse` restores it, so `f` still fires left-to-right. Verified with
a `Stdout`-effecting `f` printing `visit 10 / 20 / 30` in order.

## Why stdlib rewrite, not a codegen/runtime change

The issue floated three directions: (1) make `kai_apply` loop, (2)
codegen direct-call when a HOF is monomorphised against a known `fn`,
(3) bigger default fiber stack. (1) is moot — `kai_apply` was never the
problem. (3) hides the cost. (2) is a real optimization but far larger
than needed. The actual bug is one non-tail recursion in three stdlib
functions; fixing it in kaikai is minimal, preserves HOF polymorphism,
and rides the TCO the backend already does. Note `take`, `zip`,
`zip_with` share the non-tail shape but are bounded by their `n` /
shorter-list argument in practice; left as-is this lane (only the three
the issue's HOFs name were unbounded over arbitrary input). A follow-up
could sweep the rest if a fiber-bound caller hits them.

## Fixtures added

- `examples/effects/issue_668_map_large_in_fiber.kai` (+ `.out.expected`)
  — map/filter/flat_map over 40K elements inside a spawned fiber at the
  DEFAULT stack (no `KAI_FIBER_STACK_SIZE` escape), output `total:
  120000`. A regression to the non-tail form crashes here.
- `make test-issue-668` wired into `test` + `test-fast`.

## Verification

- repro and fixture pass at default fiber stack; selfhost byte-identical;
  test-stdlib 149 OK / 0 FAIL; demos-core green (portfolio/usd_to_eur
  exercise HOFs); map/filter/flat_map correctness + empty cases + effect
  order all confirmed.

## Follow-ups

- `demos/9d9l/weather/main.kai -p` documented the
  `KAI_FIBER_STACK_SIZE=16M` workaround inline; with this fix that note
  can be removed (not done here — out of this lane's file).
- `take` / `drop` / `zip` / `zip_with` / `concat` / `map_indexed` share
  the non-tail pattern but are bounded in normal use; a sweep is a
  cheap follow-up if any becomes load-bearing inside a fiber.
