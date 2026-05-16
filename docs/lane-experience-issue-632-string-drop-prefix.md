# Lane experience — issue #632: core.string drop_prefix / drop_suffix

**Lane:** `issue-632-string-prefix`
**Closes:** #632
**Scope:** stdlib surface expansion on `core.string` — add the two
destructive counterparts to `starts_with` / `ends_with`.
**Edition target:** Hanga Roa (2026-05-21).

## Scope as planned vs as shipped

Planned: two `pub fn`s on `stdlib/core/string.kai`, a single fixture
covering happy/edge paths, layout + roadmap doc updates, retro.

Shipped: exactly that. No drift, no follow-ups. The lane brief was
load-bearing — it pinned the Option-returning signature, the edge
cases, the fixture filename, and the doc surface to touch. Execution
was mechanical from the brief.

## Design decisions

### Option-returning vs plain String

The issue body listed two alternatives:

1. **Plain String** (Python/OCaml `removeprefix`): return the input
   unchanged on miss. Symmetric with `trim`.
2. **Option[String]** (Rust `str::strip_prefix`): return `None` on
   miss. Symmetric with `to_int`, `log2`, `div_mod`.

The issue author already concluded "the Option form is probably the
best" — and the brief pinned it. Validated decision because the
caller motivation in the body (URL routing) is *exactly* the pattern
that benefits from fusing the membership check with the slice:

```kai
match string.drop_prefix(path, "/notes/") {
  Some(rest) -> route_id(rest)
  None       -> not_found()
}
```

Plain-String semantics force the caller to re-test with
`starts_with` if they need to branch — defeating the point.

### Symmetry with the rest of `core.string`

The `Option` choice is *not* the dominant shape in the module
(`length`/`slice`/`concat`/`trim`/`repeat`/`join`/`split`/`replace`/
`pad_*`/`lines`/`chars`/`is_blank` all return non-Option). But it
matches the shape used elsewhere in core when the operation can
*meaningfully fail to apply*:

- `core.string.to_int` → `Option[Int]` (parse can fail)
- `math.int.log2` → `Option[Int]` (undefined on x ≤ 0)
- `math.int.div_mod` → `Option[Pair]` (undefined on zero divisor)

`drop_prefix` / `drop_suffix` join that family — the input string is
not the result when the affix is absent, so wrapping in `Some` /
returning `None` is the honest signal.

## Edge cases

The fixture pins six cases per operation:

| Case            | drop_prefix("abc", X)     | drop_suffix("abc", X)     |
|-----------------|---------------------------|---------------------------|
| Happy           | `("/notes/42", "/notes/")` → `Some("42")` | `("file.txt", ".txt")` → `Some("file")` |
| Empty affix     | `("abc", "")` → `Some("abc")` | `("abc", "")` → `Some("abc")` |
| Full match      | `("abc", "abc")` → `Some("")` | `("abc", "abc")` → `Some("")` |
| Affix longer    | `("abc", "abcd")` → `None` | `("abc", "xabc")` → `None` |
| Empty input     | `("", "x")` → `None`      | `("", "x")` → `None`      |
| Mismatch        | `("/notes/42", "/posts/")` → `None` | `("file.txt", ".log")` → `None` |

Empty-affix-always-matches falls out naturally from the implementation
(`starts_with(s, "")` returns `true` because the 0-length slice always
equals `""`). It's worth pinning in the fixture because it would be
easy for a future "optimisation" to short-circuit `length == 0` to
`None` and silently break the algebraic identity
`drop_prefix(s, "") == Some(s)`.

## Implementation

Trivial — both helpers compose existing `starts_with` / `ends_with`
with `string_slice`. No new runtime primitive, no resolver change, no
typer touch. Stage 0 / stage 1 / stage 2 are unaffected; the change
lives entirely in stdlib.

```kai
pub fn drop_prefix(s: String, prefix: String) : Option[String] {
  if starts_with(s, prefix) {
    let pl = string_length(prefix)
    Some(string_slice(s, pl, string_length(s) - pl))
  } else {
    None
  }
}
```

Used `string_slice` / `string_length` directly (the runtime primitive
names) rather than the `slice` / `length` wrappers because the existing
`starts_with` / `ends_with` definitions just above use the primitive
names — local consistency. The wrappers exist for *external* callers;
inside `core/string.kai` itself the primitives are the natural unit.

## Structural surprises

One: the fixture's helper was originally called `show`, which collides
with the `Show` protocol when the file types as `Option[String]`. The
typer routed `show("label", Some("x"))` through
`kai_protocols____pimpl_Show_String_show` (single-argument), producing
a stage 0 C-emit error: `too many arguments to function call`. Renamed
to `report`; built clean.

This is **memory-worthy** — see existing memory
`feedback_kaikai_prelude_name_shadowing.md`. The protocol-vs-helper
collision is the same shape as the prelude-shadowing trap, just on
the protocol axis. A `report`/`show`/`emit` style helper is endemic
in fixtures; choosing a name that doesn't shadow a protocol op should
become the default. The compiler diagnostic surfaced as a C-level
arity mismatch with no kaikai-level pointer at the call site —
worth noting for future "diagnostic quality on protocol collisions"
work.

## Fixture and coverage

- `examples/stdlib/string_drop_prefix_suffix.kai` with `.out.expected`.
- 12 cases (6 per fn), captured live from stage 2.
- Filename slot follows the `examples/stdlib/string_*.kai` convention
  (alongside `string_split_replace`, `string_pad`, `string_lines_chars`,
  `string_is_blank`).

No coverage gap added — the fixture exercises every code path in both
new functions (the `if` branch, the `else` branch, and the two length
arithmetic operations in the `Some` branch each fire on multiple
cases).

## Docs touched

- `docs/stdlib-layout.md` — `core.string` catalog row: appended
  `drop_prefix`, `drop_suffix` with a one-line semantic gloss naming
  the Option shape and the Rust precedent.
- `docs/stdlib-roadmap.md` — new row under *Current inventory*
  shipping against #632.

Did **not** touch `docs/effects-stdlib.md` (no effect surface change),
`docs/roadmap.md` (no milestone movement), or `docs/design.md`
(no language change).

## Cost vs estimate

- Estimated: 0.05 day.
- Actual: ~0.05 day. The `show` → `report` collision cost ~5 minutes
  to diagnose from the C-level error; everything else was strictly
  mechanical.

## Follow-ups left

None. The lane closes the issue cleanly. Hanga Roa surface for the
prefix/suffix family is now complete: `starts_with`, `ends_with`
(predicates) + `drop_prefix`, `drop_suffix` (destructive Option-
returning forms).

If a future lane wants to round this out further, `take_prefix(s, n)`
/ `take_suffix(s, n)` (length-bounded slicing) is a natural neighbour
but was not in the issue's scope. Defer until a real caller asks.
