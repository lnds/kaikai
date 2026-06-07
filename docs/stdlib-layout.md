# stdlib layout

Physical and conceptual organisation of `stdlib/` in the kaikai repo.
This doc is the index: what modules exist, what paths they live at,
what effects they declare, and where the line runs between stdlib and
the forthcoming `ahu` layer. It is **not** an API reference — each
module gets its own spec when implemented.

**Status markers (refreshed 2026-05-08, issue #367; re-verified
2026-05-16 in #604).** Operations in the per-module catalog below
are tagged inline:

- *(shipped)* — `pub fn` exists in the named `stdlib/<module>.kai`.
- *(planned: #N)* — tracked in an open GitHub issue; not yet in `stdlib/`.
- *(stub)* — file is committed but exposes no public surface yet
  (typically because the runtime primitives it would call have not
  landed). `stdlib/fs/dir.kai` is the canonical example.

Without these markers the catalog reads as a promise that everything
listed already exists; that drift was the trigger for #367.

Pinned decisions come from the stdlib discussion of 2026-04-24.
Doc A (`effects.md`), Doc B (`effects-stdlib.md`), and Doc C
(`effects-impl.md`) remain the sources of truth for effect semantics,
catalog, and CPS lowering respectively. This doc sits on top of B: B
lists the effects, this doc lists the modules that use them.

## Purpose

Three things this doc fixes, one at a time:

1. **The tree** — a canonical list of module paths under `stdlib/`,
   so contributors know where new code goes without a per-module
   debate.
2. **The ambition** — that kaikai's stdlib is "Go-wide": batteries
   included for networking, time, random, OS, encoding, regex, crypto
   basics. Not a minimalist core that relies on third-party packages
   for common work. The initial MVP ships a subset of this tree; see
   Open Questions for what defers.
3. **The frontier with `ahu`** — stdlib owns primitives; `ahu` owns
   OTP-style application patterns. The rule below decides edge cases
   without reopening the debate each time.

## Scope rule — stdlib vs ahu

The division follows a single operational principle:

> **stdlib provides primitives of a domain. `ahu` provides patterns
> that compose multiple primitives with policy (supervision, restart,
> lifecycle, registration).**

Worked examples of the rule in action:

| Thing                              | Location | Reason                                                               |
|------------------------------------|----------|----------------------------------------------------------------------|
| `net.tcp.connect`, `net.http.get`  | stdlib   | single-domain primitive; usable in a CLI script without ahu          |
| connection pool with backoff       | `ahu`    | policy across `Net` + `Cancel` + `Clock` + restart                   |
| `process.spawn`, `process.wait`    | stdlib   | single-domain primitive; running `ls -la`                            |
| supervised port program            | `ahu`    | composes `Process` + `supervisor` restart policy                     |
| `nursery` (scoped `Spawn` + cancel-on-fail) | stdlib | safe default scope over `Spawn` — policy in its own spec    |
| `supervisor` (restart strategies)  | `ahu`    | configurable policy, supervision tree shape                          |
| `Actor[Msg]` — mailbox + send/receive | stdlib | primitive concurrency communication, same tier as `Spawn`           |
| `GenServer`-analog (state + callbacks) | `ahu`| OTP pattern on top of `Actor[Msg]`                                   |

If it requires `Cancel` + `Clock` (timeouts, deadlines) to be usable,
that is already stdlib (both are stdlib effects). It is only `ahu`
when it introduces **restart / supervision / lifecycle / registry**
semantics.

Consequence: a complete program that speaks HTTP, reads files, spawns
subprocesses, and uses bounded concurrency can be written against
stdlib alone. `ahu` appears when the user wants OTP-style structural
supervision and named processes.

## Naming convention

Functions in stdlib live under namespaced modules. Callers reach them
as `module.function`:

```kai
list.map(xs, f)
list.filter(xs, p)
string.split(s, sep)
option.unwrap_or(o, dflt)
```

Module names are lowercase (the file path); function names are
`snake_case`. Effects keep their PascalCase by convention because they
are types declared with `effect Name { ... }`; effect ops read
`Console.print(s)`, `File.read(p)`, etc. — the same dot syntax, the
distinction is *what is on the left*, not *how it is called*.

The flat-prefix style (`list_take`, `string_concat`, `opt_map`) used
in early stdlib drafts is **retired** as the canonical user-facing
form. Module-relative names (`list.take`, `option.map`) are now the
canonical surface. The m14 milestone (#203) carried out the rename
across `stdlib/core/{list,string,option,result,char}.kai` and their
consumers in `examples/` + `stdlib/` non-core; see the *Migration
status* section below for the per-module summary and the small set
of flat aliases that survive on a typer-resolver gap (#219).

The four ubiquitous list operations — `map`, `filter`, `reduce`,
`each` — keep a flat prelude alias post-migration so common pipelines
read short (`xs |> map(f) |> filter(p)`). The aliases re-export
`list.map` / etc. The full set is reached only via `list.*`.

### Migration status (m14, #203)

| Module                  | Bare-name surface | Flat aliases removed | Flat aliases surviving | Reason for survivors                                              |
| ----------------------- | ----------------: | -------------------: | ---------------------: | ----------------------------------------------------------------- |
| `stdlib/core/list.kai`  |               29+ |                   29 |                      0 | none — final 5 retired in #227 after PR #218 closed the prelude-scope (#216) and string-interpolation (#217) resolver gaps |
| `stdlib/core/string.kai`|               13+ |                  ~14 |                      1 | `string_repeat` — typer EModCall name-only lookup (#219) collides with `list.repeat` |
| `stdlib/core/option.kai`|                10 |                    6 |                      4 | `opt_map`, `opt_filter`, `opt_zip` — typer EModCall (#219) collides with `list.{map,filter,zip}`; `opt_or` — `or` is a reserved keyword (`TkOr`) |
| `stdlib/core/result.kai`|                12 |                    6 |                      6 | `result_map`, `result_and_then`, `result_unwrap_or`, `result_or_else`, `result_unwrap_or_else`, `result_collect` — all #219 collisions with `list` / `option` exports |
| `stdlib/core/char.kai`  |                 8 |                    8 |                      0 | none — character-domain names are unique across `stdlib/core/`    |
| `stdlib/core/tuple.kai` |                 7 |                    7 |                      0 | helpers landed module-qualified only (`tuple.swap`, `tuple.map_fst`, `tuple.map_snd`, `tuple.map_pair`, `tuple.first`, `tuple.second`, `tuple.third`) — issue #348 |
| `stdlib/core/io.kai`    |               n/a |                  n/a |                    n/a | exports a single `pub fn println`; already module-relative shape  |

**Issue #219** (typer EModCall lookup is name-only) is the gating
follow-up for the 11 `opt_*` / `result_*` survivors and the lone
`string_repeat`. Once the typer resolves `module.fname` against
the explicit module qualifier instead of doing a flat
`ty_env_lookup(fname)`, the remaining flat aliases can be deleted
in a mechanical follow-up. The `opt_or` survivor is independent
— it is blocked by parser-level keyword reservation, not by #219.

### Migration status (m14 follow-up, #614)

The 21 modules outside `stdlib/core/*` listed in issue #614 keep
their flat-prefix definitions and surface the canonical qualified
form (`<module>.<op>`) through the qualified-call resolver's
prefix fallback (`me_lookup_export` in `stage2/compiler.kai`).
Closed by the m14 follow-up lane (`docs/lane-experience-issue-614-m14-followup.md`).

| Module                          | Legacy prefix | Canonical surface                              | Notes                                                          |
| ------------------------------- | ------------- | ---------------------------------------------- | -------------------------------------------------------------- |
| `stdlib/fs/file.kai`            | `file`        | `file.read`, `file.write`, `file.append`, ...  | 3 ops gained bare aliases (`read`/`write`/`append`); 5 already bare |
| `stdlib/spawn.kai`              | `fiber`       | `spawn.spawn`, `spawn.await`, ...              | 6 ops gained bare aliases (`spawn`, `await`, `select`, `cancel`, `yield`, `set_trap_exit`); `nursery` already bare |
| `stdlib/log.kai`                | `log`         | `log.debug`, `log.info`, `log.warn`, `log.error` | 4 ops gained bare aliases                                       |
| `stdlib/math/int.kai`           | `int`         | `int.min`, `int.max`, `int.gcd`, ...           | All defs stay flat (`gcd`/`fib`/`factorial` widely shadowed by fixtures + demos) |
| `stdlib/math/real.kai`          | `real`        | `real.min`, `real.floor`, `real.ceil`, ...     | 4 ops gained bare aliases (`trunc`/`floor`/`ceil`/`round_half_even`); `round` stays flat (fixture shadow) |
| `stdlib/decimal.kai`            | `dec`         | `decimal.zero`, `decimal.add`, `decimal.eq`, ... | 2 ops gained bare aliases (`from_int`/`from_parts`); rest collide with complex.kai or protocol methods |
| `stdlib/money.kai`              | `money`       | `money.make`, `money.zero`, `money.add`, ...   | All defs stay flat (collide with decimal/complex/protocol methods) |
| `stdlib/fx.kai`                 | `fx`          | `fx.pair`, `fx.rate_make`, `fx.convert`, ...   | All defs stay flat (`pair`/`lookup`/`convert` shadowed by fixtures) |
| `stdlib/array.kai`              | `array`       | `array.from_list`, `array.to_list`, `array.copy` | All defs stay flat (`from_list`/`to_list` collide with collections) |
| `stdlib/collections/set.kai`    | `set`         | `set.empty`, `set.insert`, `set.union`, ...    | All defs stay flat (clash with map / queue / stack / list)     |
| `stdlib/collections/queue.kai`  | `q`           | `queue.push`, `queue.pop`, `queue.peek`, ...   | All defs stay flat (clash with stack)                          |
| `stdlib/collections/stack.kai`  | `stk`         | `stack.push`, `stack.pop`, `stack.peek`, ...   | All defs stay flat (clash with queue)                          |
| `stdlib/path.kai`               | `path`        | `path.basename`, `path.dirname`, `path.join`, ... | All defs stay flat (`join`/`split` clash with `string.join`/`split`) |
| `stdlib/encoding/base64.kai`    | `base64`      | `base64.encode`, `base64.decode`, ...          | All defs stay flat (`encode` clashes with hex.encode)          |
| `stdlib/encoding/hex.kai`       | `hex`         | `hex.encode`, `hex.decode`, ...                | All defs stay flat (`decode` shadowed by demos)                |
| `stdlib/encoding/toml.kai`      | `toml`        | `toml.decode`, `toml.encode`, ...              | All defs stay flat                                             |
| `stdlib/uuid.kai`               | `uuid`        | `uuid.v4`, `uuid.format`, `uuid.is_valid`, ... | All defs stay flat (`parse` shadowed by demos)                 |
| `stdlib/text.kai`               | `text`        | `text.display_width`, `text.char_width`        | wcwidth-style terminal column width over codepoints (#745); East Asian Width + zero-width tables. Built on #744 `string.chars`. Codepoint-level (NOT grapheme/UAX#29) |
| `stdlib/regexp.kai`             | `regex` + `rx` (alt) | `regexp.compile`, `regexp.find_all`, `regexp.parse_pattern` | Resolver carries TWO legacy prefixes via `module_legacy_prefix_alt`; `regexp.match` unreachable (`match` is a keyword) |
| `stdlib/random_secure.kai`      | `random_secure` | `random_secure.int`, `random_secure.bytes`   | All defs stay flat                                              |
| `stdlib/net/http.kai`           | `http`        | `http.parse_url`, `http.get`, `http.post`, ... | All defs stay flat (`get`/`put`/`delete` clash with map/env/file) |
| `stdlib/protocols.kai`          | `bin`         | `bin_*` helpers — NOT a module surface today   | A dedicated `stdlib/bin.kai` split is tracked as follow-up     |

**Departure from issue #614's stated Option E ("each flat-prefix
pub fn duplicated to a bare canonical + alias"):** kaikai does not
overload `pub fn` by signature at the top level. Adding bare
canonical defs collides with sibling modules (`min(Int, Int)` vs
`min(xs: [Int])`, `from_list(xs, def): Array` vs `from_list(xs):
Set`, `push(q): Queue` vs `push(s): Stack`) and shadows user-defined
fns in fixtures and demos (`fn round`, `fn fib`, `fn factorial`,
`fn gcd`, `fn pair`, `fn lookup`, `fn convert`, `fn decode`,
`fn parse`). The compiler's `emit_ident_value` shadow path
(documented in `docs/lane-experience-m14-v1.md` §Finding 2) trips
the codegen when a user-defined local shares a name with a global
pub fn. Where the bare canonical is safe (no shadow, no top-level
clash), the lane added both forms; otherwise the qualified-call
resolver's prefix fallback alone supplies the canonical surface.

The 5 `list_*` survivors documented in the m14 Phase 1 retro
(`docs/lane-experience-issue-203-phase1-list.md`) were retired
in #227 once PR #218 closed the prelude-scope (#216) and
string-interpolation (#217) resolver gaps that had blocked the
qualified form. `stdlib/core/list.kai` now has zero flat aliases.

Stage 0 PRELUDE builtins (`string_concat`, `string_length`,
`string_slice`, `string_split`, `string_contains`, `list_length`,
`list_reverse`, `char_at`, `char_to_int`, `int_to_char`, …) are
*below* the user-facing language and were never part of the m14
migration; see `docs/m14-bootstrap-audit.md` Risk 5.

## Bootstrap layering

Modules are tagged by which compiler stage they require:

- **Stage 1 — bootstrap-safe.** Compiles with kaikai-minimal (no
  effects, no row polymorphism, no typeclasses, no `++` operator;
  parametric HM only). Only `core/*` lives here. This is the set
  that bootstraps with `kaic0` and is what `selfhost` exercises
  every CI run.
- **Stage 2 — full-kaikai.** Requires the full language (effects,
  row polymorphism, Perceus, monomorphisation, `++`). Everything
  else (`encoding/`, `collections/`, `decimal`, `money`, `loop`,
  `reader`, `writer`, `random`, `time`, `net/`, `crypto/`,
  `regexp`, `concurrent/`, `testing`).

The `stdlib/` directory does not reshuffle itself between stages; the
stage 1 driver (`kaic0` / `kaic1`) prepends only `core/*.kai` as
preludes, the stage 2 driver (`bin/kai`) prepends the full chain
(`core/*.kai` + `protocols.kai` + `effects.kai` + `random.kai` +
`encoding/*.kai` + `collections/*.kai`).

### The fixture-side rule

The same boundary applies to the test fixtures under
`examples/stdlib/`:

- Fixtures that exercise `core/*` (`list_basic`, `list_extrema`,
  `list_sort`, `list_while_uniq`, `list_flat_zip`) are written in
  **kaikai-minimal** and must remain bootstrap-safe. They use the
  pre-effect `print` builtin (no row), unqualified
  `string_concat(a, b)` rather than `a ++ b`, and avoid every
  full-kaikai sugar. Validation: they must still compile under
  `kaic1 --prelude stdlib/core/*.kai`.
- Fixtures that exercise full-kaikai modules (`base64_basic`,
  `hex_basic`, `map_basic`, `jwt_encoder`) **are full-kaikai**.
  They use `Stdout.print` with the `/ Stdout` row, the `++`
  operator, and other m7+ features. Validation lives in
  `make test-stdlib`, which routes through kaic2 with the full
  prelude chain.

The `make test-stdlib` target validates the full-kaikai layer
end-to-end (kaic2 + complete prelude chain). The bootstrap-safe
subset is validated indirectly by `make selfhost`, which compiles
`stage2/compiler.kai` through kaic1 with `core/*` as the only
prelude — if any `core/*.kai` file picks up a full-kaikai-only
construct, selfhost breaks.

> Why this split is intentional: kaikai-minimal is **the bootstrap
> language**, not "kaikai for beginners". Effects, row polymorphism,
> and the operator sugars (`++`, `@cap`, trailing lambdas) are
> full-kaikai by design — they cannot land in kaikai-minimal
> without breaking Tier 1 principle 3 ("fast compilation, LL(1)
> grammar with minor bookkeeping"). The right layer for those
> features is stage 2.

## Module tree

```
stdlib/
  core/          pure, stage 1
    list.kai
    string.kai
    option.kai
    result.kai
    char.kai
    tuple.kai
    ordering.kai
  collections/   pure, stage 2
    map.kai      (AVL-tree ordered map — #128)
    hashmap.kai  (mutable hash table, Mutable effect — shipped via #374)
    set.kai      (list-backed Set[a], O(n) — #614)
    hashset.kai  (mutable hash set over HashMap[t, Unit], Mutable effect — shipped via #375)
  math/          pure, stage 2
    int.kai      (shipped; `log2` + `div_mod` shipped via #347; `abs`, `signum`, `clamp`, `pow` provided by `Numeric for Int` — #347 closed)
    real.kai     (shipped; libm bindings via PR #359, closes #343 — was `float.kai` pre-rename, `math.float` does NOT exist)
    bits.kai     (shipped — intrinsic bit ops live here, NOT in `math.int`)
    numeric.kai  (shipped)
    complex.kai  (shipped)
  decimal.kai    pure, stage 2 (top-level module — shipped)
  money.kai      pure, stage 2 (top-level module; depends on decimal — shipped)
  fx.kai         pure, stage 2 (top-level module; depends on decimal + money — shipped via #365)
  loop.kai       row-polymorphic, stage 2 (top-level module: while, until, repeat, forever — shipped)
  reader.kai     effect: Reader[T] (top-level module: with_reader — shipped)
  writer.kai     effect: Writer[W] (top-level module: with_writer — shipped)
  io.kai         (NOTE: lives in `stdlib/core/io.kai`, not at top level; effects: Console, Stdin)
  fs/            effect: File
    file.kai     (shipped via PR #132 — `read_file`, `write_file`, `append`; `exists`, `delete`, `rename` shipped via #345 + #423)
    dir.kai      (stub — runtime primitives planned via #344)
    path.kai     pure helpers — shipped, currently lives at `stdlib/path.kai` (top-level), not `stdlib/fs/path.kai`
  os/            effects: Env, Process
    env.kai      (shipped via PR #131 + PR #143)
    args.kai     (shipped via PR #131 + PR #143)
    process.kai  (shipped — closes #346; 4 public fns: `start`, `wait`, `kill`, `exit`)
  time.kai       effect: Clock (top-level module — shipped; default handler via PR #134)
  array.kai      pure (Array[T] / [T] bridge — shipped via #366)
  random.kai     effect: Random (top-level module — shipped)
  random_secure.kai  effect: SecureRandom (top-level — shipped via PR #144, closes #140)
  log.kai        effect: Log (top-level module — shipped via PR #145, closes #141)
  net/           effects: NetTcp + NetUdp + NetDns all shipped; alias `Net = NetTcp + NetUdp + NetDns` now definable (follow-up adds the row alias)
    tcp.kai      uses NetTcp (shipped — v1; R2 reactor flipped NetTcp to fiber-parking 2026-05-16 via #630; see effects-stdlib.md sidebar)
    udp.kai      uses NetUdp (shipped — v1 datagram UDP via #354; blocking ops, IPv4 only; `bind`/`send`/`recv`/`close`/`local_port`/`addr`)
    dns.kai      uses NetDns (shipped — issue #352; `resolve`/`resolve_first`/`with_dns`; getaddrinfo shim, IPv4-only, blocking on the OS thread in v1)
    url.kai      pure URL parsing (planned — folded into http.kai parser today)
    http.kai     (shipped, client only — `http_get/post/put/delete/request`; uses NetTcp + Cancel; still uses libc `getaddrinfo` via NetTcp.connect's implicit path — splitting resolve→connect onto NetDns is the #352 follow-up, not yet done)
  encoding/      pure, stage 2
    json.kai     (shipped — Real number parsing landed via #361, surrogate-pair UTF-8 via #362)
    utf8.kai     (planned — no tracking issue)
    base64.kai   (shipped)
    hex.kai      (shipped)
  regexp.kai     pure, stage 2 (top-level — shipped 2026-04-28)
  crypto/        pure, stage 2 (shipped via PR #146)
    hash.kai     sha256, sha512 (blake3 NOT shipped — deferred to S3)
    mac.kai      hmac_sha256, hmac_sha512
  concurrent/    effects: Spawn, Cancel (Actor[Msg] deferred to docs/actors.md)
                 NOTE: `concurrent/` subdirectory does NOT exist on disk;
                 `nursery` lives at `stdlib/spawn.kai:95`, `actor` at
                 `stdlib/actor.kai`. Both shipped.
    nursery.kai  (shipped — physical path is `stdlib/spawn.kai`)
    actor.kai    (shipped — physical path is `stdlib/actor.kai`)
  testing.kai    integrates with `kai test` (planned — test syntax is shipped, no `testing.kai` module file yet)
```

## Per-module summary

Terse — one line per module, listing declared effects. Full signatures
land in each module's own spec when implemented.

### core (pure, stage 1)

- `core.list` — map, filter, foldl, foldr, foreach, length, reverse, nth, take, drop, zip, unzip, concat, any, all, find, index_of, sum, product; surface expansion (#340 — shipped): `last`, `init`, `partition`, `split_at`, `span`, `chunk`, `windows`, `intersperse`, `enumerate`, `zip3`, `scan`, `group_by`, `find_map`. `group_by` uses Erlang/Elixir consecutive-key semantics (NOT Haskell's all-equal-regardless-of-position) — the consecutive variant stays linear without an `Eq` map and is the v1 choice while generic free fns can't carry `: Eq` bounds.
- `core.string` — length, slice, to_int, concat, starts_with, ends_with, trim, repeat, join *(originals)*; split, replace, pad_left, pad_right, lines, is_blank *(shipped via #338, partial)*; drop_prefix, drop_suffix *(shipped via #632)*; **bytes, chars, char_count, byte_length, char_indices + `CharIndex` record** *(shipped via #744 — the codepoint model)*. `length`/`byte_length` count bytes; `char_count` counts Unicode codepoints; `bytes()` is the byte-level `[Char]`, `chars()` the codepoint-level `[Char]` (UTF-8 decode). `char_at`/`slice` stay byte-indexed. `split(s, "")` panics; `lines("")` returns `[]` (Python/Rust convention). `drop_prefix` / `drop_suffix` return `Option[String]` (`Some(rest)` on match, `None` otherwise) — same shape as `to_int`, `log2`, `div_mod`, mirroring Rust's `str::strip_prefix`. The remainder of #338's proposed surface (`index_of`, `to_upper`, `to_lower`, `is_empty`, `reverse`) was deferred to #396: each colliding bare name with an existing core export breaks `--include-prelude-tests` because the typer's first-arg-type narrowing per #235 does not cross modules in that mode. Tracked by the issue's "Notes for the implementer" — the resolver fix lands first, the surface follows (mirrors the #335 → #336 sequencing).
- `core.option` — is_some, is_none, map, and_then, unwrap_or, or_else
- `core.result` — is_ok, is_err, map, map_err, and_then, unwrap_or
- `core.char` — is_digit, is_alpha, is_alnum, is_space, to_lower, to_upper
- `core.tuple` — `Pair`/`Triple`/`Quad` types; helpers `swap`, `map_fst`, `map_snd`, `map_pair` (Pair) and `first`/`second`/`third` (Triple) (shipped via #348). `fst`/`snd` projections are field-access only (`p.fst`) — see module header for the function-vs-field shadowing rationale.
- `core.ordering` — `Ordering` type, chain, reverse

### collections (pure, stage 2)

- `collections.map` — AVL-tree-backed ordered map (canonical qualified-call style as of #613): `empty`, `size`, `is_empty`, `get`, `contains`, `put`, `remove`, `keys`, `values`, `to_pairs`, `from_pairs`, `update`, `fold`, `merge`, `filter`, `transform_values` (16 pub fns); `map`, `flat_map`, `filter` exported with `Pair[k, v]` element shape so `Map[k, v]` participates in `|`, `||`, `|?` per #594. Legacy flat-prefix aliases (`map_empty`, `map_size`, `map_is_empty`, `map_get`, `map_contains`, `map_put`, `map_remove`, `map_to_pairs`, `map_keys`, `map_values`, `map_from_pairs` — 11 one-liners) retained for the tongariki edition and scheduled to drop at the Orongo edition boundary.
- `collections.hashmap` — **mutable** hash table (separate chaining) behind the `Mutable` effect, shipped via #374. The default associative collection for 1.0: ~2-3× faster than the AVL `Map` on build and lookup at N≥10K (see `benchmarks/hashmap/`). Short module-relative surface (same convention as `map`/`set`): `empty`, `size`, `is_empty`, `get`, `contains`, `put`, `remove`, `keys`, `values`, `to_pairs`, `from_pairs`, `merge` — called `hashmap.put(m, k, v)` etc. (12 pub fns; no `hashmap_*` aliases). **Mutating, not persistent** — `put`/`remove` mutate in place and return `Unit`; every op carries `/ Mutable`. The carrier was redesigned from the issue body's pure HAMT (which benchmarked ~2× SLOWER than the AVL `Map`) to a mutable table; the signatures diverge from the issue's pure shapes accordingly (see lane retro). Bucket array + count + capacity in `Ref` cells; resize doubles at load factor 0.75. Iteration order unspecified (bucket-derived) but deterministic for a given insertion sequence. Keys need `impl Hash` + `==`; primitives and user **sum types** dispatch through the generic boundary, user **records** do not yet (compiler dispatch gap, see lane retro). The `m[key]` read-side sugar dispatches to `hashmap.get` (returns `Option[v]` / `Mutable`) via the typer's `synth_index`, same mechanism `Map` uses. No HashDoS mitigation in v1.
- `collections.set` — ordered set: empty, insert, remove, contains, union, intersection
- `collections.hashset` — **mutable** hash set, a thin wrapper over `HashMap[t, Unit]` (each member is a key whose value is the single `Unit` value), shipped via #375. The O(1)-average set, sibling to `HashMap` exactly as the list-backed `Set` is the sibling of the AVL `Map`. Short module-relative surface (same convention as `hashmap`/`set`): `empty`, `size`, `is_empty`, `contains`, `add`, `remove`, `to_list`, `from_list`, `union`, `intersection`, `difference`, `is_subset` — called `hashset.add(s, x)` etc. (12 pub fns; no `hashset_*` aliases). **Mutating, not persistent** — `add`/`remove` mutate in place and return `Unit`; every op carries `/ Mutable`. The set-algebra ops (`union`/`intersection`/`difference`) build and return a FRESH set, leaving both arguments untouched. Carrier is `{ inner: HashMap[t, Unit] }`; every op delegates to `collections.hashmap`, inheriting its performance, mutation model, and effect discipline. Iteration order unspecified (bucket-derived). Elements need `impl Hash` + `==`; primitives and user **sum types** dispatch through the generic boundary, user **records** do not yet (HashMap's compiler dispatch gap, inherited — see lane retro). NOTE: the issue #375 body is stale — it assumed a *persistent* HashMap and a `Hashable` protocol, neither of which shipped; this module mirrors the as-shipped mutable HashMap and the `Hash` protocol.

### math (pure, stage 2)

- `math.int` — `min`, `max`, `gcd`, `lcm`, `factorial`, `fib`, `is_prime`, `log2`, `div_mod` *(all shipped; `log2` and `div_mod` via #347)*. `abs`, `signum`, `clamp`, `pow` are NOT here — they dispatch through `Numeric for Int` (`stdlib/math/numeric.kai`); `signum` is `Numeric.sign` and `pow` is `Numeric.pow_int`. Coverage for the four Numeric ops over Int lives in `examples/stdlib/math_int_basic.kai`. Bit ops (`shl`, `shr`, `and`, `or`, `xor`, `not`, `popcount`, `leading_zeros`, `trailing_zeros`) are intrinsic — they live in `math/bits.kai`, NOT in `math/int.kai`.
- `math.real` *(shipped — was `math.float` in earlier drafts; the module file is `stdlib/math/real.kai` and the namespace is `math.real`. `math.float` does not exist.)* — `min`, `max`, `floor`, `ceil`, `round`, `round_half_even`, `trunc` *(shipped)*; libm bindings — `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10`, `pow` — *(shipped via PR #359, closes #343)*; `rem` (`fmod`) wires `impl Rem for Real` so `r1 % r2` type-checks — *(shipped, closes #364)*. `is_nan` / `is_inf` *(planned, no tracking issue)*.

### decimal (pure, stage 2)

- `decimal` — arbitrary-precision decimal arithmetic with rounding
  modes (top-level module: `decimal.add`, `decimal.from_string`, …).
  `dec_div(a, b, target_scale)` returns `Option[Decimal]`: `None` on
  division by zero, `Some(q)` otherwise *(closes #363)*.

### money (pure, stage 2)

- `money` — `Money[Currency]` with precision per currency, safe
  arithmetic (no implicit cross-currency ops); depends on `decimal`

### fx (pure, stage 2 — shipped via #365)

- `fx` — currency conversion on top of `Money` + `Decimal`. Carriers
  `FxPair`, `FxRate`, `FxTable`, `FxTimestamp`. Operations:
  `fx_pair`, `fx_rate_make`, `fx_rate_at`, `fx_table_empty`,
  `fx_table_put`, `fx_lookup`, `fx_convert`. Composition wrappers:
  `money_add_via_fx`, `money_sub_via_fx`, `money_cmp_via_fx`.
  Inverse rates are NOT auto-derived (real-world bid/ask spreads
  are asymmetric); callers register both directions explicitly or
  derive an inverse via `dec_div`. v1 has no transitive lookup,
  no rate aging, no live feed — those follow up in
  `stdlib/fx-extras.kai` once a use case appears.

### array (pure, stage 2 — shipped via #366)

Top-level module bridging the linked-list `[T]` carrier to the
contiguous `Array[T]` carrier. The runtime ships `array_make`,
`array_get`, `array_set`, `array_length`, `array_grow` plus the
indexing sugars (`a[i]`, `a[i] := v`); this module fills the
gap between the two carriers so consumers don't roll their own
loops.

Public surface today is **flat-prefix** (`array_*`); the namespaced
rename to `array.*` waits for the m14 phase-2 migration that already
covers `list.*`. Listed here as shipped:

- `array_from_list[a](xs: [a], default: a) : Array[a]` — copy a
  linked list into a fresh Array. `default` is the runtime's
  reified-element slot for `array_make`; for non-empty inputs it
  is overwritten on the same pass.
- `array_to_list[a](a: Array[a]) : [a]` — copy an Array into a
  linked list in index order. Pure read; no `Mutable` demand.
- `array_copy[a](src: Array[a], default: a) : Array[a]` — element
  copy. Result is a distinct allocation; mutating either side
  does not affect the other.

The `default: a` thread is a v1 contract: `array_make(0, init)`
needs an `init: T` to reify the element type even when `n == 0`,
and an empty input list has no element to lift. The cleaner
`[a : Default]` form waits for #341 Form 3 (free-fn tparam
bounds with protocol dispatch).

### loop (row-polymorphic, stage 2)

Top-level module exposing the canonical Koka-style control-flow
helpers as ordinary functions. Each helper is row-polymorphic in
its lambdas — the effect of the predicate / body flows out
unchanged via the row variable `e`. The functions themselves
introduce no effect; they are pure recursion with mandatory TCO.

- `loop.while[e](pred: () -> Bool / e, body: () -> Unit / e) : Unit / e`
- `loop.until[e](pred: () -> Bool / e, body: () -> Unit / e) : Unit / e` — body runs first; do-while shape
- `loop.repeat[e](n: Int, body: () -> Unit / e) : Unit / e`
- `loop.forever[e](body: () -> Unit / e) : Nothing / e` — only exits via `Cancel`

Used with the m7b §5 double-trailing-lambda sugar for the natural
Koka feel:

```kai
while  { @i > 0 }  { i := @i - 1; io.println("#{@i}") }
until  { @done }   { process_one() }
repeat(10)         { io.println("hi") }
forever            { handle_message() }
```

The bare names (`while`, `until`, `repeat`, `forever`) are
auto-imported in stage 2 so user code does not need to write
`loop.while` explicitly. The qualified form remains legal
when an explicit reference is preferred.

`while`, `until`, `repeat`, and `forever` are deliberately
**not** language keywords — they are stdlib functions, callable
like any other. A user can shadow them by binding the same name
locally; the compiler does not protect them. This is consistent
with the Tier 2 principle "few visible concepts, layered" and
with Doc CLAUDE.md's stance that ordinary stdlib functions are
preferable to language-level control structures when the row
machinery suffices.

### io (`/ Console + Stdin`)

Single top-level module. Per-function effects: `print` / `println` /
`eprint` / `eprintln` declare `/ Console`; `read_line` / `read_all`
declare `/ Stdin`.

- `io` — `print`, `println`, `eprint`, `eprintln`, `read_line`, `read_all`, `read_bytes(n)` *(byte-oriented stdin, shipped via #453 as the `read_bytes` prelude builtin + `Stdin.read_bytes` effect op; LSP framing precursor for #447)*

### fs (`/ File`)

- `fs.file` — `read_file`, `write_file`, `append` *(shipped via PR #132)*; `exists`, `delete`, `rename` *(shipped via #345 as prelude builtins; module-qualified surface `file.exists` / `file.delete` / `file.rename` shipped via #423)*; `metadata`, `read_bytes`, `write_bytes` *(deferred — follow-up to #345)*
- `fs.dir` — `list_dir`, `create_dir`, `remove_dir`, `walk` *(shipped via #344: `kai_prelude_dir_*` runtime primitives + `pub fn` wrappers in `stdlib/fs/dir.kai`. All four ride the `File` effect. v1 limits: symlinks are not followed in `walk`; `create_dir` uses fixed `0755`; `list_dir` / `walk` return `[]` on read error.)*
- `fs.path` — pure helpers: `is_absolute`, `basename`, `dirname`, `split`, `ext`, `strip_ext`, `join` *(shipped — but the file lives at `stdlib/path.kai`, not `stdlib/fs/path.kai`; the catalog name `fs.path` reflects the planned move, not today's import path. Today: `import path`.)*

### os (`/ Env + Process`)

- `os.env` — `get`, `set`, `unset`, `entries` *(shipped via PR #131 + PR #143; closes #127 — `entries` was renamed from `all` because the bare `all` collided with `list.all` for any caller importing both)*
- `os.args` — `argv`, `program_name` *(shipped via PR #131 + PR #143)*
- `os.process` — `start(cmd, args)`, `wait`, `kill`, `exit` *(shipped — closes #346; 4 public fns wrapping the four ops of `Process`. `wait_or_kill`, `pipe_stdout`, `pipe_stdin`, `signal` still deferred — pipe redirection requires runtime work; `wait_or_kill` is now unblocked (R1 reactor shipped 2026-05-15, #611, parks the fiber on `Process.wait` via SIGCHLD self-pipe) and waits on the cancel-aware wrapper, see `docs/effects-stdlib.md` §`Process` v1-status sidebar.)*
- `os.exit` — physically `process.exit` *(shipped via #346)*. Takes an exit code. Effect: `/ Process` (exiting is observable). Top-level alias `os.exit` (calling `exit(7)` without the `process.` qualifier) is a module-system design decision pending in stage 2; until then, `process.exit(code)` is the surface.

### time (`/ Clock`)

Single top-level module. Contains both the types (`Instant`,
`Duration`) and the operations (`now`, `sleep`, `deadline_in`).

- `time` — `WallTime`, `Instant`, `Duration`, `now()` (wall),
  `monotonic()`, comparison/subtraction on instants, duration
  arithmetic (millis, seconds, minutes, …), `sleep(d)`,
  `deadline_in(d)` — integrates with `Cancel`

### random (`/ Random`) and random_secure (`/ SecureRandom`)

Two separate top-level modules with **separate effects**, deliberately
not unified. A test handler that stubs `Random` does not affect
security-sensitive code paths.

- `random` — `int_range`, `float`, `bytes`, plus pure helpers
  (`shuffle`, `choice`, `sample`) built on top; the default
  handler is seedable for tests. `shuffle` runs in-place
  Fisher-Yates over a locally-built `Array[T]` (O(n), one Array
  allocation beyond the input/output lists) — the previous
  selection-sampling implementation cost O(n²) and was retired in
  PR #366. The masking discipline keeps `shuffle`'s observable row
  at `[T] -> [T] / Random`.
- `random_secure` — `int`, `bytes`, cryptographic-grade primitives;
  not seedable

### net (`/ NetTcp`, `/ NetUdp`, `/ NetDns` all shipped)

> **v1 caveat (2026-06-06).** All three leaves of the `Net = NetTcp + NetUdp + NetDns` alias now exist as builtins with runtime handlers and module files — `NetTcp` (#68), `NetUdp` (#354), `NetDns` (#352). The alias is therefore definable; a follow-up lane adds the row alias to `stdlib/effects.kai`. See `docs/effects-stdlib.md` §`NetTcp`, `NetUdp`, `NetDns` v1-status sidebars for reactor + per-effect status.

- `net.tcp` — `connect`, `listen`, `accept`, read/write *(shipped — PR #68; R2 reactor flipped the default handler to fiber-parking via `poll()` 2026-05-16, issue #630 — every blocking op parks the fiber, never the OS thread)*
- `net.udp` — `bind`, `send`, `recv`, `close`, plus pure `local_port` / `addr` *(shipped — issue #354; v1 datagram UDP over POSIX `SOCK_DGRAM`, blocking ops, IPv4 only; the m8.x reactor lifts NetTcp + NetUdp together)*
- `net.dns` — `resolve`, `resolve_first`, `with_dns` *(shipped — issue #352; `getaddrinfo(3)` shim, IPv4-only, blocking on the OS thread in v1; `resolve_all` / `reverse_lookup` deferred — reverse DNS is a separate proposal per #352)*
- `net.url` — pure: `parse`, `format`, `join`, `query_*` *(planned — `http_parse_url` exists inside `net/http.kai` as a private helper but is not exposed as a `net.url` module)*
- `net.http` — client surface (stable): `http_get`, `http_post`, `http_put`, `http_delete`, `http_request` *(shipped, `/ NetTcp + Cancel`)*; headers, body, timeouts available via the request builder. Automatic redirect following on top of the existing single-call surface: `RedirectPolicy` record, `default_redirect_policy`, `http_follow`, plus the convenience wrappers `http_get_follow` / `http_post_follow` / `http_put_follow` / `http_delete_follow` *(shipped — issue #357)* — RFC 9110 §15.4 method-rewrite rules baked in (303 → GET, 307/308 preserve method/body, 301/302 → GET on POST per browser convention); cookie persistence and cross-origin Authorization stripping deferred to the cookie-policy lane. The bare `http_request` keeps its single-call semantics so callers wanting explicit 3xx control are unaffected. Uses libc `getaddrinfo` for DNS (no `NetDns` effect yet). Server-side helpers (`#[unstable]`): `http_parse_request`, `http_serialize_response`, `http_status_reason` (pure), and `http_read_request` (`/ NetTcp`) *(shipped — issue #605)* — minimal primitives for hand-rolled HTTP/1.1 servers. The wire helpers reuse the client-side internals (`http_str_index`, `http_parse_header_lines`, `http_format_headers`, `http_header_lookup`); the convenience read loop is the only NetTcp-touching addition.

A full server framework (`manutara`-side: router, middleware,
graceful shutdown) is deferred — its design surface is much larger
than what a primitive module should expose. The `#[unstable]`
server-side helpers above are the seam manutara will sit on top of;
they are explicitly carved out of the edition contract until
manutara's first design pass exercises them.

### encoding (pure, stage 2)

- `encoding.json` — `encode`, `decode` *(shipped — Real number parsing via #361 adds `JReal(Real)` alongside `JNum(Int)`; surrogate-pair `\uD8xx\uDCxx` decode + UTF-8 emit via #362)*
- `encoding.toml` — `decode`, `encode`, `round_trip` *(shipped subset for the package manager (#405): top-level scalars, `[name]` and `[[name]]` headers, basic strings, ints/bools, inline tables, comments. Encoder emits a canonical form so `kai.lock` round-trips byte-identical. Floats / datetimes / dotted keys / multi-line strings deferred until a user-facing TOML need lands.)*
- `encoding.utf8` — `validate`, `decode`, `encode`, `chars` *(planned — no `stdlib/encoding/utf8.kai` file)*
- `encoding.base64` — `encode`, `decode` (standard + URL-safe) *(shipped)*
- `encoding.hex` — `encode`, `decode` *(shipped)*

### regexp (pure, stage 2) — **landed 2026-04-28**

- `regexp` — `regex_compile`, `regex_match`, `regex_find_all`,
  `regex_replace`, `regex_split` (top-level module). Pre-m14
  flat-prefix names; rename to `regexp.match` etc. lands in m14.
- `matches`, `regex_compile_or_panic` — refinement-side helpers
  added 2026-05-03 to support the `~r/.../` sigil + `where matches
  ~r/.../` predicate (m12.6.x #7, closing issue #85). Both are
  marked refinement-pure so they compose with `Type where ...`
  declarations.

RE2-style deterministic engine. Linear-time set-of-states (Pike)
simulation; no backreferences, no lookaround, no Unicode property
classes (ASCII semantics only). Subset shipped:
  - Char literals + escapes (`\d \D \w \W \s \S` `.` `\n \t \r \\`).
  - Character classes `[abc]`, `[^abc]`, `[a-z]`.
  - Anchors `^` `$` (input-absolute, no multiline mode).
  - Greedy quantifiers `* + ? {n} {n,} {n,m}`.
  - Grouping `(...)` (numbered captures) and `(?:...)` (non-capturing).
  - Alternation `a|b`.
  - Replacement back-references `$0..$9`, `$$` for literal `$`.

End-to-end fixture: `examples/stdlib/regex_basic.kai` (38-line
golden covering match / captures / find_all / replace / split).
Validation: `make test-stdlib` passes; selfhost OK on both C and
LLVM backends.

### crypto (pure, stage 2 — shipped via PR #146)

- `crypto.hash` — `sha256`, `sha512` *(shipped)*. `blake3` and the legacy hashes (`sha1`, `md5`) *(planned — no tracking issue; deferred to S3)*.
- `crypto.mac` — `hmac_sha256`, `hmac_sha512` *(shipped)*

Symmetric and asymmetric ciphers deferred to post-MVP — the design
surface (key management, nonces, AEAD) is deep enough to warrant its
own doc, and most use cases through MVP are hashing and HMAC for auth
tokens.

### concurrent (`/ Spawn`, `/ Cancel`)

The `Actor[Msg]` effect is deferred to `docs/actors.md`; once that
spec lands, it is added to this header.

> **Physical layout note.** No `stdlib/concurrent/` directory exists. The catalog uses the `concurrent.*` namespace as the **target** shape; the actual `pub fn` lives at the top level today.

- `concurrent.nursery` — scoped `spawn` / `await` / `select` with cancel-on-failure *(shipped — `pub fn nursery[T, e]` lives at `stdlib/spawn.kai:95`, not at `stdlib/concurrent/nursery.kai`)*. Policy detailed in its own spec; restart policies live in `ahu.supervisor`.
- `concurrent.actor` — mailbox primitive: `send`, `receive`, `self` *(shipped — `stdlib/actor.kai`, not `stdlib/concurrent/actor.kai`)*. Spec detail lives in `docs/actors.md`.

### testing

- `testing` *(planned — no `stdlib/testing.kai` module file exists; the `test "..." { ... }` block syntax and the `assert` builtin are wired in the compiler/runtime, but a stdlib module exposing assertion helpers does not exist yet)* — assertions (`assert`, `assert_eq`, `assert_ne`, `assert_raises`) and integration with the `test "..." { ... }` block syntax and `kai test` subcommand. Effect: `/ Fail` for failing assertions (caught by the test runner handler).

## Naming conventions

- **Paths on disk**: slash-separated (`stdlib/concurrent/nursery.kai`).
- **Import paths in code**: dot-separated (`import net.http`).
- **Function calls**: `module.function` (`net.http.get(url)`). No
  package-level prefixes inside function names.
- **Migration from today's `list_*` / `string_*`**: those were a
  stopgap for kaikai-minimal, which lacks modules. **m6.2 —
  qualified calls** landed 2026-04-27, and the mechanical rename
  (`list_map` → `list.map`, `string_trim` → `string.trim`, etc.)
  ran under **m14** (#203, closed). The *Migration status* table
  above lists the per-module result. The handful of surviving
  flat aliases (16 total across `list` / `string` / `option` /
  `result`) are blocked on resolver gaps (#219 for typer
  EModCall, plus prelude-scope and string-interpolation gaps for
  the `list_*` survivors) and retire once those gaps close.
  Stage 1 code (kaikai-minimal) keeps the `list_*` / `string_*`
  prefixes — the migration only applies to stage 2 code where
  qualified module calls exist.
- **Type constructors**: PascalCase (`Some`, `None`, `Ok`, `Err`).
  Unchanged.
- **Type names**: PascalCase (`List`, `String`, `Option`, `Instant`,
  `Duration`). Unchanged.
- **Function and variable names**: `snake_case`. Unchanged.

## What stays out of stdlib (ahu territory)

For reference, so the frontier is explicit in both docs:

- `ahu.supervisor` — restart strategies (`one_for_one`,
  `one_for_all`, `rest_for_one`), backoff, max-restarts-in-window.
- `ahu.app` — application lifecycle, dependency graph between apps,
  graceful shutdown propagating `Cancel` from SIGTERM/SIGINT.
- `ahu.registry` — process naming (local and optionally global).
- `ahu.genserver` — GenServer-analog: stateful process with typed
  `handle_call` / `handle_cast` / `handle_info` callbacks on top of
  `concurrent.actor`.
- Observability (`ahu.log`, `ahu.trace`, `ahu.metrics`) — tentative;
  may land in `ahu` or stay out of the initial ahu scope. See Open
  Questions.

Not in stdlib and not in ahu either (external products, each with
their own repo and name from the Rapa Nui pool per
`kaikai-docs/framework-naming.md`):

- Web framework: HTTP server, router, middleware, templating, auth.
- Fintech toolkit: ledger, audit hash-chain, ISO 20022 bindings.
  (But the `decimal` and `money` top-level modules **are** stdlib —
  they are frontloaded primitives per the roadmap.)
- IA toolkit: LLM clients, agent orchestration, vector stores.

## Open questions

1. **`concurrent.channel`** (Go-style channels).
   - (a) Ship. Con: double model of communication with
     `concurrent.actor`; "which do I use" confusion.
   - (b) Don't ship. Users wanting a queue use `concurrent.actor`
     with a minimal mailbox-owner process.
   - *Tentative: (b), no channels in stdlib.*

2. **Observability (log/trace/metrics)**: stdlib or ahu?
   - (a) All in ahu. Pro: structured logging ties into supervisor
     trees naturally. Con: writing to stderr becomes awkward for a
     script that doesn't touch ahu.
   - (b) Minimal `log` in stdlib (stderr-backed, effect `Log` or
     reusing `Console`), structured `ahu.log`/`ahu.trace` on top.
   - (c) All in stdlib, with ahu only providing handlers.
   - *Tentative: (b).*

3. **`database/sql` interface** (Go-style abstract interface, drivers
   external).
   - *Deferred to post-MVP.* Too much design surface (connection
     pooling, transactions, prepared statements, cursor lifecycle)
     for the initial stdlib push. Reassess after ahu lands.

4. **HTTP/2 and HTTP/3** in `net.http`.
   - MVP: HTTP/1.1 only. H2 and H3 post-MVP, as modules over the
     same `NetTcp` (and `NetUdp` for QUIC) — no new effect needed.
     Flagged here so the API is shaped with that evolution in
     mind (no H1-specific types leaking into public surface).

5. **`testing` auto-import**. Tests are builtin syntax; should
   the `testing` module auto-import inside `test "..." { ... }` blocks?
   - *Tentative: yes, auto-imported only within test blocks.*

## Next steps

Once this doc is reviewed and pinned:

1. Extend `docs/effects-stdlib.md` (Doc B) with declarations for
   `NetTcp`, `NetUdp`, `NetDns`, `Clock`, `Random`, `SecureRandom`,
   `Process`, matching the existing per-effect format (Declaration
   / Default handler / Error model / Stdlib helpers).
2. ~~Move the current `stdlib/core.kai` monolith into the `core/*`
   subdirectory~~ — **landed 2026-04-27** as `stdlib/core/{list,
   string,option,result,char,tuple,io}.kai`. The follow-up
   rename of function names to module-relative form
   (`list.map`, `string.trim`, `option.map`, `result.and_then`,
   `char.is_digit`, …) **landed under m14** (#203, closed
   2026-05-04) across six PRs. See the *Migration status*
   table above for the per-module summary, and
   `docs/m14-bootstrap-audit.md` for the audit that reframed
   the milestone away from a bootstrap-split into a direct
   single-tree migration. See `docs/stage2-design.md` §m6 for
   the m6.1/m6.2 split and `docs/m6.2-design.md` for the
   qualified-call design.
3. Land one stage-2 module end-to-end (candidate: `time`) as the
   template for the rest. Drives whatever compiler plumbing the
   effect requires and validates the `Clock` handler contract before
   rolling out `Net` / `Random` / `Process`.
