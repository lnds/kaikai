Part of #2340.

## What this does

A name is no longer an identity anywhere below the resolver, and the types make the old way unwritable.

- **Nodes carry ids, not names.** `ESym(SymId, inst)` for a declaration, `ELocal(LocalId)` for a binder, `EOp(OpRef)` for an effect operation (effect id, op label, and the handler instance as the local it is); `TyCon(SymId, args)`; row labels by effect id, with the instance cell as a local id; patterns and record literals by constructor / record id; `DFn` / `DType` / `DEffect` / `DProtocol` / `DConst` hold their id only. `EVar(String)` is gone: `EName(Written)` exists only before the resolver.
- **Text is a type.** What the source wrote is `Written`, readable only through one accessor. A declaration's name comes from the symbol table, a specialisation's emitted name from a table the monomorphiser returns, a local's from its binder.
- **The parser mints ids and returns the table rows**; the table is built right after loading and extended by each pass that generates declarations. Impl methods get their own symbol class, so two impls' `show` are two ids.
- **Every pass reads identities**: resolver (numbers every binder), typer (environment keyed by declaration / constructor / operation / local; no name index), monomorphiser, Perceus, unbox / unwind / purity, closure specialisation (substitution by local id; the by-name capture guards are gone), both emitters, KIR (`KPerform` with typed fields instead of the `@` / `#` / `!` op-string protocol), lints, caches.
- **One by-id spelling** (`rt_name`) for the emitted name of a contested effect or type, shared by both backends, the evidence frames, the generated impl symbols and the derives; the symbol table knows its root and its core modules.
- **Only the root module's `main` is the entry point**: `is_root_module` (implicit row allowed) and `is_entry` (what the binary starts) are two predicates by id. A grouped `kai test` keeps every file's `main` as an ordinary function.
- `origin/main` is merged (#2676, #2678, #2679, #2681 ported to the id shapes; #2679's by-name capture guard is not taken — the id mechanism covers its fixtures).

## Bugs this fixes

- **C-backend miscompile present in 0.139.6**: a `match` nested under an arm whose outer pattern is itself nested read the outer match's hoisted slot as its own binder ("panic: non-exhaustive match" at run time). Fixture `examples/match/nested_match_in_nested_arm`.
- A sum whose only variant is spelled like the type (`type Boom = | Boom`) was read as a union component of itself, so matching on it was "unknown variant". Fixture `examples/unions/self_named_variant_value`.
- A user type named `List` got an ill-typed law-check witness. Fixture `examples/sugars/shape_kind_laws_list_homonym`.
- `pop_is_sentinel` use-after-free through a root-file impl method (`examples/perceus/impl_method_borrowed_arg`).
- A call through a `__`-named function is an ordinary call for Perceus.
- A test block calling its own file's `main()` in a solo `kai test` crashed on the native backend (present in 0.139.6). Fixture `examples/test-runner/own_main`.
- A user function spelled like an impl method (`__pimpl_Show_Int_show`) was filed in the runtime impl table; a protocol op with `_` in its name taken as a value panicked; a module declaring its own `show` captured `"#{x}"`. Fixtures `adv_fn_named_like_impl`, `impl_table_op_with_underscore`, `interp_show_own_protocol`.
- `const G: String = "#{WHO}"` did not link on C and printed the raw text on native: interpolation is lifted to an expression in every declaration. Fixture `examples/sugars/const_interp_show`.
- A discontinued generator leaked its suspended frames once handler clauses were compared by id: an effect handled by a segment handle unwinds because its forwarding clause makes a segment request, not because of how its continuation is spelled. Fixture `examples/segments/discontinue_frame_values`.
- `const L: [Int] = { let a = [1, 2, 3]  list_append(a, a) }` panicked at run time (0.139.x, C), and a lambda in a const initialiser failed on both backends. Fixture `examples/perceus/const_block_initialiser`.
- A `map` pipe ending in a contested `sum` / `product` / `length` was fused with the core's by spelling. Fixtures `fused_terminal_contested_*`.
- A module that imports nothing and calls a core function reached a sibling module's homonym (`examples/packages/builtin_import`).
- A type spelled like a runtime head, or like another module's type, registered its impls under the wrong head ("no impl of Show.show for runtime head 31"); a plain record spelled like another module's `extern "C" type` was marshalled with that module's layout (now rejected at the boundary); a read `_x` binder was treated as holding no reference across an unwind. Fixtures `impl_head_homonym*`, `neg_extern_*_plain_homonym`, `examples/effects/unwind_underscore_binder`.
- A record literal whose head the typer settles to another record than the nearest one of that name kept its fields in source order (`Cell { hi: 9, lo: 2 }` read as `lo=9 hi=2`): the typer lays the fields against the record it settled. Fixtures `record_head_*`.
- `--dump-typed-core`, `--lint-json`, `--dump-call-elision` and `--strict-holes` typed against a different set of inputs than a build (an empty or unstamped registry, root-only roots): every mode takes one `TyperIn`.
- Two modules each declaring an effect with a `default {}` handler, or a type with an impl of the same protocol, no longer share emitted names (`effect_default_homonym_modules`, `impl_homonym_types_one_home`).

## Behaviour changes (intended)

- `derive` picks its generator by the protocol's id: deriving a user protocol that merely shares a core protocol's name is an error ("no derive generator for protocol `pa.Show`"); unknown / generator-less protocols are rejected.
- A type name nothing declares is rejected; `type T = X` with an undeclared `X` no longer invents a constructor.
- Every nullary constructor takes the singleton fast path, qualified or not.
- `region` is a parser mark; `region({ .. })` is an error pointing at `region { .. }`. `finally` / `initially` are op names.
- Internal spellings (`__…`) written by the user are ordinary names.
- A non-root `main` is an ordinary function: no implicit row, no default handlers; its "effect not handled" error carries a note that `main` is an entry point only in the root module. An unused private `main` of an imported module is dead code for `kai lint`.
- A handler clause resumes when it calls its own continuation binder (before: only when it was spelled `resume`).
- Purity confirms a callee by its exact id (the same-module / unique-homonym heuristics are gone).
- A `var` shadowed inside its own scope is promoted to a cell (ids tell the shadow apart).
- UFCS: a local function value named like a function several modules export is called (before: "bare name is exported by multiple modules").
- Homonymous declarations are never conflated: constructors, types, effects, functions and locals that share a spelling are distinct everywhere (core reachability, live functions, lints, mutation carriers, cons reuse, the `!` success tag).
- Emitted names: a type whose only homonym is an effect-row alias is spelled `T__home`; `__ref_pred_<T>` keeps the declared alias name.
- A refinement predicate's names are resolved: `fn pos(x: Int where x > 0)` called as `pos(0)` is rejected at compile time, as the `self` spelling already was (before: compiled and panicked at run time).
- A diagnostic naming an effect qualifies it when the program declares two of that name: "effect not handled: effects.Log" where a root `Log` and the core `Log` are both in play. An uncontested effect keeps its bare spelling.
- A test build's entry point is its runner, solo or grouped: the file's `main` is an ordinary function there. The C entry symbol of a program is `kaiu_main`.
- The interface hash of a module changed (the serialised schemes carry ids, not names), so the pinned digests of the cache selftest are re-pinned.
- A bare name is settled on the reader's ladder (own module → picks → imports → core) in one place; more than one candidate on the nearest rung is an error, never a pick. Two imports exporting a function with the same first-parameter type, called bare, compiled before (the newest won) and are now "bare name 'f' is exported by multiple modules"; a constructor of the reader's own module wins over a core or imported homonym without an annotation (before: ambiguity error).
- `#[derive(P)]` for a protocol that is not a core one picks the structural generator (Show, Eq, Ord, Hash) whose core protocol declares the same operations; BinSerialize, Layout and Json serve only their own core protocol. Two programs this is narrower for than before: a user protocol *named* `BinSerialize`, `Layout` or `Json` no longer picks that generator, and a user protocol with a core name but other operations gets "no derive generator" up front instead of impl-method errors. One it is wider for: a user protocol declaring exactly a core protocol's operations under another name derives.
- An interpolated string pattern (`"id-#{n}"`) compares against the interpolated value on both backends (native compared the raw text); a list inside it is rendered by `show`.
- A `const` whose initialiser declares a local or holds a lambda is evaluated through a function of its own (each read owns a fresh value).
- A record head (`Name { … }`, literal, pattern, update) is settled like any written name: the expected type's record first, else the ladder over records; a bare head two imports both declare a record for is the ambiguity error (before: compiled).
- A diagnostic names a callee under its module only when the name is contested in the program; a builtin and the reader's own declaration print bare.
- `--dump-typed` prints `sym#<id> <name>` for a declaration and `ident <name>` for a local.
- Cache formats are bumped (the serialised AST changed shape).

## Verification (local, macOS)

- The compiler type-checks, builds from the release boot compiler, and self-hosts; `make tier0` passes.
- Native compiler built locally: the namespace suites are at their baselines on C, the negatives and native; the solo test-runner, segment-handle, interpolation and const fixtures pass on both backends.
- The complete leak gates (C and native, 704 fixtures each) match their pins with growth 0.
- Compile cost against `origin/main` (self-built C-backend compilers, same input, alternated ×3, measured at the first push): instructions −41 % (423 G vs 719 G), compile-alloc cells −13.8 %, peak RSS 1.9 GB vs 3.7 GB.
- A full local `make -k tier1` sweep is running; the first CI round stopped each shard at its first failure.

## Merge gate — not met yet

`tools/symid-names-gate.sh --final` is the merge gate and does not pass: `tools/symid-census/pending-written.txt` lists the sites that still decide by text after the resolver (8 rows, 148 at the first push). Each is converted to an id; a row stays as text only with the owner's acceptance of its proof. Accepted so far: type-directed constructor resolution in the typer (one place, keyed by the expected type's id, ambiguity is an error), and the idiom lint's read of a combinator's declared name (its subject is the naming convention; diagnostic only).

- [x] Interpolation bodies are expressions before the resolver; the text machinery is gone.
- [x] Compiler-minted declarations and binders are known by id or a typed mark; refinement predicates are resolved.
- [x] Typer: one resolving place for written names; references are minted by id.
- [x] Emitter keys: C-symbol tables, effect instances, KIR registers, C locals, discards, constructor rows, impl heads and extern types by id.
- [ ] Type parameters and `Self` get binders with ids (6 rows); stored refinement predicates (2).
- [ ] One emitted identifier per local id; no name slot beside an id in emitter rows.
- [ ] The numeric carriers' module joins the core set, so `Decimal` needs no import again; `type T = X` with an undeclared `X` is a one-member sum again.
- [ ] `--final` wired into tier0.
