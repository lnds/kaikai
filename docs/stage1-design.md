# stage 1 design

Architectural decisions for the self-hosting compiler written in kaikai-minimal.

Stage 1 is the **dogfooding** step: a compiler for kaikai, written in kaikai itself (using only the subset that stage 0 accepts). Its first milestone is to match stage 0's capabilities — compile kaikai-minimal to portable C — and then grow to cover the rest of full kaikai incrementally.

## Relationship to stage 0

- Stage 0 (`./kaic0`) compiles **stage 1's sources** (kaikai-minimal) into C, which `cc` then turns into `./kaic1`.
- Stage 1 (`./kaic1`) compiles **enough of full kaikai to compile stage 2** into C as well. The LLVM backend, the full effect catalog (Console / Stdin / Env / File / State / Reader / Writer / Mutable), m7b sugars (trailing lambdas, naked cell read / `cap := v`, `var`, `a[i]`), fibers, and actors are deferred to stage 2 — see `docs/stage2-design.md` §*Milestones within stage 2*.
- **Fixed-point bootstrap**: once stage 1 accepts the same subset stage 0 does, `kaic1` must compile `stage1/*.kai` and produce a binary behaviorally identical to `kaic1` itself. This is the self-hosting checkpoint.

Stage 0 is not thrown away yet: it remains the one C-only ingress into the ecosystem. Stage 1 replaces it as the working compiler.

## Non-goals for stage 1

- **LLVM backend**: stage 2's job. Stage 1 keeps emitting portable C.
- **Speed**: stage 1 still targets correctness and readability. Compile-time performance is a stage 2 concern.
- **Full Perceus**: only *basic* reuse analysis lands in stage 1 (see below). Reuse-in-place and drop specialization are stage 2.
- **Effect inference**: explicit effect annotations on every function that uses effects. Inference lands in stage 2.
- **Fibers, actors, scheduler**: stage 2 m8 — see `docs/structured-concurrency.md` and `docs/actors.md`.
- **Pretty-printed errors**: `file:line:col: message` is enough. Span underlining is optional.
- **Incremental compilation**: single-shot batch compiler.

## Constraints

- **Written in kaikai-minimal**: the subset defined in `docs/kaikai-minimal.md`, nothing more.
- **No imports across files** *initially*: stage 0 compiles one file at a time, so stage 1's source has to be one monolithic file (`stage1/compiler.kai`) or several files concatenated by a trivial shell step. We choose **one file** to keep bootstrap trivial — `cc stage0/*.c -o kaic0 && ./kaic0 stage1/compiler.kai > stage1.c && cc stage1.c -o kaic1`.
- **Target size**: 5–10K lines of kaikai. Comparable to stage 0.
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
  compiler.kai          # entire compiler, one file for now
  README.md             # how to build and run
  tests/                # kaikai-level tests (run with --test)
```

Keeping everything in one file is a concession to stage 0's single-file constraint. When stage 1 gains minimal import support, we can split it along the same axes as stage 0 (`lexer.kai`, `parser.kai`, `check.kai`, `emit.kai`, `main.kai`).

## Value representation

Stage 1 is compiled by stage 0, so the runtime representation is stage 0's uniform boxing: every kaikai value at runtime is a `KaiValue*` with an RC header and a tag. The stage-1 source does not see this — it just sees kaikai types. But the consequence is clear: stage 1 is *slow*. That is fine.

Once stage 1 becomes self-hosting, we can compile it **with stage 1** (not stage 0) and take advantage of stage 1's basic Perceus for a measurable speedup. That verification is post-self-hosting-checkpoint.

## Representing compiler data in kaikai-minimal

kaikai-minimal has records, sum types, and lists. It does *not* have mutable state, hash maps, arrays, or tuples. Every stateful thing in stage 0's C is translated as follows:

| stage 0 (C) | stage 1 (kaikai-minimal) |
|---|---|
| `malloc` / `free` | handled by stage 0's RC runtime invisibly |
| `struct Lexer` with a cursor | record `Lexer { source, offset, line, col }`, return a new copy on each step |
| hash map (symbol table) | association list `[(String, Entry)]` using a sum-type pair `type Binding = Bind(String, Entry)` |
| array of tokens | linked list `[Token]`, built in reverse then reversed once (to keep building O(1)) |
| string buffer (for emit) | accumulate `[String]` chunks, join at end via `list_reverse` + fold with `string_concat` |
| early-exit in pass | return `Result[Error, AST]`, propagate by matching |
| AST mutated in place with types | return a new AST node. Since types are computed bottom-up, we just embed `Type` as a field of each node and construct the typed node fresh |

This is slower than mutation-based C, but it is what the language gives us and is still O(n) per pass modulo the association-list symbol table (which is fine — programs are small).

## AST design

Same shape as stage 0, but as kaikai sum types:

```kai
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

```kai
type Located[a] = Loc(String, Int, Int, a)
```

For stage 1, we accept the overhead of wrapping each sub-expression in `Loc`. It is boxed anyway.

## Type representation

```kai
type Type
  = TInt | TReal | TBool | TString | TChar | TUnit
  | TList(Type)
  | TFn([Type], Type)
  | TRecord(String, [(String, Type)])
  | TSum(String, [Variant])
  | TGen(String)                  (* generic parameter name *)
  | TApp(Type, [Type])            (* Option[T], Result[E,T], etc. *)
```

The check pass resolves every `TGen` appearing at a call site by unifying against the argument types, exactly like stage 0.

## Symbol table

Association lists. A `Scope` is a list of `Binding`s; scopes are nested via a parent pointer (implemented as a list of scopes `[Scope]` and traversal).

```kai
type Binding
  = BVal(String, Type)
  | BFn(String, [Type], Type)
  | BType(String, TypeDecl)
  | BCtor(String, String, [Type], Type)   (* ctor name, parent sum, arg types, result type *)

type Scope = { bindings: [Binding] }
type Env   = { scopes: [Scope] }
```

Lookups are O(n × depth). For stage 1 that is fine — programs are <10K lines.

## Emission

Mirror stage 0's emitter exactly at first:

- Each function becomes `static KaiValue *kai_<name>(KaiValue *arg0, ...)`.
- Public functions drop the `static` and keep the `kai_<name>` prefix.
- `fn main()` becomes `int main()` wrapping `kai_main`.
- Test blocks under `--test` emit extra `KaiValue *kai_test_<n>(...)` calls driven from `main`.

The emitter builds the output as a list of string chunks (`[String]`) and joins once at the end via `string_concat`. No file IO in the middle; stdout is written once.

## Features added on top of kaikai-minimal

Stage 1 must *compile* more than stage 0 does. Added features, in intended implementation order:

1. **Placeholder lambda shorthand** (`. < 5`, `.name`). Desugars to `(x) => x < 5`, `(x) => x.name` at parse time.
2. **Record update syntax** `{ p with x: 10 }`. Desugars to a full-field literal that copies the rest.
3. **Full pattern matching**: nested patterns, or-patterns, guard clauses with complex expressions. Stage 0's match is already good; stage 1 extends with compile-time exhaustiveness checking.
4. **Effects with handlers** (no inference): `effect E { op1(args) : T }`, `handle { body } with E { op1(args, resume) -> expr }`, `E.op1(arg)` (method-call form, not `perform`). Sintax follows `docs/effects.md` (Doc A); the catalog of stdlib effects supported in stage 1 is the minimum that lets stage 2 self-compile (typically `Console`, `File`, `Fail`, plus `Mutable` for the inferencer). Compiled to CPS transforms — ordinary functions plus a continuation argument. This is a big chunk and gets its own milestones; the full design is `docs/effects-impl.md` (Doc C).
5. **Monomorphized generics**: stage 0 uses uniform boxing and skips specialization. Stage 1 introduces type-directed monomorphization for the emitted C: each generic instantiation yields a specialized copy of the function.
6. **Basic Perceus** *(deferred to stage 2 m5)*: originally planned for stage 1 — insert `kai_incref`/`kai_decref` using linearity analysis, "last use" gets no incref, "drop" inserted at scope exit. **Status as of 2026-04**: only a handful of manual `kai_incref`/`kai_decref` calls are emitted at hot paths (the spread fast-path); no linearity analysis or systematic drop insertion. Decision: do not invest in stage 1 Perceus; the analysis pass lands as **stage 2 m5** alongside the typed IR. Stage 1 keeps relying on stage 0's uniform boxing and manual ops where needed. See `docs/stage2-design.md` §Milestones m5 and the §2 *Full Perceus memory management* description.
7. **`|` map pipe**: `xs | f` desugars to `map(xs, f)`. Together with `|>`, completes the Elixir-style pipeline vocabulary.
8. **`kai test` runner subcommand**: `./kaic1 test path/` discovers `.kai` files, compiles each with `--test`, runs the binaries, aggregates results.
9. **Minimal imports**: `import foo.bar` resolves to `<root>/foo/bar.kai`, compiled once per invocation, names resolved across files. Keeps stage 1 writable in one file while paving the road for stage 2.

Each feature is its own milestone, landing on its own commit.

## Error reporting

Same format as stage 0:

```
path/to/file.kai:LINE:COL: error: <message>
```

No span underlining in stage 1 — it requires carrying the source text around, which we prefer not to.

## Testing strategy

Two layers, both run from the repo root:

1. **kaikai-level tests** inside `stage1/compiler.kai`: `test "..."` blocks asserting invariants about the lexer/parser/check — compiled under stage 0 with `--test` and run.

2. **End-to-end bootstrap tests**: a Makefile target

   ```sh
   ./kaic0 stage1/compiler.kai > build/stage1.c
   cc build/stage1.c -I stage0 -o build/kaic1
   for f in examples/minimal/*.kai; do
     ./build/kaic1 $f > build/$(basename $f .kai).c
     cc build/$(basename $f .kai).c -I stage0 -o build/$(basename $f .kai)
     ./build/$(basename $f .kai)
   done
   ```

   — stage 1 compiled by stage 0 must compile the same examples stage 0 does, and their outputs must match byte-for-byte.

3. **Determinism check (stage 1)**: `kaic1 stage1/compiler.kai` run twice on the same source must produce byte-identical C output. This catches non-determinism in stage 1 (hashmap iteration order, leaked pointer addresses, timestamps) without requiring stage 1 and stage 2 to agree on emission shape. See `docs/decisions/bootstrap-relax-byte-identical-2026-05-22.md` — the stage 1 ↔ stage 2 byte-identical fixed point that previously appeared here is intentionally dropped because stage 1 (no typer) and stage 2 (full typer + effects + Perceus) are structurally distinct compilers and cannot agree on type-driven emission decisions like match tag-compare or niche-filling.

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
11+ **Feature extensions** (each its own milestone): placeholder lambdas, record update, effects + handlers, monomorphized generics, basic Perceus, `|` map pipe, minimal imports, `kai test` subcommand.

## What stage 1 still cannot do (and does not need to)

- Effect inference.
- Full Perceus (reuse-in-place, drop specialization).
- Fibers, actors, concurrency.
- LLVM backend.
- `kai fmt`, `kai repl`, `kai lsp`.
- Package manager.
- Pretty error messages with source underlines.

All of these are stage 2 or post-MVP. The point of stage 1 is: **a compiler for full kaikai, written in kaikai, that bootstraps from C in one step**. Ceilings beyond that are deferred.
