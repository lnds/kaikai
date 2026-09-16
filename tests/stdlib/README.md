# Core text contracts

`char_test.kai`, `char_unicode_test.kai`, and `string_test.kai` call the
public core modules through qualified names. They exercise all 50 public
functions across the three modules; this is API coverage, not a measured
line or branch coverage percentage. Existing inline stdlib tests remain
in place. These standalone suites avoid depending on implicit prelude
test discovery.

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

### Equivalent string boundary mutations

Some boundary mutations leave the public result unchanged. Keep these
visible in the native report and check their reasoning when the code
changes:

| Guard | Why the alternate boundary returns the same value |
|---|---|
| Left/right trim loop at the buffer edge | `char_at` returns `None` outside the buffer; its arm returns the same offset. |
| `trim` with `hi == lo` | Slicing zero bytes produces the same empty string. |
| Padding with `length == width` | Repeating the fill zero times and concatenating it preserves the input. |
| `w <= 0` in chars/count/advance/indices | The enclosing guard admits only in-range offsets, where `string_cp_len` returns 1–4. |
| Floor at zero or buffer end; ceil at buffer end | The helper either repeats the same clamp or reads the out-of-range byte sentinel `-1`, which is not a continuation byte. |
| Codepoint advance at `off == n` | One extra step can produce `n + 1`; the final byte slice clamps it to the same end of the buffer. |
| Slice with zero length or zero start | The alternate branches still produce an empty slice or a start of zero, respectively. |
| Blank check at the buffer end | The extra `char_at` returns `None`, whose arm is also `true`. |

Do not suppress all comparisons on a line indiscriminately: the
`count <= 0 or off >= n` line has both a killable count boundary and an
equivalent end boundary, while the native suppression key does not
include the column.
