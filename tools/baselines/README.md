# Gate baselines

One file per key, so two changes to different keys never conflict. The file name is the key; its first line is the value. To pin a new key, add a file. To lower one, edit its file. To retire one, delete its file.

| Directory | Key | Value | Read by |
|---|---|---|---|
| `rc-leak/` | `examples/perceus` fixture | `<c>:<native>` | `tools/rc-leak-gate.sh` |
| `rc-growth/` | `examples/perceus` fixture | `<c>:<native>` | `tools/rc-leak-gate.sh` |
| `rc-effects-growth/` | `examples/effects` fixture | `<c>:<native>` | `tools/rc-leak-gate.sh` |
| `kir-lin/` | `<corpus>-<fixture>` | KIR linearity violations on native | `tools/rc-leak-gate.sh` |
| `nscol/` | namespace-collision axis | allowed failure count | `tools/nscol-ratchet.sh`, `tests/namespace_matrix.sh` |
| `symid-written-readers/` | `<file> <fn>` | `<reads>` | `tools/symid-census/gate.py` |
| `symid-allow/` | `<file> <fn> <test> <literal>` | `<n> <reason> <why>` | `tools/symid-census/gate.py` |
| `symid-written-mints/` | `<file> <fn>` | `<why>` | `tools/symid-census/gate.py` |
| `symid-dual-slots/` | `<file> <type>` | `<why>` | `tools/symid-census/gate.py` |
| `walker-catchall/` | `<file> <fn>` | `<reason>` | `tools/walker-catchall-audit.sh` |
| `compiler-lint/` | `<file> <rule> <message hash>` | finding count | `tools/compiler-lint-ratchet.sh` |
| `diags-json-parity/` | rejected fixture path | what differs | `tools/diags-json-parity.py` |
| `runtime-globals/` | `<symbol>` | `<class>` | `tools/runtime-global-audit.sh` |

A `-` in a backend column skips the fixture on that backend; the reason lives in `tools/rc-leak-skips.txt`.

Git keeps no empty directories: a table row whose directory is absent from the tree has no pinned key, so every count it would hold is 0.

## `rc-leak/` — the exact RC ledger of `examples/perceus`

Each count is `alloc_total - free_total` from the runtime ledger under `KAI_TRACE_RC=1` on that backend. The gate fails on any count that differs from its pin, on a fixture with no file, and on a file whose fixture is gone.

- `leaked > 0` is not by itself a defect. A program that still holds its structure when `main` returns exits without a final free walk, so whatever is live at exit counts as leaked. A fixture that deliberately keeps a 200k-node spine alive reports about 200k.
- What the pin protects is the exact number: a regression that retains one extra reference per iteration moves it.
- To read a non-zero count, compare it with the same run's `live_peak`. At or below the peak, it is structure the program still holds. Well above the peak, memory accumulated and was dropped: the defect signature.
- The backends disagree on how many cells survive to exit for about a third of the corpus, so each backend has its own column. A count that moves on one backend alone is as much a regression as one that moves on both.
- When a fix lowers a count, lower the pin: the ratchet only tightens.

## `rc-growth/` and `rc-effects-growth/` — memory retained per run

Growth is `leaked(2 runs) - leaked(1 run)` under `KAI_TRACE_RC_RUNS`: what one run of `main` retains and never frees. A fixture with no file must grow by 0; a listed one must match its pin exactly. The lists only shrink, and a negative growth fails the gate because it means the ledger missed allocations. `examples/effects` is held to growth only.

## `kir-lin/` — owned references a native build leaves unpaid

The count is the number of distinct violations the KIR linearity check (`stage2/compiler/kir_lin.kai`) reports for the fixture's native build: an owned reference consumed without being held, or still held at a return, a loop back-edge or once nothing reads it. A missing file means 0. When a fix lowers a count, lower the pin; a violation the fix did not touch shows in the gate's output with its function.

## `nscol/` — failures allowed per namespace-collision axis

A missing file means 0. More failures than the count is a regression. Fewer is progress, and the count comes down so it cannot bounce back. Raise a count only for a deliberate new red-by-design fixture.

## Entry files written by `tools/lib/entries.py`

The multi-column ledgers (`symid-*`, `walker-catchall/`, `compiler-lint/`, `diags-json-parity/`, `runtime-globals/`) hold one file per entry. A file is exactly one tab-separated row: the first N columns are the key (N per gate, in the table above), the rest the value. `entries.py` derives the file name from the key, so:

- two changes that add different keys touch different files and never conflict;
- two changes that add the same key collide on one path, which is the conflict to resolve;
- a loader rejects a file whose name does not match its key, an entry of more than one line, and two files with one key.

Add or edit: `tools/lib/entries.py put tools/baselines/walker-catchall 2 <file> <fn> "<reason>"`. Retire: delete the file.

Ratchets over these directories are strict per entry: a finding with no entry fails, and an entry with no finding fails (delete its file in the change that fixed it). A change that removes one finding while another adds one cannot net out to a pass.

A `README.md` in a directory documents its format and is not an entry.

Single files that remain, each with the commits touching it since 2026-10-01: `tools/symid-census/pending-written.txt` (2), `tools/symid-census/written-eq.txt` (2), `tools/tls-hoist.allow` (7), `tools/perceus-read-baseline.txt` (4), `tools/progen-diff-skips.txt` (4), `tests/namespace_matrix_thin_baseline.txt` (4), `tools/wasm-parity-baseline.txt` (4), and the other parity baselines and `*-skips.txt` lists (0 to 3 each). Their entries are low-churn; one that starts to collide converts the same way.
