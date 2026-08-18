# examples/fmt/width — the width corpus

Every fixture here is hand-wrapped the way the documentation teaches
(`kai info pipes`, `kai info idiomatic`, the stdlib sources) and is
built so that collapsing it onto one line overflows the formatter's
line budget. It is the yardstick for `kai fmt`'s width model: the
writer must bring every line it can inside the budget (`soft == 0`)
and must not join a line break the author wrote at one of its own
break points (`collapse == 0`).

Each fixture is a pair:

- `<name>.input.kai` — the hand-wrapped source a user writes.
- `<name>.expected.kai` — what `kai fmt` produces for it, checked by
  `tests/fmt_fixtures.sh` for bytes and idempotency.

Two fixtures are controls and must not move:

- `unbreakable_atoms` — lines the formatter *cannot* shorten (a long
  string literal, a long identifier). They stay over budget forever;
  the metric classifies them as `hard` and never counts them against
  the writer.
- `already_narrow` — everything fits with room to spare, so the
  writer must leave it alone. It is the net against a writer that
  starts breaking lines that never needed breaking.

The numbers over this corpus are measured by `tools/fmt-width-report.sh`,
gated by `tests/fmt_width.sh` against `tools/fmt-width-baseline.txt`,
and explained in `docs/fmt-width.md`.
