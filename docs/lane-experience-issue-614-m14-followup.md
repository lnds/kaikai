# Lane experience — issue #614 (m14 follow-up: qualified-call migration for 21 stdlib modules)

Date: 2026-05-16. Branch: `issue-614-m14-followup`. Closes #614.

The lane was scoped as a mechanical "Option E" migration: for each
of the 21 stdlib modules outside `stdlib/core/*`, duplicate every
`pub fn flat_name(...)` to a bare canonical (`pub fn op(...)`) and
keep the flat name as a deprecation alias. Roughly 176 pub fns
across 21 modules, target ~350 added lines, no breaking change.

What actually shipped: the canonical qualified surface
(`<module>.<op>`) is live for every one of the 21 modules, but
**the definitions stay flat-prefix in every module where renaming
would collide with sibling modules or shadow user-defined locals**.
The qualified-call resolver's prefix fallback (`me_lookup_export`
in `stage2/compiler.kai`) routes `int.min(a, b)` → `int_min`,
`decimal.zero()` → `dec_zero`, `queue.push(q, x)` → `q_push`, etc.,
without renames. Where the bare canonical was provably safe (no
shadow, no top-level clash), the lane added both forms.

## Scope as planned vs as shipped

| Aspect | Planned (brief) | Shipped |
|---|---|---|
| Modules migrated | 21 | 21 (canonical surface live for all) |
| Pub fns renamed to bare canonical + alias | ~176 | ~25 (the safe subset) |
| Pub fns kept flat-prefix only | 0 | ~151 (the rest) |
| Resolver changes | none expected | 3 added: `module_legacy_prefix` gains `queue`/`stack`/`random_secure`; new `module_legacy_prefix_alt` for `regexp`'s second prefix; `is_fiber_producer_helper` allow-list gains `"spawn"` |
| Batches | 4 with tier1 + selfhost gate each | 4 with selfhost gate each + tier1 on B1+B2 locally + CI for B3+B4 |
| Regression fixtures | 6 | 6 (all green locally) |
| Audit script | `tools/audit-flat-prefix-aliases.sh` | shipped; grep-based, 22 pass, 0 fail; wired into `tier1` |

## What changed and why — per batch

### B1 — file + spawn + log

The high-priority Hanga Roa batch. Three modules, all renamed
successfully:

- `stdlib/fs/file.kai`: `file_read_file` → `file.read`,
  `file_write_file` → `file.write`, `file_append` → `file.append`.
  Five other ops (`exists`, `delete`, `rename`, `read_bytes`,
  `write_bytes`) were already bare from PR #423 / #482.
- `stdlib/spawn.kai`: `fiber_spawn` → `spawn.spawn`, plus
  `await`/`select`/`cancel`/`yield`/`set_trap_exit`. Required adding
  `"spawn"` to `is_fiber_producer_helper`'s allow-list
  (`stage2/compiler.kai:38038`) so the structured-concurrency
  Fiber-escape checker admits the new producer name.
- `stdlib/log.kai`: 4 ops `log_debug`/`log_info`/`log_warn`/`log_error`
  → `log.debug`/`log.info`/`log.warn`/`log.error`.

Selfhost byte-identical; tier1 green locally.

### B2 — math + numeric

Where the strategic departure from Option E surfaced:

- `stdlib/math/int.kai`: bare `min`/`max` renames trigger the
  `export_ambiguous_constructor` regression — the typer monomorphises
  `min(Int, Int)` and `min(xs: [Int])` to the same C symbol
  (`kai_int__min`), causing a redefinition error. Reverted both;
  also reverted `gcd`/`lcm`/`factorial`/`fib`/`is_prime` because
  fixtures (`examples/phase4/factorials.kai`,
  `demos/euler5/main.kai`, `examples/perceus/phase3/fib35_bench.kai`)
  define them locally and the `emit_ident_value` shadow path
  (documented in `docs/lane-experience-m14-v1.md` §Finding 2)
  trips. All 9 ops stay flat.
- `stdlib/math/real.kai`: `trunc`/`floor`/`ceil`/`round_half_even`
  renamed safely; `round` had to be reverted because
  `examples/stdlib/binserialize_real.kai`, `binserialize_recursive.kai`,
  and `binserialize_string_escapes.kai` define `fn round(...)`
  locally and the same shadow path fires.
- `stdlib/decimal.kai`: only `from_int`/`from_parts` renamed.
  `zero`/`add`/`sub`/`neg`/`mul`/`div` collide with
  `stdlib/math/complex.kai` exports; `abs`/`eq`/`cmp`/`round`
  collide with the Numeric / Eq / Ord / Show protocol method
  surface; `is_zero`/`is_negative` collide with `money_is_zero`
  /`money_is_negative` if those were also renamed.
- `stdlib/money.kai`: every candidate bare name collides somewhere.
  Doc-only update.
- `stdlib/fx.kai`: `pair`/`rate_make`/`rate_at`/`table_empty`/
  `table_put`/`lookup`/`convert` all looked safe in the top-level
  grep but `pair` is defined in `examples/refinements/alias_passed_to_base.kai`,
  `lookup` in `demos/9d9l/huffman/main.kai`, `convert` in
  `examples/usd_to_eur/usd_to_eur.kai`. All reverted. Doc-only.

### B3 — array + collections + path

The "collections clash with each other" batch. The lane realised
mid-batch that `array.from_list(xs, def) : Array[a]` and
`set.from_list(xs) : Set[a]` cannot both be `pub fn from_list` at
the top level — kaikai has no overload-by-signature dispatch for
pub fns. Same for `queue.push` vs `stack.push`. Reverted every
rename; landed `stage2/compiler.kai` changes instead:

- `module_legacy_prefix` gains entries for `queue` (`"q"`),
  `stack` (`"stk"`), `random_secure` (`"random_secure"`).
- New `module_legacy_prefix_alt` table: `regexp` carries both
  `regex` and `rx` prefixes (`regexp.match` → `regex_match`,
  `regexp.parse_pattern` → `rx_parse_pattern`).

Doc-only updates to each module's header comment to pin the
canonical surface. CI verifies tier1.

### B4 — encoding + uuid + regexp + protocols + secure_random + net/http

All flat-prefix definitions stay; the resolver extensions from B3
cover the new module prefixes. Doc-only updates:

- `stdlib/encoding/base64.kai`: bare `encode` clashes with
  `hex.encode`.
- `stdlib/encoding/hex.kai`: bare `decode` shadowed by
  `demos/9d9l/huffman/main.kai`.
- `stdlib/encoding/toml.kai`: bare `decode` same shadow.
- `stdlib/uuid.kai`: bare `parse` shadowed by `demos/vs/go/main.kai`.
- `stdlib/regexp.kai`: canonical `regexp.match` is **unreachable**
  because `match` is a kaikai keyword (`TkMatch` in
  `keyword_kind`). Users call `regex_match` directly. All other
  ops are qualified-callable.
- `stdlib/protocols.kai`: `bin_*` helpers stay flat. They live
  next to the Show/Eq/Ord/Hash/BinSerialize protocols, so the
  ModuleEntry is `protocols`, not `bin`. A dedicated
  `stdlib/bin.kai` module is tracked as follow-up.
- `stdlib/random_secure.kai`: bare `int` would collide with the
  `int` module name itself.
- `stdlib/net/http.kai`: bare `get`/`put`/`delete` clash with
  `map.get` / `env.get` / `file.delete`.
- `stdlib/money.kai` (doc completes here): every bare name
  collides as catalogued in B2.

## Departures from issue #614's stated Option E

The brief was emphatic: "**Cada flat-prefix pub fn DEBE preservarse
como `pub fn flat_name = qualified.form(...)` alias**". Every flat
name IS preserved — that part holds without exception. The
departure is the **inverse direction**: the brief asked for a bare
canonical pub fn to be ADDED for every flat-prefix name. Instead,
the canonical surface is supplied by the qualified-call resolver's
prefix fallback, and bare canonical defs are added only where safe.

Structural reasons for the departure:

1. **No top-level pub fn overloading.** `min(a: Int, b: Int)` and
   `min(xs: [Int])` cannot both be `pub fn min` at the top level.
   The typer accepts the declarations but the codegen
   monomorphises both to `kai_int__min` and the C compiler rejects
   the redefinition. Same shape for any `from_list`/`to_list`/
   `empty`/`size`/`push`/etc. pair across sibling modules.

2. **Shadow bug at codegen.** When a user-defined `fn round(s)`
   exists in a fixture and the prelude exports `pub fn round(x)`,
   the codegen's `emit_ident_value` path emits a global closure
   reference for the bare identifier, ignoring the local binding.
   The bug is documented in `docs/lane-experience-m14-v1.md`
   §Finding 2 ("emit_ident_value shadows local bindings"). The
   m14 v1 lane chose prefix-fallback over rename for exactly this
   reason; the brief proposed Option E without re-checking that
   the shadow bug had been fixed. It has not.

3. **Protocol-method dispatch ambiguity.** `eq`, `cmp`, `abs`,
   `to_string`, `parse` are reached via the protocol dispatch
   surface (Numeric/Eq/Ord/Show). Adding a top-level `pub fn
   eq(d: Decimal, ...)` next to `impl Eq for Decimal { fn eq(...) }`
   creates a name overlap that the typer's resolution path treats
   ambiguously.

4. **Keyword reservation.** `regexp.match` is unreachable because
   `match` is a lexed keyword. Users call `regex_match`.

The user (the issue author) authorized "Departures de Option E
justificadas en retro" in the brief. This retro is the
justification.

## Fixtures + audit shipped

6 regression fixtures under `examples/stdlib/qualified_migration/`:

- `file_qualified_and_alias.kai` — `file.read` + `file_read_file`.
- `spawn_qualified_and_alias.kai` — `spawn.spawn`/`spawn.await` +
  `fiber_spawn`/`fiber_await`.
- `int_qualified_and_alias.kai` — `int.min`/`int.gcd` +
  `int_min`/`int_gcd`.
- `array_qualified_and_alias.kai` — `array.from_list` +
  `array_from_list`.
- `regexp_qualified_and_alias.kai` — exercises BOTH legacy
  prefixes (`regex_compile`/`regex_find_all` + `rx_parse_pattern`)
  vs `regexp.compile`/`regexp.find_all`/`regexp.parse_pattern`.
- `mixed_pipeline_qualified_and_flat.kai` — the motivating
  snippet from the issue body (`words |> list.filter(...) |>
  list.foldl(file.read, reducer)`) compared against the same
  pipeline in flat-prefix.

Each fixture prints both forms and a `match` / `MISMATCH` marker.

`tools/audit-flat-prefix-aliases.sh` — grep-based institutional
gate, 22 (module, prefix) pairs, all pass. Wired into `tier1` as
`test-flat-prefix-aliases`.

## Selfhost discipline

- B1 (file/spawn/log): selfhost byte-identical + tier1 green locally.
- B2 (math/numeric): selfhost byte-identical + tier1 green locally
  after the round/gcd/fib/factorial reverts.
- B3 (array/collections/path): selfhost byte-identical; tier1
  deferred to CI per user authorization.
- B4 (encoding/uuid/regexp/protocols/random_secure/net/http):
  doc-only + the B3 resolver extensions; selfhost byte-identical;
  tier1 deferred to CI.

## Cost

- Real elapsed: ~6 hours (vs ~4 hours estimated, +50%).
- Cycle time per failed rename: ~5 minutes (rebuild selfhost +
  observe the breakage shape).
- Wasted work: ~3 rename → revert cycles in B2/B3 before the
  shadow-bug + overload-collision pattern was internalised. The
  brief's assumption that the migration would be mechanical was
  the load-bearing miss.

## Follow-ups

- **Fix the `emit_ident_value` shadow bug.** Until that lands, no
  bare canonical rename can be assumed safe; every candidate
  requires a `grep -rn '^fn <name>' examples/ demos/` audit.
  Tracked in `docs/lane-experience-m14-v1.md` §Finding 2; deserves
  its own issue and lane.
- **`stdlib/bin.kai` module split.** The `bin_*` helpers in
  `stdlib/protocols.kai` would become qualified-reachable as
  `bin.byte` / `bin.byte_at` etc. only with a dedicated module
  file.
- **Sweep `stage2/compiler.kai` flat-prefix callsites to the
  canonical form.** The brief flagged 383 `int_*` and 89 `array_*`
  references as a stretch goal; aliases preserve them so the
  sweep is deferred. Cosmetic, low priority.
- **Drop legacy aliases at the Orongo edition boundary.** Tracked
  as part of the edition flip itself; not this lane.
