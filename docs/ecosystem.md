# kaikai ecosystem

The five-project stack that ships kaikai plus the layered libraries
and products that extend it. Pinned 2026-05-11. This doc is the
authoritative reference for triage decisions ("where does feature X
go?") and release marketing ("what comes with kaikai vs what
installs separately?").

## Distribution model

Modeled on Go's distribution: the core language ships **bundled
with stdlib** in a single install. Everything outside core lives
in separate repos under `github.com/kaikailang-org/` and installs
via `kai add`.

The umbrella `kaikailang-org/*` is broader than Go's `golang.org/x/*`
namespace: it covers not only "semi-experimental extensions" but
also complete frameworks and domain toolkits. The line between
core stdlib and `kaikailang-org/*` is **API stability**, not
"experimental vs production".

## The five repos

```
kaikailang-org/
├── kaikai      ← the language (compiler + runtime + core stdlib)
├── ahu         ← platform extension (concurrency, cli, text, crypto-extra)
├── kohau       ← database / persistence
├── henua       ← DDD building blocks
└── hopu        ← fintech platform (payment, ledger, settlement)

(plus producto cliente, NOT under kaikailang-org/)
manutara        ← web framework (LiveView-shaped)
```

### Layer 1 — `kaikai` (the language)

What ships with `brew install kaikai`:

- **Compiler**: `kaic2` self-hosted, drives `cc` for codegen.
- **Driver**: `bin/kai` — `build`, `run`, `test`, `bench`, `check`,
  `fmt`, `lsp`, `init`, `add`, `install`, `update`, `show`, `clean`.
- **Runtime**: `stage0/runtime.h` — cooperative scheduler, Perceus
  RC, primitives.
- **Core stdlib** (`share/kaikai/stdlib/`):

#### Core stdlib inventory (1.0 target)

| Category | Modules |
|----------|---------|
| Primitives | `core/{list, option, result, string, char, int, bool, comparable}`, `array`, `loop`, `random`, `random_secure` |
| Protocols + effects | `protocols`, `effects` |
| Collections | `collections/{map, set, queue, stack}` |
| Math | `math/{numeric, int, real, complex}`, `decimal` |
| Encoding | `encoding/{json, toml, base64, hex}` |
| I/O + system | `fs/{file, dir}`, `os/{process, env}`, `time` |
| Crypto base | `crypto/{hash, mac}` |
| Identifiers + text | `uuid`, `regexp` |
| Observability | `log`, `trace` |
| Networking | `net/{http, tcp}` (+ futuros udp, dns, tls) |
| Finance | `money`, `fx` |
| Testing | `check` (property-based with shrinking) |
| Concurrency wrappers | `spawn`, `actor` (thin wrappers over Spawn/Actor effects) |

### Layer 2 — `ahu` (platform extension)

`github.com/kaikailang-org/ahu`. Installs with `kai add
kaikailang-org/ahu@<version>`.

**ahu is not OTP**. It reshapes the patterns OTP got right
(restart policies, stateful message loops, composable failure
containment) onto kaikai's primitives, without transliterating
features OTP needs only because of Erlang's runtime constraints
(untyped messages, hot code reload, hand-rolled supervision trees).
See ahu's `docs/design.md` §*Why ahu is not OTP*.

ahu is a **single library with multiple internal modules**, not
multiple repos. One `kai add` brings everything:

| Module | Purpose |
|--------|---------|
| `ahu/cell` | Stateful message loops (already shipped 2026-05-02 in ahu-Tongariki MVP) |
| `ahu/restart` | Restart helpers + `restartable_cell` (shipped) |
| `ahu/app` | `run_app` bootstrap (v1 placeholder, shipped) |
| `ahu/jobs` | Background jobs / queue / scheduler — Oban/Sidekiq/Celery analog (post-Tongariki) |
| `ahu/cli` | CLI framework — subcommands, flags, help generation (post-Tongariki) |
| `ahu/text/*` | i18n, unicode normalization, charset encoding (post-Tongariki) |
| `ahu/crypto/*` | Experimental crypto: curve25519, ed25519, NaCl, kdf, post-quantum (post-Tongariki) |

ahu's scope is **infrastructure common to serious applications
that isn't language primitive**. If multiple downstream consumers
(manutara, kohau, henua, hopu) would otherwise reinvent it, ahu
hosts it.

### Layer 3 — Domain toolkits

Each one a separate repo, separate versioning.

#### `kohau` (database / persistence)

`github.com/kaikailang-org/kohau`. The rongorongo tablet — the
substrate that holds inscribed data. Analog: Ecto (Elixir), GORM
(Go).

Scope: ORM, query builder, migrations, multiple backend support
(SQLite, Postgres, MySQL).

Status: not started.

#### `henua` (DDD building blocks)

`github.com/kaikailang-org/henua`. The land/territory — the
domain. Analog: Commanded (Elixir), Eric Evans's DDD vocabulary.

Scope: aggregates, value objects, domain events, event sourcing,
CQRS helpers.

Status: not started.

#### `hopu` (fintech platform)

`github.com/kaikailang-org/hopu`. The Rapa Nui word for
*intercambio / trueque* (exchange / barter) — and the role of the
swimmer-messenger in the Tangata Manu ceremony who transferred
the manutara's egg from Motu Nui to Orongo. Two senses share the
semantic root: **transferring value from A to B**.

Scope:

- Payment processing (gateway adapters, validators)
- Ledgers + double-entry accounting
- Settlements + reconciliation
- Advanced fx (forwards, hedging beyond `stdlib/fx`)
- Regulatory reporting (compliance helpers)
- Idempotency + retry policies
- Webhook handling

Note: `stdlib/money` and `stdlib/fx` stay in core stdlib (basic
building blocks). hopu is the layer **above** them.

Status: not started.

### Layer 4 — Productos cliente

Not under `kaikailang-org/`. Separate identity, separate
ownership trajectory.

#### `manutara` (web framework)

The sooty tern — the bird whose first egg of the season was the
prize at Tangata Manu. Analog: Phoenix LiveView (Elixir).

Scope:

- HTTP server + routing + templating
- LiveView equivalent (server-side state + client-side diff)
- Built on top of ahu

Status: not started.

### Layer 5 — Futuro

Repos that will be created when their domain matures.

| Repo | Tema |
|------|------|
| `kaikailang-org/ai` | LLM clients, embeddings, chains |
| `kaikailang-org/data` | DataFrames, scientific computing |

## Decision rules for "where does feature X go?"

When a new module or API is proposed:

1. **Is it a language primitive?** (effect, syntax sugar, compiler
   feature) → core (`kaikailang-org/kaikai`).

2. **Does it depend on language primitives that just landed?**
   Universal building blocks (list, option, result, json, http
   client, basic crypto) → core stdlib.

3. **Is it infrastructure common to multiple downstream apps but
   not used by every kaikai program?** (concurrency patterns, cli
   framework, i18n, experimental crypto) → `ahu`.

4. **Is it a domain abstraction layer** (database, DDD, fintech) →
   its own `kaikailang-org/*` repo.

5. **Is it a complete user-facing product** (web framework, IDE
   plugin) → repo outside `kaikailang-org/`.

6. **Is it a third-party initiative** → `github.com/<user>/<repo>`,
   installable but not endorsed.

## Versioning policy

- **kaikai**: semver. Major bumps allow API removals; minor bumps
  add features; patch bumps fix bugs. The compiler version and
  core stdlib version are indivisible (`kaikai 1.5.0` means both
  compiler and stdlib at 1.5.0).
- **kaikailang-org/* (ahu, kohau, henua, hopu)**: independent
  semver per repo. NOT coupled to kaikai's version. A given
  `ahu 0.x.y` declares which kaikai range it supports in its
  manifest.
- **manutara + third-party**: independent semver.

## Stability policy (deferred to post-1.0)

Formal stability commitments — what kaikai promises about API
compat across major versions — are pinned post-1.0. The shape of
the commitment (Go-style "1.x forever", Rust-style "edition system",
or something kaikai-specific) is decided when 1.0 ships and we have
real downstream consumers depending on it.

Until then, the working assumption is: **pre-1.0 we may break APIs
between minor versions if the break is small and well-documented;
1.0 onwards we follow whatever policy is pinned at that moment**.

## Promotion paths

### Third-party → `kaikailang-org/*`

Criteria for a popular community package being adopted into the
umbrella:

1. Author transfers ownership or joins the kaikailang-org team.
2. Code quality matches the existing repo it's joining (review
   by maintainers).
3. Demonstrated community need.
4. No overlap with an existing umbrella package.

### `kaikailang-org/*` (ahu) → core stdlib

Criteria for a module in `ahu` being promoted to core stdlib:

1. API stability demonstrated: no breaking changes for ≥ 12 months.
2. Wide use: imported by ≥ 5 packages in `kaikailang-org/*` or
   well-known third-party.
3. Auditable: coverage ≥ 80%, no open `bug` issues > 90 days.
4. Compat with stdlib stability policy (when that policy is pinned).

This avoids the Java trap: stdlib bloated with deprecated APIs
that can never be removed. Anything not provably stable lives in
`ahu` where it can evolve.

## What this doc is NOT

- Not a roadmap. Status fields are snapshots; the real planning
  lives in `docs/roadmap.md`, individual repo READMEs, and GitHub
  issues.
- Not a stability commitment. See §*Stability policy* above.
- Not an exhaustive list of every module. Core stdlib inventory
  in §Layer 1 is the current shape; new modules can be added per
  the decision rules in §*Decision rules*.
- Not authoritative for naming. Rapa Nui naming conventions are
  documented in `~/claude/projects/-Users-ediaz-work-src-github-lnds-kaikai/memory/project_naming_origin.md`.

## Related

- `docs/roadmap.md` — milestone planning.
- `docs/stability.md` (post-1.0) — stability policy when pinned.
- ahu's `docs/design.md` — ahu's own scope decisions.
- Individual repo READMEs — per-package scope.
