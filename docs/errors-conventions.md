# Error handling conventions for kaikai

Status: documentation of conventions (2026-05-03). Not a spec change.

This document consolidates the rules — scattered today across
`docs/effects-stdlib.md` per effect — for **how to express failure**
in kaikai code. Five mechanisms coexist; this doc explains when each
applies and how they compose.

## The five mechanisms

| Mechanism | When to use | Inspectable? | Recovers? |
|---|---|---|---|
| `Option[T]` | "no value" is routine, no motive worth keeping | no | yes |
| `Result[E, T]` | failure has a motive callers branch on | yes (typed `E`) | yes |
| `Fail` (effect) | abort with message; caller cannot recover meaningfully | string only | no, unhandled = compile error |
| `panic` | programming error, contract violation, unreachable | string only | no, terminates the process |
| `!` postfix | propagate `Option[None]` or `Result[Err]` to the caller | — | yes (lifts to caller) |

(For `?` / `?name` see `docs/typed-holes.md` — typed holes are an
LLM / authoring affordance, not an error-handling mechanism. They
panic at runtime if reached unfilled.)

## Rule 1 — `Option[T]` vs `Result[E, T]`

> Use `Option[T]` when "absent" is the only thing the caller needs
> to know. Use `Result[E, T]` when the caller branches on the
> *motive*.

Examples in stdlib:

- `Stdin.read_line() : Option[String] / Fail` — `None` means EOF
  (routine), `Fail` means the terminal is broken (no message worth
  keeping).
- `File.read_file(path) : Result[String, String]` — every motive
  matters: "no such file" is a different recovery path from
  "permission denied".
- `Map.get(k) : Option[V]` — "key absent" is the only motive.

**Litmus test**: if you find yourself writing `match e { _ -> None }`
on the error case of a `Result`, you should have used `Option`.
Conversely, if you write `match opt { None -> Err("missing") }`,
you should have used `Result` from the start.

## Rule 2 — `Result[E, T]` vs `Fail`

> Use `Result` when the caller can recover. Use `Fail` when there
> is nothing reasonable for the caller to do.

`Fail.fail("...")` is a one-way exit. It propagates through every
caller until a handler catches it. The handler converts the failure
into something else (a default value, a logged event, a process
exit). Callers in between do **not** branch on the message.

`Result[E, T]` is for failures the caller wants to inspect and
*continue*: retry the operation, fall back to a default, surface
the error to a user, log structured detail to an audit trail.

**Litmus test**: would you ever write `if err == "specific message"
{ ... }`? If yes, you need `Result` with a structured `E`. `Fail`
with a string is too lossy.

## Rule 3 — Never use `panic` for recoverable errors

`panic` terminates the process. It is the right call for:

- Programming errors: invariant violations, indices out of bounds
  on arrays the code is supposed to keep in range, division by
  zero in expressions where zero is structurally impossible.
- Unreachable branches: `match` arms that should never fire given
  the type structure; the runtime `__protocol_dispatch_*` panic is
  this kind.
- Bootstrap failures: a stage-2 compiler that loses track of its
  invariants is no longer trustworthy.

`panic` is **not** for:

- Failed I/O (`File`, `Net`, `Stdin`) — use the `Result` / `Fail`
  shape declared by the effect.
- Bad user input — surface it through `Result[InputError, T]`.
- Validation failures — same as above.

**In fintech / regulated production code, `panic` should be
exceptional.** A reachable `panic` in production is a bug in the
contract design, not in the input. (The runtime panic from the
polymorphic-impl bug — issue #174 — is a counter-example that needs
fixing precisely because it surfaces under user-reachable code.)

## Rule 4 — `Result[E, T]` order: `E` first, `T` second

```kai
fn parse(s: String) : Result[ParseError, Int] = ...
#                            ^^^^^^^^^^  ^^^
#                            error type  value type
```

`Ok(value)` and `Err(motive)` are the constructors. The `E`-first
order is fixed in stage 2 and mirrors the source declaration in
`stdlib/core/result.kai`.

> **Common mistake**: writing `Result[Int, String]` when you meant
> "a parse that returns `Int` or fails with `String`". The typer
> will not catch this — your function will compile and `Err(42)` /
> `Ok("hello")` will be the constructors. Read the type aloud:
> `Result[E, T]` is "a result that errors with `E` or carries `T`".

## Rule 5 — Composing errors across subsystems

Multiple subsystems each have their own error type
(`ValidationError`, `ApprovalError`, `ExecutionError`). A function
that orchestrates them needs **one error type** for its return
value. Two patterns:

**Three patterns**, listed in order of preference for new code:

- **Pattern C — unions** (canonical for new code; landed in
  issue #187, 2026-05-03).
- **Pattern A — wrapper sum type** (legacy, still valid).
- **Pattern B — `!` with auto-`From`** (deferred to post-#180).

### Pattern A — wrapper sum type (legacy)

```kai
type AppError =
  | Validation(ValidationError)
  | Approval(ApprovalError)
  | Execution(ExecutionError)

fn process_payment(req: PayReq) : Result[AppError, Receipt] = {
  let v = validate(req).map_err(Validation)!
  let a = approve(v).map_err(Approval)!
  let e = execute(a).map_err(Execution)!
  Ok(receipt(e))
}
```

This was the standard kaikai pattern before issue #187. Three
pieces:

1. A wrapper sum type (`AppError`) with one variant per subsystem.
2. `expr.map_err(Wrapper)!` at every cross-subsystem boundary.
3. Pattern matching on `AppError` at the outermost handler.

The wrapper is **explicit** — adding a new subsystem requires
extending `AppError`, which the typer enforces exhaustively at
every match. Pattern A is still appropriate when the wrapper
variant carries **additional structure** beyond the inner error
(a request id, a retry count, a context tag). For pure
"forward the inner error verbatim" cases, Pattern C (unions) is
strictly better.

`map_err` is part of the proposed `Result` API expansion (issue
#182). Until it lands, the workaround is a manual `match`:

```kai
let v = match validate(req) {
  Err(e) -> Err(Validation(e))
  Ok(x)  -> Ok(x)
}!
```

### Pattern B — `!` with auto-`From` (post-#180)

When `protocol P[A]` lands (issue #180) and `!` is extended to
invoke `From::from(err)` on type mismatch, the wrapper boilerplate
collapses:

```kai
impl From[ValidationError] for AppError {
  fn from(e: ValidationError) : AppError = Validation(e)
}
# ... analogous for ApprovalError, ExecutionError

fn process_payment(req: PayReq) : Result[AppError, Receipt] = {
  let v = validate(req)!     # ! invokes From::from automatically
  let a = approve(v)!
  let e = execute(a)!
  Ok(receipt(e))
}
```

This is the Rust idiom (`?` + `From` trait). Tracked as a follow-up
to #180. **Not** available today; Pattern C (below) covers most of
the same ground without `From` impls.

### Pattern C — unions (canonical, since 2026-05-03)

Issue #187 unified algebraic types so that `|` always means
"union of types". A subsystem error type is composed by listing
its constituents:

```kai
type ValidationError = InvalidAmount | SourceFrozen | DestinationFrozen
type ApprovalError   = OverDailyLimit | KycRequired
type ExecutionError  = SettlementBackendDown | DoubleSpendDetected

type AppError = ValidationError | ApprovalError | ExecutionError

fn process_payment(req: PayReq) : Result[AppError, Receipt] = {
  let v = validate(req)!       # Result[ValidationError, _] — D3 upcast
  let a = approve(v)!          # Result[ApprovalError, _]   — D3 upcast
  let e = execute(a)!           # Result[ExecutionError, _]  — D3 upcast
  Ok(receipt(e))
}
```

- No wrapper variants per subsystem.
- No `map_err(Wrapper)` at every boundary — the **implicit upcast**
  (D3 in `docs/unions-design.md`) accepts a value of a component
  type wherever the union is expected.
- `!` propagates through the upcast: a `Result[ValidationError, _]`
  flows into a function returning `Result[AppError, _]` directly.
- Pattern matching uses `bind : ComponentType` to delegate per
  subsystem:

```kai
match err : AppError {
  ve : ValidationError -> render_validation(ve)
  ae : ApprovalError   -> render_approval(ae)
  ee : ExecutionError  -> render_execution(ee)
}
```

Adding a new subsystem requires extending `AppError`'s union and
adding one match arm. The compiler enforces exhaustiveness at the
arm level and types each component-typed handler as if it lived
in its own bounded context.

See `docs/unions.md` for the full reference and
`examples/unions/ddd_ledger_demo.kai` for an end-to-end demo
across three bounded contexts.

#### When to use Pattern A vs Pattern C

| Situation | Use |
|---|---|
| Forward the inner error verbatim across a bounded-context boundary | **Pattern C** |
| Wrap the inner error with extra structure (request id, retry count, layer tag) | Pattern A |
| Errors from disjoint subsystems compose in one return type | **Pattern C** |
| The same component error participates in many app-level unions | **Pattern C** (no name mangling needed) |
| You need a single-tag runtime layout (FFI, serialisation pinned to a tag enum) | Pattern A |
| The wrapper is purely for type identity with no extra payload | **Pattern C** wins (less boilerplate) |
| Generic over the error type (`F[T] = Option[T] | Result[String, T]`) | Neither yet — generic unions deferred |

Default to Pattern C for new code. Migrate Pattern A code only
when the wrapper carries no extra structure and the migration is
in the natural path of other work (do not chase a clean-up).

### What kaikai deliberately does NOT have

- **Structural unions** of the TypeScript flavour (`{x: int}` is
  *automatically* a subtype of `{x: int, y: string}` because of
  field shape). kaikai's unions are **nominal**: `T <: U` only
  when `U`'s declaration explicitly lists `T` as a component.
- **Anonymous error types** (OCaml polymorphic variants:
  `` [`ErrA | `ErrB ]``). Unions are introduced by `type`
  declarations only; there is no row-typed inference.
- **Chained upcast** (`T <: U <: V` does not imply `T <: V`).
  The user names the intermediate union explicitly.
- **Implicit downcast** (union → component). The narrowing is
  always explicit via the `bind : ComponentType` pattern.

## Rule 6 — Validation and accumulating errors

`Result[E, T]` short-circuits at the first `Err`. For validations
that should report **all** failures (every rule that fired) — the
typical fintech use case for transfer / order / portfolio
validation — kaikai today requires manual accumulation:

```kai
fn validate_transfer(req: TransferRequest) : Result[[TransferError], ValidatedTransfer] = {
  let errors : [TransferError] = []
  let errors = if req.amount <= 0<USD>     { errors ++ [InvalidAmount] }     else { errors }
  let errors = if !is_account_active(req.from)  { errors ++ [SourceFrozen] }      else { errors }
  let errors = if !is_account_active(req.to)    { errors ++ [DestinationFrozen] }else { errors }
  let errors = if req.amount > daily_limit(req.from) { errors ++ [OverDailyLimit] } else { errors }
  if errors == [] {
    Ok(ValidatedTransfer { ... })
  } else {
    Err(errors)
  }
}
```

This pattern works but is verbose. A `Validation[E, T]`-style type
that accumulates over `<*>`-like composition (the Scala cats /
Haskell `Validation` model) is **not** in stdlib v1 and is **not**
proposed. If a real fintech need surfaces, it can be designed as a
follow-up.

The current guidance: when validation rules are independent (the
typical case), build a `[E]` list manually and return
`Result[[E], T]`. When they cascade (later rules depend on earlier
results), use `Result[E, T]` and `!` so the first failure
short-circuits cleanly.

## Rule 7 — `Fail` is for the unhandleable

`Fail` is a stdlib effect declared as:

```kai
effect Fail {
  fail(msg: String) : !          # never returns
}
```

Calling `Fail.fail("...")` does not return; control jumps to the
nearest installed `handle ... with Fail { fail(msg, _) -> ... }`.
There is no resume. The signature `: !` (the bottom type) reflects
this.

**`Fail` carries a `String` only.** This is intentional. If your
error needs to be inspected, structured, branched on — it is not a
`Fail`, it is a `Result[E, T]` with `E` a sum type.

The stdlib uses `Fail` in two places where the message is for human
diagnosis, not for programmatic recovery:

- `Stdin.read_line` escalates real I/O faults via `Fail.fail`.
- `Console` writes that fail at `write(2)` panic via `Fail.fail`.

User code should follow the same rule: only call `Fail.fail` when
there is genuinely no useful recovery path.

A typed-error variant of `Fail` (`Fail[E]` parameterised by a sum
type) has been considered and is **not** proposed in v1. If a
real fintech case demands it, the analysis lives in
`docs/protocols.md` §*Multi-method dispatch — analysis* (the
`protocol P[A]` mechanism that would underpin it) and a fresh
proposal would be needed.

## Quick decision tree

```
Is the failure value worth keeping for the caller?
├── No, "absent" is enough → Option[T]
└── Yes, motive matters
    ├── Caller can recover meaningfully → Result[E, T]
    │   └── Multiple subsystems' errors compose? → union (Pattern C, since #187)
    │       (Pattern A — wrapper sum + map_err — when the wrapper
    │        carries extra structure; Pattern B — auto-From — once
    │        #180 lands and only if a per-conversion impl is wanted.)
    └── Caller cannot recover (broken terminal, OOM, ...) → Fail.fail(msg)

Does it indicate a programming bug or invariant violation?
└── Yes → panic (never in fintech production paths)
```

## Cross-references

- `docs/effects-stdlib.md` §`Fail` — effect declaration, default
  handler.
- `docs/effects-stdlib.md` §`Stdin` *Error model* — first
  documented `Option + Fail` rule.
- `docs/effects-stdlib.md` §`File` *Error model* — first
  documented `Result` rule.
- `docs/typed-holes.md` — `?` and `?name` are typed holes, not
  error handling.
- `docs/syntax-sugars.md` — `!` postfix.
- `docs/unions.md` — user-facing reference for Pattern C.
- `docs/unions-design.md` — design rationale and decision log.
- Issue #187 — milestone issue that introduced Pattern C.
- Issue #182 — `Result` / `Option` stdlib API expansion (`map_err`,
  `or_else`, `transpose`, `collect`, ...).
- Issue #180 — `protocol P[A]` (enables auto-`From` on `!` as a
  follow-up).
- Issue #174 — polymorphic-impl runtime panic. The example given in
  the document of "where `panic` is *not* acceptable" applies here.
