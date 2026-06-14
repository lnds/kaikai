# Contributing to kaikai

Thank you for your interest in kaikai. This document explains how to
propose changes today — and, just as important, what to expect while the
language is still young.

## There is no formal contribution process yet

kaikai does **not** yet have a defined process for incorporating
improvements to the language or to the standard library. The core team is
still building both, and the bar for what lands — and how — is being worked
out as the language matures.

Until that process exists, **please do not open a pull request with a new
language feature or stdlib change as a first step.** It is very likely to
sit unreviewed or be declined, not because the idea is bad, but because
there is no agreed path for evaluating and integrating external proposals
yet.

## What to do instead: open an issue first

If you have an idea for the language or the standard library, **open an
issue in the official kaikai repository
([kaikailang-org/kaikai](https://github.com/kaikailang-org/kaikai)) first**
and describe your proposal. This lets you discuss its validity and fit with
the team that is building the language and the standard packages, before
anyone writes code. The kaikai organisation lives at
[github.com/kaikailang-org](https://github.com/kaikailang-org).

A good proposal issue states:

- **The problem** you are trying to solve, with a concrete example.
- **What you want kaikai to do** — the surface syntax or stdlib API you
  imagine, even if rough.
- **Why it belongs in the language / stdlib** rather than in a package
  (see *Build your own packages* below).

Run `kai info <topic>` (or `kai info` for the list) before proposing
surface syntax — it is the authoritative, always-current reference for the
forms kaikai actually has, including an explicit *NOT IN KAIKAI* section of
plausible-looking false friends. Proposing against the real surface saves
everyone a round trip.

## The language is unstable until Orongo

kaikai is **pre-1.0** and iterates fast. Each **edition** pins a stable
user-facing surface *within that edition* (see `docs/editions.md`), and the
current edition is **Hanga Roa**. But the project as a whole is still
maturing: internals, codegen, runtime, and the stdlib shape change rapidly
between releases, and editions still advance.

Until the **Orongo** edition lands — the next milestone in the language's
maturation — treat the language and the standard library as **unstable** for
the purpose of building on top of them. Pin your edition in `kai.toml`, and
expect that what you build against today may need migration as kaikai moves
toward a stable 1.0.

## Build your own packages

You do **not** have to wait for the core team to add something to the
language or the standard library. kaikai supports packages: you can build
and publish your own and depend on them from `kai.toml`, without any change
to kaikai itself.

If your idea is a library — a data structure, a domain helper, an
integration — it almost certainly belongs in a package you own, not in the
standard library. This is the fastest path to using your idea today, and it
keeps the standard library small and focused. Reserve stdlib / language
proposals for things that genuinely need to live in the core.

## Licensing of contributions

Unless you explicitly state otherwise, any contribution intentionally
submitted for inclusion in the work by you, as defined in the Apache-2.0
license, shall be dual licensed as in the [README](README.md#license),
without any additional terms or conditions.
