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
collections/  map.kai set.kai stack.kai queue.kai
math/  int.kai real.kai numeric.kai bits.kai complex.kai
crypto/  hash.kai mac.kai
encoding/  json.kai base64.kai hex.kai
fs/  file.kai dir.kai path.kai
net/  tcp.kai http.kai
os/  args.kai env.kai
top-level: actor.kai array.kai decimal.kai effects.kai log.kai loop.kai
           money.kai path.kai protocols.kai random.kai random_secure.kai
           reader.kai regexp.kai spawn.kai time.kai trace.kai
           uuid.kai writer.kai
```

What landed since the previous snapshot (2026-05-02 → 2026-05-08):

| Module                                  | Status                                                                                                                         |
|-----------------------------------------|--------------------------------------------------------------------------------------------------------------------------------|
| `fs/file`                               | shipped via PR #132 (84 LOC, 3 public fns: `file_read_file`, `file_write_file`, `file_append`); gaps tracked in #345           |
| `fs/dir`                                | doc-only stub committed; runtime primitives queued in #344 (no `pub fn` yet)                                                   |
| `os/env`                                | shipped via PR #131 (partial) + PR #143 (close, closes #127); 4 public fns                                                     |
| `os/args`                               | shipped via PR #131 + PR #143; 2 public fns (`args_argv`, `args_program_name`)                                                 |
| `os/process` (runtime + effect)         | shipped via PR #142 (closes the runtime side of #126)                                                                          |
| `os/process` (Kai wrapper)              | shipped (closes #346): 4 public fns (`process.start`, `process.wait`, `process.kill`, `process.exit`) over `builtin_process_decl` |
| `time` Clock default handler            | shipped via PR #134 — `kai_default_clock_wall_now`, `kai_default_clock_monotonic_now`, `kai_default_clock_sleep_ns` are wired  |
| `crypto/hash`, `crypto/mac`             | shipped via PR #146 (S2 #5): 626 LOC pure-Kai SHA-256/SHA-512 + 83 LOC HMAC                                                    |
| `random_secure`                         | shipped via PR #144 (closes #140): 44 LOC, 2 public fns over `getrandom(2)` / `arc4random_buf`                                 |
| `log` (stdlib minimal)                  | shipped via PR #145 (S2 #7, closes #141): 52 LOC, 4 public fns (`log_debug/info/warn/error`) over a default `Log` handler      |
| `concurrent/nursery`                    | shipped — `pub fn nursery[T, e]` lives in `stdlib/spawn.kai:95` (top-level, not a `concurrent/` subdir)                        |
| `math/real` libm bindings               | shipped via PR #359 (closes #343): sqrt, trig, exp/log, pow, atan2 over libm; `fmod` follow-up via #364 enables `Real % Real`  |
| `encoding/json` Real numbers            | shipped (closes #361): decoder accepts decimals + scientific notation; new `JReal(Real)` variant alongside `JNum(Int)`         |
| `core/tuple` helpers                    | shipped (closes #348): `tuple.swap`, `tuple.map_fst`, `tuple.map_snd`, `tuple.map_pair`, `tuple.first`/`second`/`third`. `fst`/`snd` projections stay field-access only — adding bare `pub fn fst`/`snd` poisons every existing `record.fst` access whose receiver type isn't yet pinned by inference (see module header) |
| `core/list` surface expansion           | shipped (closes #340): `last`, `init`, `partition`, `split_at`, `span`, `chunk`, `windows`, `intersperse`, `enumerate`, `zip3`, `scan`, `group_by`, `find_map`. `group_by` uses Erlang/Elixir consecutive-key semantics; key type is `Int` for v1 (same dispatch limit as `uniq`) |
| `core/string` surface expansion         | shipped partially against #338: `split`, `replace`, `pad_left`, `pad_right`, `lines`, `chars`, `is_blank`. `split(s, "")` panics; `lines("")` returns `[]` (Python/Rust convention). Five proposed helpers (`index_of`, `to_upper`/`to_lower`, `is_empty`, `reverse`) deferred to #396 — each collides on bare name with an existing core export, and `--include-prelude-tests` does not honor the typer's first-arg-type narrowing across modules (resolver fix → surface, mirroring #335 → #336) |
| `array` bridge module                   | shipped (closes #366): top-level `stdlib/array.kai` with `array_from_list`, `array_to_list`, `array_copy`. `random.shuffle` flipped from O(n²) selection-sampling to O(n) in-place Fisher-Yates over a locally-built `Array[T]`; observable row stays `[T] -> [T] / Random` (masking pass drops the inner `Mutable`) |

What's still open (planned-but-not-shipped):

| Module                          | Issue   | Notes                                                                                            |
|---------------------------------|---------|--------------------------------------------------------------------------------------------------|
| `fs/dir` runtime primitives     | #344    | `kai_prelude_dir_*` C bodies + prelude/typer wiring; `stdlib/fs/dir.kai` is a doc-only stub      |
| `fs/file` extras                | #345    | `exists`, `delete`, `rename`, `metadata`, `read_bytes` — not in `stage0/runtime.h` yet           |
| `net/udp`, `net/dns`            | (none)  | Tier S3 — no compiler builtin, no runtime handler, no module file                                |
| `net/http` server-side          | n/a     | belongs **inside** `manutara`, not stdlib (Layout §`net`)                                        |

## Tier plan

Three tiers, ordered by impact on the next two product layers
(`ahu`, `manutara`) and parallelism with the **Anga Roa**
milestone (m11 diagnostics + lsp + repl + reuse-in-place — see
`docs/roadmap.md` §`Anga Roa`).

### Tier S1 — blocks `ahu` / `manutara`, runs parallel with Anga Roa start

Four modules. Each is an independent code path; they can land in
parallel with each other and with Anga Roa's compiler-side work.

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
     blocking ops). DNS uses libc `getaddrinfo` via FFI until
     `net.dns` lands in S3.
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

### Tier S2 — parallel with Anga Roa middle/end

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

- `net/udp` — `NetUdp` effect; `bind`, `send`, `recv`.
- `net/dns` — replaces the libc `getaddrinfo` shim used by
  S1 `net.http`; explicit `NetDns` effect.
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

## Sequencing relative to Anga Roa

```
                  Anga Roa milestone
                  ┌──────────────────────────────────┐
  ──── 0.37.0 ───►│ m11 │ kai lsp │ repl │ reuse-IP │──► 1.0.0 (Orongo)
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
