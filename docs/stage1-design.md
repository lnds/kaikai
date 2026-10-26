# stage 1 design

Architectural decisions for the self-hosting compiler written in kaikai-minimal.

Stage 1 is the **dogfooding** step: a compiler for kaikai, written in kaikai itself (using only the dialect that stage 0 accepts). It compiles kaikai-minimal to portable C, and it is **frozen** at that dialect.

## Relationship to stage 0

- Stage 0 (`stage0/kaic0`) compiles **stage 1's source** (`stage1/compiler.kai`, kaikai-minimal) into C, which `cc` then turns into `stage1/kaic1`.
- Stage 1 (`stage1/kaic1`) compiles kaikai-minimal, itself included, into C. It no longer carries the obligation to compile what stage 2 writes: stage 2 is built by the previous release (`KAIC_BOOT=release`, the default) or, from a bare `cc`, by the frozen C seed (`make kaic2 KAIC_BOOT=seed`). See `docs/build-system.md`.
- **Fixed-point bootstrap**: `make -C stage1 selfhost` builds `kaic1b` (stage 1 compiled by stage 1), has it compile `compiler.kai` again, and requires the two C outputs to be byte-identical.

Stages 0 and 1 are frozen, never deleted: together they keep a C-only path into the ecosystem.

## Non-goals for stage 1

- **LLVM backend**: stage 2's job. Stage 1 keeps emitting portable C.
- **Speed**: stage 1 still targets correctness and readability. Compile-time performance is a stage 2 concern.
- **Full Perceus**: stage 1 has only a basic dup/drop pass (see below). Reuse-in-place and drop specialization are stage 2.
- **Effects**: kaikai-minimal has none beyond its IO built-ins; effects and their inference are stage 2.
- **Fibers, actors, scheduler**: stage 2 — see `docs/structured-concurrency.md` and `docs/actors.md`.
- **Pretty-printed errors**: `file:line:col: message` is enough. Span underlining is optional.
- **Incremental compilation**: single-shot batch compiler.

## Constraints

- **Written in kaikai-minimal**: the subset defined in `docs/kaikai-minimal.md`, nothing more.
- **One source file**: stage 1 is a single file (`stage1/compiler.kai`) so bootstrapping it stays trivial — `make -C stage0 && make -C stage1`, which runs `kaic0 compiler.kai > build/stage1.c` and compiles that C with `cc`.
- **Size**: about 7K lines of kaikai.
- **Runtime**: reuses `stage0/runtime.h` verbatim for the emitted C. Only stage 2 retires uniform boxing.

## Compilation pipeline

Same four passes as stage 0:

```
.kai source
  → lex       (tokenize UTF-8 source into a list of tokens)
  → parse     (tokens → AST)
  → check     (name resolution + type checking; AST annotated with types)
  → emit      (typed AST → C source on stdout)
```

Each pass is a pure function: input in, output out. No global state.

## Source layout

```
stage1/
  compiler.kai          # entire compiler, one file
  Makefile              # build, tests, selfhost
  README.md             # how to build and run
```

Stage 1 resolves `import` for the programs it compiles, but its own source stays one file.

## Value representation

Stage 1 is compiled by stage 0, so the runtime representation is stage 0's uniform boxing: every kaikai value at runtime is a `KaiValue*` with an RC header and a tag. The stage-1 source does not see this — it just sees kaikai types. But the consequence is clear: stage 1 is *slow*. That is fine.

Compiling stage 1 with itself (`kaic1b`) keeps the same boxed representation; its dup/drop pass trims RC traffic but does not unbox.

## Representing compiler data in kaikai-minimal

kaikai-minimal has records, sum types, and lists. It does *not* have mutable state, hash maps, arrays, or tuples. Every stateful thing in stage 0's C is translated as follows:

| stage 0 (C) | stage 1 (kaikai-minimal) |
|---|---|
| `malloc` / `free` | handled by stage 0's RC runtime invisibly |
| `struct Lexer` with a cursor | record `Lexer { source, offset, line, col }`, return a new copy on each step |
| hash map (symbol table) | association list `[(String, Entry)]` using a sum-type pair `type Binding = Bind(String, Entry)` |
| array of tokens | linked list `[Token]`, built in reverse then reversed once (to keep building O(1)) |
| string buffer (for emit) | accumulate `[String]` chunks, join at end via `list_reverse` + fold with `string_concat` |
| early-exit in pass | return `Result[AST, Error]`, propagate by matching |
| AST mutated in place with types | return a new AST node. Since types are computed bottom-up, we just embed `Type` as a field of each node and construct the typed node fresh |

This is slower than mutation-based C, but it is what the language gives us and is still O(n) per pass modulo the association-list symbol table (which is fine — programs are small).

## AST design

The planned shape mirrored stage 0 as kaikai sum types, with positions in a separate wrapper. The shipped `stage1/compiler.kai` carries positions as record fields instead (`type Expr = { kind: ExprKind, line: Int, col: Int }`, with the variants in `ExprKind`). The planned form:

```kaikai-minimal
type Expr
  = EInt(Int)
  | EReal(Real)
  | EBool(Bool)
  | EStr(String)
  | EChar(Char)
  | EUnit
  | EVar(String)
  | ECall(Expr, [Expr])
  | EField(Expr, String)
  | EIndex(Expr, Expr)
  | EBinop(String, Expr, Expr)
  | EUnop(String, Expr)
  | EIf(Expr, Expr, Expr)
  | EMatch(Expr, [MatchArm])
  | ELambda([String], Expr)
  | ERecord(String, [FieldInit])
  | EList([Expr])
  | ERange(Expr, Expr, Option[Expr])
  | EBlock([Stmt])
  | EPipe(Expr, Expr)
  | EPlaceholder
  | EInterp([InterpPart])
```

Positions (`file:line:col`) are carried on every node in a separate wrapper:

```kaikai-minimal
type Located[a] = Loc(String, Int, Int, a)
```

For stage 1, we accept the overhead of wrapping each sub-expression in `Loc`. It is boxed anyway.

## Type representation

Not implemented: stage 1 shipped without a type checker. Its check pass mirrors stage 0's `check.c` with name resolution only — no type inference, no exhaustiveness. The planned representation was:

```text
type Type
  = TInt | TReal | TBool | TString | TChar | TUnit
  | TList(Type)
  | TFn([Type], Type)
  | TRecord(String, [(String, Type)])
  | TSum(String, [Variant])
  | TGen(String)                  (* generic parameter name *)
  | TApp(Type, [Type])            (* Option[T], Result[T,E], etc. *)
```

The plan had the check pass resolve every `TGen` at a call site by unifying against the argument types.

## Symbol table

Association lists. The shipped environment is a stack of name scopes, `type Env = { scopes: [[String]], had_error: Bool, file: String, spans: [Span] }`. The planned typed bindings were:

```text
type Binding
  = BVal(String, Type)
  | BFn(String, [Type], Type)
  | BType(String, TypeDecl)
  | BCtor(String, String, [Type], Type)   (* ctor name, parent sum, arg types, result type *)

type Scope = { bindings: [Binding] }
type Env   = { scopes: [Scope] }
```

Lookups are O(n × depth). For stage 1 that is fine.

## Emission

Mirror stage 0's emitter exactly at first:

- Each function becomes `static KaiValue *kai_<name>(KaiValue *arg0, ...)`.
- Public functions drop the `static` and keep the `kai_<name>` prefix.
- `fn main()` becomes `int main()` wrapping `kai_main`.
- Test blocks under `--test` emit extra `KaiValue *kai_test_<n>(...)` calls driven from `main`.

The emitter builds the output as a list of string chunks (`[String]`) and joins once at the end via `string_concat`. No file IO in the middle; stdout is written once.

## Features added on top of kaikai-minimal

The plan was for stage 1 to compile more than stage 0 does. What shipped:

- **Basic Perceus**: a dup/drop pass between check and emit. The last read of a binding transfers its reference, non-last reads and closure captures `dup`, and unused parameters drop at body entry.
- **Imports**: `import foo.bar` resolves to `<root>/foo/bar.kai`, names resolved across files.

The rest of the plan — placeholder field sections (`.name`), record update, exhaustiveness checking, effects with handlers, monomorphized generics, the `|` map pipe, a `kai test` subcommand — is stage 2. Stage 1 was frozen without them.

## Error reporting

Same format as stage 0:

```
path/to/file.kai:LINE:COL: error: <message>
```

No span underlining in stage 1 — it requires carrying the source text around, which we prefer not to.

## Testing strategy

`make -C stage1 test` runs every check from the repo's `stage1/` directory against `examples/minimal/*.kai` (and stage 1's own source where it applies):

1. **Tokens**: `kaic1 --tokens` must match `kaic0 --tokens`.
2. **Parse and check**: `kaic1 --ast` and a full lex + parse + check run must succeed.
3. **End-to-end**: each example compiled by `kaic1` and by `kaic0`, run, and the outputs (stdout, and stderr under `--test`) compared.

`make -C stage1 selfhost` is the determinism check: `kaic1b` (stage 1 compiled by stage 1) compiling `compiler.kai` must produce byte-identical C. It catches non-determinism in stage 1 (hashmap iteration order, leaked pointer addresses, timestamps). Stage 1 and stage 2 are not required to agree on emission: stage 1 has no typer, and stage 2 makes type-driven emission decisions (match tag-compare, niche-filling) that stage 1 cannot. See `docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md`.

## Milestones within phase 3

Each validated and committed before moving on:

1. **Design doc** (this file).
2. **Skeleton**: entry point, CLI parsing, file IO wrapper, Makefile target that builds `./kaic1` from `./kaic0 stage1/compiler.kai`. Prints "stage1 running" given any file.
3. **Lexer**: tokenize kaikai-minimal. Unit tests in kaikai for token sequences.
4. **Parser + AST**: parse every kaikai-minimal construct. Unit tests on sum-type shapes.
5. **Check**: name resolution, type checking, exhaustiveness. Unit tests.
6. **Emit: hello-world**: enough of the emitter to compile `hello.kai`. First end-to-end bootstrap.
7. **Emit: fizzbuzz**: arithmetic, `if`, `let`, function calls, `|>`. `fizzbuzz.kai` builds and runs.
8. **Emit: match, lists, closures**: `quicksort.kai` and `interp.kai` build and run.
9. **Test runner**: `--test` mode emits test functions. `--test` on all examples matches stage 0's output.
10. **Self-hosting checkpoint**: stage 1 compiled by stage 0 compiles stage 1. Fixed-point bootstrap verified.
11. **Feature extensions**: basic Perceus and minimal imports shipped; the rest moved to stage 2 (see *Features added on top of kaikai-minimal*).

## What stage 1 still cannot do (and does not need to)

- Effect inference.
- Full Perceus (reuse-in-place, drop specialization).
- Fibers, actors, concurrency.
- LLVM backend.
- `kai fmt`, `kai lsp`.
- Package manager.
- Pretty error messages with source underlines.

All of these are stage 2. The point of stage 1 is: **a compiler written in kaikai that bootstraps from C in one step**.
