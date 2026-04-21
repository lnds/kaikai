# stage 0 design

Architectural decisions for the C bootstrap compiler of kaikai-minimal.

Stage 0 is **throwaway**: it will be replaced by stage 1 (written in kaikai-minimal) as soon as it can compile stage 1. Therefore **simplicity beats performance** throughout. It must be correct and easy to read, not fast.

## Non-goals

- **Speed**: stage 0 is not expected to be fast. Its output is not expected to be fast either.
- **Optimization**: no constant folding, no inlining, no dead code elimination. The emitted C goes through `cc -O2` which handles most of that anyway.
- **Nice error messages**: basic (`file:line:col: message`) is enough. Polish comes in stage 2.
- **Portability to Windows**: POSIX only. Linux and macOS are the MVP targets.
- **Unicode beyond lexing**: source is UTF-8; strings are UTF-8 byte sequences. No normalization, no grapheme support, no locale.

## Constraints

- **ANSI C99**, no C11. Builds on any recent `cc` (gcc, clang, tcc) without flags.
- **Zero external dependencies** beyond libc. No `readline`, no `libunistring`, no `bison`, no `flex`.
- **Hand-written** lexer and parser (no generators). The grammar is small enough.
- Target size: **5–10K lines of C**, all within `stage0/`.

## Compilation pipeline

```
.kai source
  → lexer          (lexer.c)     stream of tokens
  → parser         (parser.c)    AST
  → type checker   (check.c)     typed AST (types resolved in-place)
  → emitter        (emit.c)      C source to stdout or a file
```

Four passes, each fails fast on the first error it cannot recover from (error recovery is post-MVP).

The generated C is then compiled by the host `cc`, together with a small runtime header, into a native binary.

```
stage0 input.kai > out.c
cc out.c -o out
./out
```

## Project layout

```
stage0/
  Makefile          # single target: kaic0
  main.c            # CLI entry point and pipeline driver
  util.h util.c     # arena allocator, string buffer, error reporting
  lexer.h lexer.c   # UTF-8 lexer with newline sensitivity rules
  parser.h parser.c # recursive-descent parser producing AST
  ast.h             # AST node structs (struct-heavy, no OO)
  check.h check.c   # type checker, resolves names and types
  types.h           # type representation (used by parser + check + emit)
  emit.h emit.c     # C emitter
  runtime.h         # runtime header included by all emitted C
  tests/            # C-level unit tests (lexer, parser, check)
    run_tests.sh    # also compiles and runs examples/minimal/*.kai
```

Build: `make` produces `./kaic0`. `make test` runs unit tests and end-to-end examples.

## Value representation (the runtime)

stage 0 uses **uniform boxing**: every kaikai value at runtime is a pointer to a `KaiValue`, which has an RC header, a type tag, and a tag-dependent payload. No unboxing, no type-specific specialization.

```c
typedef struct KaiValue KaiValue;

typedef enum {
  KAI_UNIT, KAI_BOOL, KAI_INT, KAI_REAL, KAI_CHAR,
  KAI_STR,
  KAI_CONS, KAI_NIL,     /* list = nil | cons(head, tail) */
  KAI_RECORD,
  KAI_VARIANT,           /* sum type inhabitant */
  KAI_CLOSURE
} KaiTag;

struct KaiValue {
  int32_t rc;
  int32_t tag;           /* KaiTag */
  union {
    /* KAI_UNIT: no payload */
    int                  b;   /* KAI_BOOL: 0 or 1 */
    int64_t              i;   /* KAI_INT */
    double               r;   /* KAI_REAL */
    uint32_t             c;   /* KAI_CHAR, Unicode codepoint */
    struct { size_t len; char *bytes; } s;       /* KAI_STR */
    struct { KaiValue *head; KaiValue *tail; } cons;  /* KAI_CONS */
    /* KAI_NIL: no payload */
    struct { int n_fields; KaiValue **fields; const char **names; } rec;  /* KAI_RECORD */
    struct { int variant; int n_args; KaiValue **args; } var; /* KAI_VARIANT */
    struct {
      KaiValue *(*fn)(KaiValue *self, KaiValue **args, int n_args);
      int n_captures;
      KaiValue **captures;
      int arity;
    } clo;                                       /* KAI_CLOSURE */
  } as;
};
```

Why uniform boxing:

- Every function signature in the emitted C becomes `KaiValue*(...)` — no specialization per type.
- Generics (`map`, `filter`, `reduce`, `each`, `Option[T]`, `Result[E, T]`) just work because `KaiValue*` is opaque.
- RC is trivial: the header is in the same place for every value.
- We can literally write the runtime in a single header with <500 lines of C.

Cost: Int, Real, Bool, Char are heap-allocated. Allocations everywhere. Slow. Acceptable for a bootstrap.

**Trajectory**: this representation is strictly for stage 0. Stage 1 (written in kaikai-minimal, compiled by stage 0) already unboxes `Int`, `Real`, `Bool`, and `Char` into native machine values; it keeps boxing only for heap-allocated compound types. Stage 2 (LLVM backend) applies full Perceus optimization: reuse analysis, drop specialization, and in-place update. The uniform boxing chosen here is a deliberate downgrade for bootstrap simplicity, not a design ceiling.

## Reference counting

Strict discipline, no cycles possible (all values are immutable, no back-references).

```c
static inline KaiValue *kai_incref(KaiValue *v) { if (v) v->rc++; return v; }
void                     kai_decref(KaiValue *v);  /* may recurse on payload */
```

The emitter inserts `kai_incref` / `kai_decref` calls around:

- **Let bindings**: `incref` on the RHS when stored in the new variable's slot; `decref` at the end of its scope.
- **Function calls**: caller is responsible for `decref`-ing arguments it passed in and the return value if unused. (Rust-style "move" — the callee takes ownership.)
- **Pattern matching**: each matched sub-pattern incs the references it keeps, and the original scrutinee is decref'd at the end of the match.
- **Constructor calls** (record, variant, cons, closure): arguments are moved into the new object.

Rules are conservative: when in doubt, incref. Over-counting is slow, not wrong.

Cycles cannot occur because kaikai-minimal has no mutation and no back-references in data structures. `kai_decref` recursively frees subtrees without worrying about cycles.

## Generated C contract

The output is a single `.c` file.

- Top of file: `#include "runtime.h"` (runtime is installed alongside stage 0, not bundled into the output).
- Each kaikai function `foo(x: Int, y: Int) : Int` becomes:
  ```c
  KaiValue *kai_foo(KaiValue *x, KaiValue *y);
  ```
- Public (`pub`) functions keep their name; private functions are prefixed with `kai_priv_<module>_<name>` to avoid collisions when multiple modules compile together (stage 0 only handles one file, but the convention is kept).
- `fn main()` produces `int main()` in the output, which initializes the runtime, calls `kai_main`, decrefs the result, and exits.
- Tests become no-ops in the normal build. Stage 0 has a `--test` flag that includes them as extra functions called from `main`.

Each kaikai construct compiles mechanically:

| kaikai construct | Emitted C |
|---|---|
| `let x = expr` | `KaiValue *x = (expr);` |
| `if c { a } else { b }` | ternary if simple; `do {} while(0)` block with early-assigns if complex |
| `match` | chain of `if (v->tag == ...)` — decision tree is linear in stage 0 |
| `f(a, b)` | `kai_f(a, b)` with incref/decref around args |
| lambda | anonymous struct allocation with function pointer and captures |
| `[a, b, c]` | desugar to `kai_cons(a, kai_cons(b, kai_cons(c, kai_nil())))` |
| `[1..10]` | emit a loop that builds the cons chain |
| string interpolation | `kai_str_concat` chain with `kai_to_string` on interpolated values |

## Type checker

Simple because kaikai-minimal has no inference.

- **Symbol table**: nested scopes, lookups by name. `<name → Type>` per scope.
- **Per AST node**: walk top-down, emit types bottom-up. For `let x = expr` without annotation, infer the RHS type and bind `x` to it. For annotated positions, verify the expression matches.
- **Output**: the AST is mutated in place with a `Type*` on every node. The emitter reads these types.
- **Generics**: `map : ([a], (a)->b) -> [b]` is resolved by instantiating type variables from arguments at each call site. Since emission is uniform boxing, we don't generate specialized code — the type check is only for correctness.

Errors reported: type mismatch, unknown identifier, non-exhaustive match, wrong arity, unknown field.

No fancy things in stage 0: no row polymorphism, no sub-typing, no implicit coercions. Mathematical operations require matching numeric types (`Int + Int`, `Real + Real` — no `Int + Real`).

## Error reporting

Every token carries `(file, line, col, length)`. AST nodes carry a source span (first and last token positions).

Format:

```
examples/minimal/fizzbuzz.kai:4:14: error: expected String, got Int
  else if n % 3 == 0 { "Fizz" }
              ^^^^^^
  this expression has type Int
  but the `if` arm above produced type String
```

Basic but usable. No "did you mean X?", no error recovery — the compiler stops at the first error.

## Built-in prelude

Provided by `runtime.h` and declared at the top of the emitter output.

- **Primitives**: `print`, `eprint`, `read_file`, `write_file`, `read_line`, `exit`, `panic` — thin wrappers around libc with RC handling.
- **Conversions**: `int_to_string`, `real_to_string`, `string_to_int`, `string_to_real` — implemented with `snprintf` and `strtol`/`strtod`.
- **String ops**: `string_length`, `string_concat`, `string_split`, `string_contains`, `char_at`.
- **List ops**: `list_length`, `list_append`, `list_reverse`.
- **Higher-order**: `map`, `filter`, `reduce`, `each` — implemented in C using cons/nil + closure dispatch.

Everything lives in a single `runtime.h`, ~500 lines, included by the emitted C and compiled together.

## Testing strategy

Two levels:

1. **C-level unit tests** in `stage0/tests/`:
   - `lexer_test.c`: feed strings, assert token sequences.
   - `parser_test.c`: feed strings, assert AST shapes (printed form).
   - `check_test.c`: feed snippets, assert type results and type errors.
   - `runtime_test.c`: exercise runtime primitives directly.

2. **End-to-end** via a shell script:
   - Compile each file in `examples/minimal/` with stage 0.
   - Compile the resulting C with `cc`.
   - Run the binary, capture stdout.
   - Compare against a `.expected` file next to each example.
   - `stage0 --test file.kai` should also run any `test` blocks in the file and report pass/fail.

`make test` from the repo root runs both.

## Milestones within Fase 2

Implemented in this order, each validated before moving on:

1. **Skeleton**: `main.c` reads a file, `lexer.c` stub, empty Makefile that produces `./kaic0`.
2. **Lexer**: full tokenization of kaikai-minimal. Unit tests.
3. **AST + parser**: parse the whole grammar into AST nodes. Unit tests on structural shape.
4. **Type checker**: resolve names, check types, annotate the AST. Unit tests.
5. **Runtime**: `runtime.h` with value rep, RC, and prelude primitives. Standalone C tests.
6. **Emitter: hello world**: enough of the emitter to compile `hello.kai` to runnable C. First end-to-end test.
7. **Emitter: expressions**: `if`, arithmetic, `let`, function calls. `fizzbuzz.kai` compiles and runs.
8. **Emitter: pattern matching + lists + closures**: `quicksort.kai` and `interp.kai` compile and run.
9. **Test runner**: `--test` flag, `test`/`assert` blocks emit test functions.
10. **Close Fase 2**: all examples build and run; tests pass.

Each milestone commits cleanly.

## What stage 0 cannot do (and does not need to)

These are deliberately omitted — they are the job of stage 1 or later:

- Pattern match compilation into efficient decision trees (we use linear if-else chains).
- Tail-call optimization (we rely on `cc -O2` to do TCO for simple cases; deep recursion can blow the stack).
- Specialization of generics (uniform boxing is enough).
- Proper effect tracking (only a conventional `Io` soup).
- LLVM backend.
- Package manager.
- Incremental compilation.

The explicit list makes it easier to push back when scope creep tempts.
