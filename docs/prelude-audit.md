# Prelude audit — post-Core ergonomic tightening

Tracks prelude entries that may be redundant once the layered
features that subsume them are stable. **Not bug fixes** — these
are candidates for removal or deprecation when the surface area
of the prelude is audited as a single lane.

Originated as the `## Possible audit` section of
`docs/m12.8-followup.md`, relocated here 2026-05-02 so it stops
masquerading as m12.8 deferred work — the m12.8 lane is closed
end-to-end and these candidates outlive its scope.

## Candidates

- `string_concat(a, b)` — `a ++ b` (m7d §23) covers this.
- `string_concat_all(xs)` — `string_join("", xs)` covers this.
- `array_make` / `array_get` / `array_set` / `array_grow` — `a[i]`
  and `a[i] := v` (m7b §6) cover the access patterns; the
  constructor (`array_make`) might still be needed.
- `int_to_string` / `real_to_string` — if `Show.show(x)` is
  ergonomic enough post-Bug-4 (auto-Show in `#{x}`) and Gap-1
  (`impl[u: Unit] Show for Real<u>`), these become obsolete at
  user-call sites — still needed inside `impl Show` bodies.
- `string_to_int` / `string_to_real` — analogous, replaced by
  `Serialize.from_string(s)` in the Serialize protocol once
  return-type-driven dispatch lands.

## When to run this lane

After:
- The Serialize protocol with return-type-driven dispatch lands
  (replaces `string_to_*`).
- A measurable signal that user code has migrated to `++` / `a[i]`
  / `Show.show` over the legacy helpers.

The lane itself is mechanical (rename / remove + migrate
fixtures), but it changes the public prelude surface, so it
batches with other API breaks rather than landing in isolation.

## Not bug fixes

The prior `unitless` removal (m12.8 Bug 8, `f0acda2`) was a leak
of a typer-internal erasure intrinsic into the user-facing
prelude — actually broken, not just redundant. The candidates
listed here all *work*; removing them is style/hygiene, not
correctness.
