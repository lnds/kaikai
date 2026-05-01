# Lane experience — kai-fmt (Tongariki kai fmt v1)

Date: 2026-05-01. Branch: `kai-fmt`. Single squashed commit
(`bea7fb0`) plus this report.

**Caveat — instrumentation gap**: the lane brief I received did
not include the `docs/lane-instrumentation.md` snippet, so there
is no `/tmp/lane-kai-fmt-builds.tsv` to attach. The integrator's
post-merge note (2026-05-01) flagged that lane discipline has
backslid after 2026-04-28; this report is the retroactive,
honest reconstruction. Numbers below come from my visible
context and git log timestamps, not from a build log.

## Objective metrics (reconstructed)

- **Start of work**: ~2026-05-01 12:20 (parent commit `cb026b4`
  at 12:08; first edit ~12 min later after reading
  `bin/kai`, `stage2/compiler.kai` head, AST type defs around
  line 1035, lexer around line 120, and `parse_cli` around
  line 31130).
- **Final commit `bea7fb0`**: 2026-05-01 13:22.
- **Subjective wall-clock**: ~60 min of continuous work in auto
  mode, no idle stretches. Build cycle is fast (~5 s for
  `make kaic2`); most of the wall-clock went to reading the
  32 k-line `compiler.kai` to find existing helpers and to
  diagnosing two non-obvious bugs (see *Compiler errors*).
- **Build/test invocations** (from context — not from a TSV;
  approximate):
  - `make kaic2`:    ~12 invocations, 12 passes, 0 fails.
  - `make selfhost`: 4 invocations, 3 passes, 1 fail (the false
    OK from broken `set -e` chain on the first attempt — see
    *Friction points*).
  - `make tier0`:    1 invocation, 1 pass.
  - `make tier1`:    4 invocations, 4 passes.
  - `tests/fmt_fixtures.sh` (direct): ~6 invocations during
    fixture iteration, all converged green.
  - Direct `stage2/kaic2 --fmt …` smoke runs: ~30+ during
    iteration; not counted individually.

## Compiler errors I encountered

### 1. `panic: non-exhaustive match` (runtime, twice)

**At runtime**, far from the call site. The first cause was a
type mismatch the typer did NOT catch: `collect_comments_loop`'s
`[] -> ...` arm returned a `CmtScan` record where the function's
declared return was `[Cmt]`. Stage 1 emits the C call passthrough
without checking, so the broken value reached a downstream `match
xs.cmts { [] -> ...; [Cmt(...), ...] -> ... }` and matched
neither branch. Diagnosing required adding `print("DBG …")` calls
through the call chain — there was no source location in the
panic message.

**Fix**: extract `tail.acc` and `list_reverse` it into a `[Cmt]`
explicitly. ~5 build cycles to localise.

### 2. `panic: non-exhaustive match` again — `args` parameter shadow bug

After fixing (1), the SAME generic panic surfaced from a
completely different call path: `fmt_call(callee, args, s)`. The
parameter name `args` collides with the prelude's CLI builtin
`args() : [String]`. Stage 1's resolver/codegen emits
`kai_closure(&_kai_prelude_args_thunk, …)` at every use site of
the local `args` instead of the parameter, so the function body
receives garbage in its second argument and the inner match on
the (corrupt) value fails non-exhaustively.

**Diagnosis path**: dumped `stage2/build/stage2.c`, grep'd for
`kai_fmt_call`, saw `kai_closure(&_kai_prelude_args_thunk, 0, 0,
NULL)` at the call site instead of the expected `kai_args`. The
**C output was the load-bearing signal** — the panic message
itself was useless. ~3 more build cycles after the C grep.

**Fix**: rename `args` → `call_args`, `ty_args`. Saved as
[`feedback_kaikai_param_args_shadow.md`](memory) so future agents
don't repeat this. Same class as the 2026-04-27 fix, but for a
different prelude name.

### 3. `error: expected expression` on `/* TODO real */`

Self-inflicted. I ran `bin/kai fmt stage2/compiler.kai` once to
"verify self-consistency" — it rewrote the file in place
(documented gofmt-style behaviour). The new layout exposed a
parse-time issue with pre-existing `/* TODO real */` C-style
block comments at line 2245 and elsewhere; kaikai's parser
treats `/` and `*` as separate tokens, so those lines never
parsed cleanly even before. The non-fatal recovery had been
masking it. After the rewrite, selfhost's output `kaic2b.c` was
empty (kaic2 exited 1) but `set -e` in the selfhost recipe is
fragile (the `> file` redirect happens before the exit-status
check, so the OK message can fire on a 0-byte diff).

**Fix**: `git checkout stage2/compiler.kai` and re-applied my
intended additions programmatically (Mode + parse_cli + usage +
compile_source MFmt branch + the fmt section as a saved
slice). Saved as
[`feedback_kai_fmt_destructive_inplace.md`](memory) so the next
agent does not run `kai fmt` on tracked source without redirect.
~10 min lost.

### 4. Stack overflow segfault (139) on stage 2 selfhost at -O0

After the fmt code landed, `make selfhost` segfaulted in
`tokenize` on the new `stage2/compiler.kai`. Token count went
from 240 k (baseline) to 248 k (mine), and macOS caps the
main-thread stack at the **link-time** size, not the runtime
`ulimit -s` (which is what the project's `STACK :=` shell prefix
sets). 8 MB default ÷ ~32 byte/frame is roughly the threshold;
my +3.4 % of source was enough to tip it.

**Diagnosis path**: bisected `--tokens`, `--ast`, `--check`,
`--infer` to localise the segfault to tokenize specifically.
Confirmed it was depth-related by recompiling the same
`build/stage2.c` with `-O1` (clang's `-foptimize-sibling-calls`
TCOs `lex_loop`) — that worked, so the bug was C-level, not
kaikai-level.

**Fix**: `-Wl,-stack_size,0x8000000` (128 MB) on Darwin, gated
by `uname -s` so Linux CI is unaffected (it gets enough from the
runtime ulimit). Also dropped the original ~150-line
`fmt_unsupported_*` walker pre-pass and replaced it with inline
`panic(…)` calls; reduced the binary's recursion-frame budget
slightly and felt cleaner anyway.

## Friction points

### Generic `panic: non-exhaustive match` is hostile to debug

Both runtime panics I hit were the same message with no source
location. I ended up sprinkling `print("DBG …")` calls through
half of `fmt_program` to localise. A panic that printed `at
match in <fn_name> (compiler.kai:<line>)` would have shaved
~10 build cycles off this lane. Concretely:

- The C codegen already has the source position of the match
  (it emits the panic literally adjacent to the source line).
  Just stitching `<fn_name>:<line>` into the panic string
  would suffice; no kaikai-level change needed.

### `set -e` + redirect in `make selfhost` masked broken builds

When I ran `bin/kai fmt stage2/compiler.kai` and broke the
source, the next `make selfhost` reported "self-hosting fixed
point: OK" because both intermediate `kaic2b.c` and `kaic2c.c`
were 0-byte (kaic2 exited 1, the redirect created empty files,
then `diff -q` on two empty files succeeded). Took me a beat to
realize the OK was a false positive. Fix path is to chain the
redirects with `pipefail` or check `wc -c` post-write.

### Reading 32 k-line `stage2/compiler.kai` for ambient knowledge

Most of the wall-clock went to "where is X helper". Concrete
examples:

- `string_concat_all`: had to grep to find it's a prelude
  builtin (line 7686).
- `panic`: same — prelude builtin (line 7679).
- `list_length`: grep'd, found existing definition at the prelude
  table; deleted the duplicate I'd written.
- `char_to_string`: did not exist; copied the
  `show_char_ascii_table()` technique from
  `stdlib/protocols.kai:52`.

A `kaic2 --list-prelude` or similar surface would have saved
~10 min. Not blocking.

## Spec ambiguities or interpretive choices

The brief was specific. Three places I had to decide:

1. **Single-param lambda parens**. Source `(x) => x + n` parses
   the same as `x => x + n`. Brief did not pin which canonical
   form to emit. Chose `x => …` (no parens) — matches the
   trailing-lambda style elsewhere in the corpus. Idempotent
   either way.

2. **Range emission**. `[1..100]` and bare `1..100` both parse
   as `ERange`, but bare `..` is rejected outside select
   contexts (e.g. `(1..100)` doesn't parse). Chose to always
   wrap with `[ ]` so output round-trips cleanly anywhere. This
   was discovered when the first idempotency run on
   `examples/minimal/fizzbuzz.kai` failed re-parse.

3. **Best-effort comments**. Brief explicitly authorised
   best-effort if AST doesn't preserve comments. The lexer's
   `lex_skip_comment` discards them, so I built a
   parallel comment list keyed by `(line, col)` and drain it
   at line-boundary breakpoints. Trailing same-line comments
   may be promoted to the line above — recorded as
   `docs/known-regressions.md` R7 with the fix path
   (TkComment-as-token, gofmt-style).

## Did the compiler help or hinder me? Was structured JSON useful?

**Specifically asked: did `--effects-json` or `--effect-holes-json`
help?** No — fmt branches out of `compile_source` BEFORE typing
and effect inference run, by design. I never engaged the typer
for any fmt-relevant code path, so the JSON surfaces had no
input to produce.

**What signal actually carried me through:**

| Signal                                     | When it helped                                                    |
| ------------------------------------------ | ----------------------------------------------------------------- |
| `kaic2 --tokens <file>`                    | Verified lexer treatment of `/*`, `#derive`, `[1..100]` etc.       |
| `kaic2 --ast <file>`                       | Confirmed `[1..100]` parses as bare `ERange`, not `EList[ERange]`. |
| `cat stage2/build/stage2.c \| grep <fn>`   | **Load-bearing** for the `args` shadow bug — the C output was the only source of truth; the runtime panic was useless. |
| `diff -u expected actual`                  | Iterating fixtures.                                               |
| `make tier1` exit code                     | End-of-lane gate.                                                 |
| `panic: non-exhaustive match` text         | Useless. Did not localise the bug at all.                         |
| `--effects-json` / `--effect-holes-json`   | Not exercised. Lane never reached the typer.                      |
| `--holes-json`                             | Not exercised.                                                    |

**Concrete asks for the LLM-friendly bet** (Tier 3):

- Annotate the `kai_prelude_panic("non-exhaustive match")` call
  with the source `<fn>:<line>:<col>` of the originating match.
  Single-line change in stage 1's emitter; outsized payoff.
- Optionally a `kaic2 --shadow-check` warning surface for
  parameters / let-bindings that shadow prelude builtins
  (`args`, `exit`, `read_line`, …). Stage 1 has the resolver
  bug that masks the shadow; until it's fixed in stage 1, a
  static warning at parse-time would have saved both 2026-04-27
  and 2026-05-01 instances.
- The `--ast` dump is human-only. A `--ast-json` would have
  been nicer for the fixture-design step, where I wrote
  `examples/fmt/*.input.kai` by hand and verified the AST was
  what I expected. Not critical.

## Subjective summary

- **Confidence in correctness**: medium-high. Tier 1 green, 21
  fmt fixtures green, idempotency holds on 5 minimal + 7
  quickstart/phase4 examples. Round-trip parse-after-format
  verified. The trailing-comment limitation is the main known
  gap and is honestly documented.
- **Hardest sub-task**: diagnosing the `args` shadow bug.
  Generic panic + far-from-source manifestation + need to grep
  C output was the longest single debugging episode.
- **Easiest sub-task**: writing the actual pretty-printer arms.
  Mechanical once the AST shape was known.
- **Did the compiler help or hinder?** Mixed.
  - **Helped**: `--tokens`, `--ast`, the C output is grep-able.
  - **Hindered**: generic non-exhaustive-match panic; stage 1
    silently miscompiling parameter shadows.
  - **Neutral / unused**: every JSON dump (`--effects-json`,
    `--effect-holes-json`, `--holes-json`) — fmt runs before
    typing. The Tier 3 bet's instruments did not move the
    needle on this specific lane because the lane never engaged
    them.

## Limitations of this report

- Self-report bias acknowledged.
- **No build log TSV**: instrumentation snippet was missing
  from the brief. Counts above are reconstructed from git log
  + visible context, not from a `/tmp/lane-kai-fmt-builds.tsv`.
- Context truncation: error counts and reasoning excerpts only
  reflect what is currently in my context window. Earlier
  iterations of `fmt_unsupported_check` walker (since deleted)
  and the destructive-fmt incident's details may be
  understated.
- Single agent (Claude). Not generalisable across LLMs.
