# Lazy streams in the stdlib — design

> **Status: SHIPPED (2026-06-10), issue #801.** Implemented in
> `stdlib/stream.kai`; fixtures in `examples/effects/stream_*.kai` and the
> `demos/wc.kai` demo, wired into `make test-effects`. Supersedes the
> pull-thunk sketch discussed against issue #771; #771 Phase 1 (the chunked
> `File` primitive) is the substrate.
>
> **Two corrections the implementation made to the draft below**, kept
> inline because they are load-bearing:
> 1. **The carrier is a single-constructor *variant*, not a bare
>    function-type alias.** `pub type Stream[t, e] = Stream(((t) -> Bool /
>    e) -> Unit / e)`. A function-type alias has no nominal head, and the
>    `|` / `||` / `|?` pipes dispatch by head type (`TyCon` only) — so an
>    alias carrier silently declines the pipes. The nominal wrapper gives
>    the head `Stream`; Perceus reuses the one-field constructor in place
>    through every stage. (Landing this surfaced a typer bug where a
>    nominal type's higher-order function field dropped its effect row at
>    constructor instantiation — fixed in `scheme_of_variant`, infer.kai.)
> 2. **The recoverable fault is `ReadFault`, not `Fail`** — spike S2 pinned
>    this. `read_lines` carries `File + ReadFault`, and a consumer that
>    uses it must `handle ... with ReadFault` to pick a policy. The
>    galleries below are updated accordingly.
>
> Pipe dispatch on a user `pub type` is gated on edition ≥ `hanga-roa`
> (#603); the `kai` driver passes this from `kai.toml`/`EDITION`
> automatically.

## Goals

Ranked, per the language owner's brief:

1. **Ergonomics** — the common programs read as one pipe expression.
2. **Novelty where it pays off** — exploit what only kaikai has: effect
   rows, handlers with typed one-shot resume, pipe dispatch by convention.
3. **Economy** — fewer lines than Go, fewer concepts than Rust, no more
   ceremony than Elixir.
4. Lazy end-to-end: constant memory over unbounded input, for **reading
   and writing**.

Non-goals for v1: `zip`, network sources, a general iterator protocol,
parallel streams.

## What the referents do

### Go — `bufio.Scanner` + `iter.Seq` (1.23)

Go's classic surface is explicit and verbose; the 2024 `iter.Seq` redesign
is the interesting one: a sequence is **a function that takes a `yield`
callback** (`type Seq[V] func(yield func(V) bool)`). Internal (push)
iteration. Two lessons:

- **The resource lives inside the producer.** The producer opens the file,
  loops, and closes on every exit — `defer` fires when iteration stops.
  No caller-side bracket, no leak on early exit.
- **Early stop is a protocol**: `yield` returns `false` when the consumer
  is done; the producer must honor it and unwind.

The transform-a-file program in Go remains ~15 lines of ceremony (two
opens, three defers, an error check per step) because errors and the
writer side never composed with `Seq`.

### Elixir — `Stream`, `File.stream!`, `Stream.into`

The pipe benchmark. Read-transform-write is one expression:

```elixir
in_path
|> File.stream!()
|> Stream.map(&String.upcase/1)
|> Stream.into(File.stream!(out_path))
|> Stream.run()
```

Lessons:

- **The sink is composable** (`Stream.into`) — writing is not a special
  loop, it's one more stage. This is the single biggest economy win.
- A trailing `Stream.run()` is needed because every stage is lazy and
  nothing forces the pipeline. Slightly leaky.
- Errors do not compose: `File.stream!` **raises** on any IO fault. Skip
  the bad line / collect errors are not expressible in the pipeline.

### Rust — `BufRead::lines()` + `Iterator`

```rust
let reader = BufReader::new(File::open(input)?);
let mut writer = BufWriter::new(File::create(output)?);
for line in reader.lines() {
    writeln!(writer, "{}", line?.to_uppercase())?;
}
```

Lessons:

- **Per-element fallibility is real**: `lines()` yields
  `io::Result<String>`, because a fault can hit any read. Rust is honest
  about it — and pays by contaminating every element: each combinator
  stage handles `Result`, iterator adapters need `filter_map(Result::ok)`
  or collect-into-`Result` tricks.
- Resource cleanup is by RAII drop — implicit, correct, invisible.

### Scorecard

| | Go (`iter.Seq`) | Elixir | Rust | kaikai v2 target |
|---|---|---|---|---|
| read-transform-write, one expression | no | yes (+`run`) | no | **yes** |
| element type stays clean (`String`) | yes | yes | no (`Result<String>`) | **yes** |
| per-element error recovery | no | no (raises) | yes (at type cost) | **yes (handler + resume)** |
| caller-side resource ceremony | none | none | none (RAII) | **none** |
| early stop without draining | yes | yes | yes | **yes (spike S1)** |

The novelty cell is the last column of row 3: only an effect system with
typed first-class resume gets clean elements **and** per-element recovery.

## The design

### Carrier: push (internal iteration)

```kaikai
# A stream wraps a function that runs its consumer: `run` calls `yield`
# once per element, and `yield` returns false to stop early. The nominal
# wrapper (a single-constructor variant, not a bare function alias) is
# what lets `|` / `||` / `|?` dispatch on `Stream` by head type.
pub type Stream[t, e] = Stream(((t) -> Bool / e) -> Unit / e)
```

This is Go's `iter.Seq` shape, wrapped in a one-field variant so it has a
nominal head — no language change, no HKT, monomorphises like everything
else; Perceus reuses the constructor in place through every stage.
Why push wins over the pull-thunk (`() -> Option[t]`) sketch:

- **The resource is owned by the producer.** `read_lines` opens the file,
  loops `read_chunk`, and closes on normal end, early stop, *and* effect
  unwind. The caller-side `with_lines` bracket from the pull design
  disappears — cleanup is by construction, like Go's `defer` and Rust's
  drop.
- **The handler-scope footgun disappears.** Pull defers the `read_chunk`
  perform to wherever the consumer pulls — possibly outside the `handle`
  that was supposed to mock it. Push runs the whole loop wherever the
  *sink* runs; sinks force the pipeline in one expression, so the
  `handle` wraps production and consumption together.
- **`Mutable` leaves the row.** A pull thunk needs a cursor cell that
  escapes in the closure → honest `/ Mutable` forever. Push state
  (`take`'s counter, the chunk buffer position) lives and dies inside a
  single activation of the stream function — the cell never escapes, so
  masking it is *sound* under the #251/#252 discipline. File streams are
  `/ File + Fail`, list streams are pure. The alias debate (`Lines`,
  `Reader`...) evaporates: the rows are short enough to write plainly.
- **Use-after-pipe corruption disappears.** A pull stream piped twice
  shares a cursor → silent interleaved garbage (the v1 review's worst
  finding). A push stream is a *recipe*: consuming it twice re-runs the
  producer from the start (re-opens the file). Surprising at worst,
  never corrupt. Documented semantics: a `Stream` is re-runnable, not
  resumable.

What push gives up: consumer-driven interleaving (`zip`). Deferred —
see §Out of scope. When demand appears, effects can invert push→pull
with a generator handler; no other referent has that escape hatch.

### Surface (v1, all `#[unstable]`)

```kaikai
# -- sources ---------------------------------------------------------
pub fn from_list[t, e](xs: [t])     : Stream[t, e]                  # pure: row stays free
pub fn read_lines(path: String)     : Stream[String, File + ReadFault]

# -- stages (pipe-canonical: `|` `||` `|?` dispatch on these) --------
pub fn map[a, b, e](s: Stream[a, e], f: (a) -> b / e)            : Stream[b, e]
pub fn flat_map[a, b, e](s: Stream[a, e], f: (a) -> Stream[b, e] / e) : Stream[b, e]
pub fn filter[t, e](s: Stream[t, e], p: (t) -> Bool / e)         : Stream[t, e]
pub fn take[t, e](s: Stream[t, e], n: Int)                       : Stream[t, e]
pub fn take_while[t, e](s: Stream[t, e], p: (t) -> Bool / e)     : Stream[t, e]

# -- sinks (force the pipeline) --------------------------------------
pub fn fold[t, a, e](s: Stream[t, e], init: a, f: (a, t) -> a / e) : a / e
pub fn each[t, e](s: Stream[t, e], f: (t) -> Unit / e)             : Unit / e
pub fn count[t, e](s: Stream[t, e])                                : Int / e
pub fn to_list[t, e](s: Stream[t, e])                              : [t] / e   # materialises!
pub fn write_lines[e](s: Stream[String, e], path: String) : Unit / File + ReadFault + e
```

The recoverable fault travels on a dedicated resumable effect:

```kaikai
pub effect ReadFault {
  bad_chunk(msg: String) : Unit      # resumable → resume = skip the bad chunk
  open_fault(msg: String) : Nothing  # Nothing → abort-only (no resume)
}
```

`bad_chunk` returns `Unit`, so a handler can resume to skip and continue
or abandon to abort. `open_fault` returns `Nothing`: a stream whose
source cannot open has nothing to resume into, so it is abort-only by
construction. One effect for both faults keeps the row to `File +
ReadFault` (the open-design point in #801 — resolved as a second
non-resumable op on the same effect, not a separate `Fail`).

Combinator bodies are one-liners — `map` is
`(yield) => s((x) => yield(f(x)))` — which keeps the module small and
A-grade by construction.

`write_lines` is the Elixir lesson: **the writer is a sink stage**, not a
loop the user writes. It opens, drains the stream through a buffered
writer, flushes and closes on every exit path. No trailing `run()` —
sinks force the pipeline, so there is nothing left to kick.

### Errors: clean elements, per-element recovery (the flagship)

The element type is `String` — never `Result[String, String]`. Faults
travel in the row as `Fail`. The producer performs `fail` *at the point
of the fault*, and the handler's typed resume decides the policy:

- **abort** (default): no handler between the pipe and `main` → the
  `Fail` default conventions apply; the producer's cleanup still runs on
  unwind.
- **skip and continue**: a handler that resumes. The producer is written
  so that resuming a chunk-read fault means "drop that chunk, keep
  going".
- **collect and continue**: resume *and* accumulate the message — see
  gallery (e).

Rust charges every stage for fallibility; Elixir gives up recovery;
Go's `iter.Seq` never solved errors at all (the community pattern is a
side-channel `err` pointer). Handler-decided per-element policy with a
clean element type is the thing only algebraic effects buy. This is the
design's "novel where it pays off" claim, and spike S2 validates it.

### Naming and capability

- **No `File` split.** `FileRead`/`FileWrite` effects (and `Reader` /
  `Writer` aliases over them) were considered: both aliases would expand
  to the same set today (`File` is bidirectional), so the names would
  promise a capability split the typer doesn't enforce. The Console
  split (#360) is justified by three physically distinct fds; `File` has
  no analogous seam. Revisit only if a real least-authority demand
  appears.
- **No effect alias.** With `Mutable` out of the row, signatures are
  `/ File + ReadFault` — short enough plain. (`Lines` as an alias name
  was vetoed; with this design the alias itself is unnecessary.)
- Module: `stdlib/stream.kai`, imported as `stream`. `read_lines` /
  `write_lines` live there too — the file-backed source/sink are part of
  the streaming story, not of `fs`.

## Ergonomics gallery

The brief's yardstick. Each program is complete and runs in constant
memory. All forms below are verified against `stdlib/stream.kai` — the
fixtures in `examples/effects/stream_*.kai` exercise (a), (b), (e), (f);
`demos/wc.kai` exercises the fold sink. `read_lines` carries `File +
ReadFault`, so a *forcing* consumer (a sink at `main`, or a function that
returns a concrete value) must `handle ... with ReadFault` to pick a
policy; a function that just *builds* a longer pipeline propagates the
row unchanged.

**(a) Count the ERROR lines of a log of any size**

```kaikai
import stream

fn count_errors(path: String) : Int / File + ReadFault =
  stream.read_lines(path)
    |? (l) => string_contains(l, "ERROR")
    |> stream.count
```

**(b) Transform a file into another, streaming**

```kaikai
fn shout(input: String, output: String) : Unit / File + ReadFault =
  stream.read_lines(input)
    | (l) => string.to_upper(l)
    |> stream.write_lines(output)
```

One expression, no bracket, no `run()`, no `?`-per-line. (Go: ~15 lines.
Elixir: same shape plus `Stream.run`. Rust: 6 lines and a `Result`-typed
element.)

**(c) First 10 lines matching a predicate — stops reading at the 10th**

```kaikai
fn first_matches(path: String, needle: String) : [String] / File + ReadFault =
  stream.read_lines(path)
    |? (l) => string_contains(l, needle)
    |> stream.take(10)
    |> stream.to_list
```

**(d) Header: every line until the first blank one**

```kaikai
fn header(path: String) : [String] / File + ReadFault =
  stream.read_lines(path)
    |> stream.take_while((l) => l != "")
    |> stream.to_list
```

**(e) Skip unreadable chunks instead of aborting — handler-decided**

```kaikai
fn count_resilient(path: String) : Int / File =
  handle {
    stream.read_lines(path) |> stream.count
  } with ReadFault {
    bad_chunk(msg, resume) -> resume(())   # resuming = drop the bad chunk, go on
    open_fault(msg, r)     -> 0            # open failure: abort-only (Nothing, no resume)
  }
```

Same pipeline, different policy, zero changes to the stages. Drop the
`resume` call in `bad_chunk` and that policy becomes abort. No referent
can express this without changing the element type everywhere.

**(f) Mock the disk in a test — no fixture files**

```kaikai
test "count_errors over a mocked file" {
  let fed = array_make(1, false)
  let n = handle {
    handle { count_errors("fake.log") }
    with ReadFault {
      bad_chunk(m, resume)  -> resume(())
      open_fault(m, r)      -> ()
    }
  } with File {
    open_read(p, resume)        -> resume(Ok(FileHandle { fd: 1 }))
    read_chunk(h, max, resume)  ->
      if fed[0] { resume(Ok("")) }                       # EOF
      else { fed[0] := true; resume(Ok("ok\nERROR a\nok\nERROR b\n")) }
    close_file(h, resume)       -> resume(())
    open_write(p, resume)       -> resume(Err("no write"))
    write_chunk(h, d, resume)   -> resume(Err("no write"))
  }
  assert n == 2
}
```

Because production and consumption run inside one expression, the
`handle` is guaranteed to cover every `read_chunk` — the pull design
could silently escape the mock. (A mocked `FileHandle` is built with
`FileHandle { fd: N }`, not a bare `Int`.)

**(g) A grep-ish main, end to end**

```kaikai
fn main() : Unit / Console + Env + File = {
  let needle = list.nth(args(), 0) |> option.unwrap_or("ERROR")
  handle {
    stream.read_lines("app.log")
      |? (l) => string_contains(l, needle)
      |> stream.each((l) => Stdout.print(l))
  } with ReadFault {
    bad_chunk(m, resume) -> resume(())
    open_fault(m, r)     -> ()
  }
}
```

**(h) wc — lines/words/chars in one fold (the `demos/wc.kai` demo)**

```kaikai
fn wc(path: String) : Counts / File + ReadFault =
  stream.read_lines(path)
    |> stream.fold(Counts { lines: 0, words: 0, chars: 0 }) { acc, l ->
      Counts {
        lines: acc.lines + 1,
        words: acc.words + words_in(l),
        chars: acc.chars + string.length(l) + 1
      }
    }
```

One `fold` over the line stream computes all three counters in a single
constant-memory pass — a 10 GB log costs the same RAM as a 10 KB one.

## Spike gate — RESULTS (2026-06-09, `/tmp/stream-spike/`)

Both spikes ran against `bin/kai` at main `7ba98960`. **Verdict: push
carrier confirmed.** Findings, in decreasing weight:

- **S1 — early stop: PASS.** A counting producer (10 elements) under
  `take(3)` + `count` produced exactly 0,1,2 and ran its close exactly
  once. The `yield`-returns-`Bool` protocol composes through stacked
  combinators with no runtime support. `var` state works through the
  closures, and the row-polymorphic generic combinators
  (`take[t, e]`, `count[t, e]`) typecheck as written in §Surface.
- **S2 — per-element resume (skip policy): PASS.** A producer performing
  a resumable op mid-loop, resumed by a consumer-side `handle`, skips
  the faulted element and continues; cleanup runs; count is correct.
  Gallery (e) is real. **Op shape pinned:** the recoverable fault must
  be a dedicated effect with a resumable return type
  (`effect ReadFault { bad_chunk(msg: String) : Unit }`) — stdlib
  `Fail.fail` returns `Nothing` and is non-resumable by construction,
  so `read_lines` carries `File + ReadFault`, not `File + Fail`.
  (§Surface and the gallery need this row update.)
- **S2 — abort policy: cleanup does NOT run.** Abandoning the
  continuation (handler that doesn't resume) skips the producer's
  close — the fd leaks on abort. There is no `finally` in the language,
  and producer-side interception would break the skip policy (the
  producer's own handler would shadow the consumer's). **Recorded as a
  known limitation**, same honesty class as #771's cancel-safety note;
  closed later by the same cancel-aware-bracket work. In practice abort
  usually precedes process exit, bounding the damage.
- **Found dependency: #772 is a prerequisite for ergonomics.**
  `Stream[Int, Stdout + ReadFault]` does not parse — compound rows are
  rejected as alias type arguments (issue #772, previously labeled
  post-1.0), and a named row alias does not expand in that position
  either. Workaround (spike-proven): spell the function type inline.
  Every user-facing signature in the gallery needs #772 fixed to be
  writable as shown; #772 should be promoted into this work's critical
  path.
- Minor quirk found en route: `while { cond } { body }` fails row
  unification when condition and body use `var`s of different `State`
  types; recursion sidesteps it (and is house style anyway). Worth its
  own issue.

## Out of scope (v1)

- `zip` / consumer-driven interleaving — push can't; invert with a
  generator-effect handler when a real consumer appears.
- Byte/chunk-level public streams (`read_chunks`) — trivial to add over
  the same substrate when demanded; lines are the proven use case.
- A buffered `LineWriter` *handle* (incremental writes outside a
  pipeline) — `write_lines` covers the streaming case; a follow-up if
  demand appears.
- Network sources, `Stream` over actors/channels — post-1.0, same shape
  expected (`Stream[t, NetTcp + Fail]`).
- Parallel/fused execution — Perceus + monomorphisation already erase
  most intermediate structure; measure before designing more.

## Relationship to existing work

- **#771 Phase 1 is unchanged** and is the substrate: `open_read` /
  `read_chunk` / `close_file` ops on `File` (mockability is what makes
  gallery (f) work). The *write* side needs the symmetric primitives
  (`open_write` / `write_chunk`) added to that lane's scope.
- **#771 Phase 2** (`with_lines` / `fold_lines` / `each_line` bracket
  surface) is **superseded** by this design — the push carrier plus
  sinks covers all of it with one fewer concept. The issue text needs a
  re-scope before the lane launches.
- **ahu**: deletes its `stream.kai` and re-exports the stdlib carrier
  (avoids `HLAmbiguous` on `map` for head type `Stream`); its
  `from_lines` becomes `stream.read_lines`. Pull-shaped `Stream.resource`
  use cases map onto producers with internal state.
