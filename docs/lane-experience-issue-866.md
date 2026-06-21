# Lane experience — issue #866: migrate builtin effects to source

## Scope as planned

Move the 25 hardcoded builtin effects from compiler-injected AST
(`driver.kai` `builtin_*_decl`/`_ops` + `inject_unconditional`) to real
kaikai source under `stdlib/`. Mechanical migration, not architecture:
`Clock` (stdlib/time.kai) was the proven template — a source effect with
a `default { }` block bridging to the runtime via
`$extern_handler("kai_default_<eff>_<op>")`. Delete the builders, the
default-block generator, the theater `effects.kai`, and the "declared by
the compiler" confessions. Fix `Conn.fd` opacity by construction.

## Scope as shipped

All 25 effects + ~14 companion types migrated. `driver.kai` shrank from
5685 to 3063 LOC. Net diff −1210 LOC. But the lane grew a second half
the brief did not anticipate (see surprises): a parser collision forced
two renames, and the migration activated a latent shadowing bug that had
to be fixed for the lane to be sound.

File layout (all auto-loaded as part of the core set, so effects resolve
without an import):

- `stdlib/effects.kai` — IO atoms (Stdout/Stderr/Stdin/Fail/Ffi/Env/
  File/Random/SecureRandom/Log), Mutable, FileHandle, and the
  `Console`/`Io` aliases. Loads before `array.kai`, which needs Mutable.
- `stdlib/effects/concurrent.kai` — State/Reader/Writer (no default),
  Spawn/Cancel/Actor/Link/Monitor.
- `stdlib/effects/os.kai` — Signal/Process + Sig/Child/Exit.
- `stdlib/net/{tcp,udp,dns}.kai` — the Net* effects live with their
  surface helpers.

## Design decisions

**Net effects stay auto-loaded, not import-gated.** The brief suggested
the Net effects "may live in their module since those load too." Verified
false: `net/*` is NOT in the core auto-load set. Three existing programs
used a Net effect without `import net.*` — those were latent bugs (using
a Net capability without importing net), so the right fix was adding the
missing imports to the two real cases (`net/http.kai`,
`netdns_no_socket_cap.kai`), not auto-loading net. The Net effects + their
companion types are declared in `net/*.kai`, which programs that touch
them already import.

**`Env.var` → `Env.get`.** `var` is a reserved keyword (TkVar); the op-name
parser requires TkIdent, so `var(name: String)` in a source effect decl is
rejected. The op only ever worked hardcoded because the AST never passed
through the parser. A keyword should never be an op name — renamed to
`get` (the natural verb; the stdlib wrapper was already `env.get`). The
runtime symbol followed as `kai_default_env_get`.

**`bit.and/or/not/test` → `bit._and/_or/_not/_test`.** The same root cause:
the dotted bit surface used reserved words (`and`/`or`/`not`/`test`) as
field names, kept parseable by a parser patch that accepted a fixed set of
keyword tokens after `.`. Reserved words are reserved — the patch was
removed entirely and the dotted bit ops carry a leading underscore so they
stay plain identifiers. The `bit.X → bit_X` rewrite in `modules.kai`
became `bit._X → bit_X` (`"bit" ++ "_and"` = `"bit_and"`).

**Prelude-shadowing made general.** The migration turned `Io`/`Console`
from inert doc text into real aliases. That exposed a latent asymmetry:
a user `effect Io { ... }` was shadowed by the stdlib `Io` alias, because
the alias expander never checked for a local decl of the same name. Effects
and fns shadowed correctly, aliases did not — the principle "a local symbol
shadows any prelude symbol of the same name" was implemented per-class, and
aliases were the class left out. Rather than special-case aliases (the
first instinct, rejected), the fix materialises shadowing once at the
prelude/user merge point: a prelude decl whose name is declared in the
root file is dropped before merge, so no later pass sees the duplicate.
This deleted dead collision-report code (`report_import_type_collision`,
`find_exporting_module`) and the unused `module_table`/`prelude_mods`/
`target_mod_name` params threaded through the type-collision validator.

**Companion handle visibility.** `Conn.fd`/`Listener.fd`/`UdpSocket.fd`
are `priv` (the fd is read by the runtime, never by kaikai code, verified
by grep). The observable `port`/`host`/`addr` fields stay `pub` — they are
read cross-module (`local_port`, the dns fixture).

## Structural surprises

1. **The brief was wrong twice, and the code was the oracle both times.**
   "Net effects load too" (false — not in the core set) and "fixes #865
   Conn.fd opacity" (#865 is a closed doc-lie umbrella that never actioned
   Conn.fd; the opacity fix is real but #865 is the wrong reference).

2. **A mechanical migration uncovered a resolution bug.** The lane could
   not be sound without fixing alias shadowing — 38 fixtures declare their
   own `effect Io`/`Fail`, which only "passed" before because `Io` did not
   exist as a real symbol. Materialising the catalog made the latent bug
   load-bearing. The fix is general (one merge-point rule), not a patch.

3. **Keyword collisions are a real cost of moving AST to source.** Hardcoded
   AST bypasses the parser, so op names that collide with keywords (`var`)
   or field names that do (`and`/`or`/`test`) only surface once the decl is
   written in surface syntax. Two renames fell out.

## Fixtures

- `examples/stdlib/os_env_mutate.kai`, `examples/effects/m7a_7_default_env.kai`
  — exercise `Env.get` resolved from the source decl.
- `examples/stdlib/bits_dotted.kai` — the renamed dotted bit surface.
- `examples/negative/effects_phase2/netdns_no_socket_cap.kai` — capability
  gating still rejects `NetTcp.connect` under `/ NetDns`, now with explicit
  `import net.tcp`/`net.dns`.
- `test-effects` (incl. `parse_row_syntax`, `handle_alias`) is the
  regression for a user `effect Io`/`Fail` shadowing the stdlib decls.

Coverage gap: no new fixture pins the merge-point shadowing rule directly
(it is covered transitively by the 38 `effect Io` fixtures); a dedicated
positive fixture asserting "local effect wins over stdlib alias" would make
the invariant explicit.

## Cost

Larger than a mechanical migration. The migration proper (write source,
delete builders, gate selfhost) was the easy half. The second half — the
two keyword renames and the general shadowing fix with its dead-code
sweep — was unplanned and is where the judgement went. The comment cleanup
(strip contingent refs from the 8 touched compiler files) ran as a
parallel 8-agent workflow.

## Follow-ups

- A dedicated fixture for the merge-point shadowing rule.
- `report_import_type_collision`'s sibling per-class collision validators
  (`validate_fn/effect/const/axiom_name_collisions`) still walk root-only
  decls; with shadowing centralised at the merge they may be simplifiable,
  but each detects a real same-file double-decl, so this needs care.
- The `effects.kai` module doc and `km` flag the `Mutable` `default { }`
  block as one high-cogcom "function" (an artefact of counting clauses as
  branches); harmless but worth a note if `km` ever gates stdlib.
