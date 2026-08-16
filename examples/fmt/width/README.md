# examples/fmt/width — the width corpus

Every fixture here is hand-wrapped the way the documentation teaches
(`kai info pipes`, `kai info idiomatic`, the stdlib sources) and is
built so that collapsing it onto one line overflows the formatter's
line budget. That is the whole point: the corpus is the yardstick for
whether `kai fmt` has a notion of line width, and how good that notion
is.

Each fixture is a pair:

- `<name>.input.kai` — the hand-wrapped source a user writes.
- `<name>.expected.kai` — what `kai fmt` produces for it **today**.

The `.expected.kai` goldens are not aspirational. Until the writer
grows a width model they record the collapsed output, so the layout
change a width-aware writer makes shows up as a reviewable diff over
this directory instead of hiding inside a 3,000-line rewrite. A lane
that changes the layout regenerates them and the diff is the evidence.

Two fixtures are controls and must not move:

- `unbreakable_atoms` — lines the formatter *cannot* shorten (a long
  string literal, a long dotted path). They stay over budget forever;
  the metric classifies them as `hard` and never counts them against
  the writer.
- `already_narrow` — everything fits with room to spare, so a
  width-aware writer must leave it alone. It is the net against a
  writer that starts breaking lines that never needed breaking.

The numbers over this corpus are measured by `tools/fmt-width-report.sh`,
gated by `tests/fmt_width.sh` against `tools/fmt-width-baseline.txt`,
and explained in `docs/fmt-width.md`.
