# Lane experience — #1182 + #1181: stdlib name hygiene (prelude→core, length/fold canonical)

Two pre-1.0 hygiene issues in one lane, ordered A-then-B so the runtime
symbol names stabilise before the surface-name canonicalisation lands on
top. One rebase, one byte-id re-baseline at the end.

## Scope as planned vs as shipped

**Planned (#1182 — Part A):** rename the 244 `kai_prelude_*` runtime
symbols to `kai_core_*`, retire the legacy `load_prelude`/`PreludeSegment`
compiler identifiers, sweep 5 stdlib files + docs. "Mechanical but large."

**Planned (#1181 — Part B):** one canonical name per concept for
length (`size`/`length`/`count`/`len`) and fold (`reduce`/`fold`/`foldl`),
deprecate-alias the rest, note `kai migrate`.

**Shipped:** both, with three corrections the issues did not anticipate
(below). Part A touched more layers than the issue named — two runtime.h
copies, the stage-1 emitter, and a Makefile test grep — because the
symbol rename has to be atomic across every layer that emits or defines
the name. Part B ended up smaller than the issue implied, because two of
its three "redundancies" were misdiagnoses.

## The three corrections to the issues (design decisions)

1. **`list.count(p)` is not a length.** #1181 listed list as having "both
   count AND length" as redundancy. It is not: `count(xs, ^p)` is
   count-*if* (how many elements satisfy `p`), a distinct operation with
   its own intent. Left untouched.

2. **`stream.count` is not a length either.** It is a sink that drains a
   lazy stream in constant memory — sibling of `fold`/`each`/`to_list`.
   Renaming it to `length` would falsely imply O(1)/non-consuming.
   `count` is the correct name here (Elixir `Enum.count`, Rust
   `Iterator::count` both consume). Left untouched.

3. **`reduce` is not a stdlib redundancy of `foldl`.** `reduce` is a
   first-class compiler builtin (registered in infer.kai / resolve.kai,
   emitted as `kai_core_reduce`), the pipe-HOF sibling of
   `map`/`filter`/`each`/`flat_map`. `foldl` is a stdlib `pub fn`. They
   live in different layers with different invocation forms; the "no two
   names for one intent" principle vetoes redundancy *within* a layer,
   not a builtin coexisting with a stdlib fn — same as `map` (builtin)
   coexisting with any stdlib `.map`. Deprecating `reduce` would touch
   typer/resolve/emit for zero coherence gain and break demos. Left
   untouched.

Net: the length canonicalisation reduced to one rename (`stream.count`
stayed, `queue`/`stack` stayed `size` per the indexable-vs-cardinality
rule — see below), and fold reduced to `stream.fold`/`map.fold`→`foldl`
plus `string_builder.len`→`length`.

## The length rule: indexable→length, cardinality→size

Rather than force one name everywhere (which makes "size of a list" *or*
"length of a set" read wrong for someone), the canonical split is by
shape: `length` where positions exist and indexing makes sense (list,
string, vec, string_builder), `size` where the abstraction is
cardinality of an ADT with no positional access (map, set, hashmap,
hashset, queue, stack). A stack is not indexable — you see only the top —
so `size` is right there (matches Java `Stack.size()`/`Deque.size()`).
The rule is documented in the `kai info deltas` card so it is
predictable, which is the actual bar #1181 set ("can a user predict the
name they have not seen").

## Structural surprises the brief did not anticipate

- **Two `runtime.h` copies.** `stage0/runtime.h` and `stage2/runtime.h`
  are separate committed files (stage2 carries the Koka runtime; `-I`
  puts stage2 first). Renaming only stage0 left kaic2 linking against
  the stale stage2 copy → `kai_core_print` undeclared. Both must move in
  lockstep. (Known trap: `project_kaikai_two_runtimes_int_tagging_trap`.)

- **stage-1 emits the runtime symbol names.** `kaic1` compiling the
  stage-2 bundle emits `kai_prelude_string_concat` from its own
  `prelude_table`. Those C-symbol strings had to be renamed in
  `stage1/compiler.kai` too, or the emitted `build/stage2.c` mismatches
  the renamed runtime.h. But stage-1's *internal* prelude vocabulary
  (the `--prelude` flag, `prelude_table` fn, `EPrelude` type) is LIVE
  surface of kaikai-minimal — stage 1 has no core auto-load mechanism —
  so only the emitted C-symbol strings were renamed there, not the flag.

- **The `load_core` arity collision.** `load_prelude(path)` naively
  renamed to `load_core(path)` collided with the pre-existing
  `load_core(proj_dir, cache_on)` entry point (kaikai has no overloading).
  The ex-prelude single-file/loop loaders became `load_core_file` /
  `load_core_files` / `load_core_file_cached` to stay distinct.

- **`PClauses.prelude` is a false friend.** parse.kai has a `prelude:`
  field on the handler-clause struct (the var/let statements before a
  handler's op clauses) — a completely different concept. A blind
  `s/prelude/core/` would have silently corrupted it. Excluded.

- **Makefile test grep pins the emitted symbol.** `stage2/Makefile`
  greps `build/*.c` for `kai_prelude_array_make` in the issue_1179 test;
  the emitted symbol is now `kai_core_array_make`, so the grep had to
  move too or the test would silently pass/fail on a stale string.

## What is deliberately left as "prelude"

- Historical narrative: `stage2-design.md` (Phase-4 `--prelude` retiral
  story), lane retrospectives, `prelude-audit.md`. Changing these would
  make the history lie.
- Live stage-1 surface: `kaic1 --prelude` / `--include-prelude-tests` in
  docs, and stage-1's internal prelude vocabulary.
- The `audit-prelude-*.sh` tool filenames (out of lane; describe a
  historical prelude/core-scope audit).

## Deprecation mechanism

kaikai has no `#[deprecated]` attribute. Deprecation is: keep the old
name as a one-liner alias delegating to the canonical, note it in the
`#[doc]`, and add a `kai migrate` rewrite rule
(`compiler/migrate_rules.kai`, the existing Hanga-Roa→Orongo rule set).
Verified `kai migrate` rewrites `map.fold`→`map.foldl` and
`string_builder.len`→`string_builder.length` on canonical module names.
Known limitation (pre-existing, shared with the `from_pairs`→`from`
rules): migrate matches the syntactic receiver name, so an
`import string_builder as sb` alias is not rewritten — the migrate walk
does not resolve import aliases. Not fixed in this lane (out of scope,
affects every existing rule equally).

## Byte-id re-baseline

Expected and legitimate: renaming symbols changes the emitted C, so the
C and native byte-id both change vs main. Not non-determinism — selfhost
determinism (`kaic2b.c == kaic2c.c`) stayed green after both Part A and
Part B. The re-baseline is a rename artefact.

## Fixtures / coverage

No new fixture file — the lane is rename + alias, and the existing
fixtures exercise the shapes:
- `examples/effects/string_builder_build_correct.kai` updated to
  `string_builder.length`, golden unchanged (the `len=` in the output is
  literal text, not the method name).
- `examples/stdlib/map_round_out.kai`, `demos/wc.kai`,
  `demos/log_stats/main.kai` updated to `foldl`.
- The deprecated aliases are exercised by keeping them compilable; the
  `kai migrate` rules are the regression surface for the rewrite.

Gates run green locally (C backend, serial): selfhost byte-id,
test-stdlib-modules (71/71), test-info-blocks (116/116), test-doc.
Heavy gates (native parity serial, ASAN, modular selfhost) deferred to
CI per lane discipline.

## Follow-ups for next lanes

- The alias removal at the Orongo edition pin (the aliases are tongariki
  one-liners; drop them + the migrate rules become no-ops once every
  user has migrated).
- `kai migrate` import-alias resolution (pre-existing gap; would make the
  rules fire through `import X as Y`). Separate concern, own issue if
  wanted.
