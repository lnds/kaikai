# mutate

Break the code on purpose and ask whether anything notices.

## Description

A green test run proves the tests passed. It does not prove the tests
would have failed had the code been wrong — and that is the property
anyone actually wants from a suite. `kai mutate` measures it directly:
it rewrites one construct at a time, runs the suite against each
broken version, and reports the ones nobody caught.

A mutant the suite rejects is **killed**: something noticed. A mutant
that survives is a **hole**, reported with its file, line, operator and
diff. Survivors are the output; there is no score, because a percentage
is a number nobody acts on while a survivor is a fixture waiting to be
written.

```text
kai mutate                                  # the package's own modules, `kai test`
kai mutate --limit 20 --operator arm        # a first look, cheapest operator first
kai mutate --module src/parse.kai           # one module
kai mutate --oracle './run-tests.sh'        # any command that exits 0 when healthy
kai mutate --list                           # the sites, without running anything
```

## The oracle is a parameter

`kai mutate` never assumes the suite is `test` blocks. It takes a
command and judges it purely by exit code: **0 means healthy**, anything
else means the mutant was caught.

The default is `kai test`, which is what an ordinary package wants. A
codebase whose real suite is a fixture corpus, a golden comparison or a
shell script points `--oracle` at that instead:

```text
kai mutate --module src/rc.kai --oracle 'make && ./run-corpus.sh'
```

This is not an escape hatch bolted on afterwards — it is what lets the
tool work on a codebase with zero `test` blocks, which is a common and
legitimate shape. kaikai's own compiler is one: it has no `test` blocks
at all, and its suite is `examples/` compiling and matching goldens.

## Mutations come from the AST

A text-level mutation is noise: rewriting a string literal changes a
message, rewriting a comment changes nothing, and neither says anything
about the suite. Every operator here works on the parsed program, so it
only ever breaks a real construct.

The mutant itself is the original source with **one token span
replaced** — not a re-printed AST. A mutant's diff against its parent
is the mutation and nothing else, which is what makes a survivor
readable at a glance.

A mutant that does not parse is never handed to the oracle. That would
spend a full build to learn nothing, so the rewritten source is
reparsed first and the site is reported as unresolvable instead.

## Operators

| operator | example | what its survival means |
|---|---|---|
| `arm` | drop a `match` arm | a case stopped being handled and nothing noticed |
| `compare` | `>=` → `>` | the boundary of a gate is untested |
| `connect` | `and` → `or` | a gate that never fires, or always does |
| `negate` | `if c` → `if not c` | an inverted guard changes no observed output |
| `literal` | `0` → `1` | a seed or base case is unexercised |
| `call` | `f(x)` → `x` | a step could do nothing and no test would care |

`arm` comes first because it is the highest-value operator on most
codebases and by far the highest on a compiler, which is mostly `match`.
A dropped arm is the shape of an encoder that emits a tag its decoder
never learned to read: silent, because a miss is indistinguishable from
a cold start, and invisible to any green run.

Two arms are never dropped: the last one of a `match`, which would make
it non-exhaustive and turn the mutant into a compile error that says
nothing about the suite.

## Reading the output

```text
killed    src/parse.kai:88   arm
SURVIVED  src/parse.kai:141  compare

survivors — the suite did not notice these:

src/parse.kai   141  22  compare
    141c141
    <   if depth >= limit { fail() }
    ---
    >   if depth > limit { fail() }

37 mutants in 41s: 31 killed, 5 did not compile, 1 survived
```

Compile-failures are counted separately. They are kills — the mutant
was caught — but the least interesting kind, since a type error catches
them rather than a test. Folding them into the total would inflate the
figure with the cases that took the least effort to detect.

## Cost

Every mutant re-runs the oracle, so the tool costs one suite run per
mutant. That is cheap on an ordinary package and expensive on one whose
oracle rebuilds a compiler.

Three things keep it usable:

- **A failing oracle stops immediately.** A killed mutant costs the
  first fixture that notices, not the whole corpus — so write the
  oracle to check its most sensitive cases first.
- **`--limit` and `--module` are the first tools, not the last resort.**
  Survivors turn up long before a full sweep finishes; there is no
  reason to wait for one to start acting on them.
- **A compile-failure costs no oracle run at all.** It is detected by
  the build the oracle starts with.

## Equivalent mutants

Some mutants mean exactly what the original meant. They can never be
killed, and counting them as holes makes the number lie.

`tools/mutate-known-equivalent.txt` suppresses them, keyed
`<file>:<line>:<operator>` with a reason after `#`:

```text
src/rc.kai:412:literal   # the seed is overwritten before any read; 0 and 1 are indistinguishable
```

**An entry without a reason is an error, not a suppression.** A
suppression nobody justified is indistinguishable from a hole somebody
hid, so the run fails rather than silently skipping it.

## Not a gate

`kai mutate` is a diagnostic, run at discretion. It is not wired into
any test tier and should not be: the cost does not fit a per-change
gate, and a mutation score is not a pass/fail signal. If it is ever
gated, the thing to ratchet is the **survivor count for one module** —
never a global percentage.

## See also

`kai info testing` for `test` / `check` blocks, `kai info lint` for
suspect-but-valid code, `kai mutate --help` for the flags.
