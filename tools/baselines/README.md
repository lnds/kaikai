# Gate baselines

One file per key, so two changes to different keys never conflict. The file name is the key; its first line is the value. To pin a new key, add a file. To lower one, edit its file. To retire one, delete its file.

| Directory | Key | Value | Read by |
|---|---|---|---|
| `rc-leak/` | `examples/perceus` fixture | `<c>:<native>` | `tools/rc-leak-gate.sh` |
| `rc-growth/` | `examples/perceus` fixture | `<c>:<native>` | `tools/rc-leak-gate.sh` |
| `rc-effects-growth/` | `examples/effects` fixture | `<c>:<native>` | `tools/rc-leak-gate.sh` |
| `nscol/` | namespace-collision axis | allowed failure count | `tools/nscol-ratchet.sh`, `tests/namespace_matrix.sh` |

A `-` in a backend column skips the fixture on that backend; the reason lives in `tools/rc-leak-skips.txt`.

## `rc-leak/` — the exact RC ledger of `examples/perceus`

Each count is `alloc_total - free_total` from the runtime ledger under `KAI_TRACE_RC=1` on that backend. The gate fails on any count that differs from its pin, on a fixture with no file, and on a file whose fixture is gone.

- `leaked > 0` is not by itself a defect. A program that still holds its structure when `main` returns exits without a final free walk, so whatever is live at exit counts as leaked. A fixture that deliberately keeps a 200k-node spine alive reports about 200k.
- What the pin protects is the exact number: a regression that retains one extra reference per iteration moves it.
- To read a non-zero count, compare it with the same run's `live_peak`. At or below the peak, it is structure the program still holds. Well above the peak, memory accumulated and was dropped: the defect signature.
- The backends disagree on how many cells survive to exit for about a third of the corpus, so each backend has its own column. A count that moves on one backend alone is as much a regression as one that moves on both.
- When a fix lowers a count, lower the pin: the ratchet only tightens.

## `rc-growth/` and `rc-effects-growth/` — memory retained per run

Growth is `leaked(2 runs) - leaked(1 run)` under `KAI_TRACE_RC_RUNS`: what one run of `main` retains and never frees. A fixture with no file must grow by 0; a listed one must match its pin exactly. The lists only shrink, and a negative growth fails the gate because it means the ledger missed allocations. `examples/effects` is held to growth only.

## `nscol/` — failures allowed per namespace-collision axis

A missing file means 0. More failures than the count is a regression. Fewer is progress, and the count comes down so it cannot bounce back. Raise a count only for a deliberate new red-by-design fixture.
