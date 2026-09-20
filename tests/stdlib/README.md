# Core text contracts

`char_test.kai`, `char_unicode_test.kai`, and `string_test.kai` call the
public core modules through qualified names. They exercise all 50 public
functions across the three modules; this is API coverage, not a measured
line or branch coverage percentage. `string_boundaries_test.kai` adds
boundary grids and runtime sentinel checks. The target also explicitly
runs the inline string tests, including the private codepoint advance
contract, through a temporary entry with a different module name.

`core_text_properties_test.kai` contains 12 generated `check` properties:
ASCII classification and case normalization, Unicode normalization and
reference case-pair sequences, UTF-8 round-trips, reversal, slicing
against a list model, byte boundaries, trimming, concatenation, and
character-to-string casing consistency. Checks return `Bool` so the
built-in property harness can report and shrink counterexamples.

The installed 0.119 generators produce printable ASCII `Char`/`String`
values. Generated integer lists are therefore also mapped through an
explicit palette spanning all UTF-8 widths, Unicode case exceptions,
whitespace and scalar boundaries. This samples sequences from that
palette; it is not exhaustive coverage of Unicode.

The character suite checks all 128 ASCII codepoints and non-ASCII
negative cases. The Unicode suite uses explicit case pairs for every
regular pair in the supported ranges, exceptional mappings, and range
neighbours. Its reference is the simple mappings and general categories
in [UnicodeData 17.0.0](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt),
not the implementation's parity rules. String tests distinguish byte
and codepoint indices, enumerate every byte boundary across UTF-8
widths, and check construction, search, whitespace, and case conversion.

Run with the installed compiler against the checkout's stdlib:

```sh
make test-core-text KAI_TEST_DRIVER="$(command -v kai)" KAI_TEST_BACKEND=native
make test-core-text KAI_TEST_DRIVER="$(command -v kai)" KAI_TEST_BACKEND=c
```

CI runs `make test-core-text` with the repository driver in tier 1,
shard 3. Mutation remains an opt-in diagnostic, using `kai mutate`.
The target also invokes `kai check --backend=c`: the property runner in
0.119 supports C only, independently of the backend selected for `test`.
Each property runs 100 generated cases with the runtime's reproducible
seed. Run just the properties with:

```sh
KAI_STDLIB="$PWD/stdlib" kai check --backend=c tests/stdlib/core_text_properties_test.kai
```

## Native mutation testing

Use a disposable copy so interrupted mutations cannot alter the working
tree. A differently named hard link lets `kai mutate` typecheck the
subject as an ordinary entry module: passing a core file as the entry
directly collides with its auto-loaded names in kai 0.119.0. The oracle
still reads the mutated bytes through the real core module path.

```sh
repo="$PWD"
work=$(mktemp -d)
cp -R stdlib "$work/stdlib"
module=char_unicode
ln "$work/stdlib/core/$module.kai" "$work/${module}_subject.kai"
export KAI_STDLIB="$work/stdlib"
export KAI_CORE_CACHE=0
kai test --backend=native "$repo/tests/stdlib/${module}_test.kai"
kai mutate "$work" --module "$work/${module}_subject.kai" \
  --oracle "kai test --backend=native '$repo/tests/stdlib/${module}_test.kai'"
```

Use a fresh copy for each module (`char`, `char_unicode`, or `string`).
Start a bounded investigation with `--limit 20`, or select boundary
mutations with `--operator compare --operator connect`. Inspect survivors
and their diffs: an equivalent boundary guard needs an explanation,
while an observable difference needs a new failing test. Compilation
failures are reported separately by `kai mutate` and do not establish
that an assertion detected the mutation.

### String mutation oracle and equivalent sites

For `string`, include the assertion suites and generated properties in
the oracle, and map the
repository's native equivalence file to the temporary entry name:

```sh
mkdir -p "$work/tools"
sed 's|stdlib/core/string.kai:|string_subject.kai:|' \
  tools/mutate-known-equivalent.txt > "$work/tools/mutate-known-equivalent.txt"
oracle="kai test --backend=native '$repo/tests/stdlib/string_test.kai' &&
  kai test --backend=native '$repo/tests/stdlib/string_boundaries_test.kai' &&
  kai test --backend=native '$work/string_subject.kai' &&
  kai check --backend=c '$repo/tests/stdlib/core_text_properties_test.kai'"
eval "$oracle"
kai mutate "$work" --module "$work/string_subject.kai" \
  --oracle "{ $oracle; } >> '$work/oracle.log' 2>&1"
```

Use a fresh copy with `module=string` from the setup above. Run the oracle
successfully before mutating. All six native operators are in scope.
An optional `ulimit -t 15` in the oracle bounds CPU use when a mutation
prevents a loop from terminating; inspect the oracle's failure output
before treating such a rejection as evidence.

Known equivalents are recorded in `tools/mutate-known-equivalent.txt`,
with a reason for each site. They are **skipped**, not killed by tests.
The boundary fixture exercises the identities and runtime invariants
behind these explanations; passing examples alone do not prove an
arbitrary mutant equivalent. Review each entry when its source changes.

| Guard | Why the alternate boundary returns the same value |
|---|---|
| Left/right trim loop at the buffer edge | `char_at` returns `None` outside the buffer; its arm returns the same offset. |
| `trim` with `hi == lo` | Slicing zero bytes produces the same empty string. |
| Padding with `length == width` | Repeating the fill zero times and concatenating it preserves the input. |
| Singleton arm in trailing-line removal | The general list arm also produces `[x]` when its tail is empty; the earlier empty-string arm still handles `[""]`. |
| `w <= 0` in chars/count/advance/indices | The enclosing guard admits only in-range offsets, where `string_cp_len` returns 1–4. |
| Literal changes in the same width fallback | Threshold `1` still yields step `1` when width is `1`; changing the fallback value is unreachable. |
| Floor at zero or buffer end; ceil at buffer end | The helper either repeats the same clamp or reads the out-of-range byte sentinel `-1`, which is not a continuation byte. |
| Slice with zero length | The alternate branch still produces an empty slice. |
| Blank check at the buffer end | The extra `char_at` returns `None`, whose arm is also `true`. |
| Blank check's `None` result | With the original end guard, only in-range offsets reach `char_at`, which always returns `Some` there, including malformed bytes. |

The codepoint advance end guard is not excluded: its inline test checks
the private helper's bounded offset directly, so a mutant returning
`n + 1` is rejected even when the public slice would hide that error.
`slice` delegates negative-start clamping to this helper, avoiding a
second conditional with an identical zero-start result.
The native suppression key has no column; never exclude a line/operator
pair that also contains an observable mutation.

## Validation with kai 0.119.0

All 59 assertion tests pass on C and native. All 12 properties pass with
100 cases each on the C property runner. Restoring the pre-fix Unicode
module makes the reference-pair property fail; its shrunk seed `[-44]`
selects the `Ź`/`ź` pair. The corrected module passes the same check.

All six mutation operators were swept against the assertion suites:

| Module | Killed by the oracle | Did not compile | Equivalent, skipped | Survived |
|---|---:|---:|---:|---:|
| `char` | 17 | 10 | 0 | 0 |
| `char_unicode` | 116 | 23 | 0 | 0 |
| `string` | 169 | 87 | 26 | 0 |

The string sweep used three isolated copies, grouped by operators:
`arm/call/compare/connect` (130 tested, 17 skipped), `negate` (46 tested),
and `literal` (80 tested, 9 skipped). The groups cover every generated
site once. The 26 equivalents are excluded explicitly, not counted as
tests detecting a fault.

A further 20-mutant smoke run puts `kai check` first in the oracle:
9 killed, 11 compile failures, zero survivors. The generated
concatenation property reports and shrinks counterexamples for those
prefix/suffix mutations.

## HTTP redirect credentials

`make test-http-redirects` builds `http_redirect_client.kai` and observes
its requests with two Python loopback HTTP servers. The wire assertions
cover same-origin preservation, host case, different hosts and ports,
network-path references, duplicate/mixed-case credential headers, Host
regeneration, 307/308 body preservation, POST-to-GET rewriting and a chain
returning to the original origin without restoring stripped credentials.
The target also runs the inline origin comparison test in `net/http.kai`,
including omitted versus explicit default ports and unparseable URLs.

The test runs in tier 1 shard 3 on C and tier1-native shard 2 on native.
Select another toolchain/backend with `KAI_TEST_DRIVER` and
`KAI_TEST_BACKEND`. `KAI_STDLIB` can select a disposable pre-fix copy for
red/green validation of the wire test; its default is this checkout.
