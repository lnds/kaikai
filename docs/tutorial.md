# kaikai — getting started

A 30-minute tour of kaikai. By the end you will have built the
compiler from C source, run a handful of programs, and seen the
five things that make kaikai different from Python or JavaScript:
**effects in types**, **protocols + derive**, **units of measure**,
**refinements**, and **fibers that actually suspend**.

This is a tutorial, not a reference. The full design lives under
`docs/`; the [final section](#11-where-to-next) points you there.

## Sections

1. [Install kaikai from C](#1-install-kaikai-from-c) — `cc stage0/*.c`,
   bootstrap chain, zero dependencies.
2. [Your first program](#2-your-first-program) — hello world, and how
   the same program looks once you make its effects visible.
3. [The `kai` driver](#3-the-kai-driver) — `kai build` / `kai run` /
   `kai test` and how to run every snippet in this tutorial.
4. [Day-to-day kaikai](#4-day-to-day-kaikai) — records, sum types,
   pattern matching, lists, pipes, `Option` / `Result`, the postfix
   `!`. The one-day ramp.
5. [Effects](#5-effects) — why effects appear in types, how to drop
   the prefix with `use`, and how to define and handle your own
   effect.
6. [Protocols and `#derive`](#6-protocols-and-derive) — `Show`,
   `Eq`, `Hash`, `Ord` for free on records and sum types.
7. [Units of measure](#7-units-of-measure) — `Real<USD>`,
   `Real<m / sec^2>`, dimensional safety at compile time.
8. [Refinements and contracts](#8-refinements-and-contracts) —
   `Int where >= 0`, `requires` and `ensures`.
9. [Fibers and structured concurrency](#9-fibers-and-structured-concurrency) —
   spawn, await, cooperative scheduling, real interleaving.
10. [Testing](#10-testing) — `test "..." { assert ... }` and
    `kai test`.
11. [Where to next](#11-where-to-next) — design docs, more examples,
    things that don't ship yet.

## 1. Install kaikai from C

(scaffold — full content in Phase 3)

## 2. Your first program

(scaffold — full content in Phase 3)

## 3. The `kai` driver

(scaffold — full content in Phase 3)

## 4. Day-to-day kaikai

(scaffold — full content in Phase 3)

## 5. Effects

(scaffold — full content in Phase 3)

## 6. Protocols and `#derive`

(scaffold — full content in Phase 3)

## 7. Units of measure

(scaffold — full content in Phase 3)

## 8. Refinements and contracts

(scaffold — full content in Phase 3)

## 9. Fibers and structured concurrency

(scaffold — full content in Phase 3)

## 10. Testing

(scaffold — full content in Phase 3)

## 11. Where to next

(scaffold — full content in Phase 3)
