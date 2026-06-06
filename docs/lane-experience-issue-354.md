# Lane experience — issue #354: net.udp (NetUdp effect + module + runtime)

**Lane branch:** `issue-354-udp`
**Closes:** #354
**Date:** 2026-06-06
**Template lane:** NetTcp v1 (#68, commit `b1c487e`) — mirrored exactly.

## Scope as planned vs as shipped

Planned (issue #354 + brief):

- `NetUdp` effect with four ops (`bind` / `send` / `recv` / `close`)
  declared as a compiler builtin, with companion types `UdpSocket`
  and `SocketAddr`.
- Runtime default handlers over POSIX `SOCK_DGRAM` (`kai_default_netudp_*`).
- `stdlib/net/udp.kai` module: `local_port`, `bind`, `send`, `recv`,
  `close`, plus a `with_udp` default-handler installer.
- Fixtures: loopback round-trip + bind error path.
- Doc updates (layout, roadmap, effects-stdlib sidebar).

Shipped:

- Everything above **except `with_udp`** — see "Design decisions"
  below. The module instead ships an extra pure helper `addr(host,
  port) : SocketAddr` (constructs a send destination without a record
  literal at the call site).
- The `Net = NetTcp + NetUdp + NetDns` alias was deliberately **not**
  added — it still needs `NetDns` (#352). Folded into this lane only
  as a documented follow-up.

## Design decisions and alternatives considered

1. **`[Byte]` → `[Int]` narrowing.** Inherited verbatim from NetTcp:
   kaikai has no first-class `Byte`, and `resolve_ty` does not
   substitute non-effect aliases, so a `type Byte = Int` would stay a
   distinct nominal that won't unify with `Int`. `[Int]` keeps user
   code compiling. Each element is truncated to a byte before
   `sendto`. Matches the `crypto/hash` byte-array convention.

2. **`SocketAddr.host` is a textual IPv4 dotted-quad.** Same v1
   simplification the TLS lane (#351) takes. `send` parses it via
   `inet_pton(AF_INET, ...)`; `recv` formats the source via
   `inet_ntop`. A structured `IpAddr` is deferred. This keeps the
   record trivially printable (the demo asserts `src.host ==
   "127.0.0.1"`).

3. **Result Err-first.** kaikai's `Result[e, a]` is Err-first /
   Ok-second, so the decl flips the spec's `Result[UdpSocket, String]`
   to `Result[String, UdpSocket]`. `recv` returns `Result[String,
   Pair[SocketAddr, [Int]]]` — kaikai has no tuples, so the spec's
   `(SocketAddr, [Byte])` materialises as a `Pair` record with `fst` /
   `snd` slots (the `Env.vars` precedent).

4. **No `with_udp` installer (the load-bearing surprise).** The brief
   and the issue both call for a `with_udp` default-handler installer
   "following the `with_*` pattern". But:
   - NetTcp — the explicit template — **has no `with_tcp`**. The
     runtime auto-installs the default handler around `main` when
     `/ NetUdp` is in the row; that IS the install path.
   - A kaikai-bodied `with_udp` would have to write
     `handle { body() } with NetUdp { bind(...,resume) ->
     $extern_handler("kai_default_netudp_bind") ... }`. The codegen
     **rejects** `$extern_handler` outside an effect `default { }`
     block — confirmed empirically: `error: compiler intrinsic
     $extern_handler is not valid in this position`. That capability
     is Stage C of #533 (kaikai-bodied default-clause codegen), which
     is closed-but-deferred.
   - The only `with_*` helpers that ride `$extern_handler` today
     (`stdlib/time.kai`'s Clock) do so **inside an effect declaration's
     `default { }` block**, which NetUdp cannot re-declare (the
     compiler owns the decl).

   Decision (lane authority, not escalated — it's a codegen
   constraint, not a product choice): mirror NetTcp exactly. The
   default handler is the install path; no `with_udp` ships. Listed as
   a follow-up gated on Stage C. The brief's `with_udp` ask was by
   symmetry with a pattern that does not in fact exist for the network
   effects.

## Structural surprises the brief did not anticipate

1. **TWO runtime.h copies with DIFFERENT Int representations.** The
   brief said "add to `stage0/runtime.h`". But the production C
   backend (`kaic2`) compiles against `stage2/runtime.h` (resolved by
   `-I .` ahead of `-I ../stage0`), a *separate* runtime carrying the
   Koka-style Perceus RC. **stage2 boxes Ints as tagged immediates**
   (`kai_tagged_int`), so `port->tag == KAI_INT` / `port->as.i`
   dereferences a tagged pointer (`0x1` for `kai_int(0)`) → SEGV. The
   stage2 NetTcp handlers read Ints via `kai_is_int(v)` / `kai_intf(v)`
   accessors. The fix: the NetUdp block had to be added to **both**
   runtimes, and the stage2 copy must use the tagged-aware accessors
   for every `Int` read (`port`, `max`, byte-list elements,
   record-field ports). The stage0 copy keeps `->tag`/`->as.i` (stage0
   has no tagging). This cost the bulk of the debugging time — the
   crash was *before* the handler body ran (in the arg, not the code),
   and ASan reported `SEGV on address 0x5` with no symbolised frame.
   A first-line `fprintf` showing `port=0x1` was what cracked it.

2. **The compiler edit is smaller than the brief implies.** NetUdp is
   intentionally NOT pinned into `builtin_default_install_order()`
   (emit_c.kai) or `lvm_builtin_default_install_order()` (emit_llvm.kai,
   owned by #747). Its auto-generated `default { }` block (every clause
   bridging via `$extern_handler`) means it installs through the
   `user_effects_in_row` path in **both** backends. Result: zero edits
   to emit_c.kai / emit_llvm.kai — the shim, setup, and teardown all
   fall out of the existing AST walk. The only compiler-source edit is
   `driver.kai` (the effect registry), exactly as the brief scoped.

3. **`runtime_llvm.c` is in-lane, not a compiler file.** The LLVM
   backend references `kaix_default_netudp_*` forwarders (non-static
   mirrors of the static `kai_default_*` in stage0/runtime.h). These
   live in `stage0/runtime_llvm.c`, which is runtime, not the
   off-limits `emit_llvm.kai`. Added the four forwarders there.

## Fixtures added and coverage gaps

- `demos/net_udp_localhost/main.kai` + `main.out.expected` — the
  CI-exercised fixture (rides the demos baseline, which tier1-asan
  runs). bind 127.0.0.1:0 → local_port → send PING to self → recv
  (asserts byte shape + source host) → close. `demos/baseline.txt`
  bumped 34 → 35.
- `examples/effects/net_udp_localhost.kai` + `.out.expected` — mirror
  copy, same NetTcp precedent (the NetTcp lane kept both).
- Verified on **both** backends byte-identical (C + LLVM) and clean
  under ASan+UBSan on both.

Coverage gaps / not exercised by a committed fixture:

- The `send`-to-unbound-port error path the issue's acceptance lists
  (Linux ECONNREFUSED-on-next-recv vs macOS). UDP send to an
  unreachable port does **not** reliably error synchronously (no
  connection), so a deterministic golden is platform-dependent — left
  out to keep the golden stable. The bind error path (bind to a
  non-local IP → `Err`) was verified manually but is also
  platform-timing-sensitive, so not committed as a golden.
- `recv` of a zero-length datagram (legal, returns `Ok(Pair { snd:
  [] })`) is implemented but not fixtured.

## Real cost vs estimate

The mechanical work (driver.kai registry + two runtime blocks + module
+ fixtures + docs) was straightforward mirroring. The entire cost
overrun was the **two-runtimes / tagged-Int** trap (surprise #1):
~60% of the lane was spent localising a SEGV that turned out to be a
copy-paste of stage0's `->as.i` into a stage2 context where Ints are
tagged. The lesson generalises to any future runtime handler that
reads an `Int` argument: **stage2 handlers must use `kai_is_int` /
`kai_intf`, never `->tag == KAI_INT` / `->as.i`.**

## Follow-ups left for next lanes

- **`Net` alias** (`type Net = NetTcp + NetUdp + NetDns`): lands when
  #352 (NetDns) also closes. Both atomics will then exist; add the row
  alias to `stdlib/effects.kai`.
- **Reactor for UDP**: the m8.x readiness reactor lifts NetTcp and
  NetUdp blocking ops together. Today NetUdp is blocking on the OS
  thread (matches pre-R2 NetTcp).
- **`with_udp`**: ships once Stage C of #533 (kaikai-bodied
  default-clause codegen) lands and `$extern_handler` is accepted
  inside a `handle ... with` block.
- **IPv6 / multicast / connected UDP**: explicitly out of scope per
  the issue; separate lanes.
