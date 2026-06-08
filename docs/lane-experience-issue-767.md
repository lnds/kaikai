# Lane experience — issue #767: civil date/calendar v1

Lane: `civil-date` worktree. Tracking issue: #767. One new stdlib
module (`stdlib/date.kai`), three fixtures, catalog updates — plus,
at the integrator's request, the fix for #769 (m14 qualified-surface
doc drift found while building this lane; see §Structural surprises).

## Scope: planned vs shipped

The date surface as planned (issue #767 sketch) and as shipped match
1:1 — nothing dropped, nothing added. `Date` carrier, validating `make`,
Hinnant epoch conversions, calendar queries (`is_leap_year`,
`days_in_month`, `day_of_week`, `day_of_year`), arithmetic
(`add_days`, `diff_days`, `add_months` with end-of-month clamping),
`eq`/`cmp`/`lt`, strict ISO-8601 `to_string`/`parse`, and the
time.kai bridge (`from_walltime` UTC, `today() / Clock`). All v1
exclusions (tzdata/DST, time-of-day, locales, ISO week dates,
non-Gregorian) held.

## Design decisions

- **File placement: `stdlib/date.kai`, top-level.** The brief offered
  `stdlib/time/date.kai` as an alternative. `docs/stdlib-layout.md`
  has no `time/` subdirectory — `time.kai` itself is a top-level
  module, and the pure-data precedents (`decimal.kai`, `money.kai`,
  `fx.kai`) are all top-level. Subdirectories in the layout group
  effect families (`fs/`, `net/`, `os/`) or large pure families
  (`collections/`, `math/`); a single ~100-LOC pure module does not
  open a directory. `date` importing `time` mirrors `money`
  importing `decimal`.
- **Bare definitions + qualified surface.** Definitions use bare
  canonical names (`make`, `parse`, `to_string`, `eq`, `cmp`, `lt`)
  exactly like today's `decimal.kai`/`money.kai` source, reached as
  `date.make`, `date.parse`, … via the qualified-call resolver.
  Empirically verified before writing the module: a probe importing
  `decimal` + `money` + `date` resolves three coexisting
  `parse(String)` definitions correctly by module qualifier.
  Unambiguous names (`from_epoch_days`, `add_months`,
  `day_of_week`, `today`, …) are also callable bare.
- **Epoch algorithm: Howard Hinnant's civil algorithms**
  (`days_from_civil` / `civil_from_days`,
  howardhinnant.github.io/date_algorithms.html). Era-based (400-year
  Gregorian cycles of 146097 days), exact integer math over the full
  proleptic range, no lookup tables. kaikai's `/` truncates toward
  zero, so the module carries private `date_floor_div`/`date_floor_mod`
  for the two places flooring matters (era computation, month
  arithmetic, WallTime seconds); the algorithm's interior offsets
  (`yoe`, `doy`, `doe`, `mp`) are non-negative by construction and
  use plain `/` as in the reference.
- **`day_of_week` returns ISO numbering (1 = Monday … 7 = Sunday)**,
  not C's 0 = Sunday. ISO-8601 already governs the string surface;
  picking the same standard for weekday numbering keeps the module
  internally consistent. Derived as `floor_mod(epoch_days + 3, 7) + 1`
  (day 0 was a Thursday).
- **End-of-month clamping rule** (`add_months`): the day clamps to
  the last valid day of the target month (2026-01-31 + 1 = 2026-02-28;
  2024-01-31 + 1 = 2024-02-29). Documented in the module header and
  the issue as deliberately non-reversible —
  `add_months(add_months(d, 1), -1)` may not return `d`. Alternatives
  considered: overflow into the next month (Jan 31 + 1m = Mar 3,
  PostgreSQL-rejected ugliness) or returning `Option[Date]` (punishes
  the 99% case for the month-end 1%). Clamping is what business
  calendars (billing cycles, end-of-month contracts) actually want.
- **Strict parse window**: `parse` accepts exactly `YYYY-MM-DD`
  (10 chars, years 0000–9999, no signs). `make`/`from_epoch_days`
  cover the full proleptic range, so nothing is unreachable — only
  unparseable. `to_string` is total: negative years emit `-` plus the
  4-digit-padded magnitude; years > 9999 print all digits. The
  asymmetry (to_string emits shapes parse rejects) is the standard
  ISO-8601 expanded-year compromise and is documented in the header.

## Structural surprises

- **Doc → reality drift around the m14 qualified surface.** The m14
  table in `docs/stdlib-layout.md` and the `decimal.kai`/`money.kai`
  header comments stated that definitions "stay flat-prefix" and that
  qualified calls ride a `module_legacy_prefix` fallback in
  `stage2/compiler.kai`. None of that matched the code: the
  2026-05-17 canonical-only migration (commit series ending in
  fe02f1e + 4ba4e01) renamed definitions to bare canonical names and
  deleted the fallback; `me_lookup_export`
  (`stage2/compiler/modules.kai:337`) does a verbatim lookup, and
  per-module C symbols (`kai_<mod>__<name>`, #748) let same-named
  exports coexist. The lane followed the *code*, not the comments —
  then filed #769 and, on the integrator's request, fixed it in this
  same lane: a systematic sweep found the same stale template in
  **16 module headers** (not just decimal/money — also hex, base64,
  toml, uuid, regexp, path, set, queue, stack, array, int, real,
  http, random_secure, fs/file, plus protocols.kai's already-shipped
  `bin.kai` split still described as "follow-up"). All 16 rewritten;
  the m14 table in `docs/stdlib-layout.md` replaced by a
  §Qualified surface section describing the current mechanism; the
  historical #614 row in `docs/stdlib-roadmap.md` got a
  **Superseded** note.
- **Parser: `++` cannot open a continuation line.** A multi-line
  concatenation must leave `++` at the end of the previous line;
  leading-`++` continuation is a parse error. Cost one fixture
  round-trip; the module itself was written trailing-style from the
  start.
- **No surprises in the algorithm itself** — the Hinnant routines
  passed the full anchor table (Python-`datetime`-generated) on the
  first run, including negative epoch days and the year-0/era
  boundaries.

## Fixtures

Three positive fixtures under `examples/stdlib/` (auto-discovered by
the stage2 stdlib smoke loop), 115 golden lines total:

- `date_basic.kai` — validation accept/reject, leap rule
  (1900 no / 2000 yes / 2024 yes / year 0 yes), `days_in_month`,
  `day_of_week` against seven known anchors, `day_of_year` (leap and
  non-leap March 1), `add_days` across leap-day and year boundaries,
  `diff_days` signs, the 8-case clamping matrix for `add_months`,
  and per-leg `cmp` orderings.
- `date_iso.kai` — `to_string` padding (years 0, 1, 987, −1, 10000),
  5 accepted parse shapes, 14 rejection shapes (invalid calendar
  dates, out-of-range fields, wrong length/separators/signs/letters),
  parse∘to_string round-trips.
- `date_epoch.kai` — 10 epoch-day anchors in both directions
  (including −719162 = 0001-01-01 and 2932896 = 9999-12-31), the
  0000-03-01 era anchor ±1, a ±800 000-day round-trip sweep
  (step 1237, ~±2190 years), `from_walltime` floor semantics
  (secs −1 → 1969-12-31, 86399 vs 86400), nanos-ignored, and a
  `today()` sanity window (tolerance style, per
  `time_clock_default.kai` precedent — exact value varies by run).

Expected values were generated with Python's `datetime` (proleptic
Gregorian) *before* running the fixtures, so the goldens are
independent of the implementation under test.

Coverage gaps, acknowledged: no `.err.expected` negative fixture (the
module is pure kaikai — there is no new diagnostic surface to pin);
no property-based randomized testing (the sweep is deterministic);
`today()` is only window-tested (inherently non-golden).

## Quality gates

| File | km score | LOC (code) | cogcom avg / max |
|---|---|---|---|
| `stdlib/date.kai` | A (92.2) | 100 | 1.7 / 4 |
| `examples/stdlib/date_basic.kai` | A (90.3) | 78 | — |
| `examples/stdlib/date_iso.kai` | A++ (98.3) | 58 | — |
| `examples/stdlib/date_epoch.kai` | A+ (95.5) | 56 | — |

No new `km dups` groups. `date_basic` started at B+ (85.2) — inline
`match … { Some(_) -> true … }` validation checks dragged Halstead
and cognitive complexity; hoisting a 4-line `valid(y, m, d) : Bool`
helper lifted it to A without touching the golden.

## Cost vs estimate

Single short session, one compile-error round-trip (the leading-`++`
continuation), zero algorithm rework. The brief's implicit estimate
(one lane, one module + fixtures + docs) held with margin. The
up-front empirical probes (bare-name coexistence, transitive
`WallTime` visibility through `import date`) cost ~10 minutes and
prevented the only structural risk from landing in the module.

## Follow-ups left for next lanes

- `impl Ord`/`Show` protocols for `Date` (today comparison is plain
  fns, consistent with decimal/money; protocol impls would let
  `Date` ride generic sort/format paths).
- ISO week dates, civil time-of-day type, tzdata/DST — explicitly
  out of scope per #767.
