# Lane experience — issue #677 Phase 1a/1b/1c: stage 2 modularisation (2026-05-23)

## Scope as planned

Split `stage2/compiler.kai` (70 k lines, one file) into per-section
modules under `stage2/src/`, with a Makefile concat shim so kaic1
keeps consuming a single translation unit. The issue lists ~10
target modules (chars / lex / diag / parse / intervals / resolve /
desugar / infer / modules / protocols / emit_c / emit_llvm / fmt /
driver) and three phases:

- **Phase 1**: source split, concat-bootstrapped, real package for
  kaic2.
- **Phase 2**: interface persistence (`.kaii` sidecars), gated on
  #452.
- **Phase 3**: separate compilation with linking, gated on Phase 2.

This lane targeted only Phase 1, starting with the smallest module
(`chars`) as the proof of mechanism. The handoff coming into the
session named two increments: 1a (bundle infrastructure) and 1b
(chars extraction). 1c (flip to G) emerged mid-lane after a Q&A
about whether the package layout could land directly instead of
sitting behind the bundle indefinitely.

## Scope as shipped

Three increments, three commits, all green:

1. **Phase 1a (`5a1ba77`)** — Bundle infrastructure. New
   `BUNDLE_SRCS` variable + `build/bundle.kai` target that
   concatenates the listed sources with `awk 1` (preserves a
   trailing newline per file so a missing one cannot fuse tokens
   across the file boundary). `stage2/src/all.kai` is created as a
   byte-identical copy of `compiler.kai` and becomes the single
   entry in `BUNDLE_SRCS`. No code moves out of the monolith yet.
   The bundle is byte-identical to `compiler.kai`, so the emitted
   `stage2.c` is identical between `kaic1 compiler.kai` and
   `kaic1 build/bundle.kai`. This was the invariant the increment
   was designed around: anyone diffing artifacts pre / post-
   modularisation should see zero noise.

2. **Phase 1b (`cd1312e`)** — First per-module extraction.
   `stage2/src/chars.kai` receives the six char-classification
   primitives (`ch_is_digit`, `ch_is_lower`, `ch_is_upper`,
   `ch_is_alpha`, `ch_is_alnum`, `ch_is_space`) verbatim. They
   stay non-`pub` because Phase 1b is still bundle-only — no
   cross-module call actually exists. Header is plain `#`
   comments because the file is destined to land inside the
   kaic1-consumed bundle, and kaic1 does not support triple-
   quoted strings. The user accepted that this increment loses
   the byte-identical property against `compiler.kai`: the
   emitted C now differs by 52 lines, all gensym renames of the
   form `kai___list_rest_<line>_<col>__` shifted by exactly the
   line delta the extraction introduced. No semantic divergence.

3. **Phase 1c (`35e5987`)** — Flip to G (package layout).
   `stage2/kai.toml` declares the package as `compiler`.
   `stage2/main.kai` (renamed from `src/all.kai`) becomes the
   entry point. `stage2/compiler/chars.kai` (moved from
   `src/chars.kai`) gains `pub fn` and `#[doc("...")]` attributes
   per the surface proposed in issue #681. `stage2/src/` is
   deleted. `main.kai` gets a top-of-file `import compiler.chars`
   that kaic2 resolves against `compiler/chars.kai` and kaic1
   ignores. `Makefile` updated: `SRC := main.kai`, `BUNDLE_SRCS`
   tracks the new paths. The bundle path keeps working for the
   kaic1 bootstrap; the package path is now the primary input
   for kaic2. A pilot test file (`stage2/tests/test_chars.kai`,
   six tests) demonstrates that `kai test stage2/.` discovers
   sibling tests via the package manifest and runs them against
   the package's modules.

## Design decisions and alternatives considered

### Concat-bundle vs source-of-truth flip

`asu-language-architect` recommended option A (concat without
strip, header auto-generated). `linus` recommended option G (flip
source-of-truth: `src/*.kai` authoritative, `compiler.kai` as
artifact generated). The user picked "A first, eventually G".

Phase 1a + 1b implemented A. Phase 1c implemented G — earlier
than the original plan, because the Q&A round-trip "does kaikai
compile packages today?" surfaced that nothing was blocking the
flip beyond writing a 3-line `kai.toml`. The user's "no podemos
flipear a G ya?" landed at exactly the right time for the answer
to be empirical instead of speculative.

### No header preamble on `build/bundle.kai`

A header would shift downstream line numbers by 1–N. kaikai
encodes the source location into gensym names
(`kai___list_rest_<line>_<col>__`), so a 1-line header would
cause every gensym in the emitted C to differ, even though the
program is the same. Phase 1a relied on byte-identical equivalence
to convince the reader that the bundle is genuinely the same
input to kaic1 as the monolith. The mechanism it cost: a small
amount of clarity at the top of the bundle file. The clarity
moved into the Makefile recipe comment instead.

### Why `name = "compiler"`

Four candidates were on the table: `compiler` / `kaikai` /
`kaic2` / `stage2`. The package-resolver convention (per the
working `examples/packages/sibling_examples_tests/`) maps
`import <name>.<mod>` to `<root>/<name>/<mod>.kai`, so the name
becomes a visible subdirectory. The user picked `compiler` for
semantic clarity (the package IS the compiler) and the modest
risk that the name is generic enough to collide with some future
unrelated `compiler` package was judged acceptable.

### `#[doc(...)]` surface today

Issue #681 proposes `#[doc("...")]` as a first-class attribute.
That issue is open and not yet implemented. The user asked
whether kaikai's parser passes `#[doc(...)]` through silently
today — `parse_attr_unknown_body` in `compiler.kai` confirms it:
unknown attributes are parsed (consuming the optional `(args)`
and closing `]`) and dropped, with a comment that literally
names `#[doc("...")]` as the motivating example of the forward-
compat shape. So Phase 1c ships doc strings on `chars.kai` that
become machine-readable the moment #681 Phase 1 lands.

One constraint surfaced empirically: kaic1's lexer does not
support triple-quoted strings, so module-level docs that want
multiple paragraphs cannot use `#[doc("""...""")]` while the
file feeds into the kaic1-consumed bundle. The doc paragraphs
moved to a `#` comment header above the attributes; per-fn docs
are single-line `#[doc("...")]` (kaic1 accepts those, including
backticks inside the string). The compiler-architecture answer
"separate compilation lets kaic1 stop seeing this file" is
exactly the Phase 2/3 direction the issue points to.

### Test orchestration shape

`kai info testing` documents the sibling `tests/` directory
convention. Empirically: `kai test .` compiles the package once
and then runs each `tests/*.kai` file in turn, with imports
resolved against the package's modules via sibling resolution.
The pilot test file (`test_chars.kai`) follows this exactly.

Cost surprise: 75 s wall-clock to compile the 70 k-line package
+ run 6 trivial char tests. That cost is per-test-file, not per-
test, because the package compile is the dominant term. Tier 1
integration deferred until interface persistence (#452 / #677
Phase 2) drops that to <1 s per test file. Today the file
exists as evidence + as a usable manual probe; a future lane
wires the runner into the tier.

## Structural surprises the brief did not anticipate

### kaic1 supports more than expected

The pre-lane assumption was that adding `pub`, `import`, or
`#[doc(...)]` to a file destined for the bundle would break
kaic1. Empirical reality: kaic1 parses all three. The only kaic1
limitation we hit was triple-quoted strings (`"""..."""`).
Single-line strings with embedded backticks parse fine.

This rewrites the cost model for the next per-module extractions:
each module can land with `pub` + `#[doc(...)]` from day one,
even while still feeding the bundle. The triple-quote constraint
only bites module-level multi-paragraph docs; per-fn one-line
docs are free.

### `ch_is_hex_digit` separated from the `ch_is_*` block

When mapping the source, the seven `ch_is_*` predicates were
expected to be contiguous. Six of them are (lines 26–31 of the
pre-Phase-1b `compiler.kai`); `ch_is_hex_digit` sits at line 346,
buried inside the lex section because the lexer's hex-escape
path calls it immediately. Phase 1b extracted the six contiguous
ones; `ch_is_hex_digit` stayed in lex on the byte-identical
discipline that Phase 1a had pinned. Phase 1c retired that
discipline (the C output already diverges from `compiler.kai`
on gensym renames), so the seventh predicate is now free to
join `chars.kai` in a follow-up lane.

### `kai test .` reports `0/0 tests passed` for `main.kai`

The runner compiles the entry point as a no-tests test binary
before running each `tests/*.kai` file. The `0/0 tests passed`
line for `main.kai` is harmless but visually confusing. Not a
lane-blocking issue; worth noting if a future cosmetics lane
revisits the test driver's output.

## Fixtures added and coverage gaps

- `stage2/tests/test_chars.kai`: six tests exercising the
  digit / letter / underscore / alnum / space contracts of the
  extracted `chars.kai`. Includes the explicit assertion that
  `ch_is_space('\n')` is false (the newline-significance
  invariant of the kaikai lexer).

- Property checks (`check "..." with x: Char { ... }`) NOT
  added. The kaikai language supports them per `kai info
  testing`; the user's preference was "solo si kaikai ya soporta
  check forall". Verified the syntax exists, but did not write
  any in this lane. Two natural candidates for the next module
  retro: `check "ch_is_alpha implies ch_is_alnum" with c: Char
  { ... }` and `check "ch_is_digit and ch_is_alpha are disjoint"
  with c: Char { ... }`.

- No tier 1 wiring. Future lane.

## Real cost vs estimate

Issue estimated Phase 1 at "1–2 weeks. Mostly mechanical." Real
cost: one focused session, three commits, ~6 hours of work
including the design Q&A on package layout and the empirical
verification of kaic1's `import` / `pub` / `#[doc]` tolerance.

The over-estimate has two sources:

1. **Linus's pub-discipline warning was overstated** for the
   chars boundary. The six fns had no caller in `all.kai` post-
   extraction (the calls compiled clean because the bundle
   merge re-united them); only Phase 1c needed `pub fn`, and
   the change was a one-character edit per fn.
2. **kaic2's package mode was already production-ready.** The
   pre-lane mental model treated `kaic2` as a single-file
   compiler that didn't know about `kai.toml`. Empirically:
   `bin/kai build .` and `kaic2 main.kai` both work fine on a
   3-line `kai.toml` package, and `kai test .` walks the tests
   directory exactly as documented. No work needed at the
   compiler-driver layer.

Together those two cleared most of the estimated work for
chars. The estimate is still probably right for the bigger
modules (parse, infer) where the cross-module call graph is
denser and `pub` discipline becomes a real audit.

## Follow-ups for the next lanes

- **Move `ch_is_hex_digit` to `compiler/chars.kai`.** Tiny lane,
  no risk; the byte-identical invariant that was holding it in
  place is no longer active post-Phase-1c.

- **Extract `diag` (the next module in the dependency order
  from the handoff).** Diagnostics has no compiler-state deps
  other than `String`. Should follow the Phase 1c pattern:
  `pub fn` + `#[doc("...")]` + `import compiler.diag` line in
  `main.kai`.

- **Cycle audit before `protocols` and `modules`.** Both `asu`
  and `linus` flagged these as having potential bidirectional
  edges with `infer` / `resolve`. The retro for those lanes
  should include the grep cross-section that confirmed the
  edges go one way only (or document the public-surface refactor
  that broke the cycle).

- **Tier 1 integration for module tests** once #452 lands.
  Until then, `kai test stage2/.` is a manual probe; the
  module-test runner cost is dominated by the 70 k-line
  package compile.

- **Doc-discipline updates**. `docs/stage2-design.md` should
  pick up a section describing the new layout when one or two
  more modules have been extracted (premature to write it
  with just `chars`).

- **#681 dependency reverse-pointer**. When #681 Phase 1 lands,
  the `#[doc(...)]` attributes in `compiler/chars.kai` start
  showing up in `kai info builtins --json` automatically. A
  one-line update to #681's lane retro should mention that
  stage 2's module sources became its first real-world doc
  inventory.
