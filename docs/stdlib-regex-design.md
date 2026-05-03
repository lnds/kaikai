# Lane B — `stdlib/regexp.kai` MVP

Status: design draft 2026-04-28. Pre-implementation.

## Goal

Land `stdlib/regexp.kai` per `docs/stdlib-layout.md` §`regexp`:
- `compile`, `match`, `find_all`, `replace`, `split`.
- RE2-style deterministic engine (no backreferences, linear time).
- Pure full kaikai (no FFI in v1).

Unblocks **m12.6.x #7** (regex refinement predicates `String where matches ~r/.../`) and removes one of the largest gaps vs Go (`regexp` package) and Elixir (`Regex` module).

## Scope (in)

**Syntax (subset, RE2-compatible):**
- Literal characters and escapes: `a`, `\.`, `\n`, `\t`, `\\`, `\/`.
- Character classes: `[abc]`, `[^abc]`, `[a-z]`, `[0-9]`.
- Predefined classes: `\d`, `\D`, `\w`, `\W`, `\s`, `\S`, `.`.
- Anchors: `^`, `$`.
- Quantifiers: `*`, `+`, `?`, `{n}`, `{n,}`, `{n,m}` (greedy only).
- Grouping: `(...)` for capture, `(?:...)` for non-capture.
- Alternation: `a|b`.
- Capture groups: numbered (no names in v1).

**API:**
```kai
pub type Regex = ...   # opaque (likely a record around the compiled NFA)
pub type Match = { start: Int, end: Int, groups: [Option[(Int, Int)]] }

pub fn regex_compile(pattern: String) : Result[Regex, String]
pub fn regex_match(re: Regex, s: String) : Option[Match]
pub fn regex_find_all(re: Regex, s: String) : [Match]
pub fn regex_replace(re: Regex, s: String, replacement: String) : String
pub fn regex_split(re: Regex, s: String) : [String]
```

## Scope (out)

- **Backreferences** (`\1`, `\2`). RE2 explicitly excludes these — quadratic blow-up risk.
- **Lookaround** (`(?=...)`, `(?!...)`). Not in RE2.
- **Unicode property classes** (`\p{Letter}`). Subset depends on a unicode tables stdlib that does not exist yet; v1 uses ASCII semantics only.
- **Non-greedy quantifiers** (`*?`, `+?`). Add in v2 if demand surfaces.
- **Named captures** (`(?P<name>...)`). v2.
- **Inline flags** (`(?i)`, `(?m)`). v2.

## Architecture

RE2 engine in three lowering steps: pattern → AST → NFA → execution.

### 1. Lexer + parser (pattern → AST)

```kai
type RxAst
  | RxChar(Char)
  | RxAny
  | RxClass([CharRange], Bool)   # Bool = negated
  | RxAnchor(Anchor)
  | RxConcat([RxAst])
  | RxAlt([RxAst])
  | RxStar(RxAst)
  | RxPlus(RxAst)
  | RxOpt(RxAst)
  | RxRepeat(RxAst, Int, Option[Int])
  | RxGroup(Int, RxAst)         # capture group with id; non-cap is RxConcat without RxGroup wrap
```

A recursive-descent parser over the pattern string. Errors carry position for compile-time messages.

### 2. NFA construction (AST → NFA)

Thompson-style NFA: each node compiles to a sub-NFA (start, accept). Connected by ε-transitions.

```kai
type NfaState = {
  id: Int,
  transitions: [Transition],   # (Char | Class | Epsilon, target_id)
  accept: Bool
}

type Nfa = {
  states: [NfaState],
  start: Int,
  num_groups: Int
}
```

### 3. Simulation (NFA + input → match)

RE2's two-step:
- **Set-of-states simulation**: maintains the set of all live NFA states at each input position. O(n·m) where n = input length, m = NFA size.
- **Capture group tracking**: each live state carries its own group-position map (small lists, copied on branch).

Match function returns the leftmost-longest match (greedy semantics).

## Phases

1. **Research & design** (this) — done.
2. **Pattern parser** (`rx_parse_pattern : String → Result[RxAst, ParseError]`) ~250 LOC. Fixtures: `examples/regex/parse/*.kai` round-trip parse → `rx_ast_to_string` → diff.
3. **NFA construction** (`rx_compile_nfa : RxAst → Nfa`) ~150 LOC. Fixtures: `rx_compile_dump : Regex → String` golden snapshots.
4. **Simulator** (`rx_simulate : Nfa, String → Option[Match]`) ~300 LOC. Fixtures: positive (matches), negative (no match), capture groups.
5. **Public API + module** (`stdlib/regexp.kai`) ~50 LOC wrappers. `regex_compile` returns `Result`; `regex_replace` builds output with substitution syntax (`$1`, `$2`).
6. **Fixtures end-to-end** (`examples/stdlib/regex_basic.kai`, plus a battery against RFC test cases — IPv4, email subset, URL parsing).
7. **Wire in** `bin/kai` + `stage2/Makefile` `EXTRA_PRELUDE_FLAGS` + `test-stdlib`.
8. **Docs**: update `stdlib-layout.md` to mark `regexp` as landed; mark m12.6.x #7 as unblocked (still requires regex literal syntax `where matches ~r/.../` integration).

## Estimated effort

| Phase | Hours |
|---|---|
| 2. Parser | 4–6 |
| 3. NFA construction | 3–4 |
| 4. Simulator | 6–8 |
| 5. Public API | 1–2 |
| 6. Fixtures | 3–4 |
| 7. Wiring | 0.5 |
| 8. Docs | 0.5 |

**Total: 18–25 hours, 3–4 working days.**

## Components touched

| File | Type |
|---|---|
| `stdlib/regexp.kai` | New module (~750 LOC). |
| `examples/stdlib/regex_basic.kai` | New fixture + golden. |
| `examples/regex/` | New dir for engine-level test cases. |
| `bin/kai` | +1 line (`STDLIB_REGEX` + load). |
| `stage2/Makefile` | +1 line in `EXTRA_PRELUDE_FLAGS`. |
| `docs/stdlib-layout.md` | Mark landed. |

**Zero changes** to `stage2/compiler.kai` — pure stdlib lane. The `where matches ~r/.../` regex literal *syntax* needs lexer/parser integration but that lands as **m12.6.x #7 lane**, separate from this one. Lane B unblocks it.

## Risks

- **Performance**: pure-kaikai NFA simulator allocates list-of-states per input char. For pathological regexes + long inputs, this can be slow. RE2 is O(n·m); our constant factor will be higher than C, but acceptable for typical use (URLs, log lines, tokens). Bench at the end.
- **Memory**: capture groups copy position maps on NFA branching. Cap at "match" granularity (don't capture across `find_all` iterations).
- **Selfhost stability**: regexp is an optional prelude. Drop-in if anything breaks.
- **Edge cases in pattern parser**: empty alternation `(|)`, nested classes, backslash escapes inside classes. Test heavily.

## Open questions

1. **Replacement syntax**: `$1`, `$2` (Perl-style) or `\1`, `\2` (sed-style)? Recommend: `$1`/`$0` (Perl), more readable.
2. **Anchors in `find_all`**: `^` and `$` only match string start/end (not line boundaries) in v1 — multiline mode is an inline-flag concern.
3. **Empty matches in `split`**: filter or keep? Recommend: keep, matching Python `re.split` default.
4. **Naming convention**: keep `regex_*` flat-prefix today (consistent with `list_*`, `string_*`), migrate to `regexp.match` etc. in m14.
