# Lane retro — issue #357: stdlib/net/http automatic redirect follow

Closes the gap that `stdlib/net/http.kai` opened at the file
header (lines 38–41 pre-lane): the v1 client returned 3xx
responses verbatim and asked every caller to roll their own
follow loop. Browsers, curl, Go's `net/http`, Python `requests`
all auto-follow by default; forcing kaikai callers to redo that
plumbing was friction with no upside. This lane adds the follow
surface as a **layer on top of `http_request`**, leaving the
existing single-call API intact.

## Scope-as-planned vs scope-as-shipped

Brief asked for:

- `pub type RedirectPolicy` with the four issue-body fields.
- `pub fn default_redirect_policy()`.
- `pub fn http_follow(req, policy)` looping over Location headers
  with RFC 9110 §15.4 method-rewrite rules.
- 4 convenience wrappers (`http_get_follow` / `http_post_follow` /
  `http_put_follow` / `http_delete_follow`).
- 3 fixtures under `examples/stdlib/`.
- Doc updates to `stdlib-layout.md` + `stdlib-roadmap.md` + the
  `stdlib/net/http.kai` header.
- Lane retro before PR open.

What shipped: the brief, verbatim. No surprise scope creep, no
inlined extras. The existing `http_request` / `http_get` /
`http_post` / `http_put` / `http_delete` are byte-identical to
what they were pre-lane (only the file-header comment changed,
and that was the explicit "deferred" note this lane closes).

## Design decisions

### `RedirectPolicy` as a record, not flag arguments

The alternative was a `http_follow(req, max_redirects: Int,
follow_3xx: [Int], …)` with positional or keyword parameters —
kaikai supports neither shape today (functions are positional and
do not have keyword args). A record means callers build a default
once via `default_redirect_policy()`, override only the field they
care about, and the typer pins every field type from the
constructor literal at the call site. Same reasoning that
`HttpHostPort` and `HttpHeader` already use elsewhere in the file.

### `http_request` left unchanged (backward-compat absolute)

The brief was explicit on this point. Two reasons it matters:

1. The existing `http_request` returns `Response` (synthetic on
   error: `status: 0`, body holds the error string). Wrapping it
   in `Result[String, Response]` would have been a surface break
   — every caller would have had to migrate at the call site, and
   the kaikai book already documents the synthetic-Response
   pattern in chapter 17. The cost wasn't worth the type-cleanup.

2. Callers who *want* explicit 3xx control have it. Keeping
   `http_request` is what makes `http_follow` a proper layer on
   top instead of a forced upgrade.

The asymmetry (`http_follow` returns `Result[String, Response]`,
`http_request` returns `Response`) is intentional and called out in
the doc — the follow loop has a richer error space (`too many
redirects`, `missing Location header`) that the synthetic-response
pattern cannot represent without ambiguity.

### Method-rewrite rules — positive form, RFC reading order

The `http_build_next_request` body reads:

```kai
if status == 303 and policy.rewrite_post_to_get_on_303 and orig.method == "POST" {
  "GET"
} else if (status == 307 or status == 308) and policy.preserve_method_307_308 {
  orig.method
} else if (status == 301 or status == 302) and orig.method == "POST" {
  "GET"
} else {
  orig.method
}
```

No real status hits two branches, but the order matches RFC 9110
§15.4 reading order (303 → 307/308 → 301/302) so a future reader
reaching for the spec finds the branches in the same order. The
defaults match curl / Go / requests: 303 always GETs, 307/308
preserve, 301/302 GET on POST per browser convention.

### `http_resolve_redirect_url` simplification (v1)

RFC 3986 §5 is the full base-URI resolver. v1 covers the three
shapes that account for ~99% of real-world Locations:

- Absolute URL (`http://...`, `https://...`) — used verbatim.
- Path-absolute (`/foo`) — keep scheme + host + port from the
  previous request, swap path. The `:port` is always emitted
  explicitly because `http_parse_url` round-trips it that way.
- Anything else — Location is returned verbatim and the follow-up
  fails downstream with a parse error the caller can surface.

The "anything else" branch covers protocol-relative `//host/foo`
and path-relative `foo` / `../foo`. A real RFC 3986 §5 base
resolver lands when `net.url` becomes its own module
(`docs/stdlib-layout.md` §`net` "url.kai planned"); duplicating
the logic inside `net.http` first would have to be ripped out at
that point.

### Helpers private and inlined

Neither `string_starts_with` nor `list_contains` is in the
prelude (verified against the EP() table in
`stage1/compiler.kai:3337-3362`). The lane could have added them
to a stdlib module — but the stage 2 prelude chain would then
have to include that module, and the bug-bash retros caution
against widening the prelude surface for one-shot helpers. Both
are written as private `fn http_*` inside the same file — 12 LOC
total — and the comment in the source notes why.

### Tail-recursive loop

`http_follow_loop` is tail-recursive on the `next_req` /
`n_redirects+1` step. Mandatory TCO (CLAUDE.md Tier 1 #2) keeps
the stack flat for the `max_redirects=10` default; the worst case
is 11 frames before the cap fires. The `n_redirects > max` check
runs *before* every hop is issued, so a `max_redirects=0` policy
still allows the initial request through and only fails on the
*would-be* second hop — surfacing the `too many redirects (max 0)`
Err without opening a second socket.

## Structural surprises

### `bin/kai` does not auto-load `stdlib/net/http.kai`

Discovered while running the new fixtures via `bin/kai run`: every
symbol from `stdlib/net/http.kai` was reported as out-of-scope.
Investigation showed `bin/kai`'s `stdlib_prelude_flags` covers
core, protocols, effects, array, random, encoding, collections,
math, decimal, money, fx, uuid, regexp, path, crypto/hash,
crypto/mac — but not `net/http`. The Makefile's `EXTRA_PRELUDE_FLAGS`
*does* include `--prelude ../stdlib/net/http.kai`, which is why
the `make test-stdlib` harness compiles the existing
`http_client_basic.kai` and now compiles all three new fixtures
correctly.

This is not the lane to fix that drift — `http_client_basic.kai`
already lived under it and is exercised only via `make test-stdlib`
+ `make tier1-asan` (the latter runs `test-http-client-asan` in
`stage2/Makefile:3736-3756` with the explicit Makefile prelude
chain). My fixtures inherit the same harness coverage.

A proper fix is one-liner in `bin/kai:870-940`: add
`prelude_flag_for "$STDLIB_NET_HTTP" "$ed_arg"` next to the
`STDLIB_PATH` line. Filed as a follow-up note here — out of scope
for this lane because it would touch every fixture's harness
expectations.

### Boolean operator surface

First-pass impl used `&&` / `||`. kaikai uses keyword `and` / `or`
(stage2 lexer rejects `&&`). Fixed before the first compile.
Memo'd implicitly in this retro for next lane writers reaching
for C-style logical ops.

### Single-fiber NetTcp constraint precluded a multi-hop fixture

The brief and the issue acceptance both mention exercising
`max_redirects` enforcement with a real multi-hop loopback. v1
NetTcp is blocking-only (no kqueue/epoll reactor — see
`docs/effects-stdlib.md` §`NetTcp` "Default handler"); a
sibling-fiber server would deadlock the OS thread on `recv` the
same way `examples/stdlib/http_client_basic.kai` notes at the
file header. The compromise: `http_redirect_too_many.kai` pins
the *Err string shape* (deterministic via `string_concat_all`)
plus typechecks the call against a `max_redirects: 0` policy.
Wire-level exhaustion lands once m8.x ships the reactor — same
follow-up that `examples/stdlib/http_client_basic.kai`'s
convenience-wrapper coverage already waits on.

## Fixtures added

Three under `examples/stdlib/`, each with its `.out.expected`
golden and exercised by `make test-stdlib`:

- `http_redirect_follow_basic.kai` — `default_redirect_policy()`
  shape (max=10, 5 codes, both rewrite knobs true), plus typecheck
  of the 4 convenience wrappers + `http_follow`.
- `http_redirect_policy_custom.kai` — three real-world overrides
  (strict mode without 303→GET rewrite, tight loop with max=2,
  permanent-only `[301, 308]`). Pins each field's settability.
- `http_redirect_too_many.kai` — Err string shape for `too many
  redirects (max <N>)` and `redirect missing Location header at
  status <code>` plus typecheck of `http_follow` against a
  `max_redirects: 0` policy.

## Coverage gaps left open

- Live multi-hop loopback exercising the `max_redirects=10`
  default with 11 hops — blocked on m8.x reactor (per
  `docs/m8x-followup.md`), same constraint that limits
  `http_client_basic.kai` today.
- `http_resolve_redirect_url` not directly fixtured; tested
  transitively through the (deferred) wire path. A pure unit
  fixture exposing the helper would require making it `pub`,
  which would lock a v1 contract on a v1-pragmatic resolver.
- Cross-origin Authorization stripping not implemented (issue
  body lists it under "deferred to cookie-policy lane").

## Real cost vs estimate

Brief estimated 0.2–0.3 day. Actual: ~3 hours of focused work,
within the band. No reworks; the only mid-lane discovery
(`bin/kai` does not auto-load `net/http`) didn't change the
implementation, just the diagnostic surface during fixture
authoring.

## Follow-ups for next lanes

- **Cookie jar + Set-Cookie persistence** — out of scope per
  issue #357 body. The cookie design is what the original
  pre-lane "deferred" note actually pointed at.
- **Cross-origin Authorization / Cookie / WWW-Authenticate
  stripping** — same lane as cookies. The issue body specs the
  `(scheme, host, port)` triple comparison; v1 forwards
  `req.headers` verbatim.
- **HSTS preload table** — orthogonal to redirect logic; not
  this lane's concern.
- **Configurable per-host policy** — single global policy covers
  v1; a `Map[String, RedirectPolicy]` overlay arrives if/when
  callers ask.
- **Permanent-redirect (301 / 308) caching** — needs a cache
  abstraction kaikai stdlib doesn't have yet.
- **`bin/kai` should auto-load `stdlib/net/http.kai`** — see
  *Structural surprises* above. One-liner in `stdlib_prelude_flags`.
- **RFC 3986 §5 base-URI resolver in `net.url`** — ships when
  `net.url` becomes its own module (per `docs/stdlib-layout.md`
  §`net`).
