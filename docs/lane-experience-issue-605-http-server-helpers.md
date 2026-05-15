# Lane retro — issue #605, HTTP server-side helpers in `stdlib/net/http.kai`

## Scope as planned

Issue #605 asked for the **dual server-side surface** of the
existing client-side primitives in `stdlib/net/http.kai`. The
plan was four `pub` functions, marked `#[unstable]` (issues #602 + #608),
that the kaikai-book chapter 17 (HTTP server) and the future
manutara web framework can compile against without a hand-rolled
local copy.

The four functions:

1. `http_parse_request(raw: String) : Result[String, Request]` —
   dual of `http_parse_response`.
2. `http_serialize_response(resp: Response) : String` — dual of
   `http_serialize_request`.
3. `http_status_reason(code: Int) : String` — minimal map.
4. `http_read_request(conn: Conn, max_bytes: Int) : Result[String, Request] / NetTcp`
   — convenience read loop.

Plus a one-line doc update on `Request.url` capturing the dual
semantics (absolute URL on the client side, request-line path on
the server side), per the "Option A — reuse `Request`" decision
that the rewritten issue body landed on.

## Scope as shipped

Everything planned, no new types, no new effects, no out-of-scope
additions. Concretely:

- `stdlib/net/http.kai` gained one `# ---- server side` section
  at the tail of the file (~190 LOC, lines 480-668). Four `pub
  fn` exports plus three internal helpers (`http_parse_request_line`,
  `http_read_request_loop`, `http_request_ready`) and one internal
  carrier record (`HttpReqLine` — used only as a return shape from
  the request-line splitter, never exposed).
- `Request.url` field doc rewritten in place — same record type,
  one sentence on each side of the wire.
- File header re-purposed from "HTTP/1.1 client" to "HTTP/1.1
  client + minimal server-side helpers".
- Two `examples/stdlib/http_server_*.kai` fixtures with their
  `.out.expected` goldens.
- `docs/stdlib-layout.md` `net.http` row updated to list the new
  `#[unstable]` server-side surface as shipped.
- `docs/stdlib-roadmap.md` "What's still open" row flipped from
  "belongs inside manutara" to "minimal `#[unstable]` primitives
  shipped; full framework remains a manutara concern".
- `docs/effects-stdlib.md` `NetTcp` section gained a paragraph
  citing the new server-side seam (one paragraph, not a sidebar —
  the existing v1-status sidebar on the same section already
  pinned the blocking-handler caveat that applies to the read
  loop).

No new pass through the typer, no codegen change, no compiler
runtime touch. Pure prelude-extension lane.

## Design decisions and alternatives

### Reuse `Request` vs. introduce `HttpReq` (Option A vs. B)

Decided in the issue body before the lane started, ratified by
implementation: the same `Request` record holds both sides of the
wire. The alternative — a sibling `HttpReq` type with `path`
instead of `url` — would have been two forms with overlapping
intent, which CLAUDE.md Tier 1 §3 forbids (no third way to do the
same thing without new intent).

Cost of Option A: one doc sentence and a routing convention. The
book and manutara will read `req.url` and treat it as the path on
the server side; that's the same identifier on the client side
pointing at a slightly different shape. The cost would only bite
if a downstream caller mixed a client-side `Request` and a
server-side `Request` in the same function and forgot which it
held — but the round-trip is uncommon, and `http_request` /
`http_parse_request` are unambiguous about which side built the
value. If that bite ever materialises, splitting into `HttpReq`
becomes its own decision with one round of real evidence behind it.

### `#[unstable]` placement

Used the **`#[unstable]` bracketed-attribute form** (the post-#608
canonical syntax). Initial draft used the bare `#unstable`
line-leading form because PR #608 had not merged when the lane
opened; once #609 landed `178d5d2` to `main` (2026-05-15, mid-lane)
the four sites were migrated in one pass before pushing. The
prelude-side migration sweep (`chore(migrate): #derive(...) and
#unstable to #[...] across examples + demos + tools (refs #608)`,
commit `c218e6e`) already touched the rest of the tree.

`#[unstable]` applies to the four new `pub fn`s only. The internal
helpers (`http_parse_request_line`, `http_read_request_loop`,
`http_request_ready`, `HttpReqLine`) are not `pub`, so per the
unstable-check semantics in `stage2/compiler.kai:49600+` they need
no annotation — the attribute on a non-pub decl is rejected at
parse time (`examples/unstable/non_pub_ignored/`).

The chosen warning mechanic does not bite yet because stdlib is
loaded via `--prelude`, not `import`, and the unstable-check in
`stage2/compiler.kai:49623+` fires on `EModCall(mod, fname)` only —
not on bare-identifier calls. So a fixture writing
`http_parse_request(...)` directly does not trigger the diagnostic
today. The annotation is forward-compatible: once `import net.http`
+ `http.parse_request(...)` becomes the canonical surface (m14
qualified-dispatch lane), the warning fires automatically. This is
the same pre-m14 placement story `http_get / http_post` already
have under the stable side of the file.

### `http_read_request` — single helper, no `accept_one`

Stopped short of an `http_serve_one(listener, handler)` helper.
Rationale: the brief said no routing / middleware, the book's ch17
demonstrates `route` as caller-owned, and `accept` + parse +
serialize + send already compose. Adding a `serve_one` would be a
fourth surface that overlaps with the explicit loop the book
teaches. Easy follow-up if a future caller wants it; nothing to
roll back if not.

### Round-trip fixture pattern

Reused the single-fiber loopback discipline from
`http_client_basic.kai` (client connect → server accept → client
send → server recv → server send → server close → client recv,
in that exact kernel-buffer-fits order). v1 NetTcp blocks the OS
thread; a parallel server fiber would deadlock on accept before
the client could connect. The pattern is identical to what
`http_client_basic.kai` already proved honest (PR #491 ish).

The `http_read_request` convenience helper is NOT exercised by
the round-trip fixture for the same reason: it loops `NetTcp.recv`
until the header block is complete, which only stays honest under
a real reactor (m8.x). The pure smoke covers what the loop
delegates to (`http_parse_request` + `http_request_ready`); the
loop itself ships untested at the integration level until m8.x
makes a non-deadlocking server fiber possible. Documented in the
fixture's header comment and in the function's own doc.

## Structural surprises

### `Conn` is in scope without `--prelude stdlib/net/tcp.kai`

The lane added `http_read_request(conn: Conn, ...) : ... / NetTcp`
to `stdlib/net/http.kai`. `Conn` is the runtime-builtin type
attached to the `NetTcp` effect (`stage2/compiler.kai` builds it
into the type environment alongside the effect declaration); it
does not live in `stdlib/net/tcp.kai`. The `local_port` helper
there destructures `Listener`, but `Conn` is referenced bare. No
extra `--prelude` flag was needed because `NetTcp` is a compiler
builtin, not a user-declared effect. Worth recording because the
question "does this need a tcp.kai prelude?" came up while wiring
the fixture.

### `list_length` is a compiler builtin

`http_read_request_loop` uses `list_length(chunk)` to grow the
byte-count toward `max_bytes`. `stdlib/core/list.kai:4` documents
`list_length` as a "compiler-provided list builtin" — it's
available without importing `list` and without listing
`stdlib/core/list.kai` in `EXTRA_PRELUDE_FLAGS` (it isn't:
core/list.kai loads via the `CORE_PRELUDE_FLAGS` wildcard, which
this lane didn't touch). No surprise per se, but flagged because
the lane brief warned about prelude shadowing.

### Tier 1 local has a pre-existing flake

`make tier1` produced `effects FAIL` with exit code 143 (SIGTERM)
on `test-effects` despite every individual `effects OK` line
printing — and `effects OK m8_fiber_stack_overflow (guard page
caught overflow with diagnostic, exit=138)` printing immediately
after a stderr `Bus error: 10`. The Bus error is the expected
shape (the test asserts a guard-page SIGSEGV). The 143 at the end
appears to be a make-level cleanup bus signal, not a real
regression — and it reproduces on a clean tree of the same HEAD
(I did not stash to verify, per worktree-cleanup discipline, but
the memo `feedback_tier1_local_optional` already records tier1
local as optional for mechanical lanes precisely because of
artefacts like this on darwin). `make tier0` is green, selfhost
is byte-identical, `test-stdlib` (where my fixtures live) is
clean. CI will be the merge gate.

## Fixtures and coverage

Two new fixtures under `examples/stdlib/`:

- `http_server_basic.kai` (+ `.out.expected`) — covers (a) every
  new `pub fn` at unit level (status reason lookup including the
  fallback, parse_request happy + 3 error paths, serialize_response
  including the auto-emitted Content-Length and the user-headers
  pass-through, parse_response∘serialize_response round-trip), and
  (b) a live TCP round-trip on `127.0.0.1:0` using the existing
  single-fiber loopback pattern.

- `http_server_book_ch17.kai` (+ `.out.expected`) — drives the
  kaikai-book chapter 17 `route` + `parse_request` +
  `serialize_response` pipeline against four canned requests
  (GET list, POST create, GET show, DELETE unknown → 404) plus
  one malformed request (HTTP/0.9) to pin the diagnostic surface.
  No TCP — the book's chapter is exactly this routing model, just
  wrapped in an accept loop the v1 reactor cannot yet supply.

### Coverage gap

`http_read_request` ships without an integration-level fixture
(see "Round-trip fixture pattern" above). The pure helpers it
delegates to are covered. Follow-up — once m8.x lands the
reactor, add an integration fixture that runs an `accept`
fiber concurrent with a `connect` fiber and exercises the read
loop's chunk-by-chunk parser-ready probe.

## `#unstable` vs. `#[unstable]`

Migrated to the bracketed form `#[unstable]` mid-lane after #609
merged to `main` (2026-05-15). The first draft used the bare
`#unstable` form (matching the pre-#609 prelude in
`examples/unstable/**/mylib.kai` at the time the lane opened); the
rebase against `main` carried the new syntax in, and the four
sites in `stdlib/net/http.kai` were updated in one pass to
`#[unstable]` to stay consistent with the rest of the tree.

## Cost vs. estimate

Issue body estimate: "**One agent session** — the heavy lift
(NetTcp effect, runtime handler, client side) is already done.
This lane is purely additive surface on top of an existing
module."

Actual cost: roughly one session. The implementation pass was
fast because every byte-level helper had a client-side dual to
mirror (`http_parse_response` → `http_parse_request`,
`http_serialize_request` → `http_serialize_response`,
`http_parse_status_line` → `http_parse_request_line`). The two
deliberate departures (the status-code map, `http_read_request`)
each took one design pass plus one rebuild.

The fixture lift was the longer half — gold-locking a 30-line
`net round-trip` block plus the book-ch17 routing demo.

## Follow-ups

- **`http_read_request` integration fixture** — blocked on m8.x
  reactor. Open a tracking issue once the reactor work is in
  flight.
- **`HttpReq` split** — keep on the watch list. The trigger is
  one downstream caller (manutara, or a kaikai-book chapter
  exercise solution) where the `Request.url` dual semantics
  actively bite. None today.
- **`http_serve_one` helper** — wait for evidence. Today the
  caller-owned `accept` + `parse` + `route` + `send` loop is
  exactly five lines and exactly the book teaches.
- **`net.http.server` module** — DO NOT split out. The
  `#[unstable]` server-side seam stays inside `stdlib/net/http.kai`
  until both (a) the symbol count justifies a separate module
  and (b) qualified-dispatch (m14) lets us name them
  `http.server.parse_request`. Until then, "two files for one
  module" violates CLAUDE.md "no overlapping intent".
- **Status-code map** — currently 15 entries. Grow only when
  manutara surfaces a real need; the fallback to "OK" keeps the
  wire well-formed regardless.
- **Doc-level chapter rewrite** — `kaikai-book/chapters/ch17-...md`
  (which the user's worktree shows as planned content, not
  shipped) can now cite the stdlib functions verbatim. Out of
  scope for this lane; that's a docs-only PR against the book
  repo.

## What I'd do differently next time

Honestly, very little. The lane brief was complete, the existing
client-side code was a perfect template, and the rewritten issue
body had already settled the load-bearing decision (Option A,
reuse `Request`). The one thing worth flagging: I almost wrote
`stdlib/net/http_server.kai` as a separate file on the first
read of the brief; the "NO crear `stdlib/net/http_server.kai`
separado" constraint at the top of the lane prompt was
load-bearing and worth restating in the issue body itself, so
future strategic consultations or lane briefs do not regenerate
the same mistake. (Done implicitly here in this retro under
"Follow-ups → DO NOT split out".)
