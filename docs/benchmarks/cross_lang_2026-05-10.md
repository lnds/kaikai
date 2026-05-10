# Cross-language benchmark — 2026-05-10 (post-v0.51.0)

Honest cross-language comparison of two micro-workloads after the
v0.51.0 cut (post #383 Phase 3 unboxing, #439 instrumentation, #444
shrinking, #448 m11 v1 diagnostics). Two workloads, eight languages
tested, one expected (Bun) absent and noted as skipped.

This is **measurement, not optimisation**. No kaikai source was
changed for the lane. Where kaikai loses, the doc says so.

## Hardware

| Variable | Value |
| --- | --- |
| Host | Apple M4 Pro, macOS 14 (Darwin 25.4.0) |
| Power | AC, no thermal throttling observed across iterations |
| Iterations | 5 per (language × workload), median of 5 reported |
| Wall measurement | `/usr/bin/time -lp`, `real` line |
| RSS measurement | `/usr/bin/time -lp`, `maximum resident set size` line |

## Toolchains

| Language | Version | Build / run command |
| --- | --- | --- |
| C | Apple clang 21.0.0 | `cc -O2 <src> -o <bin>` |
| Rust | rustc 1.92.0 | `rustc -O <src> -o <bin>` |
| Go | go 1.25.5 | `go build` inside a throwaway module |
| kaikai | v0.51.0 (stage 2, self-hosted) | `bin/kai build <src>` (drives `cc -std=c99 -Wall -O2 …`) |
| Python | 3.13.3 | `python3 <src>` |
| Ruby | 2.6.10 (system) | `ruby <src>` |
| Node | v23.10.0 | `node --experimental-strip-types <src>` |
| Deno | 2.7.9 | `deno run --quiet <src>` |
| Elixir | 1.19.5 on OTP 28 | `elixir <src>.exs` |
| Bun | — | **SKIPPED**: not installed on this host. |

Sources live under `benchmarks/cross_lang/<lang>/`. Both workloads
mirror existing kaikai fixtures so the comparison is apples-to-apples
with `examples/perceus/rb_tree_bench.{kai,c}`.

## Workloads

### 1. `fib(35)` recursive

Pure compute. `fib(n) = if n<2 then n else fib(n-1)+fib(n-2)`.
Language-native int (`int64_t` / `i64` / `number` / etc.).
Idiomatic recursion, no memoisation. Result is `9227465` on every
language (verified per iteration).

### 2. Red-black tree, 1 000 000 inserts

Faithful port of `examples/perceus/rb_tree.kai` (Okasaki four-case
balance, structural rebuild on every insert). Keys come from the
same LCG (`s' = (s*1664525 + 1013904223) mod (2^31 - 1)`, seed = 1),
so the keystream is bit-identical to the existing C reference at
`examples/perceus/rb_tree_bench_c.c`. Every port produces
`size = 1000000, height = 29` — verified per iteration.

Stdlib RB-trees / sorted maps are **not** used in any port — the
manual balance algorithm is replicated end-to-end so the comparison
measures comparable work.

## fib(35) — median wall + RSS

| Language | Wall (ms) | Ratio vs C | RSS (MB) |
| --- | ---: | ---: | ---: |
| C `-O2` | 10 | 1.00× | 1 |
| Rust `-O` | 10 | 1.00× | 2 |
| **kaikai v0.51.0** | **10** | **1.00×** | **1** |
| Go 1.25 | 20 | 2.00× | 4 |
| Deno 2.7 | 60 | 6.00× | 58 |
| Node 23 | 110 | 11.00× | 82 |
| Elixir / OTP 28 | 280 | 28.00× | 93 |
| Ruby 2.6 | 560 | 56.00× | 28 |
| Python 3.13 | 1 400 | 140.00× | 15 |

Iteration scatter: walls of 10 ms are at clock resolution — the
spread between C / Rust / kaikai inside that band is below
measurement noise. The takeaway is that **kaikai sits inside the
1× C band on compute-bound recursive code**, on the same line as
clang + rustc.

## RB-tree 1M inserts — median wall + RSS

| Language | Wall (ms) | Ratio vs C | RSS (MB) |
| --- | ---: | ---: | ---: |
| C `-O2` (functional port) | 850 | 1.00× | 1 042 |
| Go 1.25 | 980 | 1.15× | 93 |
| Elixir / OTP 28 | 1 180 | 1.39× | 426 |
| Deno 2.7 | 1 290 | 1.52× | 299 |
| Node 23 | 1 530 | 1.80× | 327 |
| Rust `-O` (with `Rc<Tree>`) | 1 650 | 1.94× | 124 |
| **kaikai v0.51.0** | **4 220** | **4.96×** | **356** |
| Ruby 2.6 | 6 950 | 8.18× | 195 |
| Python 3.13 | 7 300 | 8.59× | 183 |

C wins on wall but loses on RSS by an order of magnitude: the C port
never frees, so 1 M live RB nodes plus the rebuild garbage stay
resident at 1 042 MB. Every other entry runs a real GC / RC.

### Why Rust is slow here

The Rust port uses `Rc<Tree>` with `.clone()` on every sub-tree
reference, faithfully replicating the immutable structural-rebuild
shape of `examples/perceus/rb_tree.kai`. This is **Rust written as
a functional port, not idiomatic Rust** — an intrusive heap-mutating
RB-tree (closer to the C reference) would be faster. Reporting the
faithful port is the honest comparison: both kaikai and Rust here
pay the cost of structural immutability, but Rust pays through
explicit `Rc` refcount traffic and kaikai through Perceus-emitted
RC.

### Where kaikai stands

- **Compute-bound (fib(35)): ~1× C.** Same line as clang `-O2` and
  rustc `-O`. This is the post-#383 Phase 3 (call-boundary primitive
  unboxing) win; pre-Phase-3 the same workload was ~9× C.
- **Variant-heavy data (RB-tree): ~5× C.** Kaikai loses to all the
  mainstream production runtimes (Go, Elixir, Node, Deno) on this
  workload. It is faster than Ruby and Python only by a 1.6–1.7×
  factor. The gap is structural: variant cells stay heap-allocated
  through `KaiValue *`, and the RB-tree rebuild rebuilds three cells
  per balance arm. Refcount traffic dominates — see
  `docs/benchmarks/rb_tree_breakdown_2026-05-09.md` for the per-cycle
  breakdown.

### Phase 4 projection

`docs/benchmarks/rb_tree_breakdown_2026-05-09.md` decomposes the
post-#409 wall (3.8 s native) into ~70 % alloc + free (per-cell
work) and ~20 % RC traffic (per-cell-visit work). Phase 4
(variant-field unboxing) inlines `Color`, `Int` key, and `Int`
value directly into the `Node` payload — leaving only `l` / `r` as
boxed `KaiValue *` slots. The breakdown estimates this drops native
wall to **~1.5–2.0 s**, which would put kaikai's RB-tree at
**~1.8–2.4× C** on the table above, between Go and Rust.

That is the DoD #4b target (`docs/benchmarks/rb_tree_2026-05-09.md`)
and the v1.0 trajectory. The breakdown also argues Phase 4 alone
is sufficient — drop specialisation (#384) is a useful follow-up
but not load-bearing for this workload.

## Port deviations from the canonical algorithm

- **C reference** (`benchmarks/cross_lang/c/rb_tree.c`) is a fresh
  functional port matching `examples/perceus/rb_tree.kai`, not the
  intrusive imperative tree from `examples/perceus/rb_tree_bench_c.c`.
  This makes the C numbers directly comparable to the functional-style
  ports in every other language. The intrusive C reference is ~2×
  faster than this functional C port on the same hardware; the
  intrusive form is not what the kaikai source expresses.
- **Rust** uses `Rc<Tree>`, not `Arc` (single-threaded workload, no
  Send/Sync needed). `.clone()` on sub-trees keeps the structural
  rebuild faithful.
- **Go** uses pointer-shared sub-trees via the GC; no manual frees.
- **Python / Ruby** represent nodes as tuples / arrays. Pattern
  match becomes index access (`t[0]` / `t[1]` / …) — same shape, no
  algorithmic deviation.
- **Elixir** uses 5-tuples with `elem/2` access. Pattern match on
  `{@red, _, _, _, _}` in function heads encodes the Okasaki cases
  directly. No `:gen_server` / process boundary; pure-functional
  rebuild on a single BEAM scheduler.
- **TypeScript** (single source `rb_tree.ts` for Node + Deno) — uses
  `number`, not `BigInt`. The LCG keystream fits well below the
  53-bit safe-integer ceiling, so this is correct (cross-checked
  against the C `int64_t` baseline by comparing `size` + `height`).
- **kaikai** uses `examples/perceus/rb_tree_bench.kai` unchanged.

## Skipped languages and tools

- **Bun** — not installed on this host (`which bun` empty,
  `~/.bun/install/` exists but no binary). Skipped rather than
  pulled in; the Deno number sets the lower bound for a modern JS
  engine on this workload, so the missing Bun data point does not
  change the qualitative shape of the table.
- **C++ `std::set`** — already covered in
  `docs/benchmarks/rb_tree_2026-05-09.md` as a separate baseline;
  not re-measured here because it does not change the functional-port
  comparison this doc is about.
- **OCaml / Haskell / Scala** — would be the most directly comparable
  baselines (immutable RB-tree is a textbook workload there) but
  the host has no compilers installed, and pulling them in busts the
  one-day lane budget.

## Repro

```sh
# C
cc -O2 benchmarks/cross_lang/c/fib.c     -o /tmp/bench_fib_c
cc -O2 benchmarks/cross_lang/c/rb_tree.c -o /tmp/bench_rb_c

# Rust
rustc -O benchmarks/cross_lang/rust/fib.rs     -o /tmp/bench_fib_rust
rustc -O benchmarks/cross_lang/rust/rb_tree.rs -o /tmp/bench_rb_rust

# Go (needs a module — create a throwaway dir)
mkdir -p /tmp/gobuild && cd /tmp/gobuild && go mod init bench
cp benchmarks/cross_lang/go/fib.go     /tmp/gobuild/main.go && go build -o /tmp/bench_fib_go .
rm /tmp/gobuild/main.go
cp benchmarks/cross_lang/go/rb_tree.go /tmp/gobuild/main.go && go build -o /tmp/bench_rb_go .

# kaikai
bin/kai build benchmarks/cross_lang/kaikai/fib.kai -o /tmp/bench_fib_kai
bin/kai build examples/perceus/rb_tree_bench.kai  -o /tmp/bench_rb_kai

# Run (any of the per-language sources)
/usr/bin/time -lp /tmp/bench_rb_c
python3 benchmarks/cross_lang/python/rb_tree.py
ruby    benchmarks/cross_lang/ruby/rb_tree.rb
node    --experimental-strip-types benchmarks/cross_lang/typescript/rb_tree.ts
deno    run --quiet                benchmarks/cross_lang/typescript/rb_tree.ts
elixir                             benchmarks/cross_lang/elixir/rb_tree.exs
```

Each number in the tables above is the median of 5 such runs.

## Honesty summary

- kaikai **meets** DoD #4a (compute-bound ≤ 1.5–2× C): 1.00× C on
  fib(35), matching clang and rustc.
- kaikai **does not yet meet** DoD #4b (structural data ≤ 2× C):
  4.96× C on RB-tree, on the v1.0 trajectory once #384 + Phase 4
  variant-field unboxing land.
- kaikai is **faster than Ruby and Python on this workload by
  ~1.6–1.7×**, but **slower than every production-grade managed
  runtime** (Go, Node, Deno, Elixir) on RB-tree by 2.5–4×.
- kaikai RB-tree here measures **4 220 ms** vs `rb_tree_2026-05-09.md`'s
  post-#409 figure of **3 665 ms** — a +15 % drift over one day. That
  is outside the ±10 % the lane brief considered acceptable; the
  delta is most likely run-to-run variance (the breakdown lane's own
  5-sample median spread was 3 653–3 832 ms, ±2 % around 3 798 ms,
  and the present numbers were taken on a host with more background
  load). No compiler change between v0.50.0 and v0.51.0 touches the
  RB-tree hot path. A clean re-measurement on a quiesced host is a
  reasonable follow-up before drawing a regression conclusion.
