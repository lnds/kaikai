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
