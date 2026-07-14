# Changelog

All notable changes to kaikai are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
This project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html);
prior to 1.0.0 minor versions may break backwards compatibility (see CLAUDE.md
"Backward compatibility — not promised until post-MVP").


Core language closed under the revised 3-level gate (`docs/stage2-design.md`
§"Update 2026-04-27 (post-REOPEN final)").

### Gate met

- **Level 1 (mechanical)**: `make selfhost` + `make -C stage2 selfhost-llvm`
  byte-identical fixed point; `make test` clean (incl. `test-demos-core`
  and `test-aspirational`).
- **Level 2 (audit + invariant verifier)**: every `_ -> k` wildcard in
  every AST walker pass justified or explicit (`7bd5a68`); rep invariant
  (b) — protocol-dispatcher references outside enclosing binders — runs
  on every compile via `validate_typer_invariants` (`0afc2a9`).
- **Level 3 (demo gate)**: `make demos-no-regression` baseline 18 OK
  + PASS demos in `demos/`; `make -C stage2 test-aspirational` runs
  `examples/aspirational/event_logger` (custom Logger effect + handler
  + effectful HOF callbacks + sum-type-with-payload `#derive(Show)`).

### Added (across the build-up to Core)

- Stages 0 / 1 / 2 (m1-m6), all with `selfhost-llvm` fixed-point green.
- Effects + handlers + parametric effects + per-instance dispatch
  (m7a / m7b / m7c).
- Ergonomic sugars: trailing lambdas, `@cap`, `var`, `a[i]`, aliases,
  `todo!`, record punning, `@` as-patterns, pipeline `_`, `++`,
  `!` postfix on `Option` / `Result` (m7b / m7d / m7e §13).
- Fibers, structured concurrency, actors with supervision (m8 v1 type
  surface; runtime is inline-eager — real scheduler is m8.x).
- Typed holes (m10).
- Perceus basic memory management (m5 rounds 1-3) + capture incref
  (m5.x #2). Linear-consumption runtime closed in v0.2.0.
- Units of measure (m12.5).
- Single-dispatch protocols + 5 stdlib protocols + `#derive(Show, Eq,
  Hash, Ord)` for records and sum types (m12.8 + m12.8.x + m12.8.z).
- Atomic per-stream effects (`Stdout` / `Stderr` / `Stdin`) with
  `Console` and `Io` aliases (m12.8 Phase 4b).
- Parametric `impl[u: Unit] Show for Real<u>` (m12.8 Phase 2).
- m12 self-host checkpoint: byte-identical fixed-point of
  `kaic2 stage2/compiler.kai`.
- Both backends (`--emit=c`, `--emit=llvm`) at parity for every shipped
  feature.

### Notes on remaining structural debt

The runtime still carries two of the three structural deficits audited
on 2026-04-28 (Linus + Eric). After v0.2.0 the third one (RC fictional)
is closed:

1. ~~RC fictional at runtime.~~ **Closed in v0.2.0 by R1 Phase 3.**
2. **Fibers do not suspend.** m8 v1 is inline-eager — `Spawn.spawn`
   runs the thunk synchronously, `await` is identity, `select` cannot
   interleave, `Cancel` cannot be delivered (no yield points). The
   BEAM-style structured-concurrency claim is type-check-only.
   Target of the **R2** lane (m8.x real fiber scheduler).
3. **m4c monomorphisation is an identity pass.** The
   `try_rewrite_show_dim_real` shortcut in m12.8 Phase 2 confirms it;
   polymorphics with `EHandle` in the body collide on `clause_fn_name`.

[Unreleased]: https://github.com/lnds/kaikai/compare/v0.8.0...HEAD
[0.8.0]: https://github.com/lnds/kaikai/compare/v0.7.2...v0.8.0
[0.7.2]: https://github.com/lnds/kaikai/compare/v0.7.1...v0.7.2
[0.7.1]: https://github.com/lnds/kaikai/compare/v0.7.0...v0.7.1
[0.7.0]: https://github.com/lnds/kaikai/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/lnds/kaikai/compare/v0.5.1...v0.6.0
[0.5.1]: https://github.com/lnds/kaikai/compare/v0.5.0...v0.5.1
[0.5.0]: https://github.com/lnds/kaikai/compare/v0.4.1...v0.5.0
[0.4.1]: https://github.com/lnds/kaikai/compare/v0.4.0...v0.4.1
[0.4.0]: https://github.com/lnds/kaikai/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/lnds/kaikai/compare/v0.2.2...v0.3.0
[0.2.2]: https://github.com/lnds/kaikai/compare/v0.2.1...v0.2.2
[0.2.1]: https://github.com/lnds/kaikai/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/lnds/kaikai/compare/v0.1.3...v0.2.0
[0.1.3]: https://github.com/lnds/kaikai/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/lnds/kaikai/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/lnds/kaikai/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/lnds/kaikai/releases/tag/v0.1.0

## v0.99.15 (2026-07-14)

### Fixed

- **runtime**: consistent atomicity on thread-shared rc fields (part of #1207) (#1226)

### Changed

- **cell-promote**: lower a non-literal pure-scalar var init to a raw register (closes #1214) (#1227)

## v0.99.14 (2026-07-14)

### Fixed

- **emit**: stop the C bootstrap seeding builtin nullary variants as immortal cells (#1225)

## v0.99.13 (2026-07-14)

### Fixed

- **cli**: resolve kai upgrade via tags, not releases; graceful rate-limit handling (closes #1223) (#1224)
- **runtime**: deep-copy the spawn thunk across threads — close the non-atomic-RC race on the closure (part of #1207) (#1218)
- **pkg**: accept git/tag dep aliases and error loudly on unknown forms (#1217)

### Changed

- **mono**: ground return-only tparams from the resolved call-site return type (closes #1208) (#1222)

## v0.99.12 (2026-07-13)

### Fixed

- **runtime**: lazy-materialise the RC free-list pools — restore ~2 MiB idle RSS (closes #1212, closes #1213) (#1216)

## v0.99.11 (2026-07-13)

### Added

- **runtime**: M:N work-stealing scheduler with copy-on-cross-thread send (part of #1207) (#1211)

## v0.99.10 (2026-07-13)

### Changed

- **mono**: specialise higher-order calls on known pure closures — inline the stage into the loop (closes #1205) (#1209)
- **runtime**: classify runtime state for M:N — TLS partition + immutability gate + isolated reactor (part of #1207) (#1210)

## v0.99.9 (2026-07-13)

### Changed

- **emit**: per-module native codegen with content-addressed object cache (closes #989) (#1204)
- **ci**: route shard-1 self-hosts through the cached c-modular path (#1206)

## v0.99.8 (2026-07-12)

### Fixed

- **emit**: incref owned guard-chain arm binders in native KIR lowering (closes #1201) (#1203)

## v0.99.7 (2026-07-12)

### Added

- **kinds**: public Composition theory + Layout kind with byte-exact encode/decode (#1199)

### Fixed

- **perceus**: converge native/C on the region RC/arena model (closes #1197) (#1198)

## v0.99.6 (2026-07-11)

### Fixed

- **typer**: resolve reassigned record var's field read inside interpolation (closes #1194) (#1196)
- **emit**: route native region ctors to the arena (closes #1193) (#1195)

## v0.99.5 (2026-07-11)

### Added

- **kinds**: kind Region — arenas as a kind over the Structural theory (closes #1123) (#1192)

### Fixed

- **typer**: roll back terminal-fusion purity-probe deferred fields (closes #1186) (#1190)

## v0.99.4 (2026-07-11)

### Fixed

- **refinements**: enforce refinement predicates at binding, param, and return downcasts (closes #1169) (#1171)

### Changed

- local scalar var costs a register — slot to raw cell on both backends (closes #1179) (#1188)
- **runtime**: lazy [a..b] ranges — O(1) representation of [Int], zero-materialisation consumers (closes #1180) (#1185)
- **emit**: C backend erases the State handler for loop-local vars (#1179 gap 1) (#1184)
- **kir**: let the dead-closure sweep clear any-capture closures so var->slot fires after inlining (closes #1173) (#1175)
- **kir**: small-function inliner on lowered KIR — reuse-class templates, loop fusion, var-cell slots (closes #1158) (#1172)
- **emit**: cross-call reuse-token donation + move reuse on KIR/native (closes #1164) (#1170)

## v0.99.3 (2026-07-10)

### Added

- **vec**: Vec surface stage 3 — O(1) slices, literal minting, fused pipeline collect (#1159)

### Fixed

- **emit**: re-land the write-side raw ctor-slot permit + root-cause the native self-compile escape (#1156) (#1161)

### Changed

- **emit**: de-opaque %KaiValue in the native emitter — inline tag tests, static-kind slot reads, tagged compares (closes #1160) (#1165)
- **emit**: full variant_slot_mask parity on native — enum and Real slots lay raw (closes #1157) (#1162)
- **runtime,kir**: sized-exact variant blocks + static-kind slot tests + inlined native ctors (closes #1136) (#1155)

## v0.99.2 (2026-07-09)

### Added

- **runtime**: Vec[T] stage 2 — raw element paths, borrow reads, qualified self-call TCO fix (#1147)
- **runtime**: Vec[T] stage 1 — flat value vector, uniqueness in-place, unboxed storage (#1142)
- **match**: range patterns — `case lo..hi ->` matches an integer interval (#1141)
- **perceus**: call_ind_borrow — borrowing indirect call unblocks the closure/HOF borrow (#1130) (#1132)
- the kind engine over AbelianGroup — user kinds, per-kind isolation, use-site resolution (#1108) (#1133)

### Fixed

- **runtime**: Vec push capacity grows only at capacity — recursive push-loop OOM (#1152)
- **kir**: test nested ctor payloads in cons-arm heads and record fields on native (#1149)
- **perceus**: modular-safe relaxed borrow inference (closes #1131) (#1138)
- **perceus**: drop the re-threaded Array container in the read loop (#1126) (#1128)

### Changed

- **perceus**: module-call consumers are linear; inline trivial forwarders (#1153) (#1154)
- **emit**: stamp purity attributes on emitted functions — nounwind + memory(none) native, ((const)) C (#1139) (#1146)
- **pipes**: fuse inline-lambda stages and sum/product/length terminals (#1145)
- **pipes**: fuse map-pipe chains into single loops (deforestation, #1134) (#1140)

## v0.99.1 (2026-07-08)

### Added

- Ref[T] surface sugar — `&` make, `:=` set, `@` deref (closes #1114, #1113) (#1116)
- **resolve**: gate `theory` declarations to the kind catalog only (#1109)
- kind/theory surface v1 (the cosmetic first cut) (#1107)

### Fixed

- **unbox**: one backend-aware rawness verdict — native reuse-in-place Int slot (#1110) (#1124)
- **kir**: lower as-binding over list-destructure on the native backend (#1118)
- **emit**: decref owned value discarded in statement position on native (#1119)
- **perceus**: drop closure params to close map-pipe record leak (#1122)

### Changed

- **perceus**: borrow read-only array prims to elide per-access RC (#1120) (#1125)
- **emit**: native inline variant-slot read (Eje 1) (#1111)
- **perceus,emit**: fuse residual RC pairs in the match protocol (#1104 Lane 2) (#1106)
- **emit**: donate the arm reuse token across the balance-call boundary (#1105)

## v0.99.0 (2026-07-06)

### BREAKING CHANGE

- money and fx are removed; a currency-taxon-based
replacement will land later

### Added

- **stdlib**: complete numeric operator surface — Decimal ops, Div for big types, uniform div-by-zero trap
- **typer**: #[constructor] sugar for Type(args) construction (#1098)
- **stdlib**: remove money and fx (#1097)
- **typer**: context-driven numeric-literal minting (#1091)
- **lexer**: reserve `taxon` and `taxology` as hard keywords (#1089)
- **native**: nested rotation reuse via dual-branch inner-cell donation (closes #1053) (#1081)
- **native**: rebuild the TRMC variant spine step into the arm-top reuse token (#1053) (#1080)
- **stdlib**: homogeneous Div operator for Complex (#1075)
- **stdlib**: NetTcp.recv_timeout — socket recv with deadline (closes #1066) (#1072)
- **runtime**: route runtime traps to a fiber abort pad instead of exit(1) (closes #1067) (#1071)
- **stdlib**: Add/Sub/Mul operators for BigInt, Rational, DecimalBig (#1068)
- **cli**: kai migrate — automated edition source migration (closes #1047) (#1055)
- **native**: per-module separate compilation with content-addressed object cache (closes #989, closes #963) (#1050)

### Fixed

- **typer**: enforce free-fn protocol bounds at the concrete call site (#1101)
- **runtime**: trap on integer div/mod by zero and INT64_MIN/-1 (#1094)
- **bench**: rb-tree harness measures native column consistently with kaikai-c (closes #1082) (#1085)
- **emit**: guard hoisted sub-pattern slot load against smaller sibling ctors (closes #1073) (#1079)
- **perceus**: variant reuse-in-place increfs a shared child (closes #1069, closes #1048) (#1078)
- **parse**: diagnose hex/binary integer literals beyond Int range (closes #1076) (#1077)
- **native**: variant Real-slot projection dups its borrowed binder (closes #1054)
- **native**: kai test emits the synthetic kai_main so the test-runner links (closes #1061) (#1063)
- **emit**: spawned-fiber builtin default resolves without regressing non-spawn handler layout (closes #1051) (#1062)
- **parse**: BigInt literal routes digits through from_string; Int literal overflow diagnoses (closes #1059) (#1060)
- **emit**: spawned-fiber builtin default resolves (not segfault) on both backends (closes #1051) (#1052)
- **native**: decref record values on scope exit (closes #1048) (#1049)
- **runtime**: single runtime owner shares fiber/arena/heap state across modular TUs (closes #1033) (#1045)

### Changed

- **native**: read known-pointer variant slots as a direct borrow (#1083) (#1102)
- **native**: i64-inline variant slots, mirroring the C backend (closes #1083) (#1100)
- **native**: variant fast-entry construction (refs #1083) (#1092)
- **native**: stamp host target-features so runtime ops inline (closes #1083) (#1088)
- **runtime**: iterative variant spine-free + reduce per-node RC traffic (refs #1083) (#1084)
- **kir**: shared home_tu partitioner + KFn.mo for modular codegen (refs #963, refs #989) (#1046)

## v0.98.0 (2026-07-02)

### Added

- **native**: struct-by-value FFI — C-ABI classification in the emitter (closes #1030) (#1041)
- **build**: add kaic2-fast dev rebuild via modular self-compile (refs #1029)
- **native**: native self-host LINK + RUN -- the native-built compiler links and runs (refs #1021) (#1026)

### Fixed

- **native**: single alias-binder collector mirroring emit_pat_binds (closes #1021) (#1044)
- **native**: add struct-FFI prim forwarders to the self-host link shim (#1043)
- **perceus**: align tcrec dropmask with pcs branch-aware skip (closes #1036) (#1040)
- **driver**: correct stale kaic1: prefix on kaic2 file-read errors (#1039)
- **emit**: rebind self-tail params through param pointers under binder shadow (closes #1031) (#1037)
- **native**: dual-branch cons reuse + recursive alias-binder dup (refs #1021, refs #1025) (#1035)
- **lexer**: stage0 skips multi-line #[...] attributes whole (closes #987) (#1032)
- **native**: incref nested variant alias binders in owned self-tail arms (refs #1025) (#1028)
- **native**: incref shared cons donor head/tail in reuse-gc (refs #1025) (#1027)
- **native**: bind sibling slots in lone-list-slot variant arms (#1024)

## v0.97.0 (2026-06-30)

### Added

- **stdlib**: DecimalBig + Rational on BigInt (numeric lane D, closes #514) (#1011)
- **typer**: fixed-width integers Int32/UInt32/UInt64/Int128 (numeric lane A, refs #514) (#1008)
- **emit**: release/debug build modes + DWARF debuginfo (closes #500) (#1007)

### Fixed

- **runtime**: share RC free-list pools across c-modular TUs (closes #1012)
- **cli**: kai info -k searches page bodies, not just topic names and taglines (#1013)
- **emit**: emit external linkage for cross-module symbols in c-modular (closes #990)

### Changed

- **build**: parallel cc + .o content-hash cache for c-modular (refs #999)

## v0.96.1 (2026-06-29)

### Fixed

- **emit**: a spawned fiber that touches an enclosing var cell diagnoses instead of segfaulting (closes #997) (#1004)
- **perceus**: incref shared-donor children consumed by rebuild calls (closes #995) (#1002)
- **emit**: drop dead capability-value alias node for by-id cells (closes #996)

### Changed

- **ci**: rebalance tier1 shards by splitting the light pool three ways

## v0.96.0 (2026-06-29)

### Added

- **install**: rustup-style curl|sh installer + kai upgrade self-update (#998)

## v0.95.0 (2026-06-29)

### Added

- **lint**: expand the kai lint catalog with six rules (refs #966, Phase 3) (#993)
- **driver**: --batch-list mode amortising core load across N files (refs #962) (#979)
- **lint**: idiom nudges — point_free_nudge + and_then_to_map_nudge (refs #966, Phase 2) (#977)
- **typer**: differential independence oracle (refs #962, Lane 3) (#973)
- **parser**: block lambda pattern binder `{ (a, b) -> ... }` (closes #970) (#971)
- **lint**: kai lint subcommand + discard_pure_value (refs #966, phase 1) (#972)
- **typer**: isolate root_fns to the target bucket (refs #962, Lane 1) (#969)
- **typer**: cut user->core protocol-op contamination in lower_protocols

### Fixed

- **parser**: suppress record-literal in if/match header position (closes #975)
- **lexer**: stage1 skips multi-line #[...] attributes whole (closes #968)
- **emit**: spawned fiber's parent-handled effect diagnoses instead of segfaulting (closes #978) (#983)
- **cli**: pass stdlib search path to kai doc for package modules (closes #976) (#981)
- **cli**: escape backticks in kai build --help heredoc

## v0.94.0 (2026-06-28)

### Added

- **effects**: close #820 — capability passing for user effects, named instances, hybrid dispatch (#965)

## v0.93.0 (2026-06-28)

### Added

- **typer**: extend point-free sections to method calls and chained fields (#964)
- **runtime**: #959 structured nursery — auto-join + cancel-on-fail
- **parser**: naked cell read, var := declaration, @ as Ref deref
- **emit,native**: route value-transportable effects through the evidence frame

### Fixed

- **fmt**: emit `var name := init` so cell declarations round-trip

### Changed

- migrate stdlib, demos, examples to naked cell read + var :=

## v0.92.0 (2026-06-27)

### Added

- **native**: implement test/bench runner on the native backend
- **emit**: thread evidence frame through C call sites (#820 L3)
- **typer**: inject evidence params + ship #789 collision diagnostic (#820 L2)
- **typer**: compute evidence obligations per function (#820 L1)

### Fixed

- **cli**: route test/bench/watch to the native backend by default
- **native**: make nemit_push_atoms pub for the evf call funnel
- **native**: pass the evidence frame through the fn thunk
- **native**: build the evidence frame with a body alloca, not entry
- **emit**: thread evidence frame into lambda/clause bodies + exempt main (#820 L3)
- **emit**: keep Actor/Console/Ffi on the by-name walk (#820 L3)
- **emit**: box unboxed pattern binder at closure capture site

## v0.91.2 (2026-06-27)

### Fixed

- **monomorph**: retarget point-free pipe callee to its mono spec

## v0.91.1 (2026-06-26)

### Fixed

- **emit**: exclude module consts from closure capture sets

### Changed

- **emit**: build prelude lookup tables once, not per call site

## v0.91.0 (2026-06-25)

### Added

- **stdlib**: add StringBuilder — amortised O(n) text accumulator
- **stdlib**: generalise [Int]-only aggregates to protocol-bounded type params
- **typer**: protocol bounds on free-fn type params
- **runtime**: KAI_MAX_HEAP heap ceiling for host containment
- **native**: FFI v2 fixed-width marshalling + struct WIP on native backend
- **stdlib**: make collection pipe-participation coherent
- **typer**: thread effect rows through nominal carrier slots (Stream[t, e])
- **ffi**: implement FFI v2 — struct-by-value, opaque handles, fixed-width types
- **stdlib**: make Instant opaque with instant_of_nanos / instant_to_nanos
- **compiler**: give effects module identity like types
- **stdlib**: migrate builtin effects from compiler AST to source
- **native**: flip the default backend from c to native
- **doc**: resolve kai doc <module> against the current package, not just stdlib
- **refinements**: emit `help: narrow` line on contract-violation panic (#750)
- **native**: run LLVM opt passes in-process before object emit (L4, #498)

### Fixed

- **typer**: make Mutable row honest for Array-backed record forwarding
- **monomorph**: key generic specs by callee module for shared names
- **typer**: honour root-fn shadowing for a protocol op in value position
- **monomorph**: rewrite qualified calls to a protocol-bounded generic
- **monomorph**: disambiguate interpolation call-site position collision by arg type
- **monomorph**: decline poly call-site synth on shape/arity mismatch
- **typer**: canonicalise root-module tag in per-module shadow guard
- **typer**: disambiguate protocol ops sharing a name across protocols
- **emit**: disambiguate homonymous effect default-handler install
- **native**: stop reuse-dup gate at the EMatch boundary
- **typer**: scope D2 ctor-uniqueness to root-file unions
- **native**: close variant reuse leak in the rb-tree shape
- **native**: gate match scrutinee dup on reuse arms, restoring cons cascade
- **compiler**: preserve module_origin through the decl cache
- **stdlib**: revert Instant field priv — it broke external constructors
- **stdlib**: correct doc-vs-code lies, trim contingent comments
- **stdlib**: replace raw control bytes with readable escapes in json.kai
- **native**: per-match scrutinee drop binder + scrutinee dup, fixing nested-match double-free
- **native**: make non-tail raw-scalar calls fully raw (no box/unbox round-trip)
- **native**: cascade decref->free for self-tail-consumed cons cells (#860)
- **native**: structural incref of owned-match alias binders (#858)
- **native**: register raw proto impls through a boxed dispatch adapter
- **native**: stop variant_match super-linear collapse (immortal cache)
- **native**: prefix every degradation-note line with `kai:`
- **ci**: retarget package-mode parity to native, skip when unavailable
- **native**: hoist call-site arg-buffer alloca to the entry block
- **effects**: arm the signal handler before ready in issue_107_signal_trap too
- **effects**: arm the signal handler before the ready marker in m8x_signal_await_parks
- **kir**: lower resume as an expression-call, not a block terminator
- **kir**: restore the local-name set on block exit so siblings don't false-shadow (#846)
- **emit**: break C declarator self-scope for a self-referential shadowing let
- **kir**: alpha-rename a shadowing let-binder to a fresh register (#846)
- **kir**: lower a MUnboxed Int let-binder raw to avoid a multi-use double-free
- **native**: lower extern "C" fn to a real shim in the in-process backend
- **typer**: bind generic effect op payload from declared row at drain (#842)
- **emit**: native reuse-in-place move semantics + variant-TRMC
- **typer**: mask Mutable for any local mutable value via unconditional default install

### Changed

- **emit**: cut self-compile RAM peak from 9.3 to 5.3 GB
- **monomorph**: concrete spec signatures so scalar fn[T:P] specs unbox
- **ast**: replace ETodo extern-body hack with dedicated EExternBody node
- **native**: emit raw sdiv/srem for Int / and %
- **native**: emit raw scalar params/returns for unboxed-signature fns
- **native**: link the runtime as bitcode before O2 (P2)
- **native**: unbox Int arithmetic and comparison
- **emit**: remove the dead llvm-text backend frontend

## v0.90.0 (2026-06-14)

### Added

- **emit**: native protocol dispatch + record-match and module-preference fixes
- **emit**: emit raw f64/i1 ops + box/unbox borders in the native backend
- **emit**: add raw f64/i1 LLVM-builder prims + Real box/unbox borrow shims
- **kir**: lower payload-bearing nested-variant sub-patterns
- **kir**: synthesise dispatch handlers for default-block effects in dead code
- **kir**: lower EBang (postfix `!`) to the Option/Result propagation fork
- **cache**: cache the core post-parse to cross the sub-150ms recompile target (#825)
- **cache**: user-file incremental cache with transitive invalidation (#455)
- **cli**: add kai doc — human reader for stdlib #[doc] attributes

### Fixed

- **typer**: never mask Mutable demanded through effect dispatch
- **emit**: key native evidence frames per install-site, not per effect
- **kir**: lower a list pattern as the head of a list ([[a, b], ..._])
- **kir**: inline unit_name as a compile-time constant
- **kir**: test a literal head in a list pattern ([""], [0, ...])
- **kir**: lower a list pattern in a variant slot (Some([x, y]))
- **kir**: test a lone variant arm's discriminating payload sub-pattern
- **emit**: native handler-clause capture ABI via Ev env channel
- **kir**: lower pipes and match guards in the native KIR path
- **kir**: lower MUnboxed Real raw (mode-slave) instead of re-boxing
- **emit**: synth dispatch shape for no-default-block dead-code performs
- **emit**: decode all C99 string-literal escapes in the native backend
- **perceus**: defer the lambda-param drop to the native lane (#817)
- **runtime**: free the unique cons spine iteratively (#817)
- **perceus**: move arm-binders at last use + drop lambda params (#817)
- **emit**: drop raw self-tail match scrutinee on the goto edge (#817)
- **kir**: bind state/log in a stateful handler's return clause
- **emit**: add native bit-op prelude shims
- **kir**: separate temp-register namespace from surface binders
- **typer**: distinct parametric effect cells in row unification (#802)
- **runtime**: always-compile incref/decref counters (#812 Finding B)
- **kir**: native-parity shared-tag sub-discrimination (burn-down 3)
- **cli**: escape backticks in kai --help heredoc

## v0.89.0 (2026-06-10)

### BREAKING CHANGE

- the `[<refinement_pure>]` fn attribute is removed.
Contract-predicate purity is now derived from the effect row, so the
opt-in marker is redundant; remove it from any signature that carries it.

### Added

- **stdlib**: lazy Stream[t, e] push carrier with pipe-canonical combinators
- **build**: wire the native backend opt-in + a parity ratchet (Lane 1.5 infra)
- **parser**: accept compound row arguments at alias instantiation
- **emit**: lower TRMC to a cctx goto-loop in the native libLLVM backend
- **stdlib**: add chunked streaming ops to the File effect
- **runtime**: add chunked file IO primitives + pool-parking handlers
- **emit**: walk stateful handlers and alias dispatch in the native libLLVM backend
- **kir**: carry the stateful flag on clause KFn
- **emit**: install builtin effect default-handlers in the native libLLVM walk
- **emit**: lower algebraic effects in the native libLLVM walk
- **emit**: generic KIR walk for the in-process libLLVM native backend
- **kir**: in-process libLLVM native backend — main->42 end-to-end
- **kir**: add TyHandle — non-RC opaque scalar for LLVM C-API handles
- **compiler**: #[doc("...")] documentation attribute — parser, AST, side table, JSON surfaces
- **kir**: complete AST→KIR lowering for interpolation, closures, effects
- **stdlib**: spawn_actor_policy — spawn an actor with an explicit MailboxPolicy
- **stdlib**: civil date module — proleptic Gregorian, ISO-8601, calendar arithmetic
- **kir**: KIR Lane 0 — node set, AST→KIR lowering, --emit=kir dumper
- **stdlib**: string index_of/to_upper/to_lower/is_empty/reverse + drop --include-prelude-tests (closes #396)
- **stdlib**: HashSet[t] over HashMap[t, Unit] (closes #375)
- **parser**: Go/Pony-style parameter type grouping
- **typer**: m[key] indexing sugar dispatches to HashMap (#374)

### Fixed

- **native-parity**: keep list_helpers + list_zip3_scan as Linux-only SIGSEGV gaps
- **kir**: close native-parity unary-minus + locals-shadow-imports (burn-down 2)
- **desugar**: make the #[unstable] wrapper transparent to pre-lowering passes
- **kir**: close native-parity missing-symbols + unbound-register slice (burn-down 1)
- **emit**: emit a raw tail in multi-statement blocks without a box round-trip
- **kir**: defer the self-call in TRMC step lowering
- **typer**: anchor return-typed and test-block bare calls beyond let-bindings
- **kir**: lower record-pattern match to a field-test decision tree
- **kir**: lower literal-discriminated match to a condbr equality chain
- **kir**: lower list-pattern match to a cons/nil decision tree
- **tco**: fire TRMC for the builtin cons `[h, ...recur(t)]`
- **fmt**: keep a last-line trailing comment inline on re-format
- **fmt**: round-trip real literals and typed lambda params
- **emit**: make the native main->42 spine reproducible (stage1 handle ABI)
- **cli**: kai fmt no longer prints 'building package'
- **build**: compile kaic2-emitted C against stage2/runtime.h

### Changed

- **stdlib**: drop #[unstable] from http server helpers
- **fmt**: split formatter into A-grade modules
- **unbox**: a divergent match arm does not block raw promotion
- **unbox**: lower a match with an unboxable result to a raw _r
- **stdlib**: migrate doc comments to #[doc(...)] attributes
- **kir**: materialise if/match join slots, KUnreachable match default
- **typer**: const as a first-class value; contract purity from the effect row
- **emit**: port i64-inline + fast variant ctor paths to the LLVM backend (closes #747)
- **emit**: LLVM backend reuses the match scrutinee in place (arm-top token)
- **emit**: LLVM backend links the unified stage2 runtime
- **stdlib**: short module-relative names for HashMap (#374)
- **stdlib**: redesign HashMap as a mutable hash table (#374)

## v0.88.0 (2026-06-06)

### Added

- **stdlib**: HAMT-backed HashMap[k, v] in collections (#374)
- NetUdp v1 — byte-level datagram UDP networking effect
- **stdlib**: net.dns — NetDns effect + resolve op (#352)

### Fixed

- **release**: ship stage2/runtime.h, not stage0's, in the tarball
- **emit**: LLVM backend implements Perceus match RC — garbage-free (#747)

## v0.87.0 (2026-06-06)

### BREAKING CHANGE

- string.chars() now returns Unicode codepoints, not
bytes; the byte-wise behaviour moved to string.bytes(). Code that
relied on chars() yielding bytes must switch to bytes(). ASCII-only
callers are unaffected (byte == codepoint).

### Added

- **emit**: separate compilation — per-module .c/.h linked as objects (#748)
- **stdlib**: text.display_width — wcwidth-style terminal column count
- **stdlib**: String/Char codepoint model — chars() returns codepoints, bytes() is the byte view
- **perceus**: wire borrow inference flip (Koka pure borrow)
- **perceus**: borrow inference analysis (Koka B1, inference only)
- **perceus**: TRMC emission — cctx goto-loop (fresh-alloc, pre-donate)
- **perceus**: TRMC modulo-cons recognition (inert)
- **perceus**: TRMC scaffolding — Koka ins-shape rb-tree + cctx runtime
- **emit**: region{} LLVM arena lowering + deep-copy-out + ctor routing (refs #120)
- **emit**: region{} LLVM backend ctx threading (refs #120)
- **emit**: route region{} variant constructors to bump-arena (refs #120)
- **emit**: route region{} record constructors to bump-arena (refs #120)
- **emit**: route region{} list constructors to bump-arena (refs #120)
- **emit**: region{} block lowers to bump-arena scope + deep-copy-out (refs #120)
- **runtime**: deep-copy-out + arena constructors for region lowering (refs #120)
- **typer**: region{} surface + side-table on normal-alloc lowering (refs #120)
- **runtime**: bump-arena primitive + sentinel decref-no-op for opt-in regions (refs #120)
- **refinements**: contract violation panic shows the offending value (#737)
- **refinements**: contract violation panic shows the offending value

### Fixed

- **ci**: namespace test-llvm/test-run build artifacts to kill a -j race
- **typer**: let a root fn shadow a same-signature stdlib export (#748)
- **emit**: resolve module-body self-calls to their own module (#748)
- **runtime**: non-cell-pool variant alloc uses libc malloc, not the slab
- **build**: link -lm in test-perceus-issue350 (fmod, Linux)
- **emit**: LLVM proto-impl shim honours the per-param unbox mask
- **emit**: O(1) stack for TCO loops + TRMC lowering on LLVM backend
- **emit**: collect kind-1 binders inside record sub-patterns
- **runtime**: mint Exited/Signaled as kind-1 raw i64 slots
- **emit**: payload-ctor table read tag from wrong EV field
- **stdlib**: use string_length in string.kai char_count test to avoid list.length collision
- **stdlib**: make list.map/filter/flat_map tail-recursive so they don't overflow the fiber stack
- **typer**: attribute prelude test blocks to their module in modular-typecheck partition
- **emit**: gate TRMC rewrite to C backend; LLVM falls back to recursion
- **runtime**: variant slots via kai_var_slots macro, not FAM-in-union
- **fnreg**: classify arity-0 scalar-return fns as unboxed candidates
- **runtime**: mint Process Exited/Signaled with boxed Int slot
- **emit**: module-qualify mono node struct names to avoid C collision
- **parse**: multi-arg match drops dead wildcard on irrefutable column
- **typer**: heterogeneous binop result takes the impl's return, not Self
- **emit**: LLVM backend threads per-param raw mask through the UFn convention
- **typer**: defer field access on an unresolved receiver instead of guessing UFCS
- **typer**: bind generic effect op payload to declared row's ty_args
- **emit**: LLVM backend handles mixed raw-param / boxed-return signatures
- **fnreg**: disqualify any lambda-bearing body from param unboxing
- **emit**: borrow-unwrap must use outer ty — Real read as Int produced garbage
- **runtime**: wrap variant FAM in a struct — C99 forbids a bare FAM in a union
- **runtime**: UB-free node-size via offsetof, not (KaiValue*)1 trick
- **emit**: mixed-sig thunk tagged-Int read + switch-default boxed alias
- **perceus**: tagged-Int safety across runtime + test harness paths
- **emit**: route all Int unbox sites through kai_intf (tagged-Int)
- **perceus**: gate variant reuse on flat + diagonal slots

### Changed

- **emit**: i64-inline variant Int slots (kind-1) with raw binders
- **unbox**: thread CtorIntSlots map through the unbox pass (no-op)
- **runtime**: hint kai_intf immediate path, keep bignum branch sound
- **emit**: single-store TRMC reuse — skip donor-identity slots
- **emit**: fix nested reuse-token donate needle (Koka 2-of-3 rotation reuse)
- **emit**: direct slot stores into reused cell in TRMC step (Koka shape)
- **runtime**: inline kai_check_unique, drop redundant singleton guard
- **emit**: reuse-variant arm binds read hoisted sub-deref local
- **emit**: elide kai_internal_dup on Int slots in TRMC rebuild
- **emit**: hoist shared nested sub-derefs across match arms (decision-tree CSE)
- **emit**: drop dead non-exhaustive panic when match has a catch-all
- **emit,runtime**: nullary ctor ident via kai_enum_by_tag array load
- **runtime**: hoist slot-mask lookup out of kai_free_value loop
- **fnreg**: unbox scalar-return fns regardless of param boxedness
- **runtime**: slab-allocate variant blocks, never free individually
- **runtime**: inline kai_incref, symmetric with the decref split
- **runtime**: inline kai_decref fast path, out-of-line the free
- **emit**: drop redundant slotmask register from in-place reuse arm
- **emit,runtime**: register payload-ctor name/mask at startup, inline kai_variant_u_fast
- **runtime**: no-zero variant alloc in kai_variant_u_fast
- **runtime**: drop redundant slotmask register from kai_variant_at
- **emit**: direct enum tag in reuse rebuild, skip singleton round-trip
- **runtime**: inline kai_int fast path, NOINLINE only the bignum tail
- **emit**: inline kai_variant_u_fast for primitive-slot ctors
- **typer**: consolidate Reader.get->ask remap; document index dispatch
- **typer**: attribute prelude builtin effects via scheme row, drop name table
- **runtime**: name copy-pasted IO/net buffer and limit literals
- **runtime**: replace copy-pasted 4096 path buffers with KAI_PATH_BUF
- **stage2**: pipe main's CLI entry instead of nested calls
- **emit**: direct enum-tag store + raw-bool if-condition (kill primitive boxing)
- **emit**: borrow node-field Int in direct comparison, strip redundant dup
- **perceus**: borrow-pure unique branch for linear rebuilds (Koka move model)
- **perceus**: reuse inner cell too in balance rotations (Koka 2-of-3)
- **perceus**: reuse outer cell in nested-pattern rebuilds (rb-tree balance)
- **runtime**: Koka-packed 8-byte header — variant node 80B to 48B
- **emit**: direct Int comparison reads operands raw, no box round-trip
- **emit**: store variant Int slots as tagged values, not raw int64
- **perceus**: full RC discipline for mixed-signature fns (reuse + wall win)
- **unbox**: mixed boxed/raw signature unboxing, composed with TRMC+reuse (WIP)
- **emit**: emit flat per-ctor structs for sum types (Koka mould, additive)
- **emit**: direct Int order-comparison (skip boxed kai_op_lt/gt/le/ge)
- **runtime**: FAM variant blocks — inline payload slots (Koka mould)
- **emit**: enum-as-int slot representation (Koka enum, C backend)
- **perceus**: consume-map + tl-gated cons-overwrite (rb-tree RSS 38.6xC -> 2.29xC)
- **perceus**: Koka arm-binder move + linear reuse-token (rb-tree 6.2xC->4.4xC wall)
- **perceus**: Koka bind-borrow + arm-top reuse for pure modulo-cons arms (rb-tree incref 96M->86M)
- **perceus**: Koka tagged-Int + reuse-token + borrow (rb-tree 14.78xC -> ~5xC) (#741)
- **perceus**: reuse-token for typed cells + drop the diagonal gate
- **perceus**: Koka reuse-token in TRMC step (drop_reuse + alloc_at)
- **perceus**: Koka tagged-Int value representation in stage2 runtime
- **runtime**: rb-tree 16.6x -> ~10x C via allocator free-list + 1:1 variant reuse

## v0.86.1 (2026-05-30)

### Fixed

- **examples**: default_polymorphic must not redefine stdlib Default impls
- **emit**: boxed shim for arity-0 Default + audit protocol impls (#736)
- **emit**: boxed shim for arity-0 Default + audit all protocol impls

## v0.86.0 (2026-05-30)

### Added

- **runtime**: RC-traffic counter + Phase 1.B.1 borrow-flip finding
- **perceus**: cons-head borrow-safe classifier (Phase 1.B.1 infra)
- **perceus**: cons-head raw-safe use-analysis (Phase 1.B.0)

### Fixed

- **emit**: register boxed shim for unboxed builtin protocol impls (#732)
- **emit**: register boxed shim for unboxed builtin protocol impls
- **tooling**: build kai-pkg/kai-lsp helpers atomically — kill parity fan-out race

### Changed

- **typer**: precompute effect-op map for coverage walk (#735)
- **typer**: precompute effect-op map for coverage walk (drop per-EHandle rescan)
- **typer**: unify name-collision validators into one parametric walker (#733)
- **typer**: unify name-collision validators into one parametric walker
- **runtime**: widen small-int cache to [-65536,65535] (Phase 1.A)
- **typer**: inf_scan_uses_* O(n^2) to accumulator
- **emit**: introduce EmitCtx record

## v0.85.1 (2026-05-28)

### Fixed

- **emit**: dense user-type head tags — nested Eq/Ord dispatch + v2h tag indexing
- **typer**: == / != / < / > / <= / >= honour custom Eq/Ord impls (root type)
- **emit**: mirror variant reuse-in-place into LLVM backend (restore C↔LLVM parity)
- **perceus**: unblock variant reuse-in-place (EModCall recognition + immortal-tag exclusion)

### Changed

- **stage0**: flatten emit.c's worst cognitive functions
- **stage0**: split parser into parser.c + parser_expr.c (both C-)
- **stage0**: flatten parser dispatchers to cut cognitive load
- **stage0**: flatten high-cognitive functions in check and lexer
- **stage0**: split parse_primary and decode_char
- **stage0**: decompose parse_pattern and parse_type_decl
- **stage0**: split check_node and kai_lex dispatchers
- **stage0**: replace name switches with enum-indexed tables
- **typer**: drop unused cmp-sugar scaffolding (synth_cmp_proto_eq/ord)

## v0.85.0 (2026-05-27)

### Added

- **runtime**: string_hash + real_bits builtins, wrapping Int arithmetic (refs #373)
- **emit**: scalar function signatures in the LLVM backend (closes #718, refs #622)
- **runtime**: add kaix_default_* forwarders for File/Stdin/Process/Env/Signal (refs #622)
- **emit**: implement TCO in the LLVM backend via tcrec sentinel (closes #706)

### Fixed

- **stdlib**: correct Hashable — string_hash, Real impl, multiplicative variant mix (closes #373, refs #374)
- **typer**: resolve shared protocol op-names by receiver type (closes #546)
- **emit**: borrow check-clause params under Perceus so string-op bodies don't SIGTRAP (closes #697)
- **desugar**: @cap inside string interpolation resolves in the enclosing scope (closes #672)
- **runtime**: honor variant slot_mask in kaix_variant_arg under LLVM (refs #622)
- **emit**: install Cancel pad + unwind on continuation-discard under LLVM (refs #622)
- **emit**: correct clause-capture env rebind + continuation resume under LLVM (refs #622)
- **emit**: resolve @var to the correct State evidence frame under LLVM (refs #622)
- **runtime**: add kaix_evidence_lookup_node_by_id LLVM mirror (refs #622)
- **emit**: re-root nested record sub-patterns in LLVM destructuring (refs #622)
- **emit**: install File/Stdin/Process + user-effect default handlers under LLVM (parity lane B, refs #622)
- **emit**: drop outgoing param-slot value on LLVM TCO back-edge (closes #709)
- **emit**: decode \xNN hex escapes in LLVM string literals (closes #618, refs #622)
- **perceus**: walk UFn fn bodies so boxed locals keep dup/drop discipline (closes #703)
- **emit**: dedup @eff.disp.name globals across emit phases
- **emit**: reuse phi references nonexistent entry block

### Changed

- **build**: compile kaic2 at -O2 (2.4x faster self-compile)
- **emit**: single-source the tcrec sentinel wire format in emit_shared

## v0.84.0 (2026-05-25)

### Added

- **infer**: extract HM typer + bidirectional inference + dumps into compiler/infer.kai (issue #677 phase 1n1)
- **refinements**: extend predicate-aware violation messages to EReal / EStr / EChar (#86)
- **stage2**: flip to package layout — kai.toml + main.kai + compiler/ (issue #677 phase 1c)
- **stage2**: bundle infrastructure for issue #677 phase 1

### Fixed

- **runtime**: dispatch user Cancel handler on sibling-initiated cancel (#682)

### Changed

- **stage2**: extract cli + driver into compiler/driver.kai, reduce main.kai to fn main() (issue #677 phase 1s)
- **emit**: extract C emitter + TCO into compiler/emit_c.kai (issue #677 phase 1r)
- **emit**: rewire emit_llvm.kai onto compiler.emit_shared, delete lvm_* mirrors (issue #677 phase 1r)
- **emit**: sink backend-shared lowering layer into compiler/emit_shared.kai (issue #677 phase 1r)
- **monomorph**: extract m4c monomorphisation pass into compiler/monomorph.kai (issue #677 phase 1n2)
- **emit**: extract LLVM backend into compiler/emit_llvm.kai (issue #677 phase 1q)
- **protocols**: extract m12.8 protocol system into compiler/protos.kai (issue #677 phase 1p)
- **perceus**: extract Perceus RC pass into compiler/perceus.kai (issue #677 phase 1n4)
- **unbox**: extract unboxing pass into compiler/unbox.kai (issue #677 phase 1n3)
- **unbox**: pre-sink fn registry + use scanner into compiler/fnreg.kai (issue #677 phase 1n3)
- **ast**: sink ProtoImplReg + 4 sibling types into compiler/ast.kai (issue #677 phase 1n0)
- **stage2**: extract AST desugar passes into compiler/desugar.kai
- **stage2**: extract module system into compiler/modules.kai
- **stage2**: extract cache codec + AST serde into compiler/cache.kai
- **stage2**: extract kai fmt into compiler/fmt.kai + module tests (issue #677 phase 1j)
- **make**: run the five costly stage2 dump targets under -j5
- **ci**: parallelise tier1 fixture-loop test scripts via xargs -P
- **stage2**: extract name resolution into compiler/resolve.kai + module tests (issue #677 phase 1i)
- **stage2**: extract the parser into compiler/parse.kai + module tests (issue #677 phase 1h H2)
- **stage2**: extract refinement-discharge engine into compiler/refinements.kai + module tests (issue #677 phase 1h H1)
- **stage2**: extract low-level util helpers into compiler/util.kai + module tests (issue #677 phase 1h H1a)
- **stage2**: parse extraction pre-flight + H0 surface cleanup (issue #677 phase 1h)
- **stage2**: un-pub audit on compiler/{chars,diag,lex,ast,intervals}.kai (issue #677 follow-up)
- **stage2**: extract integer interval lattice into compiler/intervals.kai (issue #677 phase 1g)
- **stage2**: extract AST data model + semantic Ty/Row into compiler/ast.kai (issue #677 phase 1f)
- **stage2**: extract lexer into compiler/lex.kai + module tests (issue #677 phase 1e)
- **stage2**: extract diagnostics into compiler/diag.kai (issue #677 phase 1d)
- **stage2**: finish chars extraction and retire orphan compiler.kai
- **stage2**: extract char primitives into src/chars.kai (issue #677 phase 1b)

## v0.83.3 (2026-05-23)

### Fixed

- **emit**: LLVM impl-table bitcast carries the impl's actual arity

## v0.83.2 (2026-05-23)

### Fixed

- **runtime**: line-buffer stdout (#678) + cancel reaches reactor-parked fiber (#679)

## v0.83.1 (2026-05-23)

### Fixed

- **perceus**: align skip-set across perceus boundary + retarget mono self-calls (#680)

## v0.83.0 (2026-05-23)

### Added

- **emit**: impl-table registration + LLVM dispatch shim for protocols
- **emit**: C-backend runtime dispatch shim for protocol dispatcher
- **runtime**: add impl-table hashmap + head-type tags for protocol dispatch
- **emit**: emit atom-style global variant tags across all stages

### Fixed

- **emit**: skip dispatcher-marker ETodos from hole register + lane retro
- **runtime**: align hardcoded variant tags with the atom-style convention

### Changed

- **emit**: flip LLVM match dispatch to variant_tag == N
- **runtime**: drop kai_variant, keep kai_variant_u as the only ctor
- **emit**: flip match dispatch from strcmp to variant_tag == N

## v0.82.0 (2026-05-21)

### Added

- **test**: auto-discover tests/ directory in package mode
- **fmt**: fill all unsupported arms — kai fmt covers full kaikai surface

### Fixed

- **emit**: restore `unfilled hole:` colon in runtime panic message
- **make**: add --path ../stdlib to three ASAN targets

### Changed

- **perceus**: activate branch-aware dup elimination (#599)
- **perceus**: add linear-consumer check and TCO alignment for #599
- **perceus**: land branch-aware dup infrastructure (stubbed)

## v0.81.0 (2026-05-21)

### Added

- **cli**: kai info — man/info-style language reference
- **ci**: BinSerialize Tier 2 perf gate (closes #489)

## v0.80.0 (2026-05-20)

### Added

- **runtime**: R4 reactor — Signal.await() parks the fiber (closes #671)
- **ci**: package-mode harness — close the package-consumer blind spot (closes #569)

### Fixed

- **release**: strip .github/workflows from public mirror overlay

## v0.79.0 (2026-05-20)

### Added

- **lsp**: surface unfilled holes as warning diagnostics

## v0.78.0 (2026-05-20)

### Added

- **driver**: expose --diags-json / --effects-json / --library-mode via kai build

## v0.77.1 (2026-05-20)

### Fixed

- **holes**: include location + inferred type in unfilled-hole panic

## v0.77.0 (2026-05-20)

### Added

- **lsp**: completion + signatureHelp (issue #447 v3)

## v0.76.1 (2026-05-20)

### Fixed

- **lsp**: normalize file:// URIs via realpath (issue #447)

## v0.76.0 (2026-05-20)

### Added

- **lsp**: goto-def + publishDiagnostics + documentSymbol (issue #447)

## v0.75.1 (2026-05-20)

### Fixed

- **release**: include kai-lsp binary in release tarball (issue #447)

## v0.75.0 (2026-05-19)

### Added

- **lsp**: hover-only Language Server v1 (issue #447)

## v0.74.2 (2026-05-19)

### Fixed

- **perceus**: suppress synthetic pcs identifiers from lambda captures
- **examples**: unblock 3 fixtures broken by Hanga Roa surface changes

## v0.74.1 (2026-05-19)

### Fixed

- **stdlib**: rewrite json_decode as an iterative state machine

## v0.74.0 (2026-05-19)

### Added

- **stdlib**: json drilling helpers + http auto-dechunk; slim weather demo

## v0.73.1 (2026-05-19)

### Fixed

- **runtime**: KAIKAI_STDLIB_PATH env override unblocks installed kaikai

## v0.73.0 (2026-05-19)

### BREAKING CHANGE

- any record field a downstream module reads must be
explicitly public (default today). Existing code unaffected because
priv is opt-in; no existing fields change visibility unless the user
adds `priv` to them.

### Added

- **typer**: post-typing walker enforces priv field privacy + 3 negative fixtures
- **typer**: priv keyword for record fields, default public
- **typer**: per-module duplicate-decl validation at load time
- **typer**: refinement predicates accept EModCall for pure-set ops
- **typer**: #derive(Ord) synthesises cmp + min + max
- **typer**: extend validate_pub_access to walk decl signatures
- **typer**: validate_module_pub_signatures — intra-module pub leak gate
- **typer**: validate_unit_refs hygiene pass — intra-module declared scope
- **compiler**: retire --prelude, hard-code the core module set
- **driver**: package-aware build with .  ./...  and kai.toml entry field (closes #658)
- **stdlib**: qualified-call migration — batch B4 (encoding + uuid + regexp + protocols + secure_random + net/http + money doc)
- **stdlib**: qualified-call migration — batch B3 (array + collections + path)
- **stdlib**: qualified-call migration — batch B2 (math + numeric)
- **stdlib**: qualified-call migration — batch B1 (file, spawn, log)

### Fixed

- **tools**: migrate kai-pkg to canonical toml + file API post-Hanga Roa
- **unbox**: resolve UFn classification by (name, module) not name alone
- **resolver**: canonicalise paths via realpath so core + import dedup
- **stdlib**: close 4 same-module dup/missing-import bugs surfaced by validator
- **diag**: --dump-intervals filters to target-file decls only
- **typer**: coverage walker reads effects from whole program, not module partition
- **typer**: vpa signature walker honours tparam scope to dodge variant clashes
- **stdlib**: drop stray .kai.tmp files left by helper script
- **stdlib**: restore fixture imports after --prelude retirement (partial)
- **typer**: post-typer dispatch honours local shadow over protocol op
- **typer**: reject unbound type variables in fn signatures (closes #534)

### Changed

- **typer**: modular per-module typecheck with home-bucket partition
- **typer**: TyCon carries module_origin, unification is strict
- **typer**: module_origin on DType, RecInfo, SumInfo; rerank by home_mod
- **compiler**: remove module_legacy_prefix dead code
- **stdlib,compiler**: kill the legacy-prefix fallback (MAGIC GONE)
- **stdlib**: canonical-only net/http surface (20 ops)
- **stdlib**: canonical-only int + real-remaining + array + spawn
- **stdlib**: canonical-only surface for fx + decimal + money
- **stdlib,compiler**: canonical-only regexp surface + private hex helper renames
- **stdlib**: canonical-only surface for set/queue/base64/hex
- **stdlib**: canonical-only surface for 9 m14-followup modules

## v0.72.1 (2026-05-16)

### Added

- **driver**: kai build uses kai.toml package name as the default output

## v0.72.0 (2026-05-16)

### Added

- **driver**: infer entry point from cwd when no file is named

## v0.71.0 (2026-05-16)

### BREAKING CHANGE

- programs that relied on the implicit LLVM default
now build with the C backend unless they pass --backend=llvm or set
KAI_BACKEND=llvm. The runtime behaviour is identical; only the
codegen path changes.

### Added

- **driver**: default backend is c, llvm is opt-in until Orongo

## v0.70.0 (2026-05-16)

### Added

- **edition**: flip default edition tongariki → hanga-roa
- **typer**: pure named functions auto-generalize over effect rows (closes #645)

### Fixed

- **typer**: arity- and field-aware record lookup (Path 1, closes #648)
- **typer**: same-arity private types use module-mangled names (closes #647)
- **typer**: constructor overloading at use sites (Plan B, closes #644)
- **ci**: release.yml publish-source produces self-consistent mirror

## v0.69.0 (2026-05-16)

### Added

- **stdlib**: core.string drop_prefix / drop_suffix (closes #632)

### Fixed

- **typer**: private types in prelude no longer leak into user scope (closes #643)
- **ci**: xargs subshell must be bash, not sh — export -f invisible to dash

## v0.68.1 (2026-05-16)

### Fixed

- **ci**: bash shebang for test-backend-parity — dash rejects export -f
- **stdlib**: os/time fixtures use import; drop from default preludes (closes #617)
- **stdlib**: http fixtures use import net.http; drop from default preludes (closes #631)
- **driver**: validate edition in kai.toml early with a typed diagnostic (closes #637)

## v0.68.0 (2026-05-16)

### Added

- **stdlib**: http_follow + redirect policy for net.http (closes #357)
- **runtime**: Phase R2 reactor — TCP sockets park the fiber (closes #630)
- **stdlib**: map round-out + qualified migration + pipe compatibility (closes #613)

### Fixed

- **typer**: alias instantiation with row variable + concrete row (closes #624)
- **stdlib**: map_pipes package layout + Tree no longer pub (closes #625)

### Changed

- **ci**: parallelise backend-parity sweep with xargs -P

## v0.67.0 (2026-05-15)

### Added

- **ci**: tier1-backend-parity gate every fixture on C and LLVM
- **runtime**: Phase R3 reactor — stdin parks the fiber (closes #620)
- **compiler**: --edition flag + pipe dispatch gate (refs #603)
- **driver**: kai.toml edition field load-bearing + cache partition (refs #603)

### Fixed

- **stdlib**: println declares Unit / Stdout (was Unit / Console) (closes #623)

## v0.66.0 (2026-05-15)

### Added

- **runtime**: Phase R1 reactor — file + sleep + process park the fiber (closes #611)

## v0.65.0 (2026-05-15)

### Added

- **stdlib**: HTTP server-side parse/serialize helpers (closes #605)
- **typer**: convention-based pipe dispatch via head-owner cache (closes #594)
- **lexer,parser**: #[...] attribute syntax migration (refs #608)

## v0.64.0 (2026-05-15)

### Added

- **driver**: kai.toml [unstable] opt-in flow into kaic2 (refs #602)
- **parser,typer**: #unstable annotation + import-site enforcement (refs #602)

## v0.63.1 (2026-05-15)

### Fixed

- **release**: include EDITION in tarball

## v0.63.0 (2026-05-15)

### Added

- **editions**: adopt stability without stagnation — Tongariki edition pinned

## v0.62.0 (2026-05-15)

### Added

- **typer**: boundary tagging — synth decls carry module_origin (closes #597)

## v0.61.0 (2026-05-14)

### Added

- **cache**: batch shasum + default KAI_PRELUDE_CACHE=1 (closes #592)
- **cache**: KAB2 binary on-disk format (refs #592)

## v0.60.0 (2026-05-14)

### Added

- **cache**: bin/kai cache hit/miss + sha256 + atomic write (refs #452)
- **cache**: kaic2 --prelude-cache + --emit-prelude-cache (refs #452)

### Fixed

- **cache**: avoid prelude-name shadowing in ECall/TyFn arms (closes #585)

### Changed

- **cache**: Phase A.0 stdlib cache complete — infrastructure-ready, wire-format flip pending (closes #452)

## v0.59.1 (2026-05-14)

### Fixed

- **cache**: avoid prelude-name shadowing in ECall/TyFn arms (closes #585)

## v0.59.0 (2026-05-14)

### Added

- **perceus**: variant payload unboxing — Int/Real slots inline (refs #440)
- **runtime**: variant slot abstraction — groundwork for unboxing (refs #440)

### Fixed

- **perceus**: reuse-variant fallback to typed alloc when ctor unboxes (refs #440)
- **runtime**: mint Exited/Signaled with typed Int slot (refs #440)

## v0.58.1 (2026-05-14)

### Fixed

- **emit**: LLVM backend installs Link + Monitor default handlers (closes #587)

## v0.58.0 (2026-05-14)

### Added

- **cache**: AST record serdes — Phase A.0 step 2 (refs #452)

## v0.57.1 (2026-05-14)

### Fixed

- **emit**: install Cancel default handler in LLVM backend (closes #582)

## v0.57.0 (2026-05-14)

### Added

- **cache**: KAB1 header + low-level hex byte-buffer helpers (refs #452)

### Fixed

- **cache**: rename CacheReadLine ctor RL → CRL to avoid RowLabel collision (refs #452)

## v0.56.6 (2026-05-14)

### Changed

- **typer**: collect_program_data seeds working tables from inherited delta (refs #574)
- **typer**: typecheck_program folds typecheck_module per segment (refs #574)

## v0.56.5 (2026-05-14)

### Fixed

- **emit**: LLVM backend installs Spawn default handler in kai_main (closes #570)

## v0.56.4 (2026-05-13)

### Fixed

- **emit**: pass full LamInfo table to every llvm body emission (closes #571)

## v0.56.3 (2026-05-13)

### Fixed

- **kai**: add manifest dir to search paths for intra-package imports (closes #567)

## v0.56.2 (2026-05-13)

### Changed

- **typer**: make typecheck_module the typer entry point, infer_program_with_protos a thin wrapper (refs #460)
- **typer**: extract collect_program_data from infer_program_with_protos (refs #460)
- **typer**: split build_ty_env into seed + per-module (refs #460)
- **typer**: introduce ModuleEnvDelta and typecheck_module API (refs #460)

## v0.56.1 (2026-05-13)

### Fixed

- **typer**: privacy check preserves home attribution across import chains (closes #565)
- **typer**: refinement type aliases support subtyping to base (closes #449, closes #505)
- **typer**: Actor[T] row absorption with nested with_mailbox (closes #532)
- **release**: use TAP_PAT for cross-repo gh API calls

## v0.56.0 (2026-05-13)

### Added

- **emit,examples**: default-handler install for user-declared effects (refs #558)
- **typer**: default block coverage check (closes #557)
- **language**: $extern_handler intrinsic + Cont reserved type (closes #556)
- **parser**: accept default { } block in effect declarations (refs #533)

### Changed

- **typer,emit**: remove parallel default-handler shadow lists (refs #558)
- **emit**: remove hardcoded default_<eff>_shims/setup family (refs #558)
- **emit**: unify default_setups_for / default_shims_for on AST path (refs #558)
- **stdlib**: migrate Fail default to source-level default block (refs #558)
- **stdlib**: migrate Mutable default to source-level default block (refs #558)
- **stdlib**: migrate Cancel + Link + Monitor + Spawn defaults to source-level default blocks (refs #558)
- **stdlib**: migrate Stderr + Stdin defaults to source-level default blocks (refs #558)
- **stdlib**: migrate Process + Log + Signal defaults to source-level default blocks (refs #558)
- **stdlib**: migrate Env + File defaults to source-level default blocks (refs #558)
- **stdlib**: migrate Clock default to source-level default block (refs #558)
- **stdlib**: migrate NetTcp default to source-level default block (refs #558)
- **stdlib**: migrate SecureRandom default to source-level default block (refs #558)
- **stdlib**: migrate Random default to source-level default block (refs #558)
- **stdlib**: introduce builtin_default_block_for helper (refs #558)

## v0.55.0 (2026-05-13)

### Added

- **resolver**: detect import cycles with in_progress stack (closes #538)

### Fixed

- **llvm**: wire stdlib mailbox runtime bindings (closes #523)
- **llvm**: wire stdlib/math real_* libm bindings (closes #522)
- **llvm**: qualified EModCall never falls back to prelude shortcut (closes #524)
- **typer**: protocol ops resolve inside string interpolation (closes #443)
- **typer**: reject duplicate effect / const / axiom decls (closes #543)
- **typer**: validate impl methods against protocol declaration (closes #535)
- **typer**: reject duplicate bindings + duplicate arms in patterns (refs #534)

## v0.54.3 (2026-05-12)

### Fixed

- **parser**: validate FFI surface — require /Ffi + C-ABI types (refs #536)
- **typer,driver**: reject dup fn decls + non-zero exit on missing imports (refs #538)
- **typer**: map args / exit to their effect labels (refs #537)
- **typer**: Mutable masking via callee-row consultation; close subtyping silent contract
- **stage2**: annotate #517 handle-validation helpers with Console+File
- **typer**: handle block validates residual effects + clause shape (closes #517)
- **typer,stage2**: retrofit transitive effect-row propagation (refs #528)

## v0.54.2 (2026-05-12)

### Fixed

- **typer**: propagate effect demands through qualified op calls (closes #516)
- **typer**: reject type-name collisions across module boundaries (closes #518)

## v0.54.1 (2026-05-12)

### Fixed

- **emit**: declare fs/file runtime symbols in LLVM IR prologue (closes #513)

## v0.54.0 (2026-05-12)

### Added

- **typer**: enforce `pub` at module + prelude boundaries (closes #510)

### Fixed

- **examples**: mark prelude-loaded helpers `pub` (refs #510)
- **stdlib**: add `pub` to cross-module helpers exposed by user fixtures (refs #510)

## v0.53.1 (2026-05-11)

### Fixed

- **release**: ship kai-pkg binary in release tarball + manifest smoke test (closes #512)
- **typer**: unwrap DDerive in module export collector (closes #503)

## v0.53.0 (2026-05-11)

### Added

- **driver**: default to LLVM backend when clang is in PATH
- **emit**: close LLVM emitter gaps for full stdlib support
- **build**: LLVM static integration prep for L3 in-process linking (L0)
- **driver**: bin/kai build --backend=llvm flag (L1 of LLVM-direct, refs DoD #6)

## v0.52.0 (2026-05-11)

### Added

- **driver**: bin/kai build accepts --holes / --holes-json flags
- **typer**: collectable structured Diagnostics for m11 T1-T5
- **stdlib**: fs.file.read_bytes / write_bytes return / accept Array[Byte]
- **compiler**: register file_read_bytes / file_write_bytes signatures with Array[Byte]
- **runtime**: file_read_bytes / file_write_bytes use Array[Byte] (boundary fix for #488)
- **typer**: extend def_at to resolve local bindings (issue #484)
- **compiler**: register file_read_bytes / file_write_bytes (refs #482)
- **runtime**: file_read_bytes / file_write_bytes primitives (refs #482)
- **typer**: m11 v1.x diagnostic templates 4 (wrong arity) + 5 (missing effect)
- **compiler**: extend #derive(BinSerialize) for collections/Char
- **stdlib**: BinSerialize combinators for List/Option/Char
- **compiler**: derive_binserialize_impl for records and sums (closes #459)
- **stdlib**: add BinSerialize protocol for cursor-based binary serialization
- **stdlib,compiler**: expose u8 prelude names + Byte alias
- **typer,runtime**: u8 nominal primitive — typer + runtime scaffolding
- **stdlib**: add option.first_some for N-way pure Option fallback
- **typer**: library mode — typed AST + span queries (closes #454)
- **runtime**: prelude_read_bytes for stdin byte-oriented I/O (closes #453)
- **stdlib**: fs.dir.{list_dir,create_dir,remove_dir,walk} runtime + public surface (closes #344)

### Fixed

- **typer**: distinguish Actor[T1] vs Actor[T2] in effect-row dispatch
- **typer**: resolve module-qualified record literal types
- **parser**: accept qualified path before record literal opening brace
- **stdlib,compiler,runtime**: BinCursor + LLVM byte ops for tier1
- **runtime**: update int_to_u8 error strings to int_to_byte after rename
- **stdlib,compiler**: retype BinSerialize for nominal Byte
- **demos**: rename forth.show to forth.render to avoid Show[List[T]] shadow
- **demos**: prefer n-tuple sugar over explicit Pair record
- **demos**: mini_ledger uses official record-spread syntax

### Changed

- **stdlib,compiler**: BinSerialize uses Array[Byte] for O(1) buffer reads
- **typer,runtime,stdlib**: rename u8 primitive to Byte; add bidirectional list literal check
- **compiler**: convert if-elseif Char/Int cascades to exhaustive match (audit proposal 2)
- **compiler**: collapse test/bench/check Bool triple to BuildMode enum (audit proposal 5)

## v0.51.0 (2026-05-10)

### Added

- **typer**: m11 v1 diagnostic templates — type mismatch, non-exhaustive, unbound name (closes #445)
- **check**: shrinking — per-type halving + structural reduction (closes #438)
- **bench**: median + MAD + configurable iterations + warmup (closes #437)

### Fixed

- **typer**: count variant arms toward exhaustiveness on union match (closes #436)

## v0.50.0 (2026-05-09)

### Added

- **kai-pkg**: validate package name in kai init (closes #419)
- **stdlib**: export fs.file.{exists,delete,rename} wrappers (closes #423)

### Fixed

- **parser**: terminate arrow-lambda body at pipe operators (closes #422)
- **kai-pkg**: propagate kai.toml parse errors as non-zero exits (closes #420)
- **kai-pkg**: cache by SHA instead of ref slug (closes #421)
- **typer**: propagate declared return type into multi-clause case arms (closes #430)
- **kai-pkg**: make kai add atomic on git failure (closes #418)
- **compiler**: dedup imports against prelude modules (closes #425)

## v0.49.0 (2026-05-09)

### Added

- **parser**: multi-clause function bodies (case-led arms) (closes #415)

## v0.48.0 (2026-05-09)

### Added

- **driver**: git deps + lockfile + cache + transitive resolution (closes #405)
- **driver**: kai init/install/show + local-path deps (refs #405)
- **parser,typer**: add |? filter-pipe operator (closes #412)

## v0.47.0 (2026-05-09)

### BREAKING CHANGE

- kai repl is removed. Programs that ran via kai repl
must use kai run with a script file. kai watch is the closest
"evaluate on every save" workflow.

### Added

- **tools**: kai-pkg CLI skeleton (init + show) (refs #405)
- **stdlib**: toml decoder for package manifest subset (refs #405)
- **emit**: Phase 3 unboxing for all primitives at call boundaries (closes #383)
- **stdlib**: fx — currency conversion module (closes #365)

### Fixed

- **emit**: pipe + UFn callee emits raw args (closes #383 follow-up)

### Changed

- **emit**: skip redundant decref after __perceus_drop SExprStmt (refs #403)

## v0.46.0 (2026-05-09)

### BREAKING CHANGE

- dec_div returns Option[Decimal] instead of silent
dec_zero on division by zero. Direct callers must wrap with `match`
or `?`. money_divide_scalar's return type flips from Money to
Option[Money] for the same reason.

### Added

- **stdlib**: json surrogate pair encode/decode for non-BMP (closes #362)
- **stdlib**: fs/file exists/delete/rename + runtime primitives (closes #345)
- **typer**: warn on unused local bindings (closes #381)
- **stdlib**: array module + flip random.shuffle to O(n) Fisher-Yates (closes #366)
- **stdlib**: core.string — split/replace/pad_left/pad_right/lines/chars/is_blank (partial #338)
- **stdlib**: core.list — last/init/partition/split_at/span/chunk/windows/intersperse/enumerate/zip3/scan/group_by/find_map (closes #340)
- **stdlib**: add os/process public wrappers (closes #346)
- **stdlib**: add tuple helpers swap/map_fst/map_snd/map_pair/first/second/third (closes #348)
- **stdlib**: impl Rem for Real via fmod (closes #364)
- **stdlib**: json parse Real numbers (decimals + scientific) (closes #361)

### Fixed

- **stdlib**: dec_div returns Option[Decimal] (closes #363)
- **typer**: propagate expected type into if/else branches (closes #382)
- **typer**: expand placeholder desugar to top-of-arg expressions (closes #385)
- **parser**: accept trailing commas in list/record/tuple/call args (closes #386)
- **typer**: propagate expected type into match arms (closes #379)

## v0.45.0 (2026-05-08)

### Added

- **stdlib**: math/int — add log2 and div_mod (refs #347)
- **stdlib**: add libm bindings to math/real (sqrt, trig, exp/log, pow, atan2)
- **stdlib**: re-introduce string.concat now that resolver supports file-local precedence (closes #336)
- **stdlib**: expose string.length / slice / to_int as public wrappers (closes #332)
- **typer**: record spread sugar — `T { ...p, x: 10 }` for functional update (closes #326)
- **parser**: allow bare `...` and `..._` in list spread patterns (closes #328)
- **tco**: rule 3 precise per-call-site dropmask via side table (closes #92)

### Fixed

- **typer**: restore transparent aliases for type X = T (closes #376)
- **typer**: structured violation context for contract panics (refs #86)
- **perceus**: widen self-tail-call detection in arm-drop pass (refs #350)
- **perceus**: emit decref for matched scrutinee in tail-rebuild rotation arms (closes #350)
- **build**: link -lm so libm bindings resolve on Linux glibc
- **perceus**: emit exit-drops for SLet bindings with dup-detected pass-through
- **resolver**: file-local precedence for bare-name lookup in test/bench/check bodies (closes #335)
- **perceus**: extend tcrec rule 3 with single_use_is_borrow predicate (closes #92)
- **typer**: rewrite PVariantRecord to PRecord for record-alias names (closes #325)

## v0.44.1 (2026-05-07)

### Fixed

- **runtime**: bump KAI_IMMORTAL_VAR_BUCKETS 16384 -> 262144
- **perceus**: closure capture lifecycle — drop leak rate from 27.97% to 1.14%
- **test-runner**: exclude prelude-origin tests from `kai test <file>` (closes #318)
- **emit**: escape triple-quote string contents in C codegen (closes #311)
- **typer**: Real literals stay Real through binop typing (closes #312)
- **emit**: inject pre-goto _scr decref when match-arm tail is tcrec sentinel

### Changed

- **pipeline**: reorder perceus before tcrec — eliminate implicit coupling (Eric review 2026-05-06)
- **compiler**: remove redundant // integer-division operator (closes #315)

## v0.44.0 (2026-05-06)

### Added

- **runtime+emit**: per-leak-site attribution for KAI_TRACE_RC (DIAG for Perceus emitter discipline)
- **runtime**: nullary variant singletons (closes ~63% of leak in kaic2 self-compile)
- **typer+stdlib**: % operator overloading via Rem protocol
- **runtime**: per-call-site leak attribution for KAI_TRACE_RC (closes #296)
- **emit**: RC discipline for discards — Phase 2 of #293
- **emit**: nested record destructuring drops field temp + Phase 1 fixture
- **emit**: record field access + destructuring let RC discipline (#293 Phase 1)
- **runtime**: KAI_TRACE_RC strict alloc tracing for diagnostics (Track #2 of #291)
- **perceus**: typer-aware shape predicate enables variant + record reuse-in-place (closes #210)

### Fixed

- **typer+emit**: user effect shadows stdlib (closes #308)
- **runtime**: short-string interning for top-1 leak site (Lane FIX of Perceus)
- **runtime**: immortal-payload variant singletons

## v0.43.0 (2026-05-05)

### Added

- **stdlib**: Default protocol + 6 impls (closes #258)
- **stdlib**: polymorphic impls Show/Eq/Ord/Hash for [T]/Option[T]/Result[E,T] (closes #175)

### Fixed

- **emit**: track enclosing scope so closures capture shadowed names (closes #285)

## v0.42.0 (2026-05-05)

### Added

- **typer**: polymorphic impl bounded constraints (closes #174)

## v0.41.0 (2026-05-05)

### Added

- **typer+stdlib**: protocol P[a] dispatch tracks tparam arity (#180)
- **typer**: bidirectional Self inference from let-binding annotation (#180)
- **stdlib**: heterogeneous Complex arithmetic via P[a] (#180)
- **typer**: heterogeneous synth_binop dispatch via proto_impls (#180)
- **typer**: per-impl proto-arg names tracked in ProtocolReg (#180)
- **parser+ast**: accept protocol P[A] and impl P[T1,...] for U (#180)

### Fixed

- **driver**: bin/kai uses $STDLIB_ROOT for --path resolution (closes #279)

## v0.40.0 (2026-05-05)

### Added

- **typer**: := / @ sugar over Ref[T] (closes #275)
- **release**: macOS arm64 distribution tarball + GH Actions workflow

## v0.39.0 (2026-05-05)

### Added

- **parser**: complex literal i suffix (Phase 1 of #267)
- **parser**: positional record construction T { v1, v2 } (closes #266)
- **stdlib+typer**: Ref[T] in Mutable effect (closes #257)
- **parser+ffi**: extern "C" fn syntax + name override (closes #260, closes #261)
- **typer+stdlib**: operator overloading via protocols + Complex stdlib (closes #246, closes #245)

### Fixed

- **parser+typer**: scope-aware const desugar + DConst variant (closes #269)
- **typer+stdlib**: enforce Mutable for observable Array writes + Koka-style masking (closes #251, closes #252)

### Changed

- **typer+docs**: rename Unit kind to Measure (closes #253)

## v0.38.0 (2026-05-04)

### Added

- **parser+typer**: qualified types in signatures + qualified variant constructors (closes #232, closes #234)
- **typer+codegen**: minimal FFI via [<extern_c>] axiom
- **kai**: add watch + repl shell commands for live demo
- **typer**: UFCS dispatch for receiver.method(args) (closes #205)
- **perceus**: reuse-in-place for known-unique constructors (closes #118)
- **stdlib**: expand Result + Option API (closes #182)
- **typer**: || flat-map pipe + naming-convention dispatch (closes #201)
- **typer+codegen**: union upcast in unify_heads + narrowing-pattern codegen (issue #187 phase 4)
- **typer**: match-by-type + bind-narrowing patterns over TyUnion (issue #187 phase 3)
- **resolver**: construct TyUnion from type T = A | B declarations + D2 collision error (issue #187 phase 2)
- **typer**: lower Decimal<u> literals and unify <u>-decorated records (closes #162)
- **typer**: static interval propagation with alpha + operator + call-site (closes #83)
- **typer**: regex subsumption via shape-aware containment (closes #157)
- **typer**: hex and binary integer literals (closes #156)
- **typer**: regex sigil + matches refinement predicate (closes #85)
- **typer**: allow var/let in handler clause list (closes #95, #148)
- **typer**: n-tuple parser sugar (closes #154)
- **emit**: match fast path supports PBind catch-all (closes #91)
- **stdlib**: add SecureRandom effect + random_secure module
- **runtime**: add kai_default_securerandom_* (getrandom/arc4random)
- **stdlib**: close os.env / os.args layout gap (#127)
- **emit**: inline pow_int and idiv when operands are raw
- **stdlib**: add crypto/hash + crypto/mac (S2 #5)
- **stdlib**: add Log effect builtin + log module (S2 #7, closes #141)
- **runtime**: add kai_default_log_* (stderr + ISO 8601, issue #141)
- **stdlib**: add Process effect — POSIX subprocess primitives (#126)
- **stdlib**: net.http HTTP/1.1 client over NetTcp (Tier S1 lane #3)

### Fixed

- **typer+codegen**: cross-module pub axiom export + multi-candidate name narrowing (closes #244, closes #248)
- **perceus**: elide redundant TCO/dup interaction on cons accumulator (closes #212)
- **typer+stdlib**: filter ambig bare-name warning by root usage (closes #243)
- **typer**: guard pipe-dispatch EModCall on qualified-entry presence
- **typer**: bare-name + UFCS lookup narrows by argument/receiver type (closes #235)
- **kai**: resolve --path src_dir to absolute (closes #237)
- **typer**: legacy-prefix overrides for regexp + decimal modules (closes #237)
- **kai**: auto-discover sibling modules via --path entry-dir (closes #233)
- **typer**: revert pipe-rejection diagnostic strings to surviving aliases
- **typer**: restrict target DFn tagging to pub non-main exports
- **typer**: test-stdlib-core-intrinsic registers target in module table (closes #230)
- **typer**: EModCall lookup filters by qualifier (closes #219)
- **stdlib**: drop option. qualifier in result.kai cross-file tests (refs #203)
- **typer**: qualified-call resolver covers prelude + interpolation scopes (closes #216, closes #217)
- **perceus**: resolve pcs_rewrite_expr dup interaction so reuse-in-place fires (closes #209)
- **typer**: accept Decimal<u> UoM annotation in type parser (closes #158)
- **runtime**: unset KAI_TRACE_RC before bin/kai execs program (closes #81)

### Changed

- **stdlib**: retire 5 surviving list_* aliases (closes #227)
- **stdlib**: migrate char module to module-relative names (m14 phase 5, refs #203)
- **stdlib**: migrate result module to module-relative names (m14 phase 4, refs #203)
- **stdlib**: migrate option module to module-relative names (m14 phase 3, refs #203)
- **stdlib**: migrate string module to module-relative names (m14 phase 2, refs #203)
- **stdlib**: migrate list module to module-relative names (m14 phase 1, refs #203)
- **typer**: unions cleanup post-#187 (closes #197 #198 #199)
- **typer**: introduce TyUnion as internal representation, lower TySum (issue #187 phase 1)

## v0.37.0 (2026-05-02)

### Added

- **stdlib**: add core.list canonical higher-order helpers (#106)
- NetTcp v1 — byte-level TCP networking effect

### Fixed

- **sigharness**: add POSIX/glibc feature-test macros for usleep+kill
- **unbox**: demote let-bindings escaping into #{...} interp slot (R8 / #94)

## v0.8.0 (2026-04-29)

## v0.7.2 (2026-04-29)

## v0.7.1 (2026-04-29)

## v0.7.0 (2026-04-29)

## v0.6.0 (2026-04-28)

## v0.5.1 (2026-04-28)

## v0.5.0 (2026-04-28)

## v0.4.1 (2026-04-28)

## v0.4.0 (2026-04-28)

## v0.3.0 (2026-04-28)

## v0.2.2 (2026-04-28)

## v0.2.1 (2026-04-28)

## v0.2.0 (2026-04-28)

## v0.1.3 (2026-04-28)

## v0.1.2 (2026-04-28)

## v0.1.1 (2026-04-28)

## v0.1.0 (2026-04-28)

### Changed

- replace O(n²) char-by-char builders with string_slice
- apply tail-spread fast path to LLVM emitter
- cut kaic1 → stage2 from 113s/13GB to 0.8s/2.6GB
