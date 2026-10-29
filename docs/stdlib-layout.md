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
catalog, and lowering respectively. This doc sits on top of B: B
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

```kaikai
fn main() : Unit / Stdout = {
  let xs = [1, 2, 3]
  let doubled = list.map(xs, (x) => x * 2)
  let big     = list.filter(xs, (x) => x > 1)
  let parts   = string.split("a,b", ",")
  let n       = option.unwrap_or(None, 0)
  Stdout.print("#{doubled |> length()} #{big |> length()} #{parts |> length()} #{n}")
}
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
`each` — keep a flat core alias post-migration so common pipelines
read short (`xs |> map(f) |> filter(p)`). The aliases re-export
`list.map` / etc. The full set is reached only via `list.*`.

### Migration status (m14, #203)

| Module                  | Bare-name surface | Flat aliases removed | Flat aliases surviving | Reason for survivors                                              |
| ----------------------- | ----------------: | -------------------: | ---------------------: | ----------------------------------------------------------------- |
| `stdlib/core/list.kai`  |               29+ |                   29 |                      0 | none — final 5 retired in #227 after PR #218 closed the core-scope (#216) and string-interpolation (#217) resolver gaps |
| `stdlib/core/string.kai`|               13+ |                  ~14 |                      1 | `string_repeat` — typer EModCall name-only lookup (#219) collides with `list.repeat` |
| `stdlib/core/option.kai`|                10 |                    6 |                      4 | `opt_map`, `opt_filter`, `opt_zip` — typer EModCall (#219) collides with `list.{map,filter,zip}`; `opt_or` — `or` is a reserved keyword (`TkOr`) |
| `stdlib/core/result.kai`|                12 |                    6 |                      6 | `result_map`, `result_and_then`, `result_unwrap_or`, `result_or_else`, `result_unwrap_or_else`, `result_collect` — all #219 collisions with `list` / `option` exports |
| `stdlib/core/char.kai`  |                 8 |                    8 |                      0 | none — character-domain names are unique across `stdlib/core/`    |
| `stdlib/core/tuple.kai` |                 7 |                    7 |                      0 | helpers landed module-qualified only (`tuple.swap`, `tuple.map_fst`, `tuple.map_snd`, `tuple.map_pair`, `tuple.first`, `tuple.second`, `tuple.third`) — issue #348 |
| `stdlib/core/io.kai`    |               n/a |                  n/a |                    n/a | exports `pub fn println` and `pub fn ignore`; already module-relative shape |

**Issue #219** (typer EModCall lookup is name-only) is the gating
follow-up for the 11 `opt_*` / `result_*` survivors and the lone
`string_repeat`. Once the typer resolves `module.fname` against
the explicit module qualifier instead of doing a flat
`ty_env_lookup(fname)`, the remaining flat aliases can be deleted
in a mechanical follow-up. The `opt_or` survivor is independent
— it is blocked by parser-level keyword reservation, not by #219.

### Qualified surface (canonical-only)

Since the 2026-05-17 canonical-only migration, stdlib modules define
their operations under **bare canonical names** (`pub fn make`,
`pub fn parse`, `pub fn push`, …) and callers reach them qualified
(`decimal.make`, `date.parse`, `queue.push`) or bare where the call
site is unambiguous. Three mechanisms make this work:

- **The qualified-call resolver consults the module's actual
  exports** (`me_lookup_export` in `stage2/compiler/modules.kai`) —
  a verbatim lookup. The m14-era legacy-prefix fallback
  (`module_legacy_prefix` / `module_legacy_prefix_alt`) was removed
  when the migration landed (commits fe02f1e + 4ba4e01, 2026-05-17);
  there is no silent `module.fn` → `module_fn` rewriting anymore.
- **Codegen mints per-module C symbols** (`kai_<module>__<name>`;
  separate compilation, #748), so same-named exports across modules
  coexist at link time: `decimal.parse`, `rational.parse`, `uuid.parse`
  and `date.parse` are four distinct symbols.
- **Bare call sites disambiguate by argument type/arity** (the
  typer's first-arg narrowing). Names that are ambiguous bare or
  commonly shadowed by local defs (`get`, `push`, `min`, `decode`,
  a fixture's own `fn gcd`) should use the qualified form.

Surviving flat-prefix names are *compound* names, not legacy debt:
`time.kai`'s `duration_*` / `instant_*` / `walltime_*` (three
carriers in one module) and `protocols.kai`'s `bin_*` implementation
home (re-exposed bare by `stdlib/bin.kai`'s one-line wrappers).

The migration history — which batch renamed which module, and why
issue #614's Option E was first rejected (pre-#748 there was no
per-module symbol minting, so bare names genuinely collided) and
later implemented for real — is bitácora:
`docs/lane-experience-issue-614-m14-followup.md` plus the
canonical-only commit series of 2026-05-17. Those documents describe
the path, not the present; this section is the authoritative
description of how the qualified surface works today (#769).

The 5 `list_*` survivors documented in the m14 Phase 1 retro
(`docs/lane-experience-issue-203-phase1-list.md`) were retired
in #227 once PR #218 closed the core-scope (#216) and
string-interpolation (#217) resolver gaps that had blocked the
qualified form. `stdlib/core/list.kai` now has zero flat aliases.

Stage 0 PRELUDE builtins (`string_concat`, `string_length`,
`string_slice`, `string_split`, `string_contains`, `list_length`,
`list_reverse`, `char_at`, `char_to_int`, `int_to_char`, …) are
*below* the user-facing language and were never part of the m14
migration.

## Bootstrap layering

Modules are tagged by which compiler stage they require:

- **Stage 1 — bootstrap-safe.** Compiles with kaikai-minimal (no
  effects, no row polymorphism, no typeclasses, no `++` operator;
  parametric HM only). Only `core/*` lives here. This is the set
  that bootstraps with `kaic0` and is what `selfhost` exercises
  every CI run.
- **Stage 2 — full-kaikai.** Requires the full language (effects,
  row polymorphism, Perceus, monomorphisation, `++`). Everything
  else (`encoding/`, `collections/`, `decimal`, `loop`,
  `reader`, `writer`, `random`, `time`, `net/`, `crypto/`,
  `regexp`, `concurrent/`, `testing`).

The `stdlib/` directory does not reshuffle itself between stages; the
stage 1 driver (`kaic0` / `kaic1`) prepends only `core/*.kai` as
core modules, the stage 2 driver (`bin/kai`) prepends the full chain
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
  core chain.

The `make test-stdlib` target validates the full-kaikai layer
end-to-end (kaic2 + complete core chain). The bootstrap-safe
subset is validated indirectly by `make selfhost`, which compiles
`stage2/compiler.kai` through kaic1 with `core/*` as the only
core — if any `core/*.kai` file picks up a full-kaikai-only
construct, selfhost breaks.

> Why this split is intentional: kaikai-minimal is **the bootstrap
> language**, not "kaikai for beginners". Effects, row polymorphism,
> and the operator sugars (`++`, naked cell read, trailing lambdas) are
> full-kaikai by design — they cannot land in kaikai-minimal
> without breaking Tier 1 principle 3 ("fast compilation, LL(1)
> grammar with minor bookkeeping"). The right layer for those
> features is stage 2.

## Module tree

```
stdlib/
  core/          pure, auto-loaded
    list.kai
    string.kai
    char.kai
    char_unicode.kai  (simple case mappings beyond ASCII)
    option.kai
    result.kai
    tuple.kai
    io.kai       (I/O helpers and the explicit discard; effects Console, Stdin)
    kinds.kai    (the kind/theory catalog: named unification theories and the kinds they classify)
    runtime.kai  (collection and mailbox primitives the runtime implements)
  protocols.kai  single-dispatch protocols (Show, Eq, Ord, Hash, Serialize, BinSerialize, Layout, Json, Default, Add/Sub/Mul/Div/Rem, Numeric) and their impls for the primitives
  effects.kai    the atomic effect catalog (builtin capabilities with runtime default handlers)
  effects/
    concurrent.kai  State / Reader / Writer and Spawn / Cancel / Actor / Link / Monitor
    os.kai          Signal and Process, with Sig / Child / Exit
  collections/   pure
    map.kai      (AVL-tree ordered map)
    set.kai      (AVL-backed Set[a], O(log n))
    hashmap.kai  (mutable hash table, Mutable effect)
    hashset.kai  (mutable hash set over HashMap[t, Unit], Mutable effect)
    vec.kai      (Vec[a], the pure value vector over one flat buffer)
    queue.kai    (amortised O(1) two-list FIFO queue)
    stack.kai    (LIFO stack over [a])
    convert.kai  (conversions between Map, Set and lists)
  math/          pure
    int.kai      (`log2`, `div_mod`; `abs`, `signum`, `clamp`, `pow` come from `Numeric for Int`)
    real.kai     (libm bindings; `math.float` does not exist)
    bits.kai     (intrinsic bit ops)
    numeric.kai
    complex.kai  (Complex record; + - * / through the operator protocols)
    linalg.kai   (shape-indexed `Vec[t]<n>` ops + `Matrix[t]<m, n>` over the `Dim` kind)
    bigint.kai   (arbitrary-precision integers; internal helper modules bigint_limbs, _karatsuba, _division, _shift, _pow10, _parse, _render)
  decimal.kai    pure (fixed-scale decimal)
  decimal_big.kai pure (BigInt carrier, no scale ceiling)
  rational.kai   pure (exact num/den over BigInt, gcd-normalised)
  money.kai      pure (carrier-generic `Money[t]<c: Currency>` over the `Module` kind)
  loop.kai       row-polymorphic while, until, repeat, forever
  gen.kai        effect Yield[t]: pull generators on a kept continuation
  seq.kai        lazy sequences over a memoised runtime cell
  stream.kai     lazy streams as generators over the Emit[t] effect
  reader.kai     with_reader for Reader[T]
  writer.kai     with_writer for Writer[W]
  ffi.kai        Userdata[T]: a kaikai value parked behind a void * for C callbacks (under Ffi)
  array.kai      [T] / Array[T] bridge, plus `each` for `|!`
  string_builder.kai  amortised text accumulator; `append` rides Mutable, `build` is pure
  text.kai       Unicode text measurement (terminal-column width)
  text/
    format_spec.kai  fixed-point rendering behind `#{x:.N}` and the interpolation pad specs
  bin.kai        binary encoding helpers (byte/int/list/option)
  uuid.kai       RFC 4122 UUID v4 generator + parser
  path.kai       POSIX path manipulation, auto-loaded (path_join, path_dirname, …)
  fs/            effect File
    file.kai     (read, write, append, exists, delete, rename, read_bytes, write_bytes, metadata)
    dir.kai      (list_dir, create_dir, remove_dir, walk, current, temp_dir)
    path.kai     (the `fs.path` qualifier over the auto-loaded `path` helpers)
  os/            effects Env, Process
    env.kai
    args.kai
    process.kai  (start, wait, kill, exit, the pipe family, process groups, uid)
  time.kai       effect Clock
  date.kai       pure civil calendar; `today()` rides Clock
  date/
    rfc3339.kai  (format, parse: RFC 3339 `date-time` text for `WallTime`)
  random.kai     effect Random
  random_secure.kai  effect SecureRandom
  log.kai        effect Log
  trace.kai      effect Trace: ad-hoc structured logging
  spawn.kai      wrappers over the Spawn ops, including `nursery`
  actor.kai      typed wrappers and helpers for Actor[Msg] (`with_mailbox`)
  net/           effects NetTcp, NetUdp, NetDns, NetUnix
    tcp.kai      (fiber-parking on the reactor)
    udp.kai      (datagram UDP, IPv4)
    dns.kai      (resolve, resolve_first, with_dns)
    unix.kai     (Unix-domain stream sockets)
    url.kai      (RFC 3986 URI references; pure)
    http.kai     (client + minimal server helpers; its own lenient parse_url)
  encoding/      pure
    json.kai     (json_encode, json_decode, json_decode_report)
    json_bind.kai (the JSON DOM + the `#[derive(Json)]` runtime)
    json_lex.kai (internal helper module of encoding.json)
    toml.kai     (the manifest/lockfile TOML subset)
    toml_lex.kai (internal helper module of encoding.toml)
    toml_tree.kai (internal helper module of encoding.toml)
    utf8.kai     (decode, decode_lossy, is_valid, encode over [Byte])
    base64.kai
    hex.kai
  regexp.kai     pure
  regexp/        syntax.kai (AST + parser), engine.kai (NFA builder)
  crypto/        pure
    hash.kai     sha256, sha512
    legacy.kai   sha1, md5 (interoperability only; broken for security)
    mac.kai      hmac_sha256, hmac_sha512
  web/           browser target only; `web.run` installs the effects
    run.kai      (the installer: answers Dom, Timer, Fetch, Storage with the page)
    dom.kai      (effect Dom)
    timer.kai    (effect Timer)
    fetch.kai    (effect Fetch)
    storage.kai  (effect Storage over localStorage)
    wait.kai     (park a fiber until the host has something for it)
```

## Per-module summary

Terse — one line per module, listing declared effects. Full signatures
land in each module's own spec when implemented.

### core (pure, stage 1)

- `core.list` — map, filter, foldl, foldr, foreach, length, reverse, nth, take, drop, zip, unzip, concat, any, all, find, index_of, sum, product; surface expansion (#340 — shipped): `last`, `init`, `partition`, `split_at`, `span`, `chunk`, `windows`, `intersperse`, `enumerate`, `zip3`, `scan`, `group_by`, `find_map`. The aggregates are protocol-bounded generics (#891, on the #890 free-fn bound feature): `sum`/`product` are `[T : Numeric + Add]` / `[T : Numeric + Mul]` (folding `add`/`mul` from `zero()`/`one()`), `max`/`min`/`sort` are `[T : Ord]` (via `cmp`), `uniq` is `[T : Eq]` (via `eq`). `max`/`min` keep the `Option[T]` wrapper so the empty list stays a total `None`. `group_by` uses Erlang/Elixir consecutive-key semantics (NOT Haskell's all-equal-regardless-of-position) — the consecutive variant stays linear without an `Eq` map; its key type is still `Int` for v1 (it carries no `: Eq` bound), unlike `uniq` which now does. `map_foldl` (#1134) folds `combine` over `stage(x)` in one pass — the fusion rewrite emits it for a pure `xs | stage |> foldl(...)`, materialising no mapped list. `map_sum` / `map_product` (#1143) are the ring-terminal analogues (`[b : Numeric + Add]` / `+ Mul`, pure stage), emitted for `xs | stage |> sum` / `|> product`; the range-headed targets live in `protocols` (`range_map_foldl` / `range_map_sum` / `range_map_product` + `range_step_*` variants, and `range_length` / `range_step_length` for the `length` terminal, which drops the pure stages entirely).
- `core.string` — length, slice, to_int, concat, starts_with, ends_with, trim, repeat, join *(originals)*; split, replace, pad_left, pad_right, lines, is_blank *(shipped via #338)*; **index_of, to_upper, to_lower, is_empty, reverse** *(shipped via #396 — the remainder of #338's proposed surface)*; drop_prefix, drop_suffix *(shipped via #632)*; **bytes, chars, char_count, byte_length, char_indices + `CharIndex` record** *(shipped via #744 — the codepoint model)*; **trim_left, trim_right, from_chars** *(shipped — closes #1589: one-sided trim + the `chars` inverse)*; **from_bytes + `bytes` retyped to `[Byte]`** *(shipped — closes #1978: the byte view wears the byte type)*; **`slice` re-based on codepoints + byte_slice, is_char_boundary, floor_char_boundary, ceil_char_boundary, char_index_of** *(shipped — closes #1982: the index unit is chosen, not implicit)*; **string_find** *(shipped — closes #2125: a builtin core declares with `extern "kai"`, the byte offset or -1 that `index_of` wraps in `Option`)*. **to_int_radix, int_to_string_radix** *(shipped — refs #2823: integers in radix 2..36; `to_int` stays base 10)*. **contains, count, last_index_of, char_last_index_of, split_once** *(shipped — search family: `contains` is `string_contains`; `count` is non-overlapping and `count(s, "")` is `char_count(s) + 1`; `last_index_of` returns a byte offset (pairs with `slice` / `byte_slice`, overlaps allowed, `""` gives `Some(length(s))`) and `char_last_index_of` a codepoint index (pairs with `char_slice`); `split_once` cuts at the first separator, `split_once(s, "")` is `Some(("", s))`)*. `text.format_spec` *(shipped)* — `fixed(x, digits)`, `pad_center`, `pad_zero`, behind the `#{x:.2}` / `#{s:^10}` / `#{n:05}` interpolation format specs; `fixed` is the exact printf-style `%.*f` behind `#{x:.2}`; `math.real.to_fixed` delegates to it. `length`/`byte_length` count bytes; `char_count` counts Unicode codepoints; `bytes()` is the byte-level `[Byte]`, `chars()` the codepoint-level `[Char]` (UTF-8 decode) — distinct element types, so `from_chars(bytes(s))` is a type error instead of a silent double-encode; `from_bytes` is the `bytes` inverse, `from_chars` the `chars` one. Two index units, split by name: the `char_`-prefixed `char_slice` / `char_index_of` / `char_last_index_of` / `char_count` address codepoints; `slice` / `char_at` / `index_of` / `length` / `byte_length` and the `byte_`-prefixed names address bytes. `slice(s, from, len)` takes byte offsets and clamps out of range, so an offset from `index_of` always slices correctly; it panics when an end falls inside a multibyte sequence. `char_slice(s, from, len)` is the codepoint-indexed cut and never splits a character. `byte_slice(s, from, len) : Option[String]` is the total byte-indexed cut, `None` when either end falls inside a multibyte sequence or outside the buffer. `is_char_boundary` / `floor_char_boundary` / `ceil_char_boundary` classify and snap an arbitrary byte offset (Rust 1.91's truncate-to-N-bytes primitives). The search pair stays coherent by shipping both halves: `index_of` keeps its byte offset and pairs with `slice` / `byte_slice`; `char_index_of` returns a codepoint index and pairs with `char_slice`. `reverse`/`to_upper`/`to_lower` operate by codepoint. `split(s, "")` panics; `lines("")` returns `[]` (Python/Rust convention). `drop_prefix` / `drop_suffix` return `Option[String]` (`Some(rest)` on match, `None` otherwise) — same shape as `to_int`, `log2`, `div_mod`, mirroring Rust's `str::strip_prefix`. The #396 five share bare names with core exports (`list.is_empty`, `char.to_upper`, …); first-arg-type narrowing (#235) routes the `String` receiver. (A bare call inline in a comparison still mis-narrows cross-module — issue #758 — so the intrinsic tests bind with `let` before asserting.)
- `core.option` — is_some, is_none, map, flat_map, each, filter, and_then, unwrap_or, or_else *(shipped: `map` / `flat_map` / `filter` / `each` make `Option` ride `|`, `||`, `|?`, `|!`)*
- `core.result` — is_ok, is_err, map, flat_map, each, map_err, and_then, unwrap_or *(shipped: `map` / `flat_map` / `each` make `Result` ride `|`, `||`, `|!`; no `filter`)*
- `core.char` — is_digit, is_lower, is_upper, is_alpha, is_alnum, is_space, to_lower, to_upper. ASCII-only classification and case mapping.
- `core.char_unicode` — is_lower, is_upper, to_lower, to_upper. Simple, locale-independent case mappings for ASCII, Latin U+00C0–U+017F, basic Greek U+0391–U+03A9 / U+03B1–U+03C9, and Cyrillic U+0400–U+045F. Latin Extended-A includes the alternating pair ranges and exceptional mappings (`İ` → `i`, `ı` → `I`, `ſ` → `S`, `Ÿ` ↔ `ÿ`). Multi-character mappings and contextual casing are excluded; `ß` and `ŉ` stay unchanged on uppercase. `string.to_lower_unicode` / `string.to_upper_unicode` apply these mappings to strings.
- `core.tuple` — `Pair`/`Triple`/`Quad` types; helpers `swap`, `map_fst`, `map_snd`, `map_pair` (Pair) and `first`/`second`/`third` (Triple) (shipped via #348). `fst`/`snd` projections are field-access only (`p.fst`) — see module header for the function-vs-field shadowing rationale.
- `core.ordering` — `Ordering` type, chain, reverse
- Declared runtime builtins *(shipped)* — each is an `extern "kai"` fn with a `#[doc]` in its home core module, found bare everywhere; `tools/kai-builtins.txt` lists them. `math/numeric`: `int_to_string`, `real_to_string`, the `real_*` math functions, `int_to_byte`, `byte_*` and the `int32`/`uint32`/`uint64`/`int128` conversions. `core/string`: `string_length`, `string_concat`, `string_concat_all`, `string_join`, `string_to_int`, `string_to_real`, `char_at`, `string_split`, `string_contains`, `string_slice`, `string_reverse`, `string_hash`, `string_byte_at_int`, `string_cp_at`, `string_cp_len`, `int_to_byte_string`, `int_to_le4`, `int_to_le8`, `real_bits`, `bytes_get_le`, `bytes_get_string`. `core/char`: `char_to_int`, `int_to_char`. `core/list`: `list_length`, `list_append`, `list_reverse`. `effects`: `abspath`, `__file_append`, `__file_exists`, `__file_delete`, `__file_rename`, `__file_read_bytes`, `__file_write_bytes` (the implementation of `fs.file`; user code reaches them as `file.append`, ...) and the `dir_*` four. `core/runtime`: `array_empty`, `array_same`, `vec_make`/`vec_empty`/`vec_reserve`/`vec_tail_from`, `mailbox_send`/`recv`/`recv_timeout`/`free`/`assign_owner`, `reduce` and `__deep_apply1`/`2`. `core/list`: `map`, `filter`, `each`, `flat_map` (one definition each, the runtime's). `effects`: `print`, `eprint`, `write_stdout`, `args`, `program_name`, `stdlib_path`, `__file_read`, `__file_write`, `read_line`, `read_bytes`. Still compiler-table rows (`stage2/compiler/builtins.kai`): the intrinsics that lower in place (`unit_name`, the `__*_unit` coercions, `bit_*`); the builtins passes key by id (`array_make`/`get`/`take`/`set`/`length`/`grow`, `vec_get`/`length`/`set`/`push`/`slice`/`from_list`, `panic`, `exit`, `spawn_actor_fiber`, `int_to_real`/`real_to_int`, the `bytes_*` writers); and the four `mailbox_alloc*`, whose `Pid` result a declared signature cannot return past the actor-scope escape check.

### collections (pure, stage 2)

- `collections.map` — AVL-tree-backed ordered map (canonical qualified-call style as of #613): `empty`, `size`, `is_empty`, `get`, `contains`, `put`, `remove`, `keys`, `values`, `to_pairs`, `from`, `update`, `foldl` (`fold` is a deprecated alias), `merge`, `filter`, `transform_values`, `each` (17 pub fns); `map`, `flat_map`, `filter` exported with `Pair[k, v]` element shape so `Map[k, v]` participates in `|`, `||`, `|?` per #594. `each` *(shipped)* visits the same `Pair[k, v]` elements in ascending key order, the order `map` visits them. Legacy flat-prefix aliases (`map_empty`, `map_size`, `map_is_empty`, `map_get`, `map_contains`, `map_put`, `map_remove`, `map_to_pairs`, `map_keys`, `map_values`, `map_from_pairs` — 11 one-liners) retained for the tongariki edition and scheduled to drop at the Orongo edition boundary.
- `collections.hashmap` — **mutable** hash table (separate chaining) behind the `Mutable` effect, shipped via #374. The default associative collection for 1.0: ~2-3× faster than the AVL `Map` on build and lookup at N≥10K (see `benchmarks/hashmap/`). Short module-relative surface (same convention as `map`/`set`): `empty`, `size`, `is_empty`, `get`, `contains`, `put`, `remove`, `keys`, `values`, `to_pairs`, `from`, `merge`, `each` — called `hashmap.put(m, k, v)` etc. (13 pub fns; no `hashmap_*` aliases); `each` *(shipped)* visits `Pair[k, v]` in `to_pairs` order and carries `/ Mutable + e`; `map`, `flat_map`, `filter` exported with `Pair[k, v]` element shape so `HashMap[k, v]` participates in `|`, `||`, `|?` (shipped via #876), mirroring `Map`. The pipe combinators carry `/ Mutable` (they read the table). **Mutating, not persistent** — `put`/`remove` mutate in place and return `Unit`; every op carries `/ Mutable`. The carrier was redesigned from the issue body's pure HAMT (which benchmarked ~2× SLOWER than the AVL `Map`) to a mutable table; the signatures diverge from the issue's pure shapes accordingly (see lane retro). Bucket array + count + capacity in `Ref` cells; resize doubles at load factor 0.75. Iteration order unspecified (bucket-derived) but deterministic for a given insertion sequence. Keys need `impl Hash` + `==`; primitives and user **sum types** dispatch through the generic boundary, user **records** do not yet (compiler dispatch gap, see lane retro). The `m[key]` read-side sugar dispatches to `hashmap.get` (returns `Option[v]` / `Mutable`) via the typer's `synth_index`, same mechanism `Map` uses. No HashDoS mitigation in v1.
- `collections.set` — AVL-backed insertion-ordered set (O(log n) membership/insert/remove, O(n log n) build; relinearized from the original list carrier via #936): empty, insert, remove, contains, union, intersect, diff, size, is_empty, to_list, from, `each` *(shipped — insertion order, like `map`)*; `map`, `flat_map`, `filter` exported with element shape `a` so `Set[a]` participates in `|`, `||`, `|?` (shipped via #876). `map` collapses output collisions. Carrier is `{ index: Map[a, Int], next: Int }` — the inner AVL `Map` maps each member to its insertion sequence number, so `to_list` sorts by sequence to replay insertion order while membership rides the tree. Elements must be `<`-comparable (`Int`/`Real`/`Char`/`String` + sum types deriving `Ord`); records panic on comparison, like `Map` keys. `union(a, b)` / `intersect` / `diff` keep `a`'s order.
- `collections.hashset` — **mutable** hash set, a thin wrapper over `HashMap[t, Unit]` (each member is a key whose value is the single `Unit` value), shipped via #375. The O(1)-average set, sibling to `HashMap` exactly as the list-backed `Set` is the sibling of the AVL `Map`. Short module-relative surface (same convention as `hashmap`/`set`): `empty`, `size`, `is_empty`, `contains`, `add`, `remove`, `to_list`, `from`, `union`, `intersection`, `difference`, `is_subset`, `each` — called `hashset.add(s, x)` etc. (13 pub fns; no `hashset_*` aliases); `each` *(shipped)* visits members in `to_list` order and carries `/ Mutable + e`; `map`, `flat_map`, `filter` exported with element shape `t` so `HashSet[t]` participates in `|`, `||`, `|?` (shipped via #876), mirroring `Set`. The pipe combinators carry `/ Mutable` (they read the table); `map` collapses output collisions. **Mutating, not persistent** — `add`/`remove` mutate in place and return `Unit`; every op carries `/ Mutable`. The set-algebra ops (`union`/`intersection`/`difference`) build and return a FRESH set, leaving both arguments untouched. Carrier is `{ inner: HashMap[t, Unit] }`; every op delegates to `collections.hashmap`, inheriting its performance, mutation model, and effect discipline. Iteration order unspecified (bucket-derived). Elements need `impl Hash` + `==`; primitives and user **sum types** dispatch through the generic boundary, user **records** do not yet (HashMap's compiler dispatch gap, inherited — see lane retro). NOTE: the issue #375 body is stale — it assumed a *persistent* HashMap and a `Hashable` protocol, neither of which shipped; this module mirrors the as-shipped mutable HashMap and the `Hash` protocol.
- `collections.stack` — pure LIFO stack over `[a]`: empty, size, is_empty, push, pop, peek, to_list, from_list, `each` *(shipped — top → bottom)*; `map`, `flat_map`, `filter` exported with element shape `a` so `Stack[a]` participates in `|`, `||`, `|?` (shipped via #876), preserving top → bottom order. `pop`/`peek` return `Option`.
- `collections.queue` — pure amortised-O(1) FIFO queue (Okasaki two-list): empty, size, is_empty, push, pop, peek, to_list, from_list, `each` *(shipped — head → tail)*; `map`, `flat_map`, `filter` exported with element shape `a` so `Queue[a]` participates in `|`, `||`, `|?` (shipped via #876), preserving head → tail (FIFO) order. `pop`/`peek` return `Option`.
- `collections.vec` — pure **value** vector over one flat contiguous buffer (runtime `KAI_VEC`, shipped via #1135 stage 1): `make`, `empty`, `reserve`, `from_list`, `length`, `is_empty`, `get`, `slice`, `set`, `push`, `map`, `flat_map`, `filter`, `foldl`, `each` *(shipped — index order)*, `to_list` (16 pub fns); `map`, `flat_map`, `filter` and `each` make `Vec` participate in `|`, `||`, `|?` and `|!`. Every op is effect-free — `set`/`push` return the resulting vector; the runtime mutates in place when the buffer is uniquely owned (rc == 1, the Perceus reuse check) and copies on write when shared. Element storage is unboxed for scalars (`Int`/`Real`/`Bool`/`Char`/`Byte`, raw 8-byte payloads) and small all-scalar records (≤ 8 fields, inlined at `n_fields * 8` B/elem — ~16 B/elem for a 2-field record vs ~84 B/elem on boxed `Array`); anything else falls back to boxed storage under the same CoW discipline. Contrast `Array` (mutable reference, `/ Mutable`). The stage-3 surface (shipped via #1150): `[h, ...t]` match arms and `xs[a..b]` bind O(1) slice VIEWS (offset + length over the shared buffer; a live slice pins the buffer and disables in-place writes until it dies); a list literal in `Vec`-typed context (annotation or argument position) builds a pre-sized vec (bare literals stay cons lists); a pure `Vec`-typed range pipe chain collects into one pre-sized buffer (`protocols.range_map_collect_vec`), other chains convert once via `vec_from_list`.
- `collections.convert` — immutable <-> mutable bridge (#1015 L1): 4 `to_X` conversions — `to_hashmap` (Map → HashMap), `to_map` (HashMap → Map), `to_hashset` (Set → HashSet), `to_set` (HashSet → Set). Each reuses `to_pairs`/`to_list` + the destination's `from`; the `Mutable` crossing is local and visible at the call (`m |> to_hashmap()`). Lives in its own module because a bare `import collections.map` inside `hashmap` would shadow `hashmap`'s own `pub fn map` pipe export, and one-direction-per-module would force the two collection modules to import each other (a rejected cycle). `import collections.convert` to bring the conversions into scope.

### math (pure, stage 2)

- `math.numeric` — the `Numeric` protocol (`stdlib/math/numeric.kai`), dispatched single-arg: `zero`, `one`, `abs`, `sign`, `pow_int`, `clamp`; `impl Numeric for Int` / `for Real` (and `for Decimal` in `decimal.kai`). A ring is `Numeric + Add + Mul`: `add`/`mul` are the operator protocols' operations, declared once. `list.sum` / `list.product` ask for that ring. The typer does not verify the ring axioms — implementor's contract.
- `math.int` — `min`, `max`, `gcd`, `lcm`, `factorial`, `fib`, `is_prime`, `log2`, `div_mod` *(all shipped; `log2` and `div_mod` via #347)*. `abs`, `signum`, `clamp`, `pow` are NOT here — they dispatch through `Numeric for Int` (`stdlib/math/numeric.kai`); `signum` is `Numeric.sign` and `pow` is `Numeric.pow_int`. Coverage for the four Numeric ops over Int lives in `examples/stdlib/math_int_basic.kai`. Bit ops (`shl`, `shr`, `and`, `or`, `xor`, `not`, `popcount`, `leading_zeros`, `trailing_zeros`) are intrinsic — they live in `math/bits.kai`, NOT in `math/int.kai`.
- `math.real` *(shipped — was `math.float` in earlier drafts; the module file is `stdlib/math/real.kai` and the namespace is `math.real`. `math.float` does not exist.)* — `min`, `max`, `floor`, `ceil`, `round`, `round_half_even`, `trunc` *(shipped; generic over `Real<u>`)*, `round_to(x, decimals)` *(shipped)*, `to_fixed(x, digits)` *(shipped; half-even)*; libm bindings — `sqrt`, `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `exp`, `log`, `log2`, `log10`, `pow` — *(shipped via PR #359, closes #343)*; `rem` (`fmod`) wires `impl Rem for Real` so `r1 % r2` type-checks — *(shipped, closes #364)*. NaN and infinity tests are the flat `real_is_nan` / `real_is_inf` *(shipped — declared in `math.numeric`)*.
- `math.linalg` — shape-indexed linear algebra over the `Dim` kind *(shipped — closes #1271)*. `Matrix[t]<m, n>` carries two positional `Dim` habitants; `Vec[Real]<n>` ops (`dot`) and matrix ops (`of`, `rows`, `cols`, `at`, `matmul`, `matvec`, `transpose`) state their shape rules in signatures, checked at compile time and erased at runtime. Fixed-width inline representation for `Vec[t]<n>` is the codegen follow-up.
- `math.bigint` — arbitrary-precision signed integers, pure kaikai, no GMP *(shipped — numeric lane B)*. Sign-magnitude carrier with an inline `Int`-range fast path (`Small(Int) | Big(sign, Array[Int])`); promotes on overflow, never overflows. `from_int` / `from_string` / `to_int`, `add` / `sub` / `mul` / `compare`, `neg` / `abs` / `sign`, `to_string`, and `divmod` / `div` / `rem`. The `n` literal suffix (`99n`) desugars to `bigint.from_int`. `Show` / `Serialize` / `Eq` / `Ord` / `Hash` and the total operators `Add` / `Sub` / `Mul` / `Neg` (infix `+ - *`, unary `-`) in the same module; division stays the named `div` / `divmod` (truncating). Magnitude limb arithmetic in `bigint_limbs`; multiplication is Karatsuba above a limb threshold, division is Knuth below and Burnikel–Ziegler above one, and decimal conversion splits at cached powers of ten above a size threshold, so `mul`, `from_string` and `to_string` scale subquadratically.

- `decimal_big` — arbitrary-precision fixed-point with a `BigInt` carrier *(shipped — numeric lane D, closes #514)*. `{ raw: BigInt, scale: Int }`, value `raw / 10^scale`. Unlike `decimal` (Int128 carrier, ~38-digit ceiling), it has no width or scale limit: `add` / `sub` / `mul` are total; `div` takes an explicit truncating target scale (`None` on a zero divisor). `from_int` / `from_big` / `from_parts` / `parse`, `to_string`, `eq` / `cmp` (scale-independent). `Show` / `Eq` / `Ord` and the total operators `Add` / `Sub` / `Mul` / `Neg` (infix `+ - *`, unary `-`) in the same module; `div` stays named (explicit target scale).

- `rational` — exact rationals over `BigInt` *(shipped — numeric lane D, closes #514)*. `{ num: BigInt, den: BigInt }` kept canonical (positive denominator, sign in the numerator, reduced by `gcd` at every construction). `make` / `from_int` / `from_big` / `parse`, `add` / `sub` / `mul` / `div` / `recip` / `neg` / `abs`, `numerator` / `denominator`, `cmp` / `eq`, `to_string` (`num/den`). `gcd` is the Euclidean algorithm over `BigInt`. `Show` / `Eq` / `Ord` and the total operators `Add` / `Sub` / `Mul` / `Neg` (infix `+ - *`, unary `-`) in the same module; `div` stays named (`Option` on a zero divisor).

### decimal (pure, stage 2)

- `decimal` — arbitrary-precision decimal arithmetic with rounding
  modes (top-level module: `decimal.add`, `decimal.from_string`, …).
  `dec_div(a, b, target_scale)` returns `Option[Decimal]`: `None` on
  division by zero, `Some(q)` otherwise *(closes #363)*.
  The `d` literal suffix writes a `Decimal` (`19.99d`); the type, the
  suffix and the functions all take `import decimal`.

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

- `array.from[a](xs: [a]) : Array[a]` — copy a linked list into a
  fresh Array. No caller sentinel: the non-empty case seeds
  `array_make` with the head element and overwrites every slot;
  the empty case routes through `array_empty`.
- `array_to_list[a](a: Array[a]) : [a]` — copy an Array into a
  linked list in index order. Pure read; no `Mutable` demand.
- `array.copy[a](src: Array[a]) : Array[a]` — element copy. Result
  is a distinct allocation; mutating either side does not affect
  the other. Seeds from the source's first element; empty source
  returns a length-0 array.
- `array.each[a, e](xs: Array[a], f: (a) -> Unit / e) : Unit / e` — runs `f` in index order; pure read, so `Array` participates in `|!`. `map` / `flat_map` / `filter` are not exported: pipe over `array.to_list(a)` *(shipped)*.
- `array.same[t](a: Array[t], b: Array[t]) : Bool` *(shipped)* — identity: true only for the same array. `==` on two arrays compares contents.

The empty case is served by the `array_empty[a]() : Array[a]`
core prim: `array_make` always demands an `init: T` to reify the
element slot even at `n == 0`, which a polymorphic empty input has
no element to supply, so `array_empty` builds the length-0 array
directly (`kai_array_make(0, NULL)` — the fill loop never runs).
It is effect-free: a length-0 array writes no observable slot.

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

Used with the double-trailing-lambda sugar for the natural Koka
feel:

```kaikai
import loop

fn process_one() : Bool = true

fn main() : Unit / Stdout = {
  var i := 3
  var done := false
  while  { i > 0 }  { i := i - 1; Stdout.print("#{i}") }
  until  { done }   { done := process_one() }
  repeat(10)        { Stdout.print("hi") }
  if i > 0 { forever { Stdout.print("tick") } }
}
```

The module is not auto-imported: `import loop` brings the bare names
(`while`, `until`, `repeat`, `forever`) into scope.

`while`, `until`, `repeat`, and `forever` are deliberately
**not** language keywords — they are stdlib functions, callable
like any other. A user can shadow them by binding the same name
locally; the compiler does not protect them. This is consistent
with the Tier 2 principle "few visible concepts, layered" and
with Doc CLAUDE.md's stance that ordinary stdlib functions are
preferable to language-level control structures when the row
machinery suffices.

### seq (pure, stage 2)

Lazy sequences *(shipped via PR #2726)*. A `Seq[t]` is a cell the
runtime owns: its first `force` runs the thunk it was built with and
every later read returns the same `SeqStep[t] = SeqEnd | SeqCons(t,
Seq[t])`, so a sequence bound to a name and read twice is computed
once. A function a stage keeps must be pure; the sinks may perform
effects. A `Seq` stays on the fiber that made it. Unlike a `Gen` it is
shared, and unlike a `Stream` it is not rerun. A `const` whose type
holds a `Seq` is evaluated once per fiber *(shipped via PR #2731)*.

- cell: `defer`, `delay`, `force` *(shipped)*
- sources: `from`, `from_step`, `range` *(shipped)*
- stages: `map`, `flat_map`, `filter` (pipe-canonical), `take`,
  `take_while`, `drop` *(shipped)*
- sinks: `nth`, `to_list`, `each`, `foldl` *(shipped)*
- surface: the `lazy [...]` literal builds one, and the list patterns
  `[]` / `[first, ...rest]` read one *(shipped)* — `kai info seq`

### gen (effect `Yield[t]`, stage 2)

Pull generators built on the `Yield[t]` effect *(shipped via PR #2627)*.
A producer is an ordinary function performing `Yield.yield(v)`;
`generate` handles it by keeping the continuation, so each element
comes out as `Next(v, k)` of `Gen[t, e] = Done | Next(t, Cont[Unit, Gen[t, e], e])`
and `k(())` runs the producer to its next element on its own stack
segment. The producer may perform other effects, `e`, served by the
handlers wherever the generator is stepped *(shipped: effectful
producers)*. A dropped `Gen` discontinues its producer, running its
`finally` blocks where it is dropped. The module uses
nothing a user module cannot write (`kai info effects`, *Build your own
generator*).

- source: `generate`
- stages: `map` / `flat_map` / `filter` (pipe-canonical: `|`, `||`, `|?`), `zip_with`, `take_until`
- sinks: `each`, `each_indexed`, `reduce : Option[t]` *(`each` shipped)*

The producer and the functions the stages run inside it are pure; the
sinks run in the consumer and carry its row.

### io (`/ Console + Stdin`)

Single top-level module. Per-function effects: `print` / `println` /
`eprint` / `eprintln` declare `/ Console`; `read_line` / `read_all`
declare `/ Stdin`.

- `io` — `print`, `println`, `ignore` (explicit discard), `eprint`, `eprintln`, `read_line`, `read_all`, `read_bytes(n)` *(byte-oriented stdin, shipped via #453 as the `read_bytes` core builtin + `Stdin.read_bytes` effect op; LSP framing precursor for #447)*
- TTY detection — `Stdout.is_tty()` / `Stderr.is_tty()` / `Stdin.is_tty()` effect ops bridging to `isatty(3)` on fd 1/2/0 *(shipped via #1590; the ANSI-colour gate, pairs with the `NO_COLOR` override via `Env.get`)*

### fs (`/ File`)

- `fs.file` — `read`, `write`, `append` *(shipped via PR #132; return `Result[String, String]` / `Result[Unit, String]` instead of panicking, #3196)*; `exists`, `delete`, `rename` *(shipped; the one public spelling is `file.exists` / `file.delete` / `file.rename`, and the bare core names no longer exist)*; `read_bytes`, `write_bytes`, `metadata` *(shipped; `metadata` via #3162 returns `FileMetadata { size, modified, is_dir, is_file }`, following symlinks)*
- `File` chunked / streaming ops — `open_read`, `read_chunk`, `open_write`, `write_chunk`, `close_file` *(shipped via PR #804, issue #771 Phase 1: five `File` effect ops + `FileHandle` opaque handle + R1-pool default handlers; `read_chunk` returns `Ok("")` at EOF. Typed capabilities via the `Perm` kind shipped with #1251: `open_read` mints `FileHandle<read>`, `open_write` mints `FileHandle<read + write>` (fd opened `O_RDWR`), `read_chunk`/`write_chunk` require `<read>`/`<write>`, `close_file` takes a bare handle; `file_handle(fd)` mints for handlers/tests.)*
- `stream` — lazy push streams over the chunked `File` ops *(shipped via issue #801: `stdlib/stream.kai`. Carrier `Stream[t, e]` (single-ctor variant); sources `from_list` / `read_lines`; pipe-canonical stages `map` / `flat_map` / `filter` / `take` / `take_while`; sinks `foldl` (`fold` deprecated alias) / `each` / `count` / `to_list` / `write_lines`. Recoverable faults ride the `ReadFault` effect — `bad_chunk` resumable (skip), `open_fault : Nothing` abort-only. `import stream`. Supersedes #771 Phase 2's `with_lines` / `fold_lines` / `each_line` bracket surface.)*
- `fs.dir` — `list_dir`, `create_dir`, `remove_dir`, `walk` *(shipped via #344: `kai_core_dir_*` runtime primitives + `pub fn` wrappers in `stdlib/fs/dir.kai`. All four ride the `File` effect. v1 limits: symlinks are not followed in `walk`; `create_dir` uses fixed `0755`; `list_dir` / `walk` return `[]` on read error. `current()` (`getcwd`) and `temp_dir()` (`$TMPDIR`, else `/tmp`) shipped via #3162.)*
- `fs.path` — `import fs.path` is the qualified home of the pure path helpers `is_absolute`, `basename`, `dirname`, `split`, `ext`, `strip_ext`, `join` *(shipped — the implementation is the auto-loaded `stdlib/path.kai`, so `path.join(a, b)` and the flat `path_join(a, b)` reach the same function)*.

### os (`/ Env + Process`)

- `os.env` — `get`, `set`, `unset`, `entries` *(shipped via PR #131 + PR #143; closes #127 — `entries` was renamed from `all` because the bare `all` collided with `list.all` for any caller importing both)*
- `os.args` — `argv`, `program_name` *(shipped via PR #131 + PR #143)*
- `os.process` — `start(cmd, args)`, `wait`, `kill`, `exit` *(shipped — closes #346)*, plus the popen-shaped pipe family `start_piped`, `pipe_to`, `pipe_from`, `write_stdin`, `close_stdin`, `read_stdout`, `read_all` *(shipped — closes #1591; wraps the `start_piped`/`write_stdin`/`close_stdin`/`read_stdout` ops of `Process`)* and stderr capture `pipe_errs`, `read_stderr` *(shipped — closes #2089; `start_piped` takes a `pipe_stderr` flag and `read_stderr` is its op)*, and process groups `start_in_group`, `kill_group` *(shipped — closes #2316; wraps the `start_group`/`kill_group` ops of `Process`. `wait_or_kill` and `signal` still deferred — `wait_or_kill` is unblocked (R1 reactor shipped 2026-05-15, #611, parks the fiber on `Process.wait` via SIGCHLD self-pipe) and waits on the cancel-aware wrapper, see `docs/effects-stdlib.md` §`Process` v1-status sidebar.)*, and `uid`, the effective uid of the current process *(shipped — closes #2789; wraps the `uid` op of `Process`, the value `NetUnix.peer_uid` reports for this process)*
- `os.exit` — physically `process.exit` *(shipped via #346)*. Takes an exit code. Effect: `/ Process` (exiting is observable). Top-level alias `os.exit` (calling `exit(7)` without the `process.` qualifier) is a module-system design decision pending in stage 2; until then, `process.exit(code)` is the surface.

### time (`/ Clock`)

Single top-level module. Contains both the types (`Instant`,
`Duration`) and the operations (`now`, `sleep`, `deadline_in`).

- `time` — `WallTime`, `Instant`, `Duration`, `now()` (wall),
  `monotonic()`, comparison/subtraction on instants, duration
  arithmetic (millis, seconds, minutes, …; `Duration` also answers
  `+`, `-`, `* Int`, `/ Int`, `==` and ordering), `sleep(d)`,
  `deadline_in(d)` — integrates with `Cancel`

### date (pure; `today()` rides `/ Clock`)

Single top-level module: civil calendar dates, proleptic Gregorian,
timezone-naive. Pure data + algorithms following the decimal
precedent (`Option` for fallible constructors/parsers, no panics);
the only effectful fn is `today()`.

- `date` — `Date { year, month, day }`; validating `make` returns
  `Option[Date]`; `from_epoch_days` / `to_epoch_days` (Hinnant's
  era-based civil algorithms, epoch day 0 = 1970-01-01, negative
  days reach pre-1970); `is_leap_year`, `days_in_month`,
  `day_of_week` (ISO: 1 = Mon … 7 = Sun), `day_of_year`;
  `add_days`, `diff_days`, `add_months` (end-of-month clamping,
  deliberately non-reversible); `eq`/`cmp`/`lt`; ISO-8601
  `to_string` / strict `parse` (`YYYY-MM-DD`); bridge
  `from_walltime(t)` (UTC interpretation, documented not
  configurable) and `today()` `/ Clock` *(shipped via PR #770 — closes #767)*.
- `date.rfc3339` — `format(t: WallTime) : String` (UTC `Z`, fraction only
  when non-zero, trailing zeros trimmed) and `parse(s) : Result[WallTime,
  String]` (`T`/`t`, `Z`/`z` or `±hh:mm`, 1–9 fractional digits, errors
  name the byte offset); reuses `date`'s calendar *(shipped via #3162)*.
  Out of scope v1: tzdata/DST, civil time-of-day, locales, ISO week
  dates, non-Gregorian calendars.

- `string_builder` — amortised text accumulator over a growable
  `Array[String]` of fragments. `new` / `with_capacity` construct;
  `append(sb, s)` / `append_char(sb, c)` ride `Mutable` (direct
  `array_set` into the builder's array, doubling on overflow → O(1)
  amortised); `build(sb)` joins the fragments in one pass
  (`string_concat_all`, single allocation) and is **pure**, so a
  caller that creates/appends/builds locally has `Mutable` masked at
  its boundary; `length` (`len` deprecated alias) / `is_empty` are pure
  reads. O(total) build vs
  the O(n²) of a left-fold of `++` *(shipped — closes #902)*.

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

### net (`/ NetTcp`, `/ NetUdp`, `/ NetDns`, `/ NetUnix` all shipped)

> **v1 caveat (2026-06-06).** All three leaves of the `Net = NetTcp + NetUdp + NetDns` alias now exist as builtins with runtime handlers and module files — `NetTcp` (#68), `NetUdp` (#354), `NetDns` (#352). The alias is therefore definable; a follow-up lane adds the row alias to `stdlib/effects.kai`. See `docs/effects-stdlib.md` §`NetTcp`, `NetUdp`, `NetDns` v1-status sidebars for reactor + per-effect status.

- `net.tcp` — `connect`, `listen`, `accept`, read/write *(shipped — PR #68; R2 reactor flipped the default handler to fiber-parking via `poll()` 2026-05-16, issue #630 — every blocking op parks the fiber, never the OS thread)*
- `net.udp` — `bind`, `send`, `recv`, `close`, plus pure `local_port` / `addr` *(shipped — issue #354; v1 datagram UDP over POSIX `SOCK_DGRAM`, blocking ops, IPv4 only; the m8.x reactor lifts NetTcp + NetUdp together)*
- `net.dns` — `resolve`, `resolve_first`, `with_dns` *(shipped — issue #352; `getaddrinfo(3)` shim, IPv4-only, blocking on the OS thread in v1; `resolve_all` / `reverse_lookup` deferred — reverse DNS is a separate proposal per #352)*
- `net.unix` — `NetUnix` effect (`listen`, `connect`, `accept`, `send`, `recv`, `recv_timeout`, `peer_uid`, `close`, `close_listener`) plus `listen` and the `OWNER_ONLY` mode *(shipped — issue #2186; the socket file is created already carrying its mode, `peer_uid` reads `SO_PEERCRED` / `LOCAL_PEERCRED`, stale sockets are replaced and live ones refused; the fd-level ops reuse the NetTcp handlers, so they park the fiber)*
- `net.url` — pure RFC 3986 (not WHATWG) URI references *(shipped)*: `Url` record (`scheme`, `userinfo`, `host`, `port`, `query`, `fragment` are `Option`s, `path` a `String`; components kept verbatim and still percent-encoded), `parse` (`Result[Url, String]`, errors name the byte offset), `to_string` (round-trips every accepted input, bar an empty port), `resolve` (§5.2 reference resolution, pinned by the §5.4 examples), `percent_encode` / `percent_decode` (uppercase hex over UTF-8 bytes, unreserved set kept, `+` untouched), `query_pairs` / `query_string` (decoded `(key, value)` pairs and their inverse). Contracts are pinned by `tests/stdlib/url_test.kai`. `net.http` does not use it: `http.parse_url` has its own lenient parser (identical behaviour, no import edge).
- `net.http` — client surface (stable): `http_get`, `http_post`, `http_put`, `http_delete`, `http_request` *(shipped, `/ NetTcp + Cancel`)*; headers, body, timeouts available via the request builder. Automatic redirect following on top of the existing single-call surface: `RedirectPolicy` record, `default_redirect_policy`, `http_follow`, plus the convenience wrappers `http_get_follow` / `http_post_follow` / `http_put_follow` / `http_delete_follow` *(shipped — issue #357)* — RFC 9110 §15.4 method-rewrite rules baked in (303 → GET, 307/308 preserve method/body, 301/302 → GET on POST per browser convention); cross-origin redirects strip Authorization, Proxy-Authorization, Cookie and Cookie2 case-insensitively; same-origin hops preserve them, stripped credentials never return on later hops, and Host overrides are removed on every hop. Cookie persistence remains deferred. The bare `http_request` keeps its single-call semantics so callers wanting explicit 3xx control are unaffected. Uses libc `getaddrinfo` for DNS (no `NetDns` effect yet). Server-side helpers (`#[unstable]`): `http_parse_request`, `http_serialize_response`, `http_status_reason` (pure), and `http_read_request` (`/ NetTcp`) *(shipped — issue #605)* — minimal primitives for hand-rolled HTTP/1.1 servers. The wire helpers reuse the client-side internals (`http_split_head`, `http_parse_header_lines`, `http_format_headers`, `header_lookup`); the convenience read loop is the only NetTcp-touching addition. `parse_url` is `net.http`'s own lenient parser, not `net.url` (it does not validate characters or `%` escapes); it ends the authority at the first `/`, `?` or `#` (a query-only URL gets path `/?…`) and rejects a port outside 1..65535; `RedirectPolicy` has three fields (307/308 always keep the method);  header lines need a non-empty name; `Transfer-Encoding: chunked` matches case-insensitively; `read_request` reports a body cut short by the peer closing as an error. Contracts are pinned by `tests/stdlib/http_wire_test.kai` and `tests/stdlib/http_loopback_test.kai` (wire level plus loopback servers, redirects actually followed).

A full server framework (`manutara`-side: router, middleware,
graceful shutdown) is deferred — its design surface is much larger
than what a primitive module should expose. The `#[unstable]`
server-side helpers above are the seam manutara will sit on top of;
they are explicitly carved out of the edition contract until
manutara's first design pass exercises them.

### web (`/ Js`, browser target only)

- `web.dom` — `Dom` effect: query, create, append, remove, text, attributes, styles; `listen` and `next_event` / `poll_event` (events as values: the host queues them, `next_event` parks the fiber); a 2D canvas (`context_2d`, `fill_rect`, `clear_rect`, `begin_path`, `arc`, `fill`, `stroke`, `fill_text`, `set_fill_style`, `set_stroke_style`, `set_font`) and per-pixel drawing (`new_image`, `set_pixel`, `put_image_data`, `get_image_data`, `get_pixel`) *(shipped — #3136)*
- `web.timer` — `Timer` effect: `now_ms`, `sleep_ms`, `animation_frame`, `every` (periodic events for `Dom.next_event`) *(shipped — #3136)*
- `web.fetch` — `Fetch` effect: `get`; `get_text`, `get_json`, `status` *(shipped — #3136)*
- `web.storage` — `Storage` effect over `localStorage`: `get`, `set`, `remove` *(shipped — #3136)*
- `web.run` — `run { … }` installs the four handlers; its row needs `Js`, so a program using these effects builds only for `wasm32-browser` *(shipped — #3136)*

### encoding (pure, stage 2)

- `encoding.json` — `json_encode`, `json_decode`, `json_decode_report`; drilling `get`, `at` (`Option[JsonValue]`: `None` for a missing key, out-of-range index or wrong shape; a present `null` is `Some(JNull)`) and `as_string`/`as_int`/`as_real`/`as_bool` *(shipped — `json_decode_report` returns `Result[JsonValue, JsonParseError]`, where `JsonParseError` is `{ line, column, offset, reason }`: `offset` is the 0-based byte offset, `line` is 1-based, `column` is 1-based and counts characters; named `JsonParseError` because `json_bind` already exports `JsonError`; — Real number parsing via #361 adds `JReal(Real)` alongside `JNum(Int)`; surrogate-pair `\uD8xx\uDCxx` decode + UTF-8 emit via #362; strict RFC 8259 decode, where leading zeros and raw control characters yield `None`, and a `JReal` always encodes with a fraction so it decodes back as `JReal`, via #3148)*
- `#[derive(Json)]` — typed struct binding over the same DOM *(shipped: `to_json` / `<lower(T)>_of_json` for records and sum types; field names verbatim, `Option` accepts null-or-missing, unknown keys ignored, failures are `Result[T, JsonError]` carrying the JSON path. Sum types encode under a fixed adjacently-tagged envelope `{"tag", "value"}` (nullary → `null`, single → the value, multi → a JSON array); decode routes on `tag`, an unknown tag is a located error. Per-field `#[json(rename/default/skip)]` overrides layer on top: `rename` sets the wire name, `default` supplies an absent key's value, `skip` drops a field (requiring a `default` or `Option`). `protocol Json` in `stdlib/protocols.kai`; `JsonValue` / `JsonError` / runtime in `stdlib/encoding/json_bind.kai`, kept out of the core so `JNull` does not offset every binary's RC baseline. See `docs/json-derive-design.md`.)*
- `encoding.toml` — `decode`, `decode_report`, `encode`, `round_trip`, `error_message` *(shipped subset for the package manager (#405): top-level scalars, `[name]` and `[[name]]` headers — dotted with bare or quoted segments (`[native.sqlite]`, `[native."db.pg"]`) and nesting since #2772, a table or key defined twice rejected — basic strings, ints/bools, array values (#1742, the `[native]` table), inline tables, comments. Floats via #2056 — `TReal(Real)`, optional `-`/`+` sign, fractional (`0.8`) and/or exponent (`5e+22`) parts, a digit required on each side of the `.` and after the exponent marker; the encoder appends `.0` so an integral float does not re-decode as an integer. `inf` / `nan` and `_` digit separators stay out of the subset. `decode_report` (#2056) returns `Result[TomlValue, TomlError]` naming the 1-based line and bare key the parse stopped on, so a caller can point at the offending entry instead of calling the whole file malformed; `error_message` renders it. Encoder emits a canonical form so `kai.lock` round-trips byte-identical. Datetimes / dotted keys left of `=` / multi-line strings deferred until a user-facing TOML need lands.)*
- `encoding.base64` — `encode`, `decode` (standard + URL-safe) *(shipped)*
- `encoding.hex` — `encode`, `decode` *(shipped)*; byte lists only — an integer in hex goes through core `to_int_radix(s, 16)` / `int_to_string_radix(n, 16)`
- `encoding.utf8` — `decode`, `decode_lossy`, `is_valid`, `encode` *(shipped — `decode` returns `Err(offset)` of the first ill-formed sequence and rejects overlongs, surrogates, codepoints above U+10FFFF and truncated sequences; `decode_lossy` substitutes each maximal ill-formed subpart with U+FFFD; `string.from_bytes` stays unchecked)*

### regexp (pure, stage 2) — **landed 2026-04-28**

- `regexp` — `compile`, `compile_or_panic`, `find`, `find_all`,
  `replace`, `split`, `matches` (top-level module). Matching is by
  codepoint with leftmost-first (Perl/RE2) captures; offsets are byte
  offsets. Repeat counts above 1000, patterns over 1,000,000 nodes after
  repeats, and inverted class ranges are compile errors.
- `regexp.syntax` — pattern AST types, `parse_pattern`, `ast_show`
  *(shipped)*. `regexp.engine` — NFA types, `compile_nfa`, `nfa_show`
  *(shipped)*. Debug/tooling surface; `regexp` simulates the NFA
  (`simulate`) and holds `Match`, `Capture`, `Regex` and the API.
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

- `crypto.hash` — `sha256`, `sha512` *(shipped)*. `blake3` is not shipped (see the crypto items in `docs/stdlib-roadmap.md`).
- `crypto.legacy` — `sha1`, `md5` *(shipped)*. Broken for security; a separate module so the import flags legacy use (WebSocket handshake, git object ids, UUID v3/v5, ETags).
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
- `concurrent.actor` — mailbox primitive: `send`, `receive`, `self` *(shipped — `stdlib/actor.kai`, not `stdlib/concurrent/actor.kai`)*; spawn surface `spawn_actor` (unbounded default) + `spawn_actor_policy` (explicit `MailboxPolicy` — shipped, closes #763) and current-fiber installers `with_mailbox` / `with_mailbox_policy`. Spec detail lives in `docs/actors.md`.

### testing

- `testing` — no stdlib module: `test "..." { ... }` blocks, `assert` and the `kai test` runner are built into the compiler and toolchain (`kai info testing`).

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
  EModCall, plus core-scope and string-interpolation gaps for
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
  (But the `decimal` top-level module **is** stdlib — a frontloaded
  primitive per the roadmap.)
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
   the audit that reframed the milestone away from a
   bootstrap-split into a direct single-tree migration. See
   `docs/stage2-design.md` §m6 for the qualified-call design.
3. Land one stage-2 module end-to-end (candidate: `time`) as the
   template for the rest. Drives whatever compiler plumbing the
   effect requires and validates the `Clock` handler contract before
   rolling out `Net` / `Random` / `Process`.
