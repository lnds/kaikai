# Repair rules — kaikai stage-2 compiler, "identity by type" (lane symid-names-by-type)

The node types in `stage2/compiler/ast.kai` were changed in one move so that **no name string sits where an
identity goes**. The compiler does not type-check any more. Your job is to repair the files you are given so
they type-check against the new types, **keeping what the code does**. Read this whole file first.

You work in YOUR OWN git worktree. Sources: `stage2/compiler/*.kai` (kaikai language). Syntax reference:
`<repo>/bin/kai info syntax` (never guess a form: match arms are `pat -> expr` one per line, lambdas are
`(x) => expr`, no `return`, interpolation is `"#{expr}"`).

## 1. What changed (old → new), all in `ast.kai` — read the new definitions there

Identity types (all `#[derive(BinSerialize)]`):
- `SymId` — a declaration (unchanged). `FnId(SymId, Int)` — a function instance (unchanged).
- `LocalId(Int)` — NEW: a local binder. `local_none()` (= not numbered yet), `local_eq(a, b)`, `local_raw(l)`,
  `local_minted(key: String)` (the id of a binder the COMPILER mints under a key), `local_is_minted(l)`.
- `Binder { id: LocalId, name: Written }` — NEW: a local binder where it is DECLARED. `binder_written(name)`
  (pre-resolve, not numbered), `binder_minted(key)`, `binder_numbered(b, id)`.
- `DeclRef = DrWritten(Option[Written], Written) | DrSym(SymId)` — replaces `TyRef`: a reference to a
  declaration is EITHER the text the source wrote (optional qualifier, name) OR the declaration's id. Helpers:
  `ref_written(n)`, `ref_qualified(q, n)`, `ref_sym(r)` (`sym_none()` while written), `ref_resolved(r)`.
  The old `ty_ref_*` helpers are gone.
- `OpRef { eff: SymId, op: Written, via: OpVia }`, `OpVia = ViaAmbient | ViaAlias(LocalId) | ViaRecv(LocalId)`
  — NEW: a resolved effect operation.
- `Written` (in `written.kai`) — opaque text the source wrote. `written(s)` wraps; `written_text(w)` unwraps and
  is allowed ONLY in: the resolver / scope / symbol-table files, diagnostics (messages, notes), printers
  (`fmt_*`, dumps), and emitters spelling a symbol or a runtime string. A `Written` is never compared with
  `==` and never used as a table key outside the resolver. A diagnostic may interpolate a `Written` directly
  (`"#{w}"`, it implements `Show`).

Expressions (`ExprKind`):
| old | new |
|---|---|
| `EVar(String)` | `EName(Written)` — ONLY before the resolver (parser output, pre-resolve sugar). After the resolver a name is one of the three below. |
| `ESym(SymId, Int, String)` | `ESym(SymId, Int)` — a declaration reference (id, instance). No name. |
| `ESym(local id, _, name)` / `EVar(local)` after resolve | `ELocal(LocalId)` — a read of a local binder. |
| `ESym(effect id, _, "Eff.op…")` / `EVar("Eff.op@a")` (a perform's callee) | `EOp(OpRef)` |
| `EModCall(String, String)` | `EModCall(Written, Written)` — ONLY before the resolver / typer settle it; a pass after the typer that used to build `EModCall(home, fn)` builds `ESym(id, spec_none())` (`call_site_target.kai home_fn_ref`). |
| `ERecordLit(TyRef, String, fields)` | `ERecordLit(DeclRef, fields)` — the ref is what was written (record type, or the constructor of a variant record); resolved it is that declaration's id. |
| `EHandle(body, String, SymId, targs, init, Option[String], clauses, ret)` | `EHandle(body, DeclRef, targs, init, Option[Binder], clauses, ret)` — effect ref; the `as` alias is a binder. |
| `EVariantsOf(te, [String])` | `EVariantsOf(te, [SymId])` — constructor ids. |
| `mk_ref(name, l, c)` | `mk_name(name, l, c)` (pre-resolve reference by text), `mk_local(id, l, c)`, `mk_sym(s, l, c)`. There is no `mk_ref`: every site decides which it means. |

Patterns / params / statements:
| old | new |
|---|---|
| `PBind(String)` | `PBind(Binder)` |
| `PList(ps, Option[String])` | `PList(ps, Option[Binder])` |
| `PAs(String, p)` / `PNarrow(String, te)` | `PAs(Binder, p)` / `PNarrow(Binder, te)` |
| `PVariant(Option[String], String, ps)` | `PVariant(DeclRef, ps)` — written `(qualifier, Ctor)`, resolved = the CONSTRUCTOR's id |
| `PVariantRecord(TyRef, String, fs)` | `PVariantRecord(DeclRef, fs)` — the constructor (or record type) written / its id |
| `Param { pname: String, … }` | `Param { binder: Binder, … }` (`p.pname` → `p.binder.name` : Written, `p.binder.id` : LocalId) |
| `SVar(String, …)` | `SVar(Binder, …)` |
| `HR(String, …)` (handler `return(x)`) | `HR(Binder, …)` |

Types and rows:
| old | new |
|---|---|
| `TyName(TyRef, String, args)` | `TyName(DeclRef, args)` |
| `TyCon(Option[String], String, args, SymId)` | `TyCon(SymId, args)` — identity only. Two `TyCon`s are the same type iff their ids are equal. |
| `RL(String, targs, SymId)` | `RL(DeclRef, targs)`; `rl_sym(l)`; `rl_name`/`rl_names`/`eff_ref_same` are gone |
| `Label { eff: String, ty_args, sym }` | `Label { sym, ty_args }` |
| `tycon_is(n, s, c)` | `core_ty_is(s, c)` |

Declarations (the NAME slot is gone; the id moved to the name's position):
| old | new |
|---|---|
| `DFn(pub, name, tps, ps, ret, row, body, l, c, mo, FnId)` | `DFn(pub, FnId, tps, ps, ret, row, body, l, c, mo)` |
| `DType(pub, name, tps, body, l, c, mo, xs, SymId)` | `DType(pub, SymId, tps, body, l, c, mo, xs)` |
| `DEffect(pub, name, tps, ops, dflt, l, c, mo, SymId)` | `DEffect(pub, SymId, tps, ops, dflt, l, c, mo)` |
| `DProtocol(pub, name, tps, ops, l, c, mo, SymId)` | `DProtocol(pub, SymId, tps, ops, l, c, mo)` |
| `DImpl(String proto, …, SymId)` | `DImpl(DeclRef proto, …, SymId)` |
| `DConst(pub, name, te, e, mo, l, c, SymId)` | `DConst(pub, SymId, te, e, mo, l, c)` |
| `Var(String, tys)` (a sum's variant) | `Var(SymId, tys)` — the constructor's id |
| `RI(name, tps, fields, mo, SymId)` | `RI(SymId, tps, fields, mo)`; `rec_find(recs, sym)`, `rec_field_index(recs, sym, field)` |
| `POR/PTPR/PIR/PrOR/PKey` | see `ast.kai`: the protocol's id first, names gone, labels as `Written` |

Unchanged on purpose: field names (`EField(e, String)`, `FI(String, e)`, `FD(_, String, …)`, `PField`), effect-op
and protocol-op labels where no id sits beside them (`EOp(String, …)`, `POp(String, …)`, `ChOp(String)`), type
parameters (`[String]`), module origin (`Option[String]`), unit names, positions.

## 1b. Silent traps — sites the type checker will NOT flag

The lead already renamed `EVar(` to `EName(` mechanically in every file, and left TEMPORARY shims so the whole
compiler reaches the type checker. Both hide sites that still type-check but no longer mean what they meant.
`grep` your files for each of these and decide every hit:

- **`EName(`** — every occurrence. Before the resolver (parser, surface lowering, desugar, derive/protocol
  lowering, fmt, anything that runs on parser output): it is right, keep it. After the resolver: the arm
  used to catch a LOCAL read (`ELocal(l)`), or a bare builtin/constructor/minted temporary (now `ESym` /
  `ELocal(local_minted(key))`), and as written it is dead code that silently changes behaviour — rewrite
  it to the id form it stood for. `EName(_) -> …` arms that ignore the payload are the dangerous ones:
  no type error will ever point at them.
- **A `_ ->` default** in a `match` on `ExprKind` / `PatKind` where an `EName`/`ESym` arm used to handle
  names: add the `ELocal(_)` / `EOp(_)` arm the default now swallows, if they need the treatment the name
  arm gave.
- **The TEMPORARY shims at the end of `ast.kai`** (`mk_ref`, `ty_ref_none`, `ty_ref_sym`, `ty_ref_qual`,
  `ty_ref_qualified`, `ty_ref_decl`, `ty_ref_written`, `ty_ref_stamp`, `sym_is_local`, `rl_name`, `rl_names`,
  `eff_ref_same`, `rec_find_decl`, `rec_find_with_field`, `rec_find_arity_loop`, `tycon_is`) and everything
  in `sym_read.kai` (`sym_read_kind`, …, which now return a type no match fits): rewrite EVERY use in your
  files; they will be deleted. `mk_ref` in particular compiles as a pre-resolve reference by text — after
  the resolver it must become `mk_local(…)` or `mk_sym(…)`. `sym_is_local(s)` is constantly false: delete
  the test with the branch it guarded (a local is `ELocal`).
- **`string_starts_with` / `==` on a name that came from a node** and now comes from `written_text(…)`:
  do not launder a name test through the accessor. If the test decided identity, use the id.

## 2. Where a name comes from now

- **A declaration's name**: the symbol table. `sym_name(tab: SymTab, s: SymId) : Written`,
  `sym_home(tab, s) : Written`, `sym_owner(tab, s) : SymId`, `sym_class(tab, s)`, `st_valid(tab, s)`
  (`symtab.kai`). For anything that may also be a core type / builtin / core effect with no table row use
  `decl_name(tab, s) : Written` (`decl_text.kai`). A pass that needs a name and has no table takes a
  `tab: SymTab` parameter; thread it from the caller (the driver has `sym_tab` / `rd.tab`). Do NOT put a name
  back into a node or a table row to avoid threading.
- **A specialisation's emitted name** (`ESym(s, inst)` / `DFn` with `inst != spec_none()`): `spec_text(specs:
  SpecNames, f: FnId) : Written` (`spec_names.kai`), produced by the monomorphiser and threaded to the emitters.
- **A local's name** (for emission, a dump, a diagnostic): from its `Binder` when you hold it; at a bare
  `ELocal(l)` reference, from the function's binder map: `ln_of_decl(d: Decl) : LocalNames` built once per
  declaration, `ln_text(ns: LocalNames, l: LocalId) : Written` (`local_names.kai`).
- **A declaration's home / "is it the root's" / "is it module M's"**: compare ids (`sym_owner`,
  `st_module_id(name)`), never home strings.

## 3. Locals

- The RESOLVER numbers every binder the source declares (`LocalId` 1, 2, … per walk) and writes each read as
  `ELocal(id)`. Before the resolver a binder is `binder_written(name)` and a read is `EName(written(name))`.
- A pass AFTER the resolver that mints a temporary uses `binder_minted(key)` for the binder and
  `mk_local(local_minted(key), l, c)` / `ELocal(local_minted(key))` for its reads, with the SAME key string it
  used to spell the temporary with (`"__pcs_arm_ret"`, `ret_slot_name(..)`, `"__unw_v" + n`, …). Same key ⇒ same
  binder, exactly as same spelling did before. A pass BEFORE the resolver that mints a temporary may keep
  using text (`binder_written(key)` + `mk_name(key, …)`): the resolver numbers it.
- A table, list, set or parameter that held BINDER NAMES (`[String]`, `nm: String`, name-keyed maps of locals)
  holds `LocalId`s now (`[LocalId]`, `l: LocalId`), compared with `local_eq` / by `local_raw`. Convert the
  whole helper chain in your files; do not convert an id back to text to keep a `String` table.
- "Is this reference a local?" is `ELocal(_)`. "Is this callee a declared function?" is `ESym(_, _)`.
  There is no "bare name that might be either" after the resolver; delete such fallbacks (an `EName` /
  `EModCall` arm in a pass that runs after the resolver is unreachable: remove it, or if the match must stay
  total, make it the arm that does what an unresolved leftover did before — never a by-name lookup).

## 4. Declarations

- A declaration node carries only its id. Its name is in the symbol table.
- A pass that CREATES a declaration (a dispatcher, an impl method, a derived fn, a fused fn, a specialisation,
  a lifted closure, a refinement predicate) mints the id with `sym_derive(cls, name, home)`
  (`decl_names.kai`: `NCFn`, `NCType`, `NCEffect`, `NCConst`, `NCProtocol`, `NCCtor`, …) and returns the new
  table rows (`NEnt(cls, name, home, exported)`) to its caller, which extends the table
  (`st_extend(tab, rows)`). A specialisation is NOT a new symbol: `FnId(generic's sym, inst)` plus an entry in
  `SpecNames`.
- The PARSER mints declaration and constructor ids (`parse_program` takes the file's module home) and returns
  the rows; nobody recovers a name by walking declarations.

## 5. Pass-local types

Any type defined in your files that holds a `String` (or a `Written`) beside a `SymId` / `FnId` / `DeclRef`
for the same thing (see `tools/symid-census/dual-slots.txt` for the known ones) loses the string: keep the
id, get text through §2 where it is printed. A label that has no id of its own and sits beside one becomes
`Written`.

## 6. How to work

- Fix ONLY the files assigned to you. Never edit `ast.kai`, `written.kai`, `symtab*.kai`, `decl_names.kai`,
  `decl_text.kai`, `local_names.kai`, `spec_names.kai` unless they are in your list: if you need something
  from them (a helper, a different shape), write the call as you need it and record it in the shared log
  (below); the owner adds it.
- Type errors of your files: `sh <scratchpad>/errs.sh "$(git rev-parse --show-toplevel)" <file1.kai> <file2.kai> …`
  prints the type-checker's errors that are located in those files (and the total count for them). The
  whole compiler is broken at once, so errors in OTHER files are expected and are not yours. Iterate until
  the script prints `0 errors` for your files, or only errors that are a cross-file mismatch you recorded.
  A burst of "cannot find X" usually means one parse error earlier in that file: fix the first error first.
- When you change the signature of a `pub` function, or need another file's function to take/return
  something different, APPEND one line to `<scratchpad>/repair/API.md`:
  `<file>.kai  <fn>  : <old signature>  =>  <new signature>  -- <who needs it>`; read that file before you
  decide how to call into a file that is not yours (another agent may already have changed it). Prefer the
  signature the rules imply (ids in, `tab` threaded) so two agents converge without talking.
- Keep behaviour. This is a type repair: the same decisions, taken on ids instead of names. Where the old
  code compared or looked up a NAME to decide which declaration / local / effect / constructor / type
  something is, use the id the node now carries. Where you cannot (no id exists there, or the id is not
  available without a redesign), do NOT invent a workaround with text: leave a `panic("identity: <what is
  missing> in <fn>")` on that path ONLY if the path is an error/unreachable case; otherwise stop on that
  site, leave the code as close to compiling as you can, and list it in your report under "needs design".
- No drive-by refactors, no reformatting. Comments: brief, timeless, or absent; fix or delete a comment your
  change makes false; every NEW `pub` symbol gets a one-line `#[doc("…")]`. New code in an existing big file
  stays small; do not grow `infer.kai` / `emit_c.kai` beyond what the repair needs.
- Do not build, do not run tests, do not run the corpus (`make`, `agent-verify.sh`, `fb.sh` are off limits):
  nothing builds until every file is repaired.
- Never `git stash`, never `git push`. Commit your files on your worktree's branch when done (stage files
  explicitly; message: `refactor(compiler): <area> reads identities, not names` + 2–5 lines + `Part of
  #2340`; no trailers).

## 7. Report (final message, short)

- Commit hash; files done; the error count for your files before / after.
- Cross-file changes you made or need (the lines you added to `API.md`).
- "Needs design": each site you could not repair by the rules, with file:function and what identity is missing.
- Anything that looked wrong in the existing code (do not fix; list).

## 8. Agreed names for the shared state (so parallel agents converge)

- C emitter context (`emit_c.kai EmitCtx`, owned by the emit_c-A agent): `cx.tab : SymTab`,
  `cx.specs : SpecNames`, `cx.locals : LocalNames` (set per declaration from `ln_of_decl(d)`; a lambda helper
  or clause body emitted separately gets the enclosing declaration's map).
- KIR lowering state (`kir_lower.kai LowerSt`, owned by the kir agent): `st.tab`, `st.specs`, `st.locals`,
  same meaning. KIR itself keeps its own register / symbol strings (it is emitted text); a KIR node that
  held an effect or function NAME beside its id keeps the id and spells the name from `st.tab` where the
  dump or the native emitter prints it.
- Typer environment (`infer.kai TyEnv` / `TyEntry`, owned by the infer-A agent): a value binding is keyed by
  what it binds — a declaration's `SymId`, or a local's `LocalId` — never by spelling. infer-A publishes the
  exact API (type and function names, signatures) in `repair/API.md` under the heading `## infer env API`
  as its FIRST action; the other infer agents and the typer-satellite agents write their env calls against
  it (repair other errors first while it is not there yet, and re-read API.md before touching an env call).
- Emitted name of a function: `decl_name(tab, sym)` for a declaration, `spec_text(specs, fid)` when
  `fnid_inst(fid) != spec_none()`. Emitted name of a local: `ln_text(locals, l)`. The mangling helpers that
  took a name `String` keep taking the text: read it with `written_text` at the emitter boundary.
- A pass's entry function that needs the table takes it as `tab: SymTab` right after the declarations
  (`pass(decls, tab, …)`); the driver agent threads it. Record the new signature in API.md.
- A file in nobody's batch (list: `<scratchpad>/repair/batches/*.txt`) that needs a small change for your
  plumbing (a signature threaded through it): make the change and record it in API.md.
