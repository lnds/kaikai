# kaikai name resolution

How every name in a kaikai program is declared, keyed, resolved, and
emitted — one rule, applied uniformly to every class of name.

This document does not introduce a resolution model. kaikai already has
one: `docs/kind-system-design.md` fixes the precedence for habitant
symbols, and its reasoning is general. What this document does is
**enumerate every class of name** and state that the existing rule
applies to all of them, then work out what that costs in the AST and in
codegen.

The gap it closes is not a missing rule. It is that the rule was written
for one class and never applied to the rest, so most classes resolve by
whatever their table happened to do — usually first-wins over a flat
list. The set of correctly-scoped classes coincides exactly with the set
of classes someone filed a bug about, which is the signature of patching
rather than of design.

The cost of that is not theoretical. Two modules declaring a constructor
of the same name with different payloads compile cleanly and segfault.
Two handlers written at the same line and column in different modules
collapse into one symbol, so a correct program prints the wrong answer,
and which answer depends on import order. Two same-named effects fail as
a C compiler error quoting generated C at the user. Each is a name that
reached code generation carrying only its spelling.

## §1 — The rule (already decided)

From `docs/kind-system-design.md`, stated there for habitant symbols:

> **The precedence: qualification `>` `use kind` `>` unique-symbol.** If
> none of the first three resolve a multi-kind symbol, it is a **compile
> error demanding disambiguation** — never a silent "last declared wins".
> Order-of-declaration must NOT change a program's meaning.

Generalised over every class of name, with the module dimension included:

1. **Local scope** — innermost outward: pattern binders, `let`,
   parameters, capability bindings, type parameters.
2. **Explicit qualification** — `m.f`, `m.T`, `m.Ctor`, `Currency.USD`.
3. **Explicit opening** — `use E`, `use kind Currency`; and the current
   module's own declarations.
4. **Unique symbol** — exactly one candidate across imports.
5. **Otherwise: a compile error demanding disambiguation.**

Step 5 is the whole point. Never first-wins, never last-wins, never
import-order, never declaration-order. The kind-system doc already
justifies this — *"order-of-declaration must NOT change a program's
meaning"* — and cites module-name ambiguity as the precedent it follows.
The precedent has since drifted: modules do not currently behave this way
for most classes.

**Identity.** A name's identity is `(home, kind, name)`: the module that
declares it, the kind whose habitant it is, and its spelling. Two of the
three are missing in most tables today.

## §2 — Every class of name

This is the enumeration that never existed. Its absence is the root
cause: the one prior inventory listed seven *declaration forms*, which is
a different thing and structurally excludes constructors.

A name class is not a declaration form. `type T = A | B` is **one**
declaration form introducing **two** classes: the habitant `T` of kind
`Type`, and the constructors `A`/`B`, which are values. Enumerating
declaration forms and calling it complete is exactly how constructors
fell through.

Per `kind-system-design.md`, `type` and `effect` are not distinct
syntactic categories — they are the habitant introducers of built-in
kinds, exactly as `unit` is Measure's. That de-magics the table: most
rows below are one mechanism, not many.

### Habitants (classified by a kind)

| Introducer | Kind | Structure | Qualified form |
|---|---|---|---|
| `type` | `Type` | structured (fields/variants) | `m.T` |
| `effect` | `Effect` | structured (operations) | `m.E` |
| `unit` | `Measure` | atom | `m.u`, `Measure.u` |
| `currency` | `Currency` | atom | `Currency.USD` |
| `perm` | `Perm` | atom | `Perm.read` |
| `layout` | `Layout` | atom (closed) | `Layout.be` |
| user introducers | user kinds | per kind | `K.h` |

Extern types (`extern "C" type`) are `Type` habitants whose
representation is foreign. They are not a separate class.

### Values (classified by a type)

| Class | Introduced by | Qualified form |
|---|---|---|
| Function | `fn` | `m.f` |
| Constant | `const` | `m.C` |
| Axiom | `axiom` | `m.f` |
| Extern function | `extern "C" fn` | `m.f` |
| **Variant constructor** | `type T = A \| B` | `m.A` |

Constructors are values that live in expression **and** pattern position.
That dual residence is why no declaration-form inventory catches them.

### Members (reached through their owner)

| Class | Owner | Qualified form |
|---|---|---|
| Record field | a type | — (structural, via receiver) |
| **Effect operation** | an effect | `m.E.op` |
| **Protocol operation** | a protocol | `m.P.op` |

Record fields are resolved by the receiver's type and never enter a
global table. This is correct today and stays.

### Modules and introducers

| Class | Introduced by | Qualified form |
|---|---|---|
| **Module** | a file in a package | — (it *is* the qualifier) |
| **Habitant introducer** | `kind K … with intro` | — (see below) |

A module name is a name: it competes in the `.` table of §4, a local can
shadow it, and its identity is the subject of §6. Omitting it from an
enumeration of name classes would repeat the very defect this section
exists to correct.

Habitant introducers are names too — `unit`, `currency`, `perm` and any
registered by a user kind — and they are the one class §3 cannot fully
cover. An introducer *is* the declaration syntax (`unit km`), so there is
no position to qualify. Two imported modules registering the same
introducer word therefore cannot be disambiguated by the reader, and the
only clean rule is to **reject the duplicate registration** at the
importing module. This is the single place where prohibition is the
answer rather than use-site disambiguation, and it is called out here
rather than left as an implicit exception.

### Classifiers and predicates

| Class | What it is | Qualified form |
|---|---|---|
| Kind | classifies types/habitants | `m.K` |
| Theory | classifies kinds | — (catalog-only) |
| Protocol | predicate over types | `m.P` |
| Protocol law set | property set named in a header | own namespace |
| Impl | a (protocol, type) pair | — (not named) |

A protocol is **not** a kind habitant. It has no unification algebra: it
is a predicate over types, the family of `Eq`/`Show`. It *consumes* kinds
in its parameters (`protocol Container[s: Shape]`) and *names* a law set
in its header (`protocol P[s: Shape] : Functorial`). Law names already
live in their own namespace, disjoint from the theory catalog —
`stdlib/core/kinds.kai` states this and the parser enforces both
directions.

`theory` sits one level above kinds and is declarable only in the catalog
file. It is not a peer of `type`.

### Lexically scoped (correct today)

Local bindings, function parameters, pattern binders, capability
bindings (`with E as a`), type parameters. Listed for completeness.

## §3 — Collision is legal; ambiguity is not

Two habitants of different kinds may share a spelling in the same module.
This is already decided for atoms — `unit USD` and `cur USD` are two
distinct habitants sharing the symbol `USD`, *deliberately*, because
money-with-physics needs `USD/kWh` to cancel. The same holds for
structured habitants:

```kaikai
type Log = { level: Int }        # habitant of Type
effect Log { write(s: String) }  # habitant of Effect
```

Both declarations are legal. `Log` is two habitants of two kinds.

What is **not** legal is an unresolved use. Where the syntactic position
selects the kind — an annotation selects `Type`, an effect row selects
`Effect` — the use is unambiguous and nothing is required of the user.
A `<>` suffix is weaker: it selects the habitant *category*, not the
kind, so a symbol inhabiting several kinds still resolves by the habitant
precedence of §1. Where position does not decide, §1 applies: qualify,
open, or get an error.

This is the correction to a tempting but wrong framing: that names in
"separate namespaces never collide". They do collide, at use sites where
position is not decisive. Declaring the problem away by partitioning
namespaces is what leaves the ambiguity to be resolved silently by table
order. The rule already chosen — legal to declare, error to use
ambiguously — handles it without prohibiting anything.

It follows that prohibiting `type X` + `effect X` is unnecessary. It
would buy nothing the rule does not already give, and would cost a
legitimate program its spelling.

The one exception is habitant introducers, for the reason given in §2:
an introducer is the declaration syntax itself, so a duplicate offers the
reader no position to qualify. There the answer is rejection, not
use-site disambiguation. Stating the exception is what keeps the rule
honest — "nothing needs prohibiting" was too strong.

## §4 — Qualification must exist everywhere

An error demanding disambiguation is only honest if the user has
something to type. Today they often do not:

- `m.Ctor` **parses, validates against the module's exports, and is then
  discarded** — the resolved tree keeps a bare name, in expressions and
  in patterns alike. The pattern AST has no slot for a qualifier, so the
  annotation a user writes to disambiguate does nothing.
- Effect operations, unit suffixes and kinds have no qualified surface
  at all.

So every class in §2 gets a qualified form, and **the qualifier is
semantic**: it survives lowering and selects the named declaration. A
qualifier the compiler validates and then drops is worse than no
qualifier, because it looks like a fix.

**The `.` disambiguation table.** `.` is overloaded: record field, UFCS
call, module path, effect operation. Adding the three-segment `m.E.op`
makes five. The decision is not made at one point today — the module
rewrite happens syntactically before inference, the effect check happens
during it — which is why the precedence has never been stated.

Habitant qualification is **not** in this table. `Kind.habitant` is
resolved in the parser, in `<>` position, and never becomes a field
access. It follows the habitant precedence already specified in the
kind-system document.

The order, matching what the compiler does today except where noted:

| # | Base resolves to | Result |
|---|---|---|
| 1 | **local binding** | field access, or UFCS call |
| 2 | `bit` | bitwise intrinsic (special case) |
| 3 | **module** | member of that module |
| 4 | **effect** in scope | operation |
| 5 | nothing above | unresolved-name error listing what was tried |

Module before effect is deliberate: an import is an explicit, visible
declaration at the head of the file, while an effect in scope may arrive
through an inferred row. It also matches today's behaviour, so it
introduces no change.

Three sub-rules that are real precedences and belong here:

- **Field beats UFCS.** With a local receiver, if the record has the
  field, it is a field access; UFCS applies only when it does not. A
  record with a field `area` and a function `area(r: Rect)` in scope
  resolves to the field.
- **Capability locals dispatch, not project.** A local bound by
  `handle … with E as a` is a local by §1, but `a.op` is an operation
  dispatch, not field access. Row 1 covers ordinary locals; capability
  locals take the operation path.
- **Three-segment bases.** Only `EField(EVar(m), f)` is a module-rewrite
  candidate today; `a.b.c` never resolves to a module path. Supporting
  `m.E.op` requires extending that walk to nested paths, and once
  extended, `a.b.c` becomes ambiguous with record-of-record projection
  (`user.address.city`) and with module-record-field
  (`config.default.port`). The rule: a nested path resolves as a module
  path only when the leading segment resolves to a module and the
  remainder is not a valid projection from it. This is new work, not a
  formalisation of existing behaviour.

A local named `log` makes a module `log` unreachable in that scope. That
is correct — locals must win — but the escape hatch is renaming the
local, and the diagnostic must say so rather than reporting an unknown
member.

Whether `.` should carry all five, or module paths should move to a
distinct token, is a real question. `.` is kept because changing it is an
edition-scale surface break and the table makes `.` workable. If it
proves unworkable in implementation, a separate path token is the
fallback, and it belongs to `orongo`.

## §5 — Emitted symbols

Every emitted symbol derives from `(home, …)`, plus whatever
distinguishes it within that home.

| Symbol | Form |
|---|---|
| function | `kai_<home>__<name>` |
| user function | `kaiu_<home>__<name>` |
| variant tag | hash of `(home, ctor)`, linear probe — see below |
| variant table entry | keyed `(home, ctor)`, carrying arity + payload |
| impl method | `__pimpl_<home_P>__<P>_<T>__<home_T>_<op>` — each half home-spelled only when its name is contested |
| protocol dispatcher | `__proto_<home_P>__<P>_<op>` |
| handler / clause / finally | `_kaiu_<what>_<home>__<enc_fn>_<L>_<C>` |
| test / bench / check | `__test__<home>__<L>_<C>` |
| evidence label | `<home>::<E>` |
| extern "C" fn | the C symbol verbatim — see below |

**Position is not identity.** Two modules both have a line 2, column 1.
Any symbol minted from `(line, col)` alone collides across modules, and
the collision is silent: the linker sees one definition and is content.
The compiler already knows this in one place — the clause-function
symbol includes its enclosing function precisely because specialisations
share a source position — and the same reasoning was never applied to
handler, clause and finally symbols.

**Variant tags: fix the key, keep the scheme.** The tag collision that
produces the segfault does not come from hashing. It comes from the
table deduplicating by bare constructor name: two homonymous
constructors from different modules collapse to one entry, the first
wins, and every module emits against the first one's arity and payload
layout. The hash is downstream of that.

The existing scheme — slot = hash of the name, linear probe, names
inserted in sorted order — was chosen for a property this document must
not discard, and the reason is recorded in the source: a tag is a
function of the program's *set* of constructor names, so adding or
moving a type in one module cannot renumber the tags already baked into
other modules' emitted code. That is stability under separate
compilation, and this compiler emits and caches per module.

Replacing it with a dense index over a globally sorted set would destroy
exactly that property: inserting one pair that sorts before an existing
one renumbers every tag after it, invalidating every cached module in
the program on any constructor addition anywhere. Determinism within one
compilation is not the requirement; stability across partial
recompilation is, and the current scheme already has it.

So the change is one line of intent: **the hash key becomes
`(home, ctor)` instead of `ctor`, and the table stops deduplicating by
bare name.** Distinct homes then occupy distinct slots by construction,
probing still resolves incidental hash collisions, and tag stability is
preserved. Sorting before insertion — already done — keeps probe order
independent of traversal order, which the byte-identical self-host gate
requires.

**Effect evidence.** The `Ev` struct type is module-qualified while the
runtime evidence label is bare, so push and lookup disagree and an op
call reads a function pointer at the wrong offset. Both sides now key
on one spelling: a contested effect name is respelled per declaring
module (`Emit__ea`) before resolution, in the declaration, every row
label, handler head, `use`, and perform, so the typer, the evidence
struct and the runtime label all see distinct effects. Core and root
declarations keep the bare spelling, so the fixed runtime labels
(`Cancel`, `Link`, `Monitor`, `Spawn`, `Actor`) are unchanged.

**`extern "C"` is the deliberate exception.** The emitted symbol must
match the foreign library, so it cannot be namespaced. Kaikai-side the
name is still `(home, name)` and resolves by §1; only the C symbol is
bare. Two modules declaring the same extern symbol with incompatible
signatures is a diagnosable conflict, checked before emission rather
than left to `cc`.

## §6 — Module identity

A module's home is its **package-relative path**, not its basename, and
the full identity is `(package, path)`. Two packages may each contain
`net/http`.

The home is assigned once, when the module is parsed, from its canonical
path — never from the spelling the importing file used. Today the tag
comes from the importer's chosen name, so a module's identity, and every
symbol it emits, depends on which import first reached it.

Import aliases are per-file surface: `import loop as lp` makes `lp`
resolve to the home `loop` in that file and never enters a global table.
This is correct today and stays.

**Re-export.** Uniqueness in §1 step 4 is over the *home*, not over the
import. If `a` re-exports a name from `b`, its home stays `b`; importing
both `a` and `b` yields one candidate, not an ambiguity.

## §7 — Diagnostics

Three requirements, each replacing a measured failure:

- **Ambiguity is named as ambiguity**, listing every candidate with its
  home and showing the qualified form that resolves it. A contested
  constructor currently produces a type-mismatch error suggesting the
  user extend one union with the other — advice for a problem they do not
  have, because the compiler no longer knows a collision occurred.
- **No raw C reaches the user.** Two same-named effects currently fail as
  a `cc` error quoting generated C. Any condition that would produce
  invalid or mislinked C is diagnosed in the compiler.
- **Positions belong to the file that owns them.** A diagnostic carries
  the home of the code it describes, not the file that imported it.

## §8 — Staging

Under the stability rule, surface breaks require an edition bump.

**Bug fixes — ship in `hanga-roa`.** Programs that miscompile today start
working, or start failing honestly. No program with defined behaviour
changes meaning.

- Constructor, effect-op, handler, clause, finally and test symbol
  keying. A program depending on today's behaviour depends on a segfault
  or a wrong answer; there is nothing to preserve. **A deprecation
  warning is not available for these** — the current behaviour is memory
  corruption, and corruption cannot be kept for an edition as a
  compatibility measure.
- Per-home variant tags: the hash key becomes `(home, ctor)`, the
  hashing scheme itself is kept (§5).
- Evidence labels keyed by home.
- New qualified syntax for effect ops, units and kinds — pure addition.

**Behaviour changes — announce, ship in `hanga-roa`.**

- `m.Ctor` becoming semantic changes the meaning of programs that compile
  cleanly today: a program writing `a.Node(x)` and silently receiving
  `b`'s constructor will now receive `a`'s. This is the behaviour the
  user asked for, which is precisely why it is a change and not a no-op.
- **Qualified patterns start being validated.** A pattern naming a module
  that does not exist compiles today, because the qualifier is dissolved
  without being read. Such programs will stop compiling. This is a
  different change from `m.Ctor` selecting the right constructor, and
  needs its own line: one fixes a wrong answer, the other rejects code
  that was never checked.
- **Extern signature conflicts.** Two modules independently binding the
  same C function is an ordinary pattern, and today it compiles even when
  the two kaikai-side signatures differ in ways C does not care about —
  a width or signedness spelled differently on each side. Such a program
  has defined behaviour today: it links and runs correctly. Diagnosing
  the conflict therefore produces new errors on working code, which
  fails the bug-fix bar this section sets. It is announced, and the
  diagnostic must distinguish a genuine ABI conflict from two spellings
  that agree on the C type.

**Breaking — `orongo`.** Bare use of a name provided by two imported
modules becomes an error. In `hanga-roa` it warns, naming both candidates
and the qualified form, and keeps today's resolution — which is safe here
because for this class today's resolution is a defined choice rather than
corruption.

**Not surface, still breaking.** Two consequences fall outside the
stability rule but need announcing:

- **C symbol spellings change** when homes move from basename to path.
  Emitted C symbols are not surface a user writes against, so the
  stability rule does not cover them — but three things anchor them and
  break mechanically, and each needs an owner rather than a mention:
  build-system assertions that grep for symbol spellings (which have
  previously gone permissive in silence rather than failing, so they must
  be re-verified to still assert something after the change), KIR golden
  fixtures with embedded symbol names (regenerated, with the diff read
  rather than blessed), and any external code linking emitted symbols.
  The lane that changes homes owns all three in the same change.
- **Cached artefacts are invalidated** by the AST changes in §9 — a
  one-time cache key bump. Keeping the existing tag scheme (§5) is what
  keeps this one-time: a global renumbering scheme would have made
  invalidation a permanent property rather than a migration cost.
- **Typed-interface hashing changes shape, not just value.** When a
  symbol's identity becomes `(home, kind, name)`, the interface hash that
  underpins incremental rebuild is computed over different data. This is
  the most expensive consequence in this document and it is not a bump:
  it is a change to what the hash denotes. It belongs to the same lane as
  step 6, since module identity is what changes underneath it.

The migration is mechanical only where the program is already correct.
Where a program compiled by accident against the wrong declaration,
someone must decide which module was meant — qualifying with the wrong
one preserves the bug in new syntax.

## §9 — What this costs

This cannot be a fifth corrective pass. Four exist; each rewrites an
already-misresolved tree, and each was written after a bug report. A
fifth would fix one class and leave the rest waiting for theirs.

1. **Every declaration form carries `home` in the AST.** Today one form
   has its own slot and several have none at all — for those the
   information is not dropped, there is nowhere to put it, so every
   downstream consumer guesses.
2. **`home` is populated when the module is parsed**, not stamped by a
   later traversal, so no pass observes a partially-tagged tree.
3. **Tables are keyed by `(home, name)` at construction**, not filtered
   afterwards. The four corrective passes disappear, and with them the
   ordering hazards between them.
4. **Name-carrying AST nodes carry their home.** `EVar(String)` becomes
   `EVar(Home, String)`; `PVariant(String, [Pattern])` becomes
   `PVariant(Home, String, [Pattern])`.

Point 4 is stated as arity, not as an unrepresentable-state claim. Making
a bare name unrepresentable would require a resolved AST distinct from
the surface AST — a second expression tree, and with it a second copy of
every walker: cache serialisation, formatter, typer, Perceus, KIR
lowering, both backends. No other stage of this compiler duplicates the
tree, and the language has no mechanism to make the distinction cheaply.

What extended arity buys instead is that the home cannot be *forgotten*:
every construction site fails to compile until someone supplies it, and
exhaustive matching means the compiler enumerates the sites. `Home`
carries an explicit `HUnknown` constructor for the transition, which is
greppable and ratcheted in tier0 — a count that may fall and never rise.

A ratchet prevents regression; it does not by itself create pressure to
finish, and this project has a ratchet that has outlived its migration.
So `HUnknown` ships with a closing criterion, not an open-ended budget:
**the target is zero, the tracking issue that opens the ratchet is the
one that closes it, and it closes in the same edition.** There is no
allowed residue — every construction site in the compiler knows its
module or is a bug. If a site genuinely cannot know its home, that is a
finding about this design, to be resolved here rather than absorbed by
the counter.

**Blast radius.** Positional matching over declaration constructors
appears in the high hundreds of sites across dozens of files; the
expression-variable constructor alone appears in the hundreds. These are
arity breaks — mechanical, compiler-enforced, individually trivial, and
collectively large.

The variant-pattern constructor deserves its own accounting, because
"typer key and emit table" badly understates it. It is consumed across
more than two dozen modules, and the heaviest consumers after the
emitter are **Perceus and the KIR reuse family** — ownership walkers,
borrow/escape analysis, reuse donation, pattern aliasing. Those walkers
are deliberately exhaustive, with no catch-all arm, which is what makes
them safe: an arity change surfaces every site at build time rather than
silently skipping one. It also means changing variant identity is a
Perceus-touching change, not a front-end change. Any plan that budgets
for the typer and the emitter and forgets ownership will discover the
remainder mid-migration.

**Order.** Structural first, resolution last.

**Step 1 is atomic and gates everything else.** Adding `home` to the
declaration forms that lack a slot is one indivisible lane, not a
sequence: the tag table in §5 allocates ranges per home, so a
partially-homed tree yields home ranges that shift as the remaining
forms are migrated. Homing must be complete before any tag is assigned.

2. **Preserve the qualifier through lowering, first.** Today the module
   rewrite collapses `m.Ctor` to a bare name before the typer runs, and
   surface lowering dissolves the qualified pattern form without reading
   it. Adding a home slot while both still discard it yields a slot that
   is empty at every site and a ratchet that cannot fall. This sub-step
   gates the rest of step 2. It also changes behaviour on its own: a
   qualified pattern naming a module that does not exist compiles today,
   because nothing validates it — see §8.
3. Variant constructors end-to-end — typer key, emit table, the
   `(home, ctor)` tag key, and every ownership and KIR consumer of the
   variant pattern. This closes the segfault. It is the largest step and
   should be planned as several lanes behind one flag, not one lane.
4. Handler, clause, finally and test symbols. The C backend already
   includes the enclosing function in its clause symbol; the KIR/native
   path has the same value available and does not pass it. So this is
   porting one backend's form to the other, not inventing a form — and
   the divergence means a fix verified on one backend proves nothing
   about the other. The test symbol doubles as the enclosing-function
   name for lambdas lifted out of the test body; both uses must change
   together or those lambdas keep colliding.
5. Effect operations and evidence labels. The emitter currently falls
   back to *no* qualification when it finds two same-named effects with
   different homes — the exact collision case degrades to the ambiguous
   one. That fallback must be removed in this step, not left to survive
   the refactor.
6. Impls and protocol dispatchers. The impl table keys on the
   protocol's home but on the target type's bare name, so two modules
   each declaring their own same-named type and implementing the same
   protocol for it collide — reported as a spurious "duplicate impl".
   The dispatcher symbol carries no home at all, so two protocols
   sharing an operation name mint one dispatcher between them. Both
   keys gain the type's home, matching the mangled form in §5.

   This reaches users through `#[derive]` rather than through
   hand-written impls: a package deriving `Eq` for its own `Entry` and
   another doing the same is a routine composition, and it is the shape
   this defect has been reported in. The types themselves already scope
   correctly — only the synthesised impl collides — so the failure looks
   like a naming problem and gets paid for with a rename.
7. The remaining bare-keyed tables, which share one shape: a registry
   built on the spelling alone, first entry winning. None of them
   diagnoses anything today.

   - **Extern types.** Two `extern "C" type Color` with incompatible C
     layouts (three bytes against sixteen) both compile; the first wins
     and any use of the second unpacks with the wrong layout.
   - **Kind names.** Two `kind Metric` over different theories coexist
     silently, so a type's unification algebra depends on which module
     was reached first.
   - **Habitant introducers.** Two kinds registering the same
     introducer word are accepted. This is the one class §3 cannot
     resolve at the use site — an introducer *is* the declaration
     syntax, so the duplicate must be rejected outright.
   - **Protocol laws and impl axioms.** `PL(name, thy)` and
     `AI(pname, head, thy)` drop the module that is bound right there
     in the pattern, so a law exemption declared in one module silently
     exempts an unrelated impl in another.
   - **`#[constructor]`.** Two modules with their own type and their own
     constructor attribute produce a spurious "already has a
     `#[constructor]`".
   - **`use kind`.** Habitants opened per module, keyed globally.

8. Module identity from path, and the removal of the last-loaded-alias
   mapping described in §6. Two defects belong here rather than to the
   resolver: an alias that shadows an imported module emits a duplicate
   C symbol (`redefinition of kaiu_<mod>__<fn>`), and a selective import
   naming a symbol the module does not export compiles in silence, so a
   typo in an import list is never reported.
9. The unified resolver: opened names, ambiguity as a use-site error.
10. Qualified surface for the classes that lack it, with `kai info`
    updated in the same change — the qualified forms are surface, and
    `kai info` is authoritative for surface in this repo. Three
    positions reject a qualifier today: handler heads (`with m.E`),
    effect operations (`m.E.op`), and `impl` (`impl m.P for T`).

**Every class in §2 is claimed by a step above.** That correspondence is
the point: the failure this document exists to correct was an
enumeration that covered the classes someone had complained about and
left the rest to wait for their own bug report. A step list ordered by
observed pain would repeat it.

**Quality bar during migration.** These lanes touch existing monoliths at
hundreds of sites, and threading a field through one does not improve
it. That is not licence to leave them as found.

Each lane records `make test-km-ledger` before and after, and reports
the delta in its PR. The baseline when this began: the compiler at
F (52.5) over 90k lines, with `infer.kai` at F−− (30.5) over 14k,
`emit_c.kai` at F−− (32.8), `protos.kai` at F−− (39.8), and four
corrective scope passes alive.

Two obligations follow. A lane must not make its files worse — an arity
migration that grows a monolith without cause is a regression the ledger
will show. And where a lane is already rewriting a region of one of
these files, it is expected to leave that region better: extract the
helper, split the walker, drop the branch the new keying makes dead.
Paying down structure while passing through is the only way these files
improve at all, because nobody schedules a lane whose sole purpose is to
split `infer.kai`.

The corrective passes are the sharpest measure here. `proto_scope`
alone remains of the four the refactor set out to retire. `ta_scope`
retired once transparent aliases respelled unconditionally at birth,
so expansion keys a globally unique name with no filter; `const_scope`
followed, its collision ladder moving into `name_uses` fed by
`nt_collect` boundary-stamped homes; `unit_scope` joined them, its
habitant site table now collected in `name_uses` directly from the
boundary-stamped stream. Keying at construction makes each one dead
code, so the count going from four to zero is the structural result
this refactor is for — more than any individual grade.

**Cost at compile time.** Comparing a pair instead of a string, in tables
that are already linear scans, does not change complexity. The compile-time
cost is in the mechanical churn, not in the keying.

**Risk.** The compiler is self-hosted over roughly 120 modules. Some of
them may resolve ambiguously today and compile only because of the
silence. The first build under the new resolver is expected to report
against the compiler itself, not only against user code. This cannot be
determined by inspection; it surfaces on implementation.
