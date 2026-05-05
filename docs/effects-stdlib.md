# stdlib effects

Doc A (`docs/effects.md`) pinned *how* effects work in kaikai:
rows, unification, syntax, `handle`/`resume`, inference. This
document (Doc B) pins *which* effects the stdlib ships, what their
operations look like, which helpers live alongside them, how the
current non-effect builtins migrate behind those capabilities, and
what the runtime installs around `main`. Doc C
(`docs/effects-impl.md`) will cover the CPS transform and the
handler-stack runtime.

Scope of v1: a fixed catalog of effects, with handlers either
provided natively by the runtime or expressible in kaikai-minimal.
No exception hierarchies, no user-overridable default handlers.
Closed effect aliases (Doc A §*Open questions* #5) are permitted
and used by the stdlib sparingly.

## Catalog

| Effect          | Purpose                                       | Default handler?      |
|-----------------|-----------------------------------------------|-----------------------|
| `Console`       | stdout and stderr output                      | yes (runtime, if in `main`'s row) |
| `Stdin`         | terminal input                                | yes (runtime, if in `main`'s row) |
| `Env`           | command-line args and process environment     | yes (runtime, if in `main`'s row) |
| `File`          | filesystem read/write                         | yes (runtime, if in `main`'s row) |
| `Clock`         | wall-clock and monotonic time, sleep          | yes (runtime, if in `main`'s row) |
| `Random`        | non-cryptographic randomness                  | yes (runtime, if in `main`'s row) |
| `SecureRandom`  | cryptographically-secure randomness           | yes (runtime, if in `main`'s row) |
| `NetTcp`        | TCP byte-level networking                     | yes (runtime, if in `main`'s row) |
| `NetUdp`        | UDP byte-level networking                     | yes (runtime, if in `main`'s row) |
| `NetDns`        | DNS resolution                                | yes (runtime, if in `main`'s row) |
| `Process`       | OS-level process spawn, wait, exit            | yes (runtime, if in `main`'s row) |
| `Signal`        | trap POSIX signals on the running process     | yes (runtime, if in `main`'s row) — Posix only in v1 |
| `Fail`          | abort with a message                          | no (unhandled = compile error) |
| `State[T]`      | value-threaded mutable state                  | no (user supplies)    |
| `Reader[T]`     | read-only ambient value                       | no (user supplies)    |
| `Writer[W]`     | accumulating log / output                     | no (user supplies)    |
| `Mutable`       | heap cells and arrays (the `Array` escape)    | yes (runtime-trivial, invisible in user code) |
| `Cancel`        | cooperative cancellation                      | yes (scheduler-wired) |
| `Spawn`         | fibers and nurseries                          | yes (root nursery) — cross-ref `docs/structured-concurrency.md` |
| `Ffi`           | crossings to C via `extern "C"` declarations  | yes (compiler-synthesised) |

Mailboxes and message passing (`Actor[Msg]`) are deferred to a
separate spec, `docs/actors.md`, because the mailbox model
(bounded vs unbounded, supervision, links/monitors) is its own
design surface; it does not fit inside a single table row. Doc B
stays silent on actors beyond this note.

The four stdlib-granularised effects (`Console`, `Stdin`, `Env`,
`File`) are grouped by the stdlib under a single closed alias
for convenience:

```kai
type Io = Console + Stdin + Env + File
```

Functions that only log use `/ Console`. Functions that read
user input add `/ Stdin`. Functions that read configuration get
`/ Env + File`. The `Io` alias is the umbrella when a function
does "several of the above" and the precise breakdown is not
worth spelling out. Diagnostics keep the alias when it resolves
cleanly and expand to the label set only on mismatch, per Doc A
§*Open questions* #5.

`Clock`, `Random`, `SecureRandom`, the `Net` family
(`NetTcp + NetUdp + NetDns`, with `Net` as alias), and `Process`
are kept *out* of the `Io` alias. Networking and process control
have distinct operational costs (latency, peer failure,
fork/exec weight) and a function that "logs and reads config"
should not silently gain capability over the network or over
subprocesses just because both sit under a convenience name.
Each of those effects appears explicitly in signatures.

"Default handler" means: if `main`'s row contains this effect and
no user handler appears, the runtime installs a stdlib-provided
handler. Every other effect must be closed by the user before
`main` returns, or the compiler refuses the build.

Sections are self-contained; skim the catalog, then dive where
you care.

## Syntax note: trailing lambdas

Many of the stdlib helpers in this document take a lambda as
their last argument (`try`, `with_default`, `with_state`,
`map_fail`, `nursery`, and the like). Doc B writes them in one
of two equivalent forms:

```kai
# Paren form — always legal.
let r = try(() => {
  let s = File.read_file_or_fail("config.toml")
  parse(s)
})

# Trailing-lambda form — sugar: the final `() -> T` argument may
# be written as a block outside the parens.
let r = try {
  let s = File.read_file_or_fail("config.toml")
  parse(s)
}
```

Trailing lambdas are a separate construct from `handle ... with
... { ... }`. Doc A §*Handling* pins `handle` and `with` as
reserved keywords forming a dedicated control-flow production —
not a function call with a trailing block. Trailing lambdas in
this section apply only to ordinary stdlib helpers (`try`,
`with_state`, `nursery`, etc.); the grammar does not special-case
those helpers, only `handle`/`with`.

Rule of thumb for writers of Doc B examples: prefer the
trailing form when the lambda is the whole intent of the call,
and the paren form when the lambda is one argument among peers.

Trailing-lambda syntax lands in m7b (see §*Next steps*). Until
then, every example in this doc is equally valid rewritten in
paren form.

## Syntax note: capability read/write sugar

Two small sugars make `State` and `Reader` capabilities read as
concisely as local variables without hiding that they are
capability operations.

```kai
# Read with `@cap`:  @counter   ≡   counter.get()    (State)
#                    @config    ≡   config.ask()     (Reader)
#
# Write with `cap := v`:  counter := v   ≡   counter.set(v)
```

Both are strictly limited — each sugar only applies to one
specific op shape on one specific effect:

1. **`@` applies only to `State[T].get()` and `Reader[T].ask()`.**
   Other zero-argument operations (like a hypothetical
   `Clock.now()`) still need `Clock.now()` spelled out. Narrow
   scope keeps the `@` visually meaningful — "read the current
   value of a capability".
2. **`:=` applies only to `State[T].set(v)`.** Any other
   `set`-shaped op on some other effect keeps `.set(v)`.
3. **The identifier after `@` must be a simple capability
   binding** (`@counter`, not `@config.section.level`).
4. **`@` and `:=` require an `as`-bound capability name.**
   `@State` and `State := v` are not legal; only bindings like
   `handle { ... } with State[Int](0) as counter { ... }` can
   use the sugar. Using the default capability name (`State`
   itself) forces the explicit `State.get()` / `State.set(v)`.

The `@` glyph was picked for two reasons: it is not used
anywhere else in kaikai, and it is short enough that
`@counter + 1` reads cleanly while still clearly being different
from a plain identifier — the operation is visible, consistent
with Doc A §*Context*'s capability-passing-explicit stance.

Both sugars ship in **m7b** (see §*Next steps*), alongside
trailing lambdas. Until then, `counter.get()` and
`counter.set(v)` are the only ways to spell the ops.

## Syntax note: local mutable cells with `var`

The commonest use of `State[T]` is not a long-lived threaded
state but a local mutable variable — a counter in a loop, an
accumulator in a builder function. Writing a full
`handle { ... } with State[T](init) as name { clauses }`
block for something Koka spells as `var x := 0` is friction Doc
B would rather not ship.

The `var` keyword declares a local mutable cell:

```kai
fn sum_costs(events: [Event]) : Int / Console {
  var total = 0
  each(events, (e) => {
    total := @total + e.cost
    Console.print("running: #{@total}")
  })
  @total
}
```

`var total = 0` desugars to a `handle { rest-of-block } with
State[Int](0) as total { ... }` wrapping everything from the
`var` down to the closing `}` of the enclosing block. The
generated handler uses the canonical clauses — there is no other
useful choice for a local cell:

```kai
get(resume)    -> resume(state)
set(v, resume) -> resume((), v)
return(x)      -> x
```

Consequences:

- **The cell does not escape the block's effect row.** Because
  the implicit `handle` closes `State[T]` in the same scope, the
  enclosing function's signature is unchanged. `sum_costs`
  above is `/ Console`, not `/ Console + State[Int]`.
- **Scope is the innermost block.** The cell is live from its
  `var` line to the next `}`, matching `let`.
- **Nested `var`s nest handlers.** Each `var` is its own
  `handle`, innermost first. One-shot resume stays a tail call,
  so there is no per-update runtime cost beyond the
  state-threading the handler was already doing.
- **Explicit annotation is optional.** `var total = 0` infers
  `Int`; `var total: Int = 0` is the annotated form.
- **Sibling to `let`, not replacement.** `let` is immutable
  binding; `var` is a local `State[T]` cell. Both survive,
  each with its own intent.

A `var` declaration at function-top level is the usual case;
declaring one deeper (inside an `each` callback, say) works but
is rarely what you want — the cell dies at the inner block's
`}` and is not visible to siblings.

`var` ships in **m7b**.

### Performance

A naive reading of the desugar suggests every `var` update
allocates and threads a fresh handler invocation. It does not.
`var` is always local, always uses the canonical clauses, and
never uses multi-shot `resume`, so the compiler specialises it
down to a stack-allocated mutable slot (Koka's "variable
specialization"). End result: `var counter = 0; counter :=
@counter + 1` lowers to the same native code as a C mutable
`int`. Full details live in Doc C; for Doc B it is enough to
know the ergonomy is free.

## Syntax note: array indexing
<!-- coverage: skip --> sugar shipping in m7b; coverage by m7b sugar fixtures

`Mutable.array_get` and `Mutable.array_set` get their own
concise spelling, because `a[i]` is universal enough that
writing the op name out would stand out:

- `a[i]`       ≡ `Mutable.array_get(a, i)`  — read
- `a[i] := v`  ≡ `Mutable.array_set(a, i, v)` — write

Both require `a : Array[T]` and `Mutable` in the effect row;
the checker enforces this. The same sugar is **not** extended
to `Ref[T]` — reading and writing refs keeps the explicit
`Mutable.ref_get(r)` / `Mutable.ref_set(r, v)` form. Rationale:
`Array[T]` shows up in everyday user code, `Ref[T]` is a
low-level tool used mostly by the stdlib and FFI adapters where
explicit ops double as documentation.

Array indexing sugar ships in **m7b**.

## `Console`

### Declaration

```kai
effect Console {
  print(s: String)  : Unit
  eprint(s: String) : Unit
}
```

Two ops, both appending `\n` to the string before writing. Match
the minimal prelude's `print`/`eprint`. Neither carries a failure
type: under the default handler, the common recoverable fault
(pipe closed on the other side, `EPIPE`) is absorbed silently,
and any remaining fault is catastrophic enough to panic.

### Default handler

Runtime-installed around `main` when `Console` is in the row.
Each clause calls `Ffi` (`fputs(s, stdout)` / `fputs(s, stderr)`,
then `fputc('\n', …)`) and resumes with `()`.

Fault handling inside the clause:

- The runtime installs `SIG_IGN` on `SIGPIPE` at startup, so a
  closed downstream pipe returns `EPIPE` from the write instead
  of killing the process. The clause treats `EPIPE` as a no-op —
  nothing is written, the clause resumes, the program keeps
  running. This is what lets `kai run foo.kai | head` terminate
  cleanly.
- Any other `errno` (e.g. `EBADF`, `ENOSPC` on stdout to a file)
  is treated as a panic: the runtime writes a banner to the
  original stderr (via the raw fd it captured at startup) and
  calls `exit(1)`.

### Capture / interception

A test harness that wants to capture stdout writes its own
`handle { body() } with Console { print(s, resume) -> {
captured.push(s); resume(()) } ; eprint(s, resume) -> resume(())
}`. The default only installs when no user handler covers the
effect in scope.

## `Stdin`

### Declaration

```kai
effect Stdin {
  read_line() : Option[String] / Fail
}
```

Single op. `None` means EOF or closed pipe (routine, not an
error); a real I/O fault escalates through `Fail`. See §*Error
model* below for the rule.

### Error model

`Option[T] / Fail` is the pattern for ops where the caller wants
to stop on absence but never to inspect a fault message in
detail. `read_line` fits because "no more input" is structural
and every real fault reduces to "the terminal is broken, bail".
`File` uses `Result` instead for the opposite reason (callers
branch on the motive).

### Default handler

Runtime-installed around `main` when `Stdin` is in the row. The
clause calls `fgets`. Cases:

- Some bytes read ending in `\n`: strip the trailing `\n` and
  resume with `Some(line)`.
- Some bytes read with no trailing `\n` (last line of a file
  with no final newline, or a terminal peer that closed mid-
  line): resume with `Some(line)` as-is.
- Zero bytes and EOF: resume with `None`.
- `ferror`: invoke `Fail.fail("read_line: …")` and do not
  resume. This is the one exception to "every default-handler
  clause resumes" across the stdlib.

The caller cannot distinguish "line ended with `\n`" from "line
ended at EOF without `\n`" — matches Python's `input()`, Rust's
`BufRead::read_line`, Go's `bufio.Scanner.Text()`. If that
distinction matters, drop to raw byte IO via a future `Stdin`
extension.

## `Env`

### Declaration

```kai
effect Env {
  args()                : [String]
  var(name: String)     : Option[String]
}
```

- `args()` returns the command-line arguments passed to the
  process, excluding the program name.
- `var(name)` returns `Some(value)` if the environment variable
  `name` is set, `None` otherwise. Looking up an unset variable
  is a value, not a failure.

### Why `Env` and not pure

In kaikai a pure function is deterministic and depends only on
its arguments. Both `args()` and `var(name)` depend on the
surrounding process state — the invoking command line and the
environment block — not on their arguments alone, so they cannot
be pure. Filing them under `Env` preserves the invariant without
a special case in the inferencer.

### Default handler

Runtime-installed around `main` when `Env` is in the row. `args`
reads a pre-captured `argv` slice; `var` calls the libc `getenv`
via `Ffi` and lifts the `NULL` result to `None`. Both always
resume; no failure path.

### What's not in v1 (planned extensions)

Kept as a shortlist for future `Env` expansion — shape and
timing decided when the first real use case pushes them:

- Working directory: `cwd() : String`, `chdir(path: String) :
  Result[Unit, String]` (the latter likely behind
  `Mutable` rather than `Env`, since it changes process state).
- Process identity: `pid() : Int`, `hostname() : String`.
- Full environment block: `vars() : [(String, String)]` for
  iteration over every variable.
- Mutating the environment: `set_var(name, value)`,
  `unset_var(name)` — same `Mutable`/`Env` split question as
  `chdir`.

## `File`

### Declaration

```kai
effect File {
  read_file(path: String)                       : Result[String, String]
  write_file(path: String, content: String)     : Result[Unit, String]
}
```

Separate from `Console`, `Stdin`, and `Env` because its error
shape is different: filesystem faults carry inspectable motives
("no such file", "permission denied", "disk full") that callers
routinely branch on, whereas the other three either cannot fail
or distinguish only "nothing more" from "real fault". Keeping
`File` separate lets a signature like `/ Console + Env` mean
"touches terminal output and process args, does not open disk".

### Error model

Both ops return `Result[_, String]`. See §`Stdin` *Error model*
for the rule that picks `Result` vs `Option + Fail`: `File`
lands on the `Result` side because callers branch on the
motive. v1 keeps the payload as a raw `String` message; a
structured `FileError` sum is deferred.

### Stdlib helper

The common "abort if anything goes wrong" wrapper lifts `Result`
into `Fail`:

```kai
pub fn read_file_or_fail(path: String) : String / File + Fail {
  match File.read_file(path) {
    Ok(s)  -> s
    Err(m) -> Fail.fail("read #{path}: #{m}")
  }
}
```

### Default handler

Runtime-installed around `main` when `main`'s row contains
`File`. Each clause delegates to `Ffi` (`fopen`/`fread`/`fwrite`/
`fclose`) and resumes with an `Ok` or `Err` value depending on
the C call's return. No clause ever short-circuits — errors flow
as data, not as `Fail`.

Because the default is installed automatically, `main` does not
need to write `handle { ... } with File { ... }` itself.

### Overwrite semantics for `write_file`

`write_file(path, content)` truncates and rewrites `path` if it
exists, or creates it if it does not. Matches `fopen(path, "w")`
and matches Python/JS expectations. Appending to an existing
file and "create only if missing" are not in v1; stdlib can add
them as `append_file` and `write_file_new` when needed.

### What's not in v1 (planned extensions)

Kept as a shortlist for future `File` expansion — shape and
timing decided when the first real use case pushes them:

- Existence and metadata: `exists(path)`, `stat(path) :
  Result[Metadata, String]`.
- Append / create-only: `append_file`, `write_file_new`.
- Directories: `list_dir`, `create_dir`, `remove_dir`.
- Deletion and rename: `delete(path)`, `rename(from, to)`.
- Binary IO: `read_bytes`, `write_bytes` (today everything is
  UTF-8 `String`).
- Streaming: chunked read/write without loading the whole file.
  Likely needs a companion effect (a stateful file handle), not
  just new `File` ops.

## `Clock`

### Declaration

```kai
effect Clock {
  now()              : WallTime
  monotonic()        : Instant
  sleep(d: Duration) : Unit
}
```

- `now()` returns the current wall-clock time as a `WallTime`.
  Subject to wall-clock jumps (NTP corrections, manual clock
  changes); use for user-facing timestamps, not for measuring
  intervals.
- `monotonic()` returns a monotonic timestamp as an `Instant` —
  a clock that never goes backwards. Its origin is unspecified;
  only differences are meaningful. Use for measuring elapsed time
  and for deadlines.
- `sleep(d)` suspends the current fiber for at least `d`. The
  scheduler may resume the fiber later than `d` if other work is
  pending — `d` is a lower bound, not a deadline. If the fiber's
  row contains `Cancel`, a cancellation delivered while the fiber
  is asleep wakes it through the standard `Cancel.raise()` path.

`WallTime`, `Instant`, and `Duration` are stdlib types declared
by the `time` module (see `docs/stdlib-layout.md`). The two
timestamp types are deliberately distinct so the type system
prevents mixing wall-clock and monotonic readings in arithmetic
— a common source of bugs (Rust and Go take the same approach).
Subtraction of two `Instant` values yields a `Duration`;
`monotonic() - earlier` is the standard "how long has it been"
idiom. `WallTime` supports comparison and formatting but
intervals between two `WallTime` values are advisory only,
because the wall clock can jump.

### Why `Clock` and not pure

Reading the clock is non-deterministic and depends on process
state (which clock source is selected, how the OS is set). Same
argument as `Env`: a function whose result depends on surrounding
process state cannot be pure.

### Default handler

Runtime-installed around `main` when `Clock` is in the row.

- `now` calls `clock_gettime(CLOCK_REALTIME, ...)` via `Ffi`.
- `monotonic` calls `clock_gettime(CLOCK_MONOTONIC, ...)`.
- `sleep` registers the fiber as suspended with the scheduler's
  timer wheel, keyed on `monotonic() + d`. The scheduler resumes
  the fiber when the deadline elapses, or earlier if `Cancel` is
  delivered.

The handler never short-circuits; clock ops always resume.

### Error model

`clock_gettime` is infallible on supported platforms (Linux,
macOS, FreeBSD; the only documented `EINVAL` returns are for
clock IDs the runtime never passes). A failure here is treated
the same as `Console`: panic with a banner on stderr.

### What's not in v1 (planned extensions)

- Calendar arithmetic (years, months, time zones, DST). v1 keeps
  `Instant` and `Duration` opaque and timezone-agnostic; calendar
  types and conversions land in a future `time.calendar`
  extension.
- Named timers (cancellable via handle): `at(deadline: Instant) :
  Timer`, `cancel(t: Timer) : Unit`. The `sleep`-via-`Cancel`
  pattern covers most needs; named timers are for cases where
  one fiber arms a timer that another fiber cancels.

## `Random`

### Declaration

```kai
effect Random {
  int_range(lo: Int, hi: Int) : Int
  float()                     : Float
  bytes(n: Int)               : [Byte]
}
```

- `int_range(lo, hi)` returns a uniformly-distributed value in
  the half-open interval `[lo, hi)`. Calling with `lo >= hi` is
  a panic — there is no meaningful value to return; the
  inferencer does not catch this.
- `float()` returns a value in `[0.0, 1.0)` with 53 bits of
  precision.
- `bytes(n)` returns a list of `n` bytes drawn from the same
  stream.

`Random` is for non-cryptographic uses: simulations, sampling,
test fixtures. Cryptographic randomness lives in `SecureRandom`
(next section), declared as a separate effect for the safety
reason spelled out there.

### Why `Random` and not pure

Each call returns a different value depending on the PRNG state.
Same justification as `Env` and `Clock`: the result depends on
ambient process state (the PRNG's hidden cursor), not on
arguments alone.

### Default handler

Runtime-installed around `main` when `Random` is in the row. The
handler holds a per-process PCG64 instance, seeded from
`SecureRandom.bytes` at startup. Each op draws from that instance
and resumes; the handler never short-circuits.

The default handler is **not deterministic across runs**. Tests
that need reproducibility install their own `Random` handler with
a fixed seed, in the same shape as the `Console` capture pattern.

### Error model

PRNG ops are infallible. `int_range(l, h)` with `l >= h` is a
panic (programming error), not a `Fail`.

### What's not in v1 (planned extensions)

- Distributions: `gaussian(mean, stddev)`, `exponential(rate)`,
  `poisson(lambda)`. Compose on top of the four primitive ops
  and live in the `random` stdlib module rather than as new ops.
- Sampling helpers: `choice(xs)`, `shuffle(xs)`, `sample(xs, k)`.
  Same locality argument — pure given the underlying primitives.
- Reseeding: `seed(s: Int)`. Useful for fuzzing harnesses;
  deferred pending a use case that demands it.

## `SecureRandom`
<!-- coverage: skip --> effect declared, post-MVP; remove this marker when fixture lands

### Declaration

```kai
effect SecureRandom {
  bytes(n: Int) : [Byte]
  int()         : Int
}
```

A cryptographically-secure RNG. The op set is intentionally
minimal: bytes and ints are enough for crypto primitives that
live above this effect; specialised distributions or stream APIs
are out of scope.

### Why separate from `Random`

`SecureRandom` is kept deliberately separate from `Random` so
that a test handler stubbing `Random` cannot weaken
security-sensitive paths (token generation, key material,
nonces). A signature `/ SecureRandom + Random` is unusual but
legal: it declares both that the function uses non-secure
randomness *and* secure randomness, with no implicit conflation
of the two.

### Default handler

Runtime-installed around `main` when `SecureRandom` is in the row.

- Linux: `getrandom(2)` with `flags = 0` (block until the kernel
  pool has enough entropy on first call; never blocks afterward).
- macOS / *BSD: `arc4random_buf(3)`.
- Windows (post-MVP): `BCryptGenRandom`.

The handler never short-circuits unless the syscall actually
fails, which on these platforms only happens under catastrophic
conditions (kernel pool unrecoverable). v1 treats such failures
as panics, the same as `Console` and `Clock`.

### Error model

Infallible in v1 from the caller's perspective. The handler may
panic if the OS RNG is unavailable, but no `Result` / `Fail`
shape is exposed — code that needs cryptographic randomness
cannot run if the OS cannot provide it.

### What's not in v1 (planned extensions)

- `key(n: Int) : Key` returning a typed key wrapper. Belongs
  with the future `crypto.key` module, not with `SecureRandom`.
- A streaming API for very large draws. The current `bytes(n)`
  is fine through MVP-scale use cases.

## `NetTcp`, `NetUdp`, `NetDns` (alias `Net`)
<!-- coverage: skip --> effect declared, post-MVP; remove this marker when fixture lands

### Declaration

```kai
effect NetTcp {
  connect(host: String, port: Int)              : Result[Conn, String]
  listen(host: String, port: Int)               : Result[Listener, String]
  accept(l: Listener)                           : Result[Conn, String]
  send(c: Conn, data: [Byte])                   : Result[Int, String]
  recv(c: Conn, max: Int)                       : Result[[Byte], String]
  close(c: Conn)                                : Unit
}

effect NetUdp {
  bind(host: String, port: Int)                 : Result[UdpSocket, String]
  send(s: UdpSocket, dst: SocketAddr, data: [Byte]) : Result[Int, String]
  recv(s: UdpSocket, max: Int)                  : Result[(SocketAddr, [Byte]), String]
  close(s: UdpSocket)                           : Unit
}

effect NetDns {
  resolve(host: String)                         : Result[[IpAddr], String]
}

type Net = NetTcp + NetUdp + NetDns
```

The three effects cover the byte-level networking capability,
each focused on one protocol family. The `Net` alias bundles
them when a function does several at once and the precise
breakdown is not worth spelling out — same pattern as `Io =
Console + Stdin + Env + File`.

Higher-level protocols (HTTP, WebSocket, gRPC) live in stdlib
modules that *use* the relevant effects — they do not introduce
new effects of their own. An HTTP client uses `/ NetTcp + NetDns`;
a UDP-only DNS-over-UDP probe uses `/ NetUdp + NetDns`.

`Conn`, `Listener`, `UdpSocket`, `IpAddr`, and `SocketAddr` are
opaque types declared by the `net.tcp` / `net.udp` / `net.dns`
modules. v1 treats them as nominal handles; their internal
representation is implementation-defined.

### Why three effects with an alias

The three families are kept distinct rather than fused into a
single `Net` effect because:

- Capability gating at the protocol boundary is meaningful: a
  component that should only resolve names but never open
  sockets uses `/ NetDns` and is statically prevented from going
  further.
- Each effect has few ops (DNS is a single op), keeping the
  catalog readable and the per-effect handler small.
- The `Net` alias keeps the brief case ergonomic: a function
  that uses TCP, UDP, and DNS together writes `/ Net`, exactly
  as it would have under a single fused effect.

### Error model

Every op except the `close` variants returns `Result[_,
String]`. Network errors are rich and callers routinely branch
on motive (host not found, connection refused, peer closed,
timeout). v1 keeps the payload as a raw `String`; a structured
`NetError` sum is deferred, same as `File`.

`NetTcp.close` and `NetUdp.close` return `Unit` rather than
`Result`: closing a socket can technically fail, but no caller
does anything useful with that information. The handler logs
and swallows.

`NetTcp.recv` returns `Ok([])` when the peer has closed the
connection cleanly. Callers distinguish "no data yet" from "peer
gone" by the empty list, matching the POSIX `recv() == 0`
convention. Asking for `max = 0` is a panic — there is no useful
"read zero bytes" call.

### Default handler

Runtime-installed around `main` for each of `NetTcp`, `NetUdp`,
`NetDns` independently when present in the row. Each op maps to
a POSIX socket call (`socket`, `connect`, `bind`, `listen`,
`accept`, `send`, `recv`, `close`) via `Ffi`. DNS resolution
goes through `getaddrinfo(3)`.

Blocking ops (`NetTcp.connect`, `NetTcp.accept`, `NetTcp.send`,
`NetTcp.recv`, the UDP equivalents, `NetDns.resolve`) suspend
the fiber via the scheduler's reactor (kqueue on macOS / *BSD,
epoll on Linux). The underlying file descriptor is set to
`O_NONBLOCK` and the fiber yields until the readiness event
fires, exactly the same mechanism as `Clock.sleep`. A `Cancel`
raised mid-call unwinds out of the op with no half-written
state visible to user code.

### Stdlib helper

The "abort if anything goes wrong" pattern lifts `Result` into
`Fail`, same shape as `read_file_or_fail`:

```kai
pub fn tcp_connect_or_fail(host: String, port: Int) : Conn / NetTcp + Fail {
  match NetTcp.connect(host, port) {
    Ok(c)  -> c
    Err(m) -> Fail.fail("connect #{host}:#{port}: #{m}")
  }
}
```

### What's not in v1 (planned extensions)

- TLS: `tls_wrap(c: Conn, cfg: TlsConfig) : Result[Conn,
  String]`. Lands as a separate `Tls` effect, not as new
  `NetTcp` ops, because the configuration surface (cert chains,
  SNI, ALPN) is large enough to warrant its own design.
- Unix domain sockets: `uds_connect`, `uds_listen`. Same shape
  as TCP, deferred until a use case demands them. Likely a
  fourth `NetUds` effect under the same `Net` alias.
- Raw / packet sockets: out of scope for stdlib; FFI is the
  right tool for such low-level work.
- HTTP/2 and HTTP/3: the `net.http` stdlib module ships
  HTTP/1.1 in v1; the multiplexed protocols come post-MVP as
  modules over the same `NetTcp` (and `NetUdp` for QUIC) — no
  new effect needed.

## `Process`

### Declaration

```kai
effect Process {
  start(cmd: String, args: [String])  : Result[Child, String]
  wait(c: Child)                      : Result[Exit, String]
  kill(c: Child, sig: Signal)         : Result[Unit, String]
  exit(code: Int)                     : Nothing
}
```

- `start` launches a child process and returns a handle.
  Deliberately not named `spawn` to avoid clash with the `Spawn`
  effect (kaikai fibers); see §*Why `Process` and not `Spawn`*.
  Standard fds (stdin/stdout/stderr) inherit from the parent
  unless the caller redirects them via the `os.process` module's
  pipe helpers, which build on top of these ops.
- `wait` blocks until the child exits and returns its exit
  status. The fiber suspends via the scheduler's reactor
  (`pidfd_open` on Linux; SIGCHLD-driven on macOS / *BSD).
- `kill` delivers a signal to the child. The signal set is the
  POSIX-canonical subset (`SIGTERM`, `SIGKILL`, `SIGINT`, …)
  declared by the `os.process` module.
- `exit` terminates the current process with the given code. It
  never resumes; the return type is `Nothing`. The canonical user
  surface for this op is `os.exit` (top-level helper exposed by
  the stdlib `os` module per `docs/stdlib-layout.md`); calling
  `Process.exit` directly is legal but not idiomatic.

`Child`, `Exit`, and `Signal` are opaque types declared by the
`os.process` module.

### Why `Process` and not `Spawn`

`Spawn` is the kaikai-fibers effect — lightweight in-process
units of execution that share an address space. `Process` is the
OS-level effect — separate processes with their own address
space, lifecycle, and signal handling. The two are unrelated in
implementation and deliberately kept apart; conflating them
would let a function that "only spawns a fiber" silently gain
the right to start a subprocess. The op-level naming reinforces
this: `Process.start` rather than `Process.spawn` so the word
"spawn" is reserved for the fiber primitive.

### Error model

`start`, `wait`, and `kill` return `Result[_, String]`. Common
faults a caller might branch on: "binary not found" (start),
"child already reaped" (wait), "no such process" (kill). v1
keeps the payload as a raw `String`, same as `File` and the
`Net` family.

`exit` does not have an error model — it does not resume.

### Default handler

Runtime-installed around `main` when `Process` is in the row.

- `start` on POSIX is `fork(2)` + `execvp(3)`; on Windows
  (post-MVP) `CreateProcess`.
- `wait` blocks via the scheduler's reactor: on Linux, register
  the child's `pidfd` with epoll and yield; on macOS / *BSD, a
  SIGCHLD handler wakes the awaiting fiber. Cancellation
  unwinds out of the wait without reaping the child — see
  `wait_or_kill` below for the canonical cancel-aware pattern.
- `kill` is `kill(2)`.
- `exit` is libc `_exit(code)`. No flushing of stdio buffers; the
  caller must `print` everything they want printed before
  calling `exit`. (kaikai's `Console` writes are unbuffered, so
  the practical impact is small.)

### Stdlib helper

A cancel-aware wait that signals the child if its own fiber is
cancelled, then re-raises:

```kai
pub fn wait_or_kill(c: Child, on_cancel: Signal) : Result[Exit, String] / Process + Cancel {
  handle {
    Process.wait(c)
  } with Cancel {
    raise(_) -> {
      let _ = Process.kill(c, on_cancel)
      Cancel.raise()
    }
  }
}
```

The common call is `wait_or_kill(child, SIGTERM)`. The handler
runs only when the wait is interrupted by cancellation; on a
clean exit, control flows past the `with` clause and the
`Result` from `Process.wait` is returned untouched.

### What's not in v1 (planned extensions)

- Stdio redirection helpers: `start_with_pipes`, returning a
  `Child` plus reader/writer endpoints for stdin/stdout/stderr.
  Likely lives in `os.process` rather than as new `Process` ops.
- Working-directory and environment overrides per start:
  `start_with_env`, `start_in_dir`. Same locality argument.
- Process-group and session control (`setsid`, `setpgid`).
  Useful for daemonising; out of MVP scope.
- Async signal handling for the *current* process is a separate
  effect — see `Signal` below.

## `Signal`

### Declaration

```kai
type Sig = SigInt | SigTerm | SigHup | SigUsr1 | SigUsr2

effect Signal {
  on(sig: Sig)  : Unit
  off(sig: Sig) : Unit
  await()       : Sig
}
```

- `on(sig)` subscribes the program to `sig`. The runtime blocks
  the underlying POSIX signal at the process level so the kernel
  queues delivery instead of taking the default disposition.
  Idempotent — repeat calls are no-ops.
- `off(sig)` unsubscribes and unblocks. A signal that arrived
  while subscribed but has not yet been drained by `await` will
  be delivered with the default disposition as soon as `off`
  returns; for SIGINT/SIGTERM that means the program dies. Call
  `await` to drain the queue first when this matters.
- `await()` blocks the current OS thread until any subscribed
  signal arrives, then returns the corresponding `Sig` variant.
  An empty subscription set is treated as `{SigInt}` so a
  programmer who forgot `on()` still wakes on Ctrl-C.

### Default handler

Runtime-installed around `main` when `Signal` is in the row.
Implementation: `sigprocmask(SIG_BLOCK, ...)` for `on`, the
matching unblock for `off`, and `sigwait(2)` for `await`.

`sigwait` is synchronous and runs on the calling thread. The
implementation chose it over an async `sa_handler` + drain-on-
yield-point because building a `KaiValue` variant in a signal
handler is not async-signal-safe; the BEAM-style `on_cancel(sig)`
shape sketched in issue #107 also requires Cancel to honour
user-installed handlers on runtime-triggered cancellation, which
v1 does not (`docs/fibers-honesty-targets.md`). Both points
disappear once the m8.x scheduler lands; until then, `on/off/
await` is the shape.

### Platform

Posix-only in v1. macOS and Linux both implement the POSIX
signal API used here.

Windows and WASM map to a smaller subset of `Sig`:
- Windows can deliver Ctrl-C via `SetConsoleCtrlHandler` →
  surface as `SigInt`. `SigTerm` / `SigHup` / `SigUsr*` have no
  Windows equivalent and would map to `Result[Sig, String]` in a
  v2 shape, or be silently absent.
- WASI does not expose signals. The `Signal` effect would not
  install on WASI builds; programs that need it would not link.

Both ports wait on the same lane that brings non-POSIX I/O
backends; they are out of scope for the v1 effect.

### Limitations (v1)

- `await()` blocks the OS thread. Other fibers cannot make
  progress while it is parked. Acceptable when `main` parks on
  `Signal.await()` after spawning workers; reactor-driven
  non-blocking integration with the Spawn scheduler waits on
  m8.x.
- SIGCHLD is not exposed — `Process.wait` already reaps
  children internally.
- Real-time signals (SIGRTMIN+n) and the `siginfo_t` payload
  (`si_pid`, `si_uid`, `si_value`) are out of scope.
- The `on_cancel(sig)` shape from issue #107 (signal arrival
  fires `Cancel.raise()` in the calling fiber) is queued for
  the same lane that lets Cancel honour user-installed
  handlers on runtime-triggered cancel.

### Typical use

Graceful shutdown of a long-running program:

```kai
fn run_server(...) : Unit / Spawn + NetTcp + Console = ...

fn main() : Int / Signal + Spawn + NetTcp + Console = {
  let server = Spawn.spawn(() => run_server(...))
  Signal.on(SigInt)
  Signal.on(SigTerm)
  let _ = Signal.await()
  Stdout.print("shutting down")
  Spawn.cancel(server)
  Signal.off(SigInt)
  Signal.off(SigTerm)
  0
}
```

Drove the reopening of lnds/ahu's `run_app` Tongariki lane (issue
#107). The shape above is what `ahu.run_app(root)` wraps once the
nursery integration lands.

## `Fail`

### Declaration

```kai
effect Fail {
  fail(msg: String) : Nothing
}
```

Declared in Doc A. `Nothing` is the empty type; any clause
handling `Fail.fail` must either not call `resume`, or prove it
has a value of type `Nothing` to pass (impossible), so the
handler *must* short-circuit. Doc A §*Discarding the continuation*
covers this.

### Stdlib helpers

```kai
# Run a fallible computation and capture the error message.
pub fn try[T](body: () -> T / Fail) : Result[T, String] {
  handle { body() } with Fail {
    fail(msg, resume) -> Err(msg)
    return(x)         -> Ok(x)
  }
}

# Run with a default on failure; swallows the message.
pub fn with_default[T](default: T, body: () -> T / Fail) : T {
  handle { body() } with Fail {
    fail(msg, resume) -> default
    return(x)         -> x
  }
}

# Lift a Result into Fail.
pub fn unwrap_or_fail[T](r: Result[T, String]) : T / Fail {
  match r {
    Ok(v)  -> v
    Err(m) -> Fail.fail(m)
  }
}

# Rewrite the message of any Fail.fail raised by body, then
# re-raise. Common case: add context ("while parsing X: …").
pub fn map_fail[T](f: (String) -> String, body: () -> T / Fail) : T / Fail {
  handle { body() } with Fail {
    fail(msg, resume) -> Fail.fail(f(msg))
    return(x)         -> x
  }
}
```

`try` is the idiomatic replacement for `try`/`catch`. The doc's
own spec rejects a `try` keyword; this is a plain function.

### Error payload shape

v1 `Fail` carries `String`. Richer payloads (structured errors,
error chains) would need `Error[E]` — an effect parameterised by
the payload type. Deferred to a later doc; the `String` form
covers the 80th-percentile case and lets stdlib helpers compose.

## `State[T]`

### Declaration

```kai
effect State[T] {
  get()      : T
  set(v: T)  : Unit
}
```

Declared in Doc A. A handler for `State[T]` takes an initial
value via `with State[T](v0)` and threads `state` through
`resume`'s optional second argument. See Doc A §*State: a handler
with its own state* for the worked example.

### Stdlib helpers

```kai
pub fn modify[T, e](f: (T) -> T / e) : Unit / State[T] + e {
  State.set(f(State.get()))
}

pub fn with_state[T, S](init: T, body: () -> S / State[T]) : (S, T) {
  handle { body() } with State[T](init) {
    get(resume)    -> resume(state)
    set(v, resume) -> resume((), v)
    return(x)      -> (x, state)
  }
}
```

`with_state` is the canonical drainer: it threads `init`, runs
`body`, and returns the final value paired with the final state.
Swap the `return` clause for `x` alone if the final state is not
wanted.

### Idiomatic usage (m7b)

The most common pattern is a local mutable cell. With `var`
plus `@` / `:=` (all m7b), this reads like ordinary imperative
code — no `handle` in sight:

```kai
fn sum_costs(events: [Event]) : Int / Console {
  var total = 0
  each(events, (e) => {
    total := @total + e.cost
    Console.print("running: #{@total}")
  })
  @total
}
```

When the final state is part of the function's result — a
builder-style API where the caller gets back `(result, final)`
— reach for `with_state` and trailing-lambda call form:

```kai
fn tokenise(src: String) : (AST, Int) {
  with_state(0) {
    let ast = parse_tracking_cursor(src)
    ast
  }
  # returns (ast, final_cursor)
}
```

`parse_tracking_cursor` has signature `(String) -> AST /
State[Int]`; inside the `with_state` body the default `State`
capability name is in scope, so calls look like `State.get()` /
`State.set(v)` (the `@` / `:=` sugar needs an `as`-bound name,
per §*Syntax note: capability read/write sugar* rule 4).

`modify` is useful inside a `with_state` body (or any handler
that keeps `State` under its default capability name), when the
update rule is a named function:

```kai
with_state(initial_config) {
  each(patches, (p) => modify((cfg) => apply_patch(cfg, p)))
  State.get()
}
```

For straight updates that read-then-write a rebound capability,
write `counter := @counter + 1` directly — that shape is what
the sugar is for. `modify` buys the most when the transform is
already a named function (`modify(normalise_fields)`).

### When to reach for `State[T]` vs `Mutable`

`State[T]` is **value-semantics under the hood**: each `set`
replaces a pure value. Two fibers sharing `State[T]` is not
sharing mutation — each fiber gets its own handler instance. Use
`Mutable` when you need *location* semantics (shared mutable
memory, imperative arrays).

## `Reader[T]` and `Writer[W]`

Two standalone effects for two specialised patterns of `State`-
like state: read-only ambient values (`Reader`) and
write-only accumulation (`Writer`). Each is its own effect
declaration — not sugar over `State[T]` — so diagnostics say
`Reader` / `Writer` by name rather than `State[[W]]` and so
handlers can be specialised independently.

```kai
effect Reader[T] {
  ask() : T
}

effect Writer[W] {
  tell(w: W) : Unit
}

pub fn with_reader[T, S](env: T, body: () -> S / Reader[T]) : S {
  handle { body() } with Reader[T] {
    ask(resume)  -> resume(env)
    return(x)    -> x
  }
}

pub fn with_writer[W, S](body: () -> S / Writer[W]) : (S, [W]) {
  handle { body() } with Writer[W]([]) {
    tell(w, resume) -> resume((), log ++ [w])  # `log` is the handler state
    return(x)       -> (x, log)
  }
}
```

`with Writer[W]([])` parallels `with State[T](init)` — it
declares that the handler carries its own state, initialised
here to `[]`, threaded through `resume`'s second argument and
referred to as `log` inside the clauses. `Writer[W]` always
starts empty; accumulating onto a pre-existing list is
concatenation at the call site (`prefix ++ snd(with_writer {
...})`).

### Telling two effects apart by their type parameter

`Reader[DbConfig]` and `Reader[HttpConfig]` are distinct
effects — the type parameter discriminates them the same way
`State[Int]` and `State[String]` are distinct. A function that
reads both adds both to its row: `/ Reader[DbConfig] +
Reader[HttpConfig]`. No alias is needed to disambiguate the two
in diagnostics; a stdlib or project can still declare
`type Db = Reader[DbConfig]` for readability, but it is never
required to avoid ambiguity.

### Mental model

The three `State`-like effects cover three different shapes of
"non-pure value through a computation":

- **`State[T]`** — the body reads and writes a current value.
  `set` replaces the previous value.
- **`Reader[T]`** — the body only reads, via `ask`; the
  handler decides what value to serve each time. The body has
  no way to change what the next `ask` will return. *Reader is
  not a guarantee that repeated `ask`s return the same value*:
  a handler is free to compute a fresh answer per call (e.g.
  one that serves the current system clock). What Reader rules
  out is the body expressing writes — the write capability is
  simply not in the effect declaration.
- **`Writer[W]`** — the body only writes, via `tell`; the
  handler accumulates in an internal state (typically `[W]`)
  that is harvested at the handler's close via `return`. The
  body cannot read back what it has written mid-stream. If
  reading back matters, reach for `State[[W]]` instead.

The read/write sugar follows the shape:

| Effect      | `@cap` (read) | `cap := v` (write) |
|-------------|:-------------:|:------------------:|
| `State[T]`  | ✓             | ✓                  |
| `Reader[T]` | ✓             | —                  |
| `Writer[W]` | —             | —                  |

`@cap` applies where the effect exposes a single read op;
`cap := v` applies where the effect exposes a value-replacing
`set` op (the State pattern, not the Writer accumulation
pattern).

## `Mutable`

The provisional `Array[T]` escape from CLAUDE.md — mutation that
is not visible in the value's type — migrates to sit behind the
`Mutable` effect. Writing a row with `Mutable` is the source of
truth that a function mutates memory; calling `Mutable`'s
operations without a handler in scope is a type error, same as
any other effect.

### Declaration

```kai
effect Mutable {
  ref_make[T](init: T)                              : Ref[T]
  ref_get[T](r: Ref[T])                             : T
  ref_set[T](r: Ref[T], v: T)                       : Unit

  array_make[T](length: Int, init: T)               : Array[T]
  array_length[T](a: Array[T])                      : Int
  array_get[T](a: Array[T], i: Int)                 : T
  array_set[T](a: Array[T], i: Int, v: T)           : Unit
  array_grow[T](a: Array[T], new_length: Int, init: T) : Unit
}
```

`Mutable` uses a per-operation type generic: `[T]` in each op
signature quantifies independently at the call site, so a
single handler instance serves `Ref[Int]`, `Ref[String]`, and
every `Array[T]` instantiation in the program. Parameterising
the effect itself (à la `State[T]`) would lock one handler to
one type, which is useless here.

This form — operations with their own `forall T. …` quantifier
— is the amended version of Doc A §*Out of scope for v1* item
3. The type checker treats `Mutable.array_get` as having type
`forall T. (Array[T], Int) -> T / Mutable` and reuses the
existing Hindley–Milner generalisation machinery at each call
site. Row polymorphism at the call site remains out of scope.

### Out-of-bounds behaviour

`array_get`, `array_set`, and `array_grow` panic via the
runtime's audited escape (`kai_prelude_panic`) when `i < 0` or
`i >= array_length(a)`. None of the ops returns `Option[T]` or
otherwise signals the condition as a value — the contract is
"the caller is responsible for staying in bounds". This is the
same discipline as C / Rust's unchecked indexing, with the
difference that the panic is observable and produces a stack
trace rather than being undefined behaviour. The array-indexing
sugar (`a[i]`, `a[i] := v`; see
`docs/syntax-sugars.md` §4) inherits this behaviour verbatim.

### Default handler

The runtime's default `Mutable` handler is trivial: every clause
performs the native allocation / load / store via `Ffi` and
`resume`s with the result. It is installed by default for `main`
rows containing `Mutable` because the alternative ("every
program that touches an array must write a handler") is
pointless.

An explicit `with Mutable { ... }` block in user code exists only
to *observe* or *intercept* mutation — test harnesses that want
to log every `array_set`, for instance. Doc C defines the exact
wire format for such observers.

### Observable-effects discipline (issue #251 + #252)

`Mutable` follows the *observable-effects* school (Koka, Eff):
the row captures what the *caller* observes during the call, not
what the function does internally. For `Array[T]`, this gives
two precise rules:

1. **`array_set` requires `Mutable` IF AND ONLY IF the mutation
   is observable to the caller.** The mutation is observable
   when the caller shares a reference to the mutated Array — the
   Array is a parameter, captured from an enclosing scope, or
   read from a global / record field originating outside the
   function. The mutation is NOT observable when the Array was
   constructed locally inside the function (`array_make`, or a
   chained `array_set` / `array_grow` of an already-local
   binding) and the function never passes the Array to another
   helper before returning. Returning the Array as the
   function's tail value is fine — the caller receives a
   settled value, not a witnessable mutation.

2. **`array_get` and `array_length` NEVER require `Mutable`,
   regardless of the Array's provenance.** Reading is pure from
   the caller's POV; the same is true for `array_make` (a
   constructor, not a write).

The typer enforces this by collecting Mutable demand at every
`array_set` / `array_grow` call site, then walking the typed
function body once to classify each demand as local (the target
Array was constructed in this function and never escapes via a
non-`array_*` call) or external (anything else). A function
whose every demand is local has `Mutable` *masked* from its
inferred row — the same way `var x = init` masks `State[T]` at
the surrounding block boundary. A function with even one
external demand keeps `Mutable` and must declare it.

### Three worked examples

#### Example 1 — Local construction, NOT observable, NO `Mutable`

```kai
fn build_circle(verts: Int) : Array[Real] {
  let f = array_make(verts * 2, 0.0)   # local
  var i = 0
  repeat(verts) {
    f[@i * 2]     := raylib.cos(...)   # array_set on LOCAL f
    f[@i * 2 + 1] := raylib.sin(...)
    i := @i + 1
  }
  f                                     # returned; caller never witnesses the mutation
}
```

`Array[Real]` (no `/ Mutable`) — the masking pass classifies
every demand as local because `f` originates from `array_make`
and never crosses a function-call boundary inside the body.

#### Example 2 — Parameter mutation, IS observable, REQUIRES `Mutable`

```kai
fn set_vertex(f: Array[Real], i: Int, x: Real, y: Real) : Unit / Mutable {
  f[i * 2]     := x   # array_set on PARAMETER f
  f[i * 2 + 1] := y
}
```

`/ Mutable` is required: the caller's `f` has new values after
the call returns. Omitting the row is a type error with the
`array_set`-call site pinpointed in the diagnostic.

#### Example 3 — Read-only, NO `Mutable` regardless of provenance

```kai
fn draw_buffer(buf: Array[Int]) : Unit / Ffi {
  var i = 0
  repeat(buf_len) {
    let n = buf[@i]   # array_get — pure read, never observable
    if n < max_iter {
      raylib.draw_rectangle(..., color_for(n))
    }
    i := @i + 1
  }
}
```

`/ Ffi` only. No `Mutable`. `array_get` is pure regardless of
`buf`'s provenance.

### Conservative escape rule (v1 scope)

Edge case explicitly out of scope for v1: passing a
locally-constructed Array to ANOTHER function that mutates it.
The conservative classification treats every Array passed as an
argument to a non-`array_*` callee as escaping (the callee
might mutate it), forcing `Mutable` in the caller's row even if
the binding originated from `array_make` here. A future
refinement may track callee purity. Today the workaround is to
keep mutation inline inside the constructing function.

### Why this discipline

- **Approachable surface (Tier 2 #5)**: a function returning a
  freshly-built Array reads as pure to the user, the same way
  `let xs = [1, 2, 3]` does. Threading `/ Mutable` through every
  helper that uses a local scratchpad would be needless friction.
- **Effects visible (Tier 1 #1)**: the row still captures every
  observable effect — the caller's mental model is intact.
  Mutation through a parameter / capture / global is observable
  and therefore in the row.
- **`var` precedent**: kaikai already masks `State[T]` for
  `var x = init` because the State doesn't escape the block. The
  same logic applies to `Array[T]` whose lifetime is bounded by
  the function body.

### Idiomatic usage

```kai
fn fill_squares(n: Int) : Array[Int] {
  let xs = array_make(n, 0)
  var i = 0
  repeat(n) {
    xs[@i] := @i * @i   # local Array — Mutable masked
    i := @i + 1
  }
  xs
}

fn sum(xs: Array[Int]) : Int {
  var total = 0
  var i = 0
  repeat(array_length(xs)) {
    total := @total + xs[@i]   # array_get — never raises Mutable
    i := @i + 1
  }
  @total
}

fn set_first(xs: Array[Int], v: Int) : Unit / Mutable {
  xs[0] := v   # parameter mutation — Mutable required
}
```

`Ref[T]` keeps the explicit op form:

```kai
let counter = Mutable.ref_make(0)
Mutable.ref_set(counter, Mutable.ref_get(counter) + 1)
```

In practice `Ref[T]` is rare in application code — `var` plus
`State[T]` covers almost every cell-valued use case, and
`Ref[T]` is reserved for cases where the cell has to outlive a
block and be passed across functions without being threaded
through `State[T]`'s handler.

### Migration plan for existing code

`Mutable` landed in m7b #2b (2026). Stage 2's inferencer now
ships the effect as a real `DEffect` declaration (see
`builtin_mutable_decl`) with per-op generics over `T`, and a
default handler whose clauses delegate to the existing
`kai_prelude_array_*` runtime entry points.

Issue #251 + #252 (2026-05) added the demand-collection +
provenance-masking pass: bare `array_set` / `array_grow` calls
raise a Mutable demand, the masking pass at the end of each
function body classifies the demand as local-non-escaping or
external, and the row is computed accordingly. Bare
`array_make / array_length / array_get / array_set / array_grow`
remain reachable as prelude builtins because the desugars for
array-index sugar (`a[i]` / `a[i] := v`, m7b #6) and the `var`
specialisation (m7b #16) emit bare calls. Users may still write
the explicit `Mutable.array_*` form — it routes through the
op-call pathway and adds the same label.

## `Cancel`

### Declaration

```kai
effect Cancel {
  raise() : Nothing
}
```

A single operation: `raise` behaves like `Fail.fail` — no value
to pass, so any handler must short-circuit. Unlike `Fail`, the
*caller* rarely invokes `raise` directly; it is the scheduler
(via `Spawn`) that injects `Cancel.raise()` into a fiber marked
for cancellation at its next effect op.

### Handling for cleanup

A fiber that holds a resource and wants to release it on
cancellation wraps the resource-using body in a `Cancel`
handler. The handler runs for its side effects and does not
`resume`, so the fiber unwinds out of the wrapped block:

```kai
fn process_log(path: String) : Unit / Console + File + Cancel {
  match File.read_file(path) {
    Err(m) -> Console.eprint("open failed: #{m}")
    Ok(contents) -> {
      let tmp = "#{path}.partial"
      handle {
        each(lines(contents), (line) => {
          Console.print("line: #{line}")
          append_tmp(tmp, line)
        })
      } with Cancel {
        raise(resume) -> {
          Console.eprint("cancelled: discarding partial #{tmp}")
          # resume is not called; the fiber unwinds
        }
      }
    }
  }
}
```

If the fiber is cancelled while iterating lines, the `Cancel`
handler runs the cleanup (log a message, drop the partial
file), and the fiber continues unwinding out of the enclosing
scope. Outside of a wrapping `Cancel` handler, an unhandled
`Cancel.raise()` unwinds the fiber cleanly, releasing any
Perceus-managed values on the way.

### Unwind through nested handlers

When `Cancel.raise()` propagates up the handler stack,
**intermediate handlers' `return` clauses do not run**. The only
cleanup that executes during unwind is the code inside a
`with Cancel { raise(_) -> ... }` clause the programmer
installed on purpose. Other effects' `return` clauses are
skipped (Doc A §*Discarding the continuation*: `return` runs
only on the normal-completion path, which a `Cancel` unwind is
not). Perceus still decrements reference counts for every value
going out of scope, so memory is not leaked — but explicit
resource closing (closing file handles, rolling back
transactions) requires an explicit `Cancel` handler at the
scope that owns the resource.

### Delivery points

`Cancel.raise()` is delivered by the scheduler only at yield
points — effect operation sites. A tight CPU loop with no
effect calls (pure arithmetic, pure list traversal with no
stdlib ops) cannot be interrupted until it reaches one. The
canonical fix is to call `Spawn.yield()` periodically inside
the loop: yield cedes control to the scheduler, and if the
fiber is flagged for cancellation the scheduler injects
`Cancel.raise()` there. Same discipline as Trio, Kotlin
coroutines, Eio.

```kai
fn crunch(n: Int) : Int / Spawn + Cancel {
  var total = 0
  each(range(0, n), (i) => {
    total := @total + expensive(i)
    if i % 1000 == 0 { Spawn.yield() }    # cancellation point
  })
  @total
}
```

Note: the fiber itself never *invokes* `Cancel.raise()` — that
is always the scheduler's job. What the fiber controls is
*where* it is willing to be interrupted, and that is expressed
via `Spawn.yield()` (or any other effect operation already in
the code).

Further delivery details (sibling-crash propagation, timeout
interaction, nursery-driven cancellation) live in
`docs/structured-concurrency.md` — Doc B does not duplicate them.

## `Spawn`

Cross-reference: `docs/structured-concurrency.md`. That doc is
the authoritative spec for fibers, nurseries, and the scheduler.
Here we only pin the effect signature so the catalog is complete.

```kai
effect Spawn {
  spawn[T, e](f: () -> T / e)   : Fiber[T]
  await[T](f: Fiber[T])         : T
  select[T](fs: [Fiber[T]])     : T
  yield()                       : Unit
  cancel[T](f: Fiber[T])        : Unit
}
```

`Spawn` uses per-op generics (the amended Doc A §*Out of scope
for v1* item 3, see §`Mutable`) plus a row variable on `spawn`'s
argument. The row variable reuses the existing row polymorphism
from Doc A; the per-op `T` is the type-generic piece shared with
`Mutable`.

### `Spawn.cancel` vs the `Cancel` effect

Two related names, one distinction worth pinning:

- **The `Cancel` effect** is what a fiber *receives* when
  cancellation reaches it — its single op `raise()` is injected
  by the scheduler at the next yield point.
- **`Spawn.cancel(f)`** is what a fiber *calls* to ask the
  scheduler to deliver `Cancel.raise()` into another fiber `f`.

In short: `Spawn.cancel(child)` triggers a `Cancel.raise()` on
`child`'s side. Both names are intentional — `Spawn.cancel`
lives with the rest of the fiber-lifecycle ops; the `Cancel`
effect captures the receiving-end semantics.

### Default handler

The root nursery installed by the runtime when `main`'s row
contains `Spawn`. Nested `nursery (n) => {...}` blocks desugar
to `handle { ... } with Spawn as n { ... }`, as noted in
`docs/structured-concurrency.md` §*Implementation notes*.

## `Ffi`

The escape hatch for calling into C. Every `extern "C" fn`
declaration installs a synthetic operation on `Ffi`:

```kai
extern "C" fn abs(n: Int) : Int                 # uses Ffi

fn absolute(x: Int) : Int / Ffi {
  abs(x)
}
```

The example uses `Int` to keep the surface minimal; interop
involving strings, pointers, or compound structs requires an
additional type layer (`CString` and friends) specified in a
future dedicated FFI doc.

### Default handler

Compiler-synthesised: `Ffi` operations compile directly to the C
ABI call at the declared symbol. There is no user-written clause
to run; the "handler" is a compile-time lowering step.

### Usage discipline

Direct `Ffi` use is an **audited escape** (CLAUDE.md Tier 1). It
appears in the `runtime/` directory and in the default handlers
for `Console`, `Stdin`, `Env`, `File`, `Mutable`, and `Spawn` —
every effect whose behaviour ultimately touches the OS. Pure
effects (`Fail`, `State[T]`, `Reader[T]`, `Writer[W]`) and
`Cancel` do not use `Ffi`. Userland almost never writes
`extern "C"` directly, because the stdlib exposes `Console` /
`File` / `Mutable` etc. for the common cases.

### Safety

`Ffi` is outside the guarantees the rest of the effect system
provides. Specifically:

- The compiler trusts the declared signature of each `extern
  "C" fn`: a mismatch between the kaikai types and the real C
  symbol is **undefined behaviour**, not a type error. The
  author of the declaration is responsible for cross-checking
  the C header.
- Perceus does not manage memory that crosses the FFI boundary.
  Any pointer, buffer, or string passed to C must have its
  lifetime managed by the caller — the RC walker sees kaikai
  values, not C-side allocations.
- Effects other than `Ffi` are not preserved across the call.
  A C callback that wants to raise `Fail` has to go through an
  explicit bridge back into a handler; it cannot invoke a
  kaikai operation directly.

These constraints are why `Ffi` is listed as an audited escape
alongside `panic` and unfilled `?` in CLAUDE.md Tier 1. Every
`extern "C" fn` declaration should be treated as a small,
reviewed surface — ideally consolidated inside `runtime/` or a
single stdlib module per third-party library.

## Migration of existing builtins
<!-- coverage: skip --> historical migration plan, not a feature

The table below maps every kaikai-minimal builtin
(`docs/kaikai-minimal.md` §*Built-in functions*) to its
post-effects form.

| Current builtin                                | New form                                   | Effect row |
|-----------------------------------------------|--------------------------------------------|------------|
| `print(s)`                                    | `Console.print(s)`                         | `Console` |
| `eprint(s)`                                   | `Console.eprint(s)`                        | `Console` |
| `read_line()`                                 | `Stdin.read_line()`                        | `Stdin + Fail` (real I/O faults) |
| `read_file(path)`                             | `File.read_file(path)`                     | `File` |
| `write_file(p, c)`                            | `File.write_file(p, c)`                    | `File` |
| `args()`                                      | `Env.args()`                               | `Env` |
| `int_to_string`, `real_to_string`, …          | unchanged                                  | pure |
| `string_length`, `string_concat`, …           | unchanged                                  | pure |
| `list_length`, `list_append`, `list_reverse`  | unchanged                                  | pure |
| `exit(code)`                                  | `exit(code)` — builtin, not an op          | audited escape, no effect |
| `panic(msg)`                                  | `panic(msg)` — builtin, not an op          | audited escape, no effect |
| `array_make / get / set / grow`               | `Mutable.array_*` (or `a[i]` / `a[i] := v`) | `Mutable` |
| (new) `ref_make / get / set`                  | `Mutable.ref_*`                            | `Mutable` |
| `map`, `filter`, `reduce`, `each`             | unchanged signatures, rows stay open       | `/ e` (inherited from the callback) |

A function that does "several of the above" — say, a CLI tool
that reads args, opens a config file, and logs progress — can
spell its row as `/ Env + File + Console` or use the stdlib
alias `/ Io` (= `Console + Stdin + Env + File`). Prefer the
granular form when only one or two capabilities are used;
reach for `Io` when the function is genuinely doing
"everything".

Two builtins stay as direct calls outside any effect: `exit` and
`panic`. They are non-resumable by construction (return type is
the bottom `a` / `Nothing`), so treating them as effect ops would
not buy anything — the type already prevents the caller from
continuing, and a handler could not meaningfully catch them. Both
remain audited escapes per CLAUDE.md Tier 1.

### What users have to change

- Bodies that call `print` / `read_file` / etc. must name the
  effect at the call site: `Console.print(s)`,
  `File.read_file(path)`, and so on. No implicit rewriting from
  the bare builtin name — the whole point of the migration is
  to surface which effects a function uses, so the explicit
  form is required from m7a onward.
- Public signatures gain the effects their body now names:
  `/ Console`, `/ File + Fail`, `/ Console + Env`, or the
  umbrella `/ Io`. Existing signatures without `/` become
  *pure* and will fail to type-check if their body calls any
  effect op — this is the intended failure mode.
- Array callers gain `/ Mutable`. Since arrays are confined to
  the compiler today, the radius is small.

A one-shot codemod (`kai fmt --upgrade-effects`) performs the
mechanical conversion (`print` → `Console.print`, etc., plus
effect-row updates on signatures). It ships in m7a alongside the
first type checker that can see effects; migration to the m7b
sugars (`a[i]`, `@counter`, `var x = 0`) is a separate pass,
done by hand or by a second codemod. Implementation detail
lives in Doc C.

## `main` and the runtime

### Allowed `main` signatures

Any effect whose default handler the runtime installs may
appear in `main`'s row, individually or in combination:

```kai
fn main()                                             # pure
fn main() : Int                                       # pure, returns exit code
fn main() : Unit / Console                            # only logs
fn main() : Int  / Console + Env                      # reads args, logs, exits with code
fn main() : Unit / Console + Stdin                    # interactive: read lines, echo
fn main() : Unit / File + Console                     # batch: open files, log progress
fn main() : Unit / Io                                 # the alias: Console + Stdin + Env + File
fn main() : Unit / Io + Mutable                       # ... plus array / ref use
fn main() : Unit / Io + Mutable + Spawn               # ... plus fibers; `Cancel` is implied by Spawn
```

Prefer the granular effects when only one or two apply (they
document the program's actual surface); reach for `/ Io` when
the program does essentially all of `Console + Stdin + Env +
File`. Any effect without a runtime-installed default handler
(`Fail`, `State[T]`, `Reader[T]`, `Writer[W]`) in `main`'s row
is rejected by the compiler:

```
error: main cannot have unhandled effect `Fail`
  |
7 | fn main() : Unit / Io + Fail {
  |                        ^^^^
  = note: effects without a runtime-installed default handler
          must be handled inside the program.
  = help: wrap the effectful code in `try { ... }` or `handle`.
```

### Installation order

The runtime wraps `main` in nested handlers, innermost first.
Only effects present in `main`'s row get a handler installed;
the others are absent from the stack entirely.

```
Ffi                                   (always innermost, compiler-synthesised)
  Mutable                             (if present)
    Console / Stdin / Env / File      (any present; order among them does not matter)
      Cancel                          (if Spawn is present)
        Spawn                         (if present; root nursery)
          main()
```

- `Ffi` is innermost because every other default handler
  ultimately dispatches through it.
- `Console`, `Stdin`, `Env`, `File` sit in the middle as a
  commutative group: none depends on another, so the compiler
  is free to nest them in any order. Pretty-printers pick a
  canonical order for diagnostics.
- `Cancel` is inside `Spawn` because the root nursery is what
  flags fibers for cancellation and delivers `Cancel.raise()`.

### `bin/kai run` specifics

- `kai run file.kai` → `kai build` + exec the output binary.
- Exit code: `Unit` → 0, `Int` → the returned value, any panic
  → 1 + stderr banner.
- stdin/stdout/stderr inherit from the invoking shell — no
  redirection plumbing inside the runtime.
- Signals:
  - `SIGINT` (Ctrl-C) — with `Spawn` present: triggers
    `Spawn.cancel(root)`, giving cleanup handlers a chance to
    run; without `Spawn`: program exits with code 130 directly.
  - `SIGTERM` — same dispatch as `SIGINT`, exit code 143
    without `Spawn`.
  - `SIGPIPE` — ignored at startup so `EPIPE` surfaces as a
    write error inside the `Console` default handler, which
    absorbs it silently. See §`Console` *Default handler* for
    the fault-handling detail.

## Out of scope for v1

- **User-overridable default handlers.** The runtime's
  defaults for `Console`, `Stdin`, `Env`, `File`, `Mutable`,
  `Cancel`, `Spawn`, and `Ffi` are built-in; users cannot swap
  them for alternate implementations at link time.
- **First-class handler values.** Handlers appear only at
  `handle ... with ...` sites, consistent with Doc A.
- **`Error[E]` parameterised over payload.** `Fail(String)` is
  the v1 story; richer errors are a later extension.
- **Handler composition combinators** (`merge`, `lift`, `rename`
  à la Koka). The nesting rule from Doc A is enough for v1.
- **Actors and mailboxes (`Actor[Msg]`).** Deferred in full to
  `docs/actors.md`; Doc B does not ship a signature, so callers
  cannot plan against one yet.
- **Distributed actors.** Phase 5+ per `docs/design.md`.
- **Immutable O(1)-access container (`Vector[T]`).** See
  §`Mutable` *Known gap*: `Array[T]` is mutable-only and
  `List[T]` is O(n); a persistent `Vector[T]` covers the gap
  and is the next collection to land after m7b.

## Open questions

1. **Per-operation type generics.** `Mutable`, `Spawn`, and
   possibly others need operations with their own `[T]`
   quantifier; Doc A §*Out of scope for v1* item 3 currently
   forbids this.
   *Decided:* allow per-op HM generalisation of *type*
   parameters (row parameters stay out of scope). The type
   checker treats `Mutable.array_get` as having type
   `forall T. (Array[T], Int) -> T / Mutable` and each call
   site picks `T` independently. Doc A §*Out of scope for v1*
   item 3 is amended accordingly; implementation lands in m7b.

2. **`Writer[W]` as primitive or sugar.** Is `Writer[W]` its own
   effect declaration, or stdlib sugar over `State[[W]]`?
   *Decided:* its own effect declaration, shipping in m7b.
   Diagnostics say `Writer` instead of `State[[String]]`, and
   the handler can keep a one-line body using `with Writer[W]([])`
   plus the canonical `tell` / `return` clauses — symmetric with
   `State[T](init)`.

3. **Grace-period short form for IO ops.** Do we keep `print(s)`
   callable during migration, auto-elaborating to
   `Console.print(s)`?
   *Decided:* no. The point of the migration is to force the
   effects used by each function to appear in its row; a
   silent rewrite defeats the goal. The codemod (below) handles
   the one-time mechanical conversion.

4. **Codemod timing.** Does `kai fmt --upgrade-effects` ship in
   m7a alongside the first type checker that can see effects,
   or slip to m7b once the ergonomic sugars land?
   *Decided:* m7a. The codemod rewrites bare builtins to their
   explicit effect form (`print` → `Console.print`, etc.) and
   adds the corresponding rows to signatures. Migration to the
   m7b sugars (`a[i]`, `@counter`, `var x = 0`) is a separate
   pass and can be done by hand or by a second codemod later.

5. **Exit-code shape of `main`.** `fn main() : Int / Io` is
   accepted; is it idiomatic, or should the convention be
   `fn main() : Unit / Io` plus explicit `exit(code)`?
   *Decided:* accept both. The style guide prefers `Int` return
   for the normal case — clearer, composes with testing — and
   keeps `exit(code)` as the escape hatch for early exits from
   deep call trees that do not want to propagate through `Fail`.

6. **Multiple `Mutable` scopes.** v1 has one global `Mutable`
   handler (the runtime default). Do we allow user-nested
   `with Mutable { ... }` for observation/mocking, or forbid it
   to keep the handler-stack shape predictable?
   *Decided:* allow nested observation handlers. The innermost
   handler wins (Doc A semantics), so a test harness can wrap
   a body with a `Mutable` interceptor to log every `array_set`.
   Such a handler sees stdlib-internal `Mutable` calls too;
   Doc C pins the exact wire format so observers are
   deterministic.

## Next steps

- **Doc C** — `docs/effects-impl.md`: the CPS transform in the
  stage-2 pipeline, `TyFnT` gaining an effect-row slot, the
  handler-stack runtime representation, per-op type generics
  implementation, interaction with monomorphisation and fibers
  (m8), diagnostic quality for row-mismatch and
  effect-not-handled errors, codemod specification for
  `kai fmt --upgrade-effects`.

- **`docs/actors.md`** — covers `Actor[Msg]`, `Pid`, mailbox
  policies, supervision, links/monitors. Separate doc because
  the design surface is large and its shape does not affect the
  Doc B catalog.

- **`docs/syntax-sugars.md`** — future consolidation of the
  call-site sugars decided in Doc B (trailing lambdas, `@cap` /
  `cap := v`, `var x = init`, `a[i]` / `a[i] := v`). These are
  general language features, not effect-specific; Doc B introduces
  them next to the effects that motivate them, but a standalone
  syntax spec avoids scattering the grammar across effect docs.

Milestone m7 splits into two sub-milestones to keep the blast
radius tractable:

- **m7a — mechanics**: row unification in the checker, `TyFnT`
  with an effect-row slot, CPS transform of ops and handlers,
  handler-stack runtime, default handlers for `Console`,
  `Stdin`, `Env`, `File`, `Mutable`, `Fail` (and `Ffi` as
  compiler-synthesised), basic diagnostics for row-mismatch and
  effect-not-handled. End state: `fn main() : Unit / Console {
  Console.print("hi") }` compiles and runs end-to-end. Effects
  function, just verbosely.

- **m7b — ergonomics**: closed effect aliases (`type Io =
  Console + Stdin + Env + File`), per-operation type generics
  (Doc A §*Out of scope for v1* item 3 amended), trailing
  lambdas for call-site syntax (`try { body }`, `with_state(0)
  { body }`, `nursery { n -> ... }`), capability read/write
  sugar (`@counter` for `State.get`/`Reader.ask`; `counter :=
  v` for `State.set`), local mutable cells (`var x = 0`),
  array indexing sugar (`a[i]` / `a[i] := v` over
  `Mutable.array_{get,set}`), `Reader[T]` / `Writer[W]` as
  their own effects (Open question #2 resolved in their
  favour). End state: Doc B reads on the page the way its code
  samples suggest.

Within each sub-milestone the ordering is dependency-driven:
`Ffi` and `Mutable` first (unblocks stage 2's array migration),
then the console/file/fail triple (unblocks userland programs),
then `State` and its kin, then `Cancel`/`Spawn` with the
scheduler.
