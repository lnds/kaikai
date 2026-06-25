# stdlib roadmap

Per-module rollout plan for `stdlib/`, organised by what unblocks
what. The catalog and effect bindings live in
`docs/stdlib-layout.md` (Doc *Layout*) and `docs/effects-stdlib.md`
(Doc B). This doc sits on top of both: Layout pins the *shape* of
each module, this one pins the *order*.

Pinned 2026-05-02 after the post-Tongariki gap audit. Open Q2 of
Layout (observability — log/trace/metrics) was resolved on the
same date as **option (b)**: a minimal `log` lives in stdlib over
a dedicated `Log` effect; structured `ahu.log` / `ahu.trace`
sit on top.

**Refreshed 2026-05-08** (issue #367 — doc reconciliation). The
*Current inventory* table below is the corrected snapshot; most of
Tier S1 and S2 shipped between 2026-05-02 and 2026-05-04 and the
old "not started" markers no longer reflect reality. The Tier
plan sections that follow describe the design intent of each lane
at the time it was queued — read those alongside the inventory
above to see which acceptance bars have been met.

**Re-verified 2026-05-16** (issue #604 docs honesty audit). The
inventory below was spot-checked against `stdlib/` and closing
PR numbers; no new drift caught. New rows added this cycle:
`collections/map` round-out + qualified migration + pipes (#613),
`core/string` drop_prefix/drop_suffix (#632), `net/http` automatic
redirect following (#357). `net/http` server-side `#[unstable]`
helpers (#605) shipped under the existing "What's still open" row
and have been moved into the inventory.

## Why now

Tongariki (m1–m13 + m14 partial) closed at 0.37.0. The language
is feature-complete enough that the next wave of value comes from
the libraries on top, not from new compiler features. Two
products depend directly on stdlib reaching parity:

- **`ahu`** — OTP-style application layer (supervisors, registry,
  GenServer-analog). Needs `fs`, `os`, `time` (full Clock
  handler), and structured logging primitives to express its
  default behaviours.
- **`manutara`** — web framework (Phoenix-LiveView style). Needs
  `net.http` client (for outbound calls in templates / fetchers)
  and indirectly drives the eventual server-side `net.http.server`
  module that lives **inside manutara**, not stdlib.

Both products are post-MVP. Stdlib gaps are what gates them.

## Current inventory (refreshed 2026-05-08)

What ships in `stdlib/` today:

```
core/   list.kai string.kai option.kai result.kai char.kai tuple.kai io.kai
collections/  map.kai hashmap.kai set.kai hashset.kai stack.kai queue.kai
math/  int.kai real.kai numeric.kai bits.kai complex.kai
crypto/  hash.kai mac.kai
encoding/  json.kai base64.kai hex.kai
fs/  file.kai dir.kai path.kai
net/  tcp.kai http.kai
os/  args.kai env.kai
top-level: actor.kai array.kai date.kai decimal.kai effects.kai fx.kai
           log.kai loop.kai money.kai path.kai protocols.kai random.kai
           random_secure.kai reader.kai regexp.kai spawn.kai time.kai
           trace.kai uuid.kai writer.kai
```

What landed since the previous snapshot (2026-05-02 → 2026-05-08):

| Module                                  | Status                                                                                                                         |
|-----------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| `fs/file`                               | shipped via PR #132 (3 public fns: `file_read_file`, `file_write_file`, `file_append`); minimum-viable extras shipped via #345 (`file_exists`, `file_delete`, `file_rename` as prelude builtins riding `File`); module-qualified surface `file.exists` / `file.delete` / `file.rename` shipped via #423 (closes the gap where `import fs.file` did not expose the #345 ops); `metadata`/`read_bytes`/`write_bytes` deferred (need record type / `[Int]` byte-array conversion); chunked streaming ops `open_read`/`read_chunk`/`open_write`/`write_chunk`/`close_file` + `FileHandle` shipped via PR #804 (issue #771 Phase 1, R1-pool default handlers); lazy line surface shipped via issue #801 (`stdlib/stream.kai` — push carrier, pipe-canonical combinators, `read_lines`/`write_lines`, `ReadFault` recovery; supersedes #771 Phase 2 bracket surface) |
| `fs/dir`                                | shipped via #344: 4 public fns (`dir.list_dir`, `dir.create_dir`, `dir.remove_dir`, `dir.walk`) over `kai_prelude_dir_*` primitives; rides `File`; v1 walk does not follow symlinks |
| `os/env`                                | shipped via PR #131 (partial) + PR #143 (close, closes #127); 4 public fns                                                     |
| `os/args`                               | shipped via PR #131 + PR #143; 2 public fns (`args_argv`, `args_program_name`)                                                 |
| `os/process` (runtime + effect)         | shipped via PR #142 (closes the runtime side of #126)                                                                          |
| `os/process` (Kai wrapper)              | shipped (closes #346): 4 public fns (`process.start`, `process.wait`, `process.kill`, `process.exit`) over `builtin_process_decl` |
| `time` Clock default handler            | shipped via PR #134 — `kai_default_clock_wall_now`, `kai_default_clock_monotonic_now`, `kai_default_clock_sleep_ns` are wired  |
| `crypto/hash`, `crypto/mac`             | shipped via PR #146 (S2 #5): 626 LOC pure-Kai SHA-256/SHA-512 + 83 LOC HMAC                                                    |
| `random_secure`                         | shipped via PR #144 (closes #140): 44 LOC, 2 public fns over `getrandom(2)` / `arc4random_buf`                                 |
| `log` (stdlib minimal)                  | shipped via PR #145 (S2 #7, closes #141): 52 LOC, 4 public fns (`log_debug/info/warn/error`) over a default `Log` handler      |
| `concurrent/nursery`                    | shipped — `pub fn nursery[T, e]` lives in `stdlib/spawn.kai:95` (top-level, not a `concurrent/` subdir)                        |
| `actor` spawn with explicit policy      | shipped (closes #763): `spawn_actor_policy(policy, body)` in `stdlib/actor.kai` — spawned actor's mailbox honours an explicit `MailboxPolicy` (bounded back-pressure / drop policies); new runtime primitive `kai_mailbox_alloc_bounded_unowned` (owner-reassign protocol, no parent `mailbox`-slot clobber); fixtures `examples/effects/issue_763_spawn_actor_policy_{block_sender,drop_oldest}.kai` |
| `math/real` libm bindings               | shipped via PR #359 (closes #343): sqrt, trig, exp/log, pow, atan2 over libm; `fmod` follow-up via #364 enables `Real % Real`  |
| `encoding/json` Real numbers            | shipped (closes #361): decoder accepts decimals + scientific notation; new `JReal(Real)` variant alongside `JNum(Int)`         |
| `encoding/json` surrogate-pair UTF-8    | shipped (closes #362): decoder folds `\uD8xx\uDCxx` pairs into 4-byte UTF-8 sequences; BMP `\uXXXX` emits 1–3 UTF-8 bytes; encoder emits raw UTF-8 verbatim per RFC 8259 §8.1; lone or invalid surrogates yield `None` |
| `core/tuple` helpers                    | shipped (closes #348): `tuple.swap`, `tuple.map_fst`, `tuple.map_snd`, `tuple.map_pair`, `tuple.first`/`second`/`third`. `fst`/`snd` projections stay field-access only — adding bare `pub fn fst`/`snd` poisons every existing `record.fst` access whose receiver type isn't yet pinned by inference (see module header) |
| `core/list` surface expansion           | shipped (closes #340): `last`, `init`, `partition`, `split_at`, `span`, `chunk`, `windows`, `intersperse`, `enumerate`, `zip3`, `scan`, `group_by`, `find_map`. `group_by` uses Erlang/Elixir consecutive-key semantics; its key type is still `Int` for v1 (it carries no `: Eq` bound), unlike `uniq` which is now `[T : Eq]` (see the aggregate-generalisation row) |
| `core/list` + `math` aggregate generalisation | shipped (closes #891, on #890's free-fn protocol bounds): `sum`/`product` → `[T : Numeric]`, `max`/`min`/`sort` → `[T : Ord]`, `uniq` → `[T : Eq]`. `max`/`min` keep the `Option[T]` wrapper. `Numeric` (`stdlib/math/numeric.kai`) extended to a ring with `add`/`mul`/`zero`/`one` (impls for Int/Real/Decimal); `sum` folds `add` from `zero()`, `product` folds `mul` from `one()`. Fixtures `examples/stdlib/aggregate_{overload_arity,numeric_ring,ord_eq,uniq_wrapper_eq,qualified_call}.kai`. Surfaced four compiler bugs, three fixed upstream (#894 op-name collision, #899 bound-blind shadowing, #900 interpolation collision) and the fourth — qualified-call monomorphisation (`EModCall` callees were never rewritten to their `__mono__` spec, so `list.sum(...)` ran the un-instantiated generic and panicked on the nullary `zero()`) — fixed in this lane (`fix(monomorph)`). Selfhost byte-identical on C + native |
| `core/string` surface expansion         | shipped partially against #338: `split`, `replace`, `pad_left`, `pad_right`, `lines`, `chars`, `is_blank`. `split(s, "")` panics; `lines("")` returns `[]` (Python/Rust convention). Five proposed helpers (`index_of`, `to_upper`/`to_lower`, `is_empty`, `reverse`) deferred to #396 — each collides on bare name with an existing core export, and `--include-prelude-tests` does not honor the typer's first-arg-type narrowing across modules (resolver fix → surface, mirroring #335 → #336) |
| `core/string` prefix/suffix family      | shipped (closes #632): `string.drop_prefix(s, prefix) : Option[String]` and `string.drop_suffix(s, suffix) : Option[String]` complete the `starts_with`/`ends_with` pair. `Some(rest)` when the affix is present, `None` otherwise — same shape as `to_int`, `log2`, `div_mod`; mirrors Rust's `str::strip_prefix`. Fuses membership check with slice so URL routers and CLI parsers stop hand-rolling the `if starts_with { slice(...) }` boilerplate the issue body called out |
| `array` bridge module                   | shipped (closes #366): top-level `stdlib/array.kai` with `array_from_list`, `array_to_list`, `array_copy`. `random.shuffle` flipped from O(n²) selection-sampling to O(n) in-place Fisher-Yates over a locally-built `Array[T]`; observable row stays `[T] -> [T] / Random` (masking pass drops the inner `Mutable`) |
| `decimal` div-by-zero hardening         | shipped (closes #363): `dec_div` returns `Option[Decimal]` (`None` on zero divisor) instead of silent `dec_zero`. **Breaking** — direct callers must `match` the result. `money.money_divide_scalar` flips to `Option[Money]`; `money_ratio` propagates `None` |
| `fx` currency conversion (top-level)    | shipped (closes #365): `stdlib/fx.kai` adds `FxPair`/`FxRate`/`FxTable`/`FxTimestamp` with `fx_table_put`/`fx_lookup`/`fx_convert` plus composition wrappers (`money_add_via_fx`, `money_sub_via_fx`, `money_cmp_via_fx`). v1 is in-memory only, no auto-inverse, no transitive lookup, no rate aging — explicit by design (see issue body). Wired into `stage2/Makefile` `EXTRA_PRELUDE_FLAGS` and `bin/kai`'s prelude chain |
| `collections/hashmap` mutable hash table | shipped (closes #374; #375 HashSet built on it): `stdlib/collections/hashmap.kai` — a **mutable** separately-chained hash table behind the `Mutable` effect. `HashMap[k, v] = { buckets: Ref[Array[[Pair]]], count: Ref[Int], cap: Ref[Int] }`; resize doubles at load factor 0.75. 12 pub fns with short module-relative names (`hashmap.put`/`get`/`empty`/…, same convention as `map`/`set` — no `hashmap_*` aliases), but **signatures diverge from the issue's PURE shapes** — `put`/`remove` mutate in place and return `Unit`, every op carries `/ Mutable`. **Carrier redesign mid-lane:** the issue body specified a pure HAMT; it was implemented, benchmarked ~1.6–2.3× SLOWER than the AVL `Map` (a persistent trie pays alloc + rebuild per `put` and a list-indexed walk per `get` level), and replaced with the mutable table on the user's call. The mutable table is ~2–3× FASTER than `Map` on build + lookup at N≥10K (`benchmarks/hashmap/`). Iteration order unspecified (bucket-derived) but deterministic per insertion sequence. Keys need `impl Hash` + `==`; primitives + user sum types dispatch through the generic boundary, user records do not yet (pre-existing compiler dispatch gap, same class as the AVL `Map`'s `<`). `m[key]` read sugar dispatches to `hashmap.get` (returns `Option[v]` / `Mutable`) via the typer's `synth_index` — the same one-arm mechanism `Map` uses; selfhost stays byte-identical (additive arm). No HashDoS mitigation in v1. Fixtures `examples/stdlib/hashmap_basic.kai` (10K + 100K round-trip, iteration stability, in-place mutation) and `hashmap_collision.kai` (forced one-bucket chain via a constant-hash sum-type key). NOTE: `kai bench` segfaults on any `/ Mutable` block body (pre-existing bench/effect bug) — the benchmark times via `Clock` instead. Selfhost byte-identical (additive). |
| `collections/hashset` mutable hash set | shipped (PR #757, closes #375): `stdlib/collections/hashset.kai` — a **mutable** hash set, a thin wrapper over the sibling `HashMap[t, Unit]` (each member is a key whose stored value is the single `Unit` value `()`). `HashSet[t] = { inner: HashMap[t, Unit] }`; every op delegates to `collections.hashmap`, inheriting its performance, mutation model, and effect discipline wholesale — the O(1)-average set, sibling to `HashMap` exactly as the list-backed `Set` (#614) is the sibling of the AVL `Map`. 12 pub fns with short module-relative names (`hashset.add`/`remove`/`contains`/`union`/…, same convention as `hashmap`/`set` — no `hashset_*` aliases): `empty`, `size`, `is_empty`, `contains`, `add`, `remove`, `to_list`, `from_list`, `union`, `intersection`, `difference`, `is_subset`. **Mutating, not persistent** — `add`/`remove` mutate in place and return `Unit`, every op carries `/ Mutable`; the set-algebra ops (`union`/`intersection`/`difference`) build and return a FRESH set, leaving both arguments untouched. **The issue #375 body is STALE:** it assumed a *persistent* HashMap and a `Hashable` protocol, neither of which shipped — HashMap landed mutable (the pure HAMT benchmarked ~2× slower than the AVL `Map`) and the hashing protocol is `Hash`, not `Hashable`. This module mirrors the as-shipped mutable HashMap accordingly (do not restore the issue's persistent signatures). Elements need `impl Hash` + `==`; primitives + user sum types dispatch through the generic boundary, user records do not yet (HashMap's compiler dispatch gap, inherited verbatim — lift a record element to a primitive id field). Iteration order unspecified (bucket-derived). Fixtures `examples/stdlib/hashset_basic.kai` (Int + String round-trip, 100K resize, in-place mutation, `from_list` dedup) and `hashset_ops.kai` (union/intersection/difference/is_subset on small Int sets with known results, args-unchanged checks). Selfhost byte-identical (additive — pure stdlib, no compiler change); C + LLVM parity verified. |
| `collections/map` round-out + qualified migration + pipes | shipped (closes #613): `stdlib/collections/map.kai` rounds out the public surface with `update`, `fold`, `merge`, `filter`, `transform_values`; migrates the 11 flat-prefix exports (`map_put`, `map_get`, …) to qualified canonical form (`map.put`, `map.get`, …) per the m14 audit's Option E. Flat-prefix aliases retained as one-liners for the tongariki edition, scheduled to drop at the Orongo boundary. Adds `map`/`flat_map`/`filter` pair-shaped exports so `Map[k, v]` participates in the `|`/`||`/`|?` pipe convention introduced by #594. Selfhost byte-identical (only stage2 reference is the `"map_get"` callee string synthesised by the indexing-sugar lowering, kept resolvable through the alias). |
| collections pipe-participation coherence | shipped (PR #876): closes the asymmetry where `Map`/`Stream`/`[T]`/`Option`/`Result` rode the `|`/`||`/`|?` pipes but `Set`, `Stack`, `Queue`, `HashSet`, `HashMap` did not. All five now export the canonical `map`/`flat_map`/`filter`. Element shapes: `Set`/`Stack`/`Queue`/`HashSet` are element-shaped (`a`/`t`); `HashMap` is pair-shaped (`Pair[k, v]`), mirroring `Map`. `Set.map`/`HashSet.map` collapse output collisions; `Map.map`/`HashMap.map` collapse duplicate output keys (last wins). The mutable `HashSet`/`HashMap` combinators carry `/ Mutable` (they read the table). `flat_map` is written with its own row-threading loop rather than delegating to the pure `list.flat_map`, so `f`'s effect row survives. Fixtures: `examples/pipes/collections/{set,stack,queue,hashset,hashmap,map}_pipes.kai`, wired into the new `test-pipes-collections` tier (compiled with `--edition hanga-roa` to engage non-List head dispatch). Selfhost byte-identical (additive — pure stdlib, no compiler change). |
| `net/http` automatic redirect following  | shipped (closes #357): `stdlib/net/http.kai` gains `RedirectPolicy` (4 fields: `max_redirects`, `follow_3xx`, `rewrite_post_to_get_on_303`, `preserve_method_307_308`), `default_redirect_policy`, `http_follow`, plus 4 convenience wrappers (`http_get_follow`, `http_post_follow`, `http_put_follow`, `http_delete_follow`). RFC 9110 §15.4 method-rewrite rules baked in; bare `http_request` and the existing `http_get`/`http_post`/`http_put`/`http_delete` keep their single-call semantics (no breaking change). Cookie persistence + cross-origin Authorization stripping stay deferred to the cookie-policy lane. Selfhost byte-identical. |
| m14 follow-up — 21 modules qualified-call surface | shipped (closes #614): pins the canonical user-facing surface for the 21 stdlib modules outside `stdlib/core/*` (`fs/file`, `spawn`, `log`, `math/int`, `math/real`, `decimal`, `money`, `fx`, `array`, `collections/{set,queue,stack}`, `path`, `encoding/{base64,hex,toml}`, `uuid`, `regexp`, `random_secure`, `net/http`, `protocols`). Each module exposes its qualified form (`file.read`, `spawn.spawn`, `int.min`, `decimal.zero`, `queue.push`, `regexp.compile`, …) via the qualified-call resolver's prefix fallback (`me_lookup_export` in stage2/compiler.kai) plus, for `regexp`, a second prefix table (`module_legacy_prefix_alt`) so both `regexp.match` (→ `regex_match`) and `regexp.parse_pattern` (→ `rx_parse_pattern`) resolve. `queue` / `stack` / `random_secure` join the existing `char` / `regexp` / `decimal` entries in `module_legacy_prefix`. Definitions stay flat-prefix; lane retro `docs/lane-experience-issue-614-m14-followup.md` documents the departure from issue #614's stated Option E (kaikai has no top-level pub fn overloading; renames collide with sibling modules and shadow user-defined fns). Where bare canonical names are safe (no shadow, no top-level clash), the lane added both forms — `file.read/write/append`, `spawn.spawn/await/select/cancel/yield/set_trap_exit`, `log.debug/info/warn/error`, `real.trunc/floor/ceil/round_half_even`, `decimal.from_int/from_parts`. Selfhost byte-identical. **Superseded 2026-05-17:** the canonical-only migration renamed all definitions to bare canonical names and removed the legacy-prefix fallback entirely (#769) — current mechanism in `docs/stdlib-layout.md` §Qualified surface. |
| `net/udp` datagram UDP                   | shipped (closes #354): `NetUdp` effect (`builtin_netudp_decl` in `stage2/compiler/driver.kai`) — `bind`/`send`/`recv`/`close` over POSIX `SOCK_DGRAM` — plus companion types `UdpSocket` / `SocketAddr` and the `stdlib/net/udp.kai` module (`local_port`, `bind`, `send`, `recv`, `close`, `addr`). Runtime default handlers `kai_default_netudp_*` in `stage0/runtime.h` + `stage2/runtime.h`, LLVM forwarders in `runtime_llvm.c`. v1: IPv4 only, blocking (no reactor — m8.x lifts NetTcp + NetUdp together), `[Byte]`→`[Int]`, `recv` returns `Pair[SocketAddr, [Int]]`. No `with_udp` (the default handler is the install path; a kaikai-bodied installer needs `$extern_handler` outside `default {}`, which is Stage C of #533). The `Net = NetTcp + NetUdp + NetDns` alias still waits on `net/dns` (#352). Selfhost byte-identical (C + LLVM). |

| `date` civil calendar (top-level)        | shipped via PR #770 (closes #767): `stdlib/date.kai` — pure proleptic-Gregorian `Date { year, month, day }`, timezone-naive. Validating `make` returns `Option[Date]`; `from_epoch_days`/`to_epoch_days` use Hinnant's era-based civil algorithms (epoch day 0 = 1970-01-01; negative days reach pre-1970, exact over the full proleptic range); `is_leap_year`, `days_in_month`, `day_of_week` (ISO 1 = Mon … 7 = Sun), `day_of_year`; `add_days`/`diff_days`/`add_months` (end-of-month clamping, deliberately non-reversible); `eq`/`cmp`/`lt`; ISO-8601 `to_string` + strict `parse` (`YYYY-MM-DD`, years 0000–9999); bridge `from_walltime(t)` (UTC interpretation, documented not configurable) and `today() / Clock` — the only effectful fn. v1 excludes tzdata/DST, civil time-of-day, locales, ISO week dates, non-Gregorian calendars. Fixtures `examples/stdlib/date_{basic,iso,epoch}.kai` with `.out.expected` goldens (incl. negative epoch days, 1900/2000/2024 leap rules, clamping matrix, parse-rejection matrix, ±800k-day round-trip sweep) |
| `string_builder` (top-level)             | shipped (closes #902): `stdlib/string_builder.kai` — an amortised text accumulator over a growable `Array[String]` of fragments. `append` / `append_char` ride `Mutable` (direct `array_set` into the builder's array); `build` joins the fragments in one pass (`string_concat_all`, single allocation) and is **pure** — reads never demand `Mutable`, so a caller that creates/appends/builds locally has `Mutable` masked at its boundary. O(total) build vs the O(n²) of a left-fold of `++`. 7 pub fns: `new`, `with_capacity`, `append`, `append_char`, `build`, `len`, `is_empty`. No runtime or compiler change (the bytes→string prim turned out unnecessary — `string_concat_all` already does the one-pass join). Fixtures `examples/effects/string_builder_{build_correct,masks_mutable,append_requires_mutable}.kai`. Surfaced a masking-pass soundness hole (#903) — an Array-only record forwarded to a `/ Mutable` callee drops `Mutable` despite observable mutation; out of lane, not relied on by this module |

What's still open (planned-but-not-shipped):

| Module                          | Issue   | Notes                                                                                            |
|---------------------------------|---------|--------------------------------------------------------------------------------------------------|
| `fs/file` extras (M2)           | #345 follow-up | `metadata` (`FileMetadata` record + `stat(2)`), `read_bytes`/`write_bytes` (`[Int]` byte arrays). M1 (`file_exists`, `file_delete`, `file_rename`) shipped via #345 |
| `net/http` server-side primitives | #605  | minimal `#[unstable]` helpers (`http_parse_request`, `http_serialize_response`, `http_status_reason`, `http_read_request`) shipped in `stdlib/net/http.kai`. A full server framework (router, middleware, graceful shutdown) remains a `manutara` concern. |

(`net/udp` shipped via #354 and `net/dns` via #352 — both moved out of
this open list into *Current inventory*. The `Net` alias follow-up is
the only net-surface item left.)

## Tier plan

Three tiers, ordered by impact on the next two product layers
(`ahu`, `manutara`) and parallelism with the **Hanga Roa**
milestone (m11 diagnostics + lsp + reuse-in-place — see
`docs/roadmap.md` §`Hanga Roa`; REPL removed permanently per
#406 and `docs/decisions/repl-removal-2026-05-09.md`).

### Tier S1 — blocks `ahu` / `manutara`, runs parallel with Hanga Roa start

Four modules. Each is an independent code path; they can land in
parallel with each other and with Hanga Roa's compiler-side work.

1. **`fs/`** — file + directory operations on top of `File`.
   - Surface: `fs.file.read_file`, `fs.file.write_file`,
     `fs.file.append`, `fs.file.exists`, `fs.file.delete`,
     `fs.file.rename`, `fs.file.metadata`; `fs.dir.list_dir`,
     `fs.dir.create_dir`, `fs.dir.remove_dir`, `fs.dir.walk`;
     `fs.path.*` (re-export of existing `path.kai` helpers).
   - Why now: ahu's default supervisor logs to disk; manutara
     serves static files; both need `fs.file` immediately.
   - Dependency: `File` effect already declared in `effects.kai`.
   - Acceptance: `examples/stdlib/fs_basic.kai` round-trips a
     write → read → delete cycle under tier1.

2. **`os/`** — env / process / args on top of `Env` + `Process`.
   - Surface: `os.env.get/set/unset/all`,
     `os.args.argv/program_name`,
     `os.process.start/wait/wait_or_kill/pipe_stdout/pipe_stdin/signal/kill`,
     top-level `os.exit(code)`.
   - Why now: ahu's config loader pulls from env vars (OTP
     analogue of `:application.get_env/2`); manutara reads
     `PORT`, `DATABASE_URL`, etc. on startup.
   - Dependency: `Env` and `Process` effects already in
     `effects-stdlib.md` (Doc B); confirm runtime ops exist in
     `stage0/runtime.h`.
   - Acceptance: `examples/stdlib/os_basic.kai` reads `$HOME`,
     spawns `/bin/echo hello`, asserts on stdout.

3. **`net.http`** (client only) — HTTP/1.1 over existing
   `NetTcp`.
   - Surface: `net.http.get/post/put/delete/request`, request
     builder (headers, body, timeout via `Cancel`), response
     decoder (status, headers, body bytes / text).
   - Out of scope: server-side HTTP — that lives **inside
     manutara**, not stdlib. Decision pinned in Layout §`net`.
   - Why now: any non-trivial ahu/manutara service makes
     outbound HTTP calls (auth providers, webhooks, downstream
     APIs).
   - Dependency: `NetTcp` shipped (v1 limitations: IPv4 only,
     blocking ops). `net.dns` shipped (issue #352) — `net.http`
     still uses `NetTcp.connect`'s implicit `getaddrinfo` path;
     splitting resolve→connect onto `NetDns` is the #352 follow-up.
   - Acceptance: `examples/stdlib/http_client_basic.kai`
     issues a GET against an in-test localhost server (spawned
     via `nc -l` or a small kaikai listener) and asserts on the
     response body.

4. **`time` Clock default handler** — bridges `Clock` ops to
   the OS.
   - Surface: no new public API. Implements the *default
     handler* behind the existing `time.now / monotonic / sleep
     / deadline_in` wrappers, so calling them no longer needs
     a user-installed handler.
   - Mechanism: `clock_gettime(CLOCK_REALTIME)` for
     `wall_now`, `clock_gettime(CLOCK_MONOTONIC)` for
     `monotonic_now`, `nanosleep` for `sleep_ns` (cooperative
     once m8.x ships; spin-yield until then).
   - Why now: ahu timers, manutara request deadlines, and any
     `Cancel`-aware loop need `Clock` to "just work" without
     ceremony.
   - Dependency: `Ffi` capability for the syscalls; runtime
     side already has the C wrappers (verify
     `kai_default_clock_*` in `stage0/runtime.h`).
   - Acceptance: `examples/stdlib/time_clock_default.kai` calls
     `time.now()` and `time.sleep(millis(10))` without
     installing a handler; asserts elapsed monotonic ≥ 10 ms.

### Tier S2 — parallel with Hanga Roa middle/end

Three modules. Independent code paths; not on `ahu`/`manutara`'s
critical path but needed for production-quality services.

5. **`crypto/hash` + `crypto/mac`** — sha256, sha512, blake3,
   hmac_sha256, hmac_sha512.
   - Why: auth tokens, session signing, content addressing.
   - Acceptance: `examples/stdlib/crypto_hash_basic.kai`
     covers a known SHA-256 test vector and an HMAC vector
     from RFC 4231.

6. **`random_secure`** — separate `SecureRandom` effect, NOT
   unified with `Random` (deliberate — see Layout §`random`).
   - Why: token generation, password reset URLs.
   - Mechanism: `getrandom(2)` on Linux, `arc4random_buf` on
     macOS, both via `Ffi`.

7. **`log` (stdlib minimal)** — Q2 (b) lands here.
   - Surface: `log.debug/info/warn/error(msg: String)`. Single
     `Log` effect with a default handler that writes to stderr
     prefixed with level + timestamp. No structured fields, no
     redaction, no rotation — that surface is `ahu.log`'s job.
   - Why: any script that wants better than `eprintln` should
     be able to grab `log.info("…")` without pulling in ahu.
   - Acceptance: `examples/stdlib/log_basic.kai` emits four
     levels, golden checks the stderr format.

### Tier S3 — post-1.0 (Orongo era)

Defer until after Orongo (1.0.0) ships. Each is significant
design surface on its own.

- ~~`net/udp` — `NetUdp` effect; `bind`, `send`, `recv`.~~
  **Shipped early via #354** — pulled forward out of S3 because the
  `Net` alias was undefinable until UDP landed (see *Current
  inventory*). v1 is blocking + IPv4-only; the reactor + IPv6 are the
  remaining S3-era follow-ups.
- ~~`net/dns`~~ — shipped pre-1.0 (issue #352): `NetDns` effect
  (`resolve` / `resolve_first` / `with_dns`). The libc
  `getaddrinfo` shim inside `net.http` still stands; migrating
  `net.http` to split resolve→connect onto `NetDns` is the
  remaining #352 follow-up.
- `net/http2`, `net/http3` — over the same `NetTcp` (and
  `NetUdp` for QUIC). API shaped in S1 to avoid H1-specific
  types leaking into the public surface.
- `crypto/blake3`, `crypto/aead` — symmetric ciphers (XChaCha20-Poly1305
  is the preferred default), key management.
- `database/sql` — Go-style abstract interface, drivers
  external. Big design surface; reassess after ahu lands.
- `concurrent/parallel` — bounded data-parallel `map`/`reduce`
  over a `Spawn`-based worker pool. Distinct from `concurrent.nursery`
  (which is *structured*, not *parallel*).

## Sequencing relative to Hanga Roa

```
                  Hanga Roa milestone
                  ┌──────────────────────────────────┐
  ──── 0.37.0 ───►│ m11 │ kai lsp ✓ │ reuse-IP ✓   │──► 1.0.0 (Orongo)
                  └──────────────────────────────────┘
                          │              │
        Tier S1 ──────────┘              │
        (fs, os, net.http,               │
         Clock handler)                  │
                                         │
        Tier S2 ─────────────────────────┘
        (crypto/hash+mac, random_secure, log)

        Tier S3 ───────────────────────────────────► (post-1.0)
        (net/udp, net/dns, http2/3, crypto/aead, sql, parallel)
```

`kai lsp` ✓ shipped v0.75.0 → v0.79.0 (issue #447). REPL is not
on the diagram: removed permanently per #406. `reuse-in-place` ✓
shipped (issue #210 closed 2026-05-06; #118 + #209 extended to
linearly-unique spines).

S1 starts immediately (parallel lanes spawned 2026-05-02). S2
opens once S1 is in CI. S3 stays gated behind Orongo — no agent
should pick up an S3 item without an explicit go.

## Discipline reminders for stdlib lanes

The standard kaikai lane discipline applies (see CLAUDE.md
"Testing discipline"), with three stdlib-specific notes:

- **Effect already declared, helpers added.** Most S1 / S2
  modules wrap an effect that *already exists* in
  `effects.kai`. Don't redeclare it; import and add the public
  wrappers + a default handler.
- **End-to-end fixture under `examples/stdlib/`.** Every new
  module ships a fixture that exercises the public surface and
  is wired into `make test-stdlib` (and therefore `make
  tier1`). The fixture must FAIL before the module lands.
- **Selfhost contract.** New stdlib modules must not break
  selfhost on either C or LLVM backends. If selfhost goes
  red, the fix shape is wrong.

## Where pending stdlib work lives

GitHub Issues, labelled `stdlib`. The retired tracking docs
(`m5x-followup.md`, `unboxing-phase2-followup.md`, etc.) are
gone per PR #99; this roadmap is the *single* place that lists
the planned modules and their order. Anything else is either
already built (see Layout) or open as an issue.
