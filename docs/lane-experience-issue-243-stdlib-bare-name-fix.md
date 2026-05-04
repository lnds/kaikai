# Lane experience — issue #243 stdlib bare-name fix

## Objective metrics

- Lane span: 2026-05-04T19:12:48-04:00 → 2026-05-04T19:39:56-04:00 (~27 min wall).
- Builds run (TSV appended below): tier0, tier1, tier1-asan, test-stdlib-core-intrinsic, selfhost, selfhost-llvm — all OK.
- Files modified: 4 stdlib consumers + 1 stage2 typer/diagnostic file.
- LOC delta (approx): stage2/compiler.kai +130 / -10; stdlib/* +6 / -6.

## Inventory (sites found per ambiguous name, files touched)

The briefing listed 9 ambiguous bare names: `repeat`, `map`, `filter`, `zip`, `unwrap_or`, `and_then`, `or_else`, `unwrap_or_else`, `collect`. Empirical grep of `stdlib/**/*.kai` (excluding `core/` and self-defining declarations) found bare-name call sites for only 2 of those:

| Name      | Sites in stdlib (non-core) |
|-----------|----------------------------|
| `repeat`  | `loop.kai:42` (recursive self-call — not a cross-module bare call); `decimal.kai:183` already qualified as `string.repeat` |
| `map`     | `encoding/json.kai:96, 97`; `net/http.kai:182, 245`; `collections/map.kai:234, 237` |
| others    | 0 sites |

The 9 warnings on `kai run /tmp/hi.kai` did NOT come from those call sites. They came from `report_ambig_efns` in `stage2/compiler.kai:9557`, which fires once per name exported by 2+ modules sans root-file shadow, regardless of usage. The original m6.2 v2 design comment (l. 9550-9556) explicitly justified the no-body-walk shortcut. After PR #241 the prelude permanently exports e.g. `option.map` AND `result.map` — both legitimate, both intentional — so the namespace warning fires forever even on hello-world.

This invalidated the briefing's "mechanical migration" premise: qualifying the 6 stdlib call sites does not change what the user sees. The user authorized option B (touching `stage2/compiler.kai` to filter the warning by actual root-file usage) after I reported the gap.

## Files modified

**stdlib (consumer migration, 6 call sites):**

- `stdlib/encoding/json.kai:96, 97` — `map(xs, json_encode)` → `list.map(...)`, `map(ps, json_encode_pair)` → `list.map(...)`.
- `stdlib/net/http.kai:182, 245` — `map(bs, http_byte_to_str)` → `list.map(...)`, `map(hs, http_format_header)` → `list.map(...)`.
- `stdlib/collections/map.kai:234, 237` — `map(map_to_pairs(m), …)` → `list.map(...)` ×2.

These were all morally `[T] → list.map`, no `option.map`/`result.map` sites in non-core stdlib.

**stage2/compiler.kai (warning filter):**

- `report_ambig_efns` now takes a second `used: [String]` argument and only emits per name present in that set.
- New `collect_root_bare_callees` + `walk_bare_callees(_list|_arms|_fields|_elems|_stmts|_clauses)` walker traverses only decls whose module-origin is `None` (root file) and accumulates names appearing in `ECall(EVar(name), _)` callee position. Mirrors the existing `collect_call_labels` walker structure for parity.
- Call site at l. 42454 passes `collect_root_bare_callees(typed_prog.decls, [])` as the usage set.
- Updated comment block to cite issue #243 and explain the new bounded-walk cost model.

## Verification

**Hello-world acceptance gate:**

```sh
$ echo 'fn main() { print("hi") }' > /tmp/hi.kai
$ bin/kai run /tmp/hi.kai
hi
$ echo $?
0
```

No `warning:` lines, no `error:` lines. Exit 0.

**Multi-module acceptance gate:**

```sh
$ cat > /tmp/multi-test/util.kai <<EOF
pub fn double(n: Int) : Int = n * 2
EOF
$ cat > /tmp/multi-test/main.kai <<EOF
import util
fn main() { print(int_to_string(util.double(7))) }
EOF
$ (cd /tmp/multi-test && bin/kai run main.kai)
14
```

Clean exit, expected output.

**Tier gates (all green, recorded in TSV at end of doc):**

| Gate                          | Outcome | Elapsed |
|-------------------------------|---------|---------|
| `make tier0`                  | OK      | 45s     |
| `make tier1`                  | OK      | 292s    |
| `make tier1-asan`             | OK      | 53s     |
| `make -C stage2 test-stdlib-core-intrinsic` | OK | 3s |
| `make selfhost`               | OK      | 30s     |
| `make -C stage2 selfhost-llvm`| OK      | 41s     |

Selfhost byte-identical on both backends — the new walker and filter are deterministic.

## Friction points

1. **Briefing premise was wrong.** It read the 9 warnings as cross-module bare-name dispatch failures and prescribed a mechanical consumer migration. The actual emit site is the namespace ambiguity check in the typer, fired without regard to usage. Empirical grep of stdlib turned up only 6 call sites, all `map`-on-list, which would have left 8 of the 9 warnings unaddressed. I had to surface the gap and ask before continuing.
2. **`make selfhost-llvm` is not a root target.** It only exists at `stage2/selfhost-llvm`. The instrumentation script in the briefing assumed it was a root target. I corrected the TSV (see annotation below).
3. **Auto-mode permission gate paused on `tier1-asan`.** The sandbox flagged my `stage2/compiler.kai` edit as out-of-lane-scope and refused to run `tier1-asan`. The user re-authorized explicitly; gate then passed.
4. **No reproducible "9 type errors" on this HEAD.** The issue body documented warnings + 9 type errors. I observed only the 9 warnings, exit 0, `hi` printed correctly. Likely commit `62430f7` ("guard pipe-dispatch EModCall on qualified-entry presence") fixed the type errors before this lane started.

## Subjective summary

The lane scope as written did not match the symptom. Detecting that early and reporting before acting was the right call. Once authorized, the fix had two parts: (a) qualify 6 stdlib sites for hygiene (matches the spirit of the issue and removes future ambiguity if the typer's narrowing ever degrades), and (b) make the warning fire only when the root file actually uses an ambiguous name (matches what the user sees).

The walker pattern was a near-copy of `collect_call_labels`, so adding it was mechanical. The filter logic in `report_ambig_efns` is one `if list_has(...)` line. Total compiler-side change is small in concept; large in line count because the AST has many node kinds.

## Limitations

- The walker treats only `ECall(EVar(name), _)` as a bare-name use. Bare names appearing as first-class values (`let f = map`) or as args to higher-order helpers (`pipe(xs, map)`) are NOT counted. Those cases would still resolve to whichever module the typer picks first, silently. If we want strict parity with the original "warn on every ambiguous reference" intent, we would also count `EVar(name)` outside callee position. Deferred — the user-visible incident is hello-world ambiguity warnings, which the current filter resolves.
- The walker also visits root-file `DTest`/`DBench`/`DCheck` bodies. Test-only bare-name uses now emit warnings too (correct).
- Module-origin filtering relies on `DFn`'s last `Option[String]` field. Other Decl kinds (`DType`, `DEffect`, `DImport`, etc.) don't carry bodies that could call bare names, so they are skipped without ambiguity.

## Build TSV

```
timestamp	cmd	outcome	elapsed_s
2026-05-04T19:29:18-04:00	tier0	OK	45
2026-05-04T19:34:20-04:00	tier1	OK	292
2026-05-04T19:38:10-04:00	tier1-asan	OK	53
2026-05-04T19:38:18-04:00	selfhost-llvm-from-root	NOTARGET	0
2026-05-04T19:38:23-04:00	test-stdlib-core-intrinsic	OK	3
2026-05-04T19:39:05-04:00	selfhost	OK	30
2026-05-04T19:39:48-04:00	selfhost-llvm	OK	41
```
