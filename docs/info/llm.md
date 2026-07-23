# llm

Bootstrap guide for an agentic AI pointed at a kaikai repo — what
kaikai is, where to read, and how to use the tooling built for you.

## The cheap path (read this first)

Reference reads cost you tokens; buy the smallest answer that works.
In order of preference:

1. **One API? `kai doc <module>.<symbol>`** — signature + doc of a
   single symbol (`kai doc string.split`). `kai doc <module>` lists a
   module; `kai doc` lists the modules. This is the default move for
   "what does X take / return / do".
2. **Before your first line of kaikai: `kai info deltas`** — the
   ~60-line card of where kaikai deliberately breaks Rust/Go/Python
   habits (`#` comments, `:=` assignment, `++` is concat, no
   `return`, …). One read prevents the classic prior collisions.
3. **A form you have not written before? `kai info syntax`** — the
   one-page sheet of every form kaikai has, with a NOT-IN-KAIKAI
   section of false friends. Treat it as ground truth.
4. **Full topic pages (`kai info effects`, `kai info match`, …) are
   the fallback, not the default.** Read one only when you are stuck
   or working deep in that feature — each page is a big read, and
   most questions are answered by 1–3 above.

Never extrapolate from another language; check first.

## What kaikai is

kaikai is a functional language with static typing (Hindley–Milner
extended with effect rows), algebraic effects as a first-class
primitive, and Elixir-style pipelines. It compiles natively through
LLVM. Memory is managed by Perceus reference counting inside
BEAM-style isolated fibers — there is no garbage collector and no
borrow checker. Effects are visible in the type of every function
that performs them.

## The topic list

```text
syntax      the one-page cheat sheet of forms
deltas      prior-collision card — kaikai vs. your habits
idiomatic   how to write kaikai well, with the wrong reaches to avoid
effects     algebraic effects and handlers
fibers      structured concurrency, Spawn/await/cancel
actors      send/receive/self over Actor[Msg]
match       pattern matching, exhaustiveness
contracts   requires / ensures + refinement-type predicates
pipes       the four pipe operators and their intent
units       units of measure on Real
protocols   single-dispatch protocols
holes       typed holes for incremental development
loop        while / until
packages    the path-is-the-package model
ffi         crossing to C via the Ffi capability
testing     builtin test "..." { } + assert
lsp         editor integration surface
builtins    doc tree of the auto-loaded core modules
```

Topic pages open with a TL;DR — if the first ten lines do not answer
you, `kai info <topic> --json --section <name>` fetches one section
instead of the whole page.

## LLM-facing tooling

kaikai is built on a bet (the project's Tier-3 strategic principle,
**LLM authorability**) that typed holes, structured JSON, and stable
rules let an LLM author kaikai despite weak prior exposure. The
acceptance bar the project sets itself: an LLM with JSON access
completes the top 80% of typical functions within one round of
compilation. Three features make that reachable.

### Structured JSON of any topic

Every `kai info` page has a `--json` form, and `--section <name>`
narrows it to the sections you name (repeatable; case-insensitive):

```text
$ kai info pipes --json --section Description
{ "topic": "pipes", "title": "pipes", "tagline": "...",
  "sections": { "Description": "..." } }
```

Prefer the JSON form when you are parsing, not reading — and prefer
`--section` over paying for the whole object.

### Typed holes

Write `?` (or `?name`) where you do not yet know an expression, then
let the compiler tell you the type it must have and the local
bindings in scope. `kai info holes` is authoritative; the workflow
is: stub → compile → read the report → fill.

```kaikai
fn scale(xs: [Int], factor: Int) : [Int] = {
  let result = ?
  result
}

fn main() : Int = 0
```

```text
$ kai build --holes file.kai
file.kai:2:16: type hole ?
  expected: [Int]
  in scope (local):
    factor : Int
    xs : [Int]
```

`kai build --holes-json` emits the same per-hole report as a JSON
array (`expected_type`, `in_scope`, `candidates`, position) designed
for you to consume directly. Both forms report the LOCAL scope —
parameters and lets around the hole. When you genuinely need every
reachable binding (stdlib included), `--holes-scope` /
`--holes-json-scope` dump the full picture; it is large, so reach
for `kai doc` first.

### Typecheck without building

`kai typecheck file.kai` answers "does this compile?" from the
front-end alone — no codegen, no link — several times faster than a
full build. Exit 0 means well-typed; on error the diagnostics are
identical to a build's. The JSON report flags ride it
(`kai typecheck file.kai --diags-json`), so the compile-fix loop is:
typecheck, read the JSON, patch, repeat; build once it comes back
clean. Errors that only surface in later phases (monomorphisation,
backend subset gaps) can still fail that final build — typecheck-clean
is necessary, not sufficient.

### Structured diagnostics

Compiler diagnostics come as stable JSON alongside the human text.
When you build or run and hit an error, prefer the JSON surfaces
(`--json` family, `--holes-json`, `--effect-holes-json`) over parsing
the formatted output.

## Where to read

The confirmed external references:

```text
source repo   https://github.com/kaikailang-org/kaikai
the book      https://github.com/kaikailang-org/kaikai-book
              (The kaikai Programming Language)
project site  https://kaikai-lang.org
```

In-repo, `docs/design.md` is the design overview and the pinned
semantics docs (`docs/effects.md`, `docs/effects-stdlib.md`,
`docs/protocols.md`, `docs/structured-concurrency.md`,
`docs/actors.md`) carry the deep semantics. But for *current surface
syntax*, `kai info` wins over any doc prose — docs can drift; the
`kai info` pages are kept current as a release gate.

## The authoring loop

The concrete recipe for writing a kaikai function as an agent:

```text
1. kai info deltas                (once per session — the collisions)
2. kai doc <module>.<symbol>      (each API you are about to call)
3. write it, using ? holes where unsure; check a form against
   kai info syntax if you have not written it before
4. kai build / kai run            (read diagnostics; prefer --json)
5. stuck on a feature? only then read its topic page
```

`kai doc` for the APIs, holes for the unknowns, and JSON diagnostics
for the errors are how you hit the one-round bar: the top 80% of
typical functions correct on the first compile.

## See also

`kai info deltas`, `kai info syntax`, `kai info idiomatic`,
`kai info holes`, `kai info packages`
