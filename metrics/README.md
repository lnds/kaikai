# metrics/

Aggregate metrics extracted from lane retros (`docs/lane-experience-*.md`).

## `build-history.tsv`

One row per `make` invocation logged by lanes. Columns:

| Column | Type | Notes |
|---|---|---|
| `lane` | string | derived from filename (`docs/lane-experience-<lane>.md`) |
| `timestamp` | ISO 8601 with offset | from the lane's TSV |
| `cmd` | string | `tier0`, `tier1`, `tier1-asan`, `selfhost`, `selfhost-llvm`, etc. |
| `outcome` | string | typically `OK` / `FAIL`; some lanes log custom (`FAIL_PRE_EXISTING_signal_trap`, etc.) |
| `elapsed_s` | int or `-` | seconds; `-` when not measured |

Schema is informal — different lanes log slightly different command names
or outcome conventions. Treat the TSV as a **dataset for exploration**,
not a strict-schema warehouse.

## How it is generated

`./regen.sh` walks `docs/lane-experience-*.md`, extracts the `## Build TSV`
section from each, prefixes each row with the lane name, and writes
`build-history.tsv`.

Re-run after each lane merge to keep the history current. The lane brief
template should append a step at the end:

```bash
# After committing the lane experience doc, refresh the aggregate:
cd /path/to/kaikai && ./metrics/regen.sh && git add metrics/build-history.tsv
```

If a row is missing, the lane retro probably lacks a properly-formatted
`## Build TSV` section. Add the section in the retro and re-run.

## Sample queries

```bash
# Average tier1 wall-clock across lanes (numeric only):
awk -F'\t' '$3=="tier1" && $5 ~ /^[0-9]+$/ {sum+=$5; n++} END {print sum/n " s avg, n=" n}' \
  metrics/build-history.tsv
```

```bash
# Lanes that hit a tier1 FAIL or non-clean outcome:
awk -F'\t' '$3=="tier1" && $4 != "OK"' metrics/build-history.tsv
```

```bash
# Distribution of tier1-asan elapsed (sortable):
awk -F'\t' '$3=="tier1-asan" && $5 ~ /^[0-9]+$/ {print $5 "\t" $1}' \
  metrics/build-history.tsv | sort -n
```

```bash
# Per-lane gate count (how many gates each lane ran):
awk -F'\t' 'NR>1 {c[$1]++} END {for (l in c) print c[l] "\t" l}' \
  metrics/build-history.tsv | sort -rn | head
```

## Caveats

- Lanes pre-2026-05 have varying schemas (`make test` vs `tier1`, etc.).
  Filter by lane name pattern when comparing apples to apples.
- `elapsed_s = -` rows do not contribute to time analysis. About a third
  of historic rows lack a numeric elapsed.
- Lane retros are the source of truth. The aggregate is regenerated, not
  edited by hand.
