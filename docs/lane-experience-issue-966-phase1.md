# Lane experience — issue #966 phase 1: `kai lint` skeleton + `discard_pure_value`

## Scope as planned vs as shipped

**Planned (brief):** the `kai lint` subcommand skeleton + JSON output +
ONE rule (`discard_pure_value`), verified on the native default. Not the
full rule catalog — phase 1 is the command working with one real lint.

**Shipped:** exactly that. `kai lint [--json] <file|package>` is wired
beside `build`/`run`/`test`/`fmt` (bin/kai wrapper + `MLint`/`MLintJson`
driver modes), reporting `discard_pure_value` with dual human-text and
JSON output. The compiler core is unchanged in severity; the lint is
opt-in and always exits 0. A new A-grade module `stage2/compiler/lint.kai`
holds the rule. Two fixtures (one positive, one negative) + four goldens
+ a `test-lint` Make target wired into `test`/`test-fast`. A `kai info
lint` page documents it. `inf_ty_to_string` promoted to `pub` as the
shared type-display entry point.

Did not close #966 — this is phase 1 of an incremental rule catalog.

## Design decisions and alternatives considered

**Purity signal — type-based, not syntactic.** The decisive choice. A
purely syntactic "subtree contains no effect-op / handler" scan (the
shape `--effects-json` uses) structurally cannot close the negative
contract: a user fn that performs an effect transitively shows no literal
op at its call site, so a syntactic scan would false-positive on
`effectful_call()`. The lint instead reads the **callee's inferred row**
on each `ECall` — label-free row = pure. That makes a transitive-effect
call correctly read as effectful. A spike (`--dump-typed` over three
fixtures) confirmed the AST shape before any code was written.

**`row_is_pure` = label-free, tail ignored.** The asu consult warned that
a pure fn may carry `labels=[], tail=Some(n)` (a free row variable). My
first implementation demanded `tail=None` and reported nothing — every
pure call read as effectful. The compiler itself treats label-free rows
as pure regardless of the tail (see `inf_row_to_string_suffix`'s comment:
the tail is internal bookkeeping). Aligning `row_is_pure` to that — the
canonical purity signal — was the fix, not a patch.

**Bias: false-negatives over false-positives.** A linter that warns on
correct code gets disabled wholesale, taking its true positives with it
(Clippy/`go vet` doctrine). The predicate is "provably pure": any node
that cannot be proven pure (`ty = None`, an op with no inferred type) reads
as impure and stays silent.

**One extractor, two walks.** The first cut had two parallel ~20-arm
`ExprKind` matches (one for the effect scan, one for the finding walk).
That scored B+ on `km` (cognitive complexity D, one function at 15).
Collapsing them into a single `child_exprs(kind) : [Expr]` flattener that
both walks share dropped the file to A++ (97.4), cogcom avg 1.3 / max 4,
zero duplicate groups. The duplication was the design smell; the score
just surfaced it.

## Structural surprises the brief did not anticipate

**stage1 triple-string UTF-8 bug (#968).** The module `#[doc("""...""")]`
used an em-dash. `kaic1` (which compiles the self-host bundle) rejects
multibyte UTF-8 inside a triple-string while accepting it in a
single-line string; stage2 handles both. The failure surfaced as a
misleading `_main` link error, not a lex error, because `kaic1` emits a
truncated `build/stage2.c`. Isolated with a two-fixture repro, filed as
#968 (compiler/regression), worked around by keeping the module doc
ASCII (also the correct comment default). Out of lane — not fixed inline.

**Selfhost vs bundle privacy gap.** `make kaic2` (bundle concat) compiled
clean while `make selfhost` (real imports) failed twice: first on a
missing `import compiler.lint`, then on two `infer` helpers being `priv`.
The bundle masks both missing-import and privacy errors; only selfhost
catches them. Resolved by importing `compiler.lint`, promoting
`inf_ty_to_string` to `pub`, and re-implementing the trivial effect-op
name test locally rather than exporting an `infer` private.

**`ECall` line/col marks the parens.** The discard initially pointed at
the argument `(`, not the start of the call. A small `discard_pos` helper
prefers the callee's position so the warning points at the expression
start (rustc/Clippy convention).

## Fixtures added and coverage

- `examples/lint/discard_pure_value_positive.kai` — `{ a_pure_int();
  c_unit() }`; the Int discard is reported. Goldens: `.lint.expected`,
  `.lint-json.expected`.
- `examples/lint/discard_pure_value_negative.kai` — an effectful discard,
  an op-call discard, and a tail value; none reported. Empty text golden,
  `[]` JSON golden.
- `test-lint` Make target diffs both reporter modes against goldens and
  asserts exit 0 in every case; wired into `.PHONY`, `TEST_LIGHT_TARGETS`
  (so `make test` runs it), and `test-fast`.

Coverage gap left for later rules: the lint walks lambda bodies, match
arms, and nested blocks (the structural recursion reaches them), but no
fixture yet exercises a discard nested inside a lambda or match arm — the
positive fixture is a top-level block. A follow-up rule lane should add
one when it touches the walker.

## Cost vs estimate

The mechanical wiring (driver modes, wrapper, bundle, imports) was
routine. The real time went to two things the brief could not have
predicted: the stage1 UTF-8 bug (a misleading link error that needed
isolating before it could be filed) and the `row_is_pure` tail subtlety
(the lint silently reported nothing until the purity signal matched the
compiler's own). Both were caught by running the thing, not by reading
the code — the spike-first discipline paid for itself.

## Follow-ups for next lanes

- The rule catalog is open: idiom nudges (`(c) => c.name` vs `.name`),
  redundant-pattern checks (`if x { true } else { false }`), dead-code,
  and effect-row smells (a public signature over-declaring effects). Each
  is its own small rule lane, named, added to `lint_program`.
- Allow/deny: each lint already names itself; a `--deny <lint>` /
  inline-silence surface is unbuilt and belongs to a later phase.
- #968 (stage1 triple-string UTF-8) should be fixed so compiler sources
  can carry non-ASCII triple-string docs without breaking the self-host.
