# llm

Bootstrap guide for an agentic AI pointed at a kaikai repo — what
kaikai is, where to read, and how to use the tooling built for you.

## Description

This page is for an LLM with weak prior exposure to kaikai. You will
extrapolate from Haskell, Python, JS, or Elixir and get kaikai wrong;
the tooling here exists to stop that. Read it once, then use
`kai info` as your standing source of truth.

## What kaikai is

kaikai is a functional language with static typing (Hindley–Milner
extended with effect rows), algebraic effects as a first-class
primitive, and Elixir-style pipelines. It compiles natively through
LLVM. Memory is managed by Perceus reference counting inside
BEAM-style isolated fibers — there is no garbage collector and no
borrow checker. Effects are visible in the type of every function
that performs them.

## kai info is your first source of truth

Before writing ANY `.kai` code, run `kai info syntax` — the one-page
cheat sheet of every form kaikai actually has. It carries a
NOT-IN-KAIKAI section listing the false friends (operator sections,
`\x -> body` lambdas, list comprehensions, `do { }` blocks, type
classes, `throw`/`catch`, `return` statements) that look plausible
but do not exist. Treat that section as ground truth.

Then read the per-topic page for the feature you are using. The
topics:

```text
syntax      the one-page cheat sheet (read this first)
idiomatic   how to write kaikai well, with the wrong reaches to avoid
effects     algebraic effects and handlers
fibers      structured concurrency, Spawn/await/cancel
actors      send/receive/self over Actor[Msg]
match       pattern matching, exhaustiveness
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

The rule, verbatim: never extrapolate from another language. Running
`kai info <topic>` is the cheap, always-correct check. For idiom
specifically — when your code compiles but reads wrong — read
`kai info idiomatic`.

## LLM-facing tooling

kaikai is built on a bet (the project's Tier-3 strategic principle,
**LLM authorability**) that typed holes, structured JSON, and stable
rules let an LLM author kaikai despite weak prior exposure. The
acceptance bar the project sets itself: an LLM with JSON access
completes the top 80% of typical functions within one round of
compilation. Three features make that reachable.

### Structured JSON of any topic

Every `kai info` page has a `--json` form with the same content as
structured data, for programmatic consumption:

```text
$ kai info pipes --json
{ "topic": "pipes", "title": "pipes", "tagline": "...",
  "sections": { "Description": "...", "Examples": "..." } }
```

Prefer the JSON form when you are parsing, not reading.

### Typed holes

Write `?` (or `?name`) where you do not yet know an expression, then
let the compiler tell you the type it must have and the bindings in
scope. `kai info holes` is authoritative; the workflow is: stub →
compile → read the report → fill.

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
  in scope:
    factor : Int
    xs : [Int]
    ...stdlib functions in scope...
```

`kai build --holes-json` emits the same per-hole report as a JSON
array (`expected_type`, `in_scope`, position) designed for you to
consume directly. Holes are the prototype of the LLM-authorability
bet — reach for them whenever you are unsure of a type.

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
1. kai info syntax + kai info idiomatic   (forms + idiom)
2. kai info <topic>                        (the feature you need)
3. write it, using ? holes where unsure
4. kai build / kai run  (read diagnostics; prefer the --json forms)
5. iterate
```

`kai info` for the forms, holes for the unknowns, and JSON
diagnostics for the errors are how you hit the one-round bar: the
top 80% of typical functions correct on the first compile.

## See also

`kai info idiomatic`, `kai info syntax`, `kai info holes`,
`kai info effects`, `kai info packages`
