# Lane experience — stream-dogfood

Goal: write one non-trivial demo that USES `stdlib/stream.kai` end-to-end
(`read_lines` + the dispatching pipes + `ReadFault` recovery + a sink) and
report the API gaps the use surfaced. The demo is the vehicle; the gap
findings below are the deliverable. This lane CONSUMES the API — it does
not edit `stdlib/stream.kai` or the collection sources. The findings carry
minimal repros and a classification so the integrator can file issues.

All findings below were **re-verified against `main` after rebasing onto
`#874` (`feat(typer): thread effect rows through nominal carrier slots`,
Closes #773)**, which landed mid-lane and closed two of the gaps the first
pass found. The "Closed by #874" section records those so the integrator
does not file them; the live gaps are what remains.

## Scope as planned vs as shipped

Shipped: `demos/log_stats/` — SEEDS a sample log with the `write_lines`
sink, then reads it back: keeps the WARN/ERROR/FATAL lines with the `|?`
filter pipe and folds severities into a count `Map`; uses the `||` flat-map
pipe to explode problem lines into words and counts the long tokens; and
recovers from a synthetic `bad_chunk` via a resuming `handle`. `km score`
A++ (98.7), cogcom avg 1.3 / max 4, 81 LOC. Verified `OK` against the demos
Makefile, and native==C parity verified locally (`KAI_BACKEND=c` vs
`native`, identical output, both exit 0).

The demo uses the dispatching pipes over the `Stream` head genuinely:
`read_lines(path) |? is_problem || words_of |? long` is one pipe
expression with three `Stream`-dispatched pipes. This works — once the
edition is pinned (gap #1) and `File` is handled in an enclosing scope
(gap #5). The `ReadFault` recovery itself is gap-free.

The demo seeds its input through `write_lines` to an absolute `/tmp` path
rather than shipping a disk fixture read by a cwd-relative path. The first
version shipped `fixtures/app.log` and read `log_stats/fixtures/app.log`,
which worked under the demos Makefile (run from `demos/`) but FAILED the
tier1-native parity ratchet: that harness runs binaries from the repo root,
the relative path did not resolve, and the resulting `open_fault` abort
diverged between backends (gap #2). Self-seeding makes the demo
cwd-independent and exercises one more sink (`write_lines`, the twin of
`read_lines`), so it passes both backends in any directory.

One harness change was required and made: `demos/Makefile` now passes
`--edition hanga-roa` (see gap #1). It does not touch `stdlib/` or the
collections.

---

## Closed by #874 (do NOT file — verified fixed on the rebased tree)

### `flat_map` row-equality forcing a pure sub-stream to lie about its row — FIXED

First pass: `flat_map`'s sub-stream had to be annotated with the *parent's*
full row (`Stream[String, File + ReadFault]`) even when produced by the
pure `from_list`, or unification failed with a `__row_in_type_position__`
placeholder leaking into the diagnostic. Root cause was the effect row in a
nominal `TyCon` slot resolving to a phantom type var.

Post-#874: `words_of` now declares its honest row `Stream[String,
ReadFault]` and `flat_map` unifies it into the `File + ReadFault` parent
correctly. The demo's `words_of` carries the honest `ReadFault`-only row.
Verified compiling.

### `read_lines` under an explicit `ReadFault` handle SEGFAULTED — FIXED (now a clean type error)

First pass: `handle { read_lines(path).count() } with ReadFault { ... }`
COMPILED and then SEGFAULTED (exit 139). The `Stream` carrier hid the
`File` row from the handle's coverage check, so a missing `File` handler
became a runtime crash instead of a type error — a soundness hole.

Post-#874: the same code now fails at compile time with `effect not
handled: File` — the row threads through the carrier slot and the typer
catches the unhandled `File`. The segfault is gone; what remains is the
ergonomic placement question, captured as live gap #5 below.

---

## Live gaps

Severity order: bugs first, then papercuts.

### #1 — Convention pipe-dispatch is edition-gated, and both kaic2's default and the demos harness used the pre-gate edition — PAPERCUT (tooling; fixed in this lane's Makefile)

First pass reported "pipe dispatch does not resolve over an imported
`Stream` head" — `from_list([...]) |? p` failed with `no module declaring
type Stream is in scope`. That diagnosis was **incomplete**. The dispatch
is gated on **edition >= hanga-roa** (`build_head_owner_map` walks user
`pub type` heads only on hanga-roa; tongariki keeps the hardcoded `List`
only). Two things conspired:

- **kaic2 defaults to `tongariki`** (the oldest edition) for flagless
  invocations (`stage2/compiler/driver.kai` ~1310: "as the oldest edition
  (tongariki) so flagless invocations ... default ... defensively").
- **`demos/Makefile` passed no `--edition`**, so every demo compiled in
  tongariki, where convention pipe-dispatch over a stdlib head does not
  exist.

The repo's `EDITION` file is `hanga-roa`, and the stage2 test harness runs
the stream fixtures with `--edition hanga-roa --path ../stdlib`
(`stage2/Makefile` ~2728). With that flag, `from_list([...]) |? p` and the
official `row_carrier_pipe` fixture both compile and run. Repro:

```sh
# fails (default tongariki):
kaic2 --path stdlib f.kai            # error: no module declaring type `Stream`
# works (repo edition):
kaic2 --edition hanga-roa --path stdlib f.kai
```

```kaikai
# f.kai
import stream
fn big(x: Int) : Bool = x > 2
fn main() : Unit / Stdout = {
  let n = handle { (from_list([1,2,3,4]) |? big) |> (s => s.count()) }
            with ReadFault { bad_chunk(m, resume) -> resume(()) open_fault(m, _r) -> 0 }
  print("n=#{n}")          # → n=2 under --edition hanga-roa
}
```

**Fixed in this lane:** `demos/Makefile` now pins `EDITION := --edition
hanga-roa` in its kaic2 invocation. All 36 demos still pass (33 OK + 3
no-golden, 0 fail). Without it, no demo can exercise the convention pipes
over a stdlib head — which is most of what the pipe feature is for.

Two judgement calls for the integrator: (a) should kaic2's flagless default
track the repo `EDITION` rather than the oldest edition, so a plain
`kaic2 f.kai` in a hanga-roa repo sees hanga-roa surface? The "defensive
oldest" default means the surface a user gets silently depends on a flag
they did not pass, and the diagnostic (`no module declaring type Stream`)
does not mention the edition. (b) The `no module declaring type T`
diagnostic could hint "this type's pipe dispatch requires edition
hanga-roa" when the type IS a known head in a later edition.

Classification: **papercut** (tooling / edition-default + an
edition-blind diagnostic). No stdlib/compiler-logic change; the dispatch
itself is correct on the right edition. The Makefile fix is in this lane.

### #2 — `open_fault` abort + a `||` flat-map stage diverges between backends (C crashes/hangs, native exits 0) — BUG (backend; surfaced by the demos parity ratchet)

Found when tier1-native's parity ratchet ran the demo with its fixture
absent (the harness runs binaries from the repo root, not `demos/`, so an
early `cwd`-relative-path read fails). When `read_lines` cannot open the
file, `open_fault` aborts the producer. If the pipeline that aborts has a
`||` flat-map stage, the C backend crashes/hangs in the teardown of that
abort (exit 124 timeout in CI, 139 segfault locally), while native exits 0.

Minimal repro (run from a directory where the path does not resolve):

```kaikai
import stream
fn is_p(l: String) : Bool = string.length(l) > 0
fn words_of(line: String) : Stream[String, ReadFault] = from_list(string.split(line, " "))
fn long(w: String) : Bool = string.length(w) > 4
fn noisy(path: String) : Int / File + ReadFault =
  (read_lines(path) |? is_p || words_of |? long) |> (s => s.count())
fn main() : Unit / Stdout + File = {
  let w = handle { noisy("NOPE/missing.log") } with ReadFault {
    bad_chunk(m, resume) -> resume(())
    open_fault(m, _resume) -> { print("open fail: #{m}"); 0 }
  }
  print("count: #{w}")
}
# C backend:    prints "open fail: ...", "count: 0", then SEGFAULT (139)
# native:       prints the same, exits 0
```

The `count_by_level` stage (no flat-map, just `|? ... fold`) handles the
same open-fail cleanly on both backends — it is the `||` flat-map stage's
abort teardown that diverges. So the trigger is specifically
`open_fault`-abort through a `flat_map`-bearing stream on the C backend.

**Why the demo no longer trips it:** the demo now SEEDS its input with
`write_lines` to an absolute `/tmp` path before reading, so the file is
always present and the abort path never runs under the parity harness —
native and C agree (verified locally: `KAI_BACKEND=c` vs
`KAI_BACKEND=native`, identical output, both exit 0). The bug is real and
independent of the demo; the demo is just no longer the thing that surfaces
it in CI. A dedicated negative fixture (missing-file + flat-map) would pin
it for the integrator.

Classification: **bug** (backend — abort-path teardown of a `flat_map`
stream on C-direct). Higher priority than the papercuts: it is a
correctness divergence between the two backends.

### #3 — `if x != None { body }` mis-parses the body as a record literal — BUG (parser; misleading diagnostic)

```kaikai
fn classify(x: Option[Int]) : Int =
  if x != None { 1 }
  else { 0 }
```

```
error: expected `{` after `if` condition
  --> ...:3:3        (points at the `else`)
```

The `{` IS present. The parser consumes `None { 1 }` as a record literal
(`None` is a constructor, `{` follows), so the `if` body is swallowed and
the parser hits `else` where it wanted the body. The diagnostic points away
from the cause. Bisection:

- `if (x != None) { 1 } else { 0 }` — parens around the cond → compiles.
- `if string.length(line) != 0 { 1 } else { 0 }` — cond ends in a literal
  → compiles.
- `if x != None { 1 } else { 0 }` — cond ends in the `None` constructor,
  bare body → fails.

Any `if`/`else if` whose condition ends in a bare nullary constructor and
is immediately followed by `{` trips it. This is the documented "`Ctor {`
after `==` = record literal" trap, but it bites in ordinary
`if x != None { ... }` code and the message hides it. Still live post-#874.

Classification: **bug** (parser ambiguity + misleading diagnostic). The
demo avoids it with a `has(...)` helper that `match`es the `Option`.

### #4 — Handler clause whose `resume` binder is not named `resume` emits undeclared-identifier C — BUG (codegen)

```kaikai
import stream
fn main() : Unit / Stdout = {
  let n = handle {
    from_list([1,2,3]) |> (s => s.count())
  } with ReadFault {
    bad_chunk(m, r)  -> r(())
    open_fault(m, r) -> 0
  }
  print("#{n}")
}
```

kaic2 accepts it; the emitted C does not compile:

```
error: use of undeclared identifier 'kai_r'; did you mean 'kai_m'?
  return kai_apply(kai_r, 1, (KaiValue *[]){kai_unit()});
```

Renaming the continuation binder `r` → `resume` makes it compile. The clause
codegen is name-sensitive to the continuation binder: any name other than
`resume` is referenced in the body but never emitted as a local. A user who
names the continuation anything else gets broken C, not a kaikai error.
Still live post-#874.

Classification: **bug** (codegen — clause continuation binder name-bound to
the literal `resume`). The demo uses `resume` throughout.

### #5 — A `File`-performing stream under a user `ReadFault` handle needs `File` handled in an *enclosing* scope; not obvious, undocumented — PAPERCUT (post-#874 ergonomics)

The natural shape still does not work, but now it's a clean type error
(post-#874), not a segfault:

```kaikai
fn main() : Unit / Stdout = {                       # no File in row
  let n = handle { read_lines(path).count() }
            with ReadFault { ... }
  ...
}
# error: effect not handled: File
```

`read_lines`'s row is `File + ReadFault`. A `handle ... with ReadFault`
discharges only `ReadFault`; `File` must be handled by an enclosing scope.
The runtime auto-installs the `File` default at `main`, so the fix is to put
`File` in `main`'s row and keep the analysis helper at `File + ReadFault`:

```kaikai
fn count_by_level(path: String) : Map[String, Int] / File + ReadFault = ...
fn main() : Unit / Stdout + File = {              # File in main's row
  let counts = handle { count_by_level(path) } with ReadFault { ... }
  ...
}
# runs clean
```

This is correct effect discipline, but undiscoverable: nothing in
`read_lines`'s `#[doc]` or `kai info effects` tells a user that a
`ReadFault` handle around `read_lines` requires `File` to be handled above
it. The demo encodes the pattern; a doc note would save the next user the
isolation work. (Pre-#874 this was a segfault — that part is fixed.)

Classification: **papercut** (effect-scope ergonomics + doc gap). No code
change needed beyond docs.

### #6 — `open_fault` handler clause requires a `resume` it can never use — PAPERCUT (typer)

`ReadFault.open_fault(msg) : Nothing` is abort-only by construction (the
`#[doc]`: "a stream whose source cannot be opened has nothing to resume
into"). The clause must still take a trailing `resume`:

```kaikai
open_fault(msg) -> map.empty()
# error: handler clause `open_fault` has wrong arity ...
#   the clause must take those plus a trailing `resume`, for a total of 2
```

The compiling form is `open_fault(msg, _resume) -> ...` with a dead
`_resume`. An op whose return type is `Nothing` can never be resumed, so
requiring (and forcing the user to name-and-ignore) the continuation is
ceremony. The arity check could special-case `Nothing`-returning ops. Still
live post-#874.

Classification: **papercut** (handler arity ergonomics for non-resumable
ops).

### #7 — Multiline UFCS chains do not parse — PAPERCUT (parser)

Single-line UFCS works; breaking it across lines does not, in either style:

```kaikai
let n = from_list([1,2,3,4])
  .filter(big)          # error: `.filter` is a field access on a non-record value
  .count()
```

```kaikai
let n = from_list([1,2,3,4]).
  filter(big).          # error: expected field name after `.`
  count()
```

Only the single-line `from_list([1,2,3,4]).filter(big).count()` parses. With
the convention pipes available (gap #1 fixed via edition), the demo uses
pipes and does not need long UFCS chains — but UFCS remains the fallback
when a stage signature is not pipe-canonical, and it cannot be wrapped for
readability. Still live post-#874.

Classification: **papercut** (parser — UFCS continuation across newlines).

---

## Doc gaps

### D1 — `read_lines` `#[doc]` does not mention the `File`-handler-placement requirement

Per gap #5, `handle { read_lines... } with ReadFault` requires `File` to be
handled in an enclosing scope, or the typer rejects with `effect not
handled: File`. The `read_lines` `#[doc]` (or `kai info effects`) should
note that a `ReadFault` handle must sit inside a scope where `File` is
already handled (e.g. `File` in `main`'s row). Owned by the `stream.kai`
lane/integrator.

### D2 — nothing surfaces the edition dependency of pipe dispatch

Per gap #1, the convention pipes over a stdlib head require edition
hanga-roa, and a flagless kaic2 silently uses tongariki where they fail with
an edition-blind message. `docs/protocols.md` / `kai info pipes` could state
that convention dispatch over non-`List` heads is hanga-roa+.

---

## What worked smoothly

- **The dispatching pipes over `Stream`** (on hanga-roa) — `read_lines(path)
  |? is_problem || words_of |? long` dispatches `|?` (filter) and `||`
  (flat_map) on the `Stream` head in one expression and runs correctly
  (`words=29`). This is the headline capability and it works.
- **`ReadFault` recovery semantics** — resuming `bad_chunk` to skip and
  continue works exactly as the `#[doc]` promises (the `flaky_source` pass:
  skip the corrupt frame, surviving elements reach the sink,
  `survived=2`). The differentiating feature, sound.
- **`read_lines` itself** — opens, splits on `\n`, strips the newline,
  yields the final unterminated line, counts in constant memory
  (`lines=12` on a 12-line file). No leak or miscount.
- **`Map` interplay** — `map.update(m, key, default, f)` as a count-bump and
  `map.fold` for ordered output composed with the stream fold frictionlessly;
  in-order key emission gave a stable golden.
- **`|>` apply pipe** — `s |> (x => f(x))` reads well for the final sink
  application and is edition-independent.
- **#874's carrier-row threading** — the row now survives the nominal
  `Stream[t, e]` slot, so effectful producers are checked (the old phantom
  type var is gone). Closed gaps #5/#4-segfault from the first pass.

## Fixtures added

- `demos/log_stats/main.kai` — the demo program (A++ 98.7 / cogcom max 4),
  self-seeding (`write_lines` → `read_lines`), cwd-independent.
- `demos/log_stats/main.out.expected` — golden stdout; verified `OK` by the
  demos Makefile and native==C by the parity harness.
- `demos/Makefile` — `EDITION := --edition hanga-roa` pinned (gap #1).

No on-disk fixture: the demo writes its own input, so it needs no
`fixtures/` dir and runs from any cwd (the first version's `fixtures/app.log`
was removed when self-seeding replaced it — see gap #2).

## Follow-ups for the integrator (issues to file)

1. **#2 `open_fault` abort + `||` flat-map teardown diverges C-vs-native** —
   `runtime`/codegen + parity. The highest-severity bug: a correctness
   divergence between backends. Repro in the gap. A dedicated negative
   fixture (missing-file + flat-map) would pin it.
2. **#3 `if x != None { ... }` record-literal mis-parse + misleading
   diagnostic** — `compiler` (parser). Bug.
3. **#4 name-sensitive `resume` binder in clause codegen** — codegen/
   `runtime`. Repro: resume named `r` → undeclared `kai_r` in emitted C.
   Bug.
4. **#1 edition-default / demos-harness edition** — `compiler` (driver) +
   tooling. Decide whether flagless kaic2 should track the repo `EDITION`,
   and make the `no module declaring type T` diagnostic edition-aware. The
   demos Makefile fix is already in this lane. Papercut.
5. **#5 `File`-handler placement under `ReadFault` handle** — doc (D1) +
   maybe a typer hint. Papercut.
6. **#6 `open_fault` arity requires unusable `resume`** — `typer`. Papercut.
7. **#7 multiline UFCS does not parse** — `compiler` (parser/lexer).
   Papercut.
8. **D1/D2 doc notes** — `read_lines` File-placement; pipe-dispatch edition
   dependency. Owned by the `stream.kai` lane / integrator.

## Real cost vs estimate

The lane's value was the gap isolation and the re-verification discipline.
The first pass found seven gaps; rebasing onto #874 (which landed mid-lane)
closed two (flat_map row-equality; the `read_lines`-under-handle segfault →
now a clean type error) and reframed the headline one (#1 was not "pipe
dispatch broken" but an edition gate the demos harness was on the wrong side
of — fixed in the Makefile). The tier1-native parity ratchet then surfaced
gap #2 — a real C-vs-native abort-teardown divergence the demo tripped only
because its first version read a cwd-relative fixture the harness does not
provide. Reworking the demo to self-seed via `write_lines` both removed the
fragility and exercised one more sink; native==C parity is now verified
locally. The demo body is small; the cost was the isolation (the open_fault
abort bisected through with-flat-map vs without; the post-#874 segfault
bisected through pure-vs-`File` source; #1 bisected through
constructor-vs-function head before the edition flag turned up in the
harness Makefile) and the honest re-run against the moved tree.
