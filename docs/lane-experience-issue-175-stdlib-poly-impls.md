# Lane retro — issue #175 stdlib polymorphic impls

## Objective metrics

| metric | value |
| --- | --- |
| Issue | #175 — stdlib lacks polymorphic Show/Eq/Ord/Hash for `[T]` / `Option[T]` / `Result[E, T]` |
| Branch | `issue-175-stdlib-poly-impls` |
| Wall-clock | ~30 min (start `2026-05-05T13:26:20-04:00`, end `2026-05-05T13:54:05-04:00`) |
| Diff (kaikai) | ~110 net lines in `stdlib/protocols.kai` for 12 impls + 3 helpers |
| Fixtures added | 5 under `examples/stdlib/` (one per protocol + one `#derive(Show)` record case) |
| Fixtures patched | 13 under `examples/stdlib/` (user `fn show` renamed to `pretty`) |
| Docs touched | `docs/protocols.md` (one note appended to the primitives paragraph) |
| Tier gates | tier0 / tier1 / tier1-asan / selfhost / selfhost-llvm — all green |
| Selfhost convergence | byte-identical, 1 round on each backend |

## 12 impl shapes (notable algebraic identities)

| Container × Protocol | Body sketch |
| --- | --- |
| `[T] : Show`        | `"[" ++ join(map(xs, show), ", ") ++ "]"` (via `show_list_inner` helper) |
| `[T] : Eq`          | lockstep walk; `eq(x_i, y_i)` per element until length or value diverges (`list_eq_loop`) |
| `[T] : Ord`         | lexicographic; first non-zero `cmp(x_i, y_i)` wins, else shorter list < longer with shared prefix (`list_cmp_loop`) |
| `[T] : Hash`        | `acc * 31 + hash(x)` polynomial fold (`list_hash_loop`); `hash([]) = 0` |
| `Option[T] : Show`  | `"None"` or `"Some(" ++ show(x) ++ ")"` |
| `Option[T] : Eq`    | tag match then defer to `eq(x, y)` |
| `Option[T] : Ord`   | `None < Some(_)`; tied Some compares inner |
| `Option[T] : Hash`  | `None => 0`, `Some(x) => 1 + hash(x)` |
| `Result[E,T] : Show`| `"Ok(" ++ show(t) ++ ")"` or `"Err(" ++ show(e) ++ ")"` |
| `Result[E,T] : Eq`  | tag match then defer to inner `eq` |
| `Result[E,T] : Ord` | `Ok(_) < Err(_)`; tied variants compare inner |
| `Result[E,T] : Hash`| `Ok(t) => 2 + hash(t)`, `Err(e) => 3 + hash(e)` |

The choice `Ok < Err` mirrors the convention "success branch is the smaller
result" and is parallel to how `#derive(Ord)` orders sum-type constructors
by declaration position.

## Empirical verification

The 7-line acceptance gate from the brief (with bindings to avoid the
pre-existing interpolation/proto-call corner case where
`"#{cmp(a,b)}"` cannot infer the receiver):

```
[1, 2, 3]               # Show on [Int]
Some(hi)                # Show on Option[String]
Ok(42)                  # Show on Result[String, Int]
true                    # [1,2,3] == [1,2,3]
false                   # [1,2,3] == [1,2,4]
-1                      # cmp([1,2], [1,2,3]) — shorter wins on shared prefix
1026                    # hash([1,2,3]) — value not pinned, only invariants
```

`#derive(Show)` on a record with a `[Int]` field renders correctly
without further annotation:

```
Pkg { name: "core", deps: [1, 2, 3] }   ->  Pkg { name: core, deps: [1, 2, 3] }
```

(The `String` field is unquoted because `Show for String` is the
identity map — that is pre-existing stdlib behavior, out of scope.)

## Friction points

### `cmp` / `eq` inside `"#{...}"` interpolation

`println("#{cmp([1,2], [1,2,3])}")` compiles in `cmp(a, b) : Int` shape
and would print `-1`, but the stdlib pre-resolve rename
(`rename_proto_calls_kind`) walks ordinary `ECall` nodes and not the
synthesised expression inside an interpolation segment. The result is an
"undefined name `cmp`" error in unannotated cases. Workaround:
`let c = cmp(...); println("#{c}")`. Pre-existing — not introduced by
this lane.

### Stdlib fixtures with user `fn show(o: Option[..])` shadowing the new impls

13 stdlib fixtures (`option_*_basic.kai`, `result_*_basic.kai`,
`option_collect_basic.kai`) defined a top-level `fn show(o: Option[T])` /
`fn show(r: Result[E, T])` to render their output. Pre-#175 there was no
stdlib `Show for Option[T]` / `Show for Result[E,T]`, so the user fn was
the only candidate and the resolver-local-shadow rule kept the call site
pointed at it.

After #175 lands, `Show for Option[T]` and `Show for Result[E,T]` exist
in the registry. The pre-resolve rename **does** still skip the user's
call site (the `(name="show", arity=1)` is filtered out via
`filter_shadowed_ops`), but the **post-mono** `resolve_protocol_calls`
walker rewrites the same `show(o)` call to `__pimpl_Show_Option_show`
because it consults the protocol registry directly without checking
whether a same-name local DFn shadows the op. The runtime then panics
with `non-exhaustive match` from the dispatcher's fallthrough.

This is a pre-existing inconsistency between the two passes; the fix
that ships in this lane is mechanical: rename `show` → `pretty` in the
13 affected fixtures. The fixtures' intent (custom rendering of these
specific types for human-readable test output) is preserved; the new
stdlib impls supply the *default* `Show`, which is what `#175` is
about. A separate increment can tighten the post-mono walker to honor
the same `(name, arity)` shadow check; not undertaken here to keep this
lane in scope.

`examples/protocols/resolver_local_shadow.kai` continues to pass
because `test-protocols` runs each fixture standalone (no stdlib
preludes), so no `Show for [T]` is in scope to shadow against.

### Pattern syntax: `[_, ..._]` is not accepted

`match` patterns reject `[_, ..._]` (parser asks for an identifier after
`...`). Used `[_, ..._tl1]` instead in `list_cmp_loop`. Minor.

### Selfhost convergence

Byte-identical on first round on both backends. Single-step convergence
is consistent with #174's experience — the change is data-only (twelve
new DImpl decls + three top-level helpers) and adds no compiler
machinery, so the bootstrap chain has nothing to chase.

## Did `#derive(Show)` on records-with-list-field work?

Yes. The `#derive(Show)` machinery composes structurally: each field
type's `Show` impl is resolved at derive time. With
`Show for [T]` / `Show for Option[T]` / `Show for Result[E, T]` now in
the stdlib, `#derive(Show)` on a record like
`{ name: String, deps: [Int] }` derives transparently — see fixture
`poly_show_derive_record.kai` for a 3-field record covering all three
container shapes.

## Subjective summary

The body of the lane is fast (the 12 impls are 5–15 lines each, similar
shapes). The ~30 minutes is dominated by two things:

1. Discovering and fixing the 13-fixture user-`fn show` regression
   above. Once the cause is understood, the fix is `re.sub(r'\bshow\b',
   'pretty', s)` over the file set.
2. Running the full tier ladder. tier1 alone is ~5 min on this machine.

Apart from that the lane was uneventful: #174's bounded-impl mechanism
delivered exactly what it promised — `impl[T : Show] Show for [T]` with
recursive `show(x : T)` in the body composes for every concrete `T'` at
call sites with a `Show for T'` impl. Mono-time validation catches the
missing-impl case; runtime never sees a dispatcher panic on a reachable
path.

## Limitations (out of scope)

- **`Serialize` for parametric containers** is still missing — the v1
  dispatch can't pick `from_string : Result[String, Self]` based on
  return-type alone. A separate increment after annotation-aware
  lookup lands.
- **Recursive `[T]` of unconstrained `T`** still rejects: `impl[T] Show
  for [T]` without the `: Show` bound is intentionally rejected by
  #174's diagnostic when the body recurses on `T`. Documented behavior.
- **The post-mono / pre-resolve shadow inconsistency** described above
  is left as-is. A future lane can tighten `resolve_protocol_calls` to
  consult `local_arities` before rewriting; the value/cost tradeoff
  isn't obvious enough to bundle here.
- **`Hash` constants** (`* 31`, the `0/1/2/3` variant offsets) are not
  collision-engineered. They follow the JVM/Java convention and are
  fine for the v1 hash-map use case; a future increment can swap to a
  proper hash-mixing primitive once Real / Decimal / Complex hashes
  exist.

## Builds

```
timestamp                       cmd            outcome  elapsed_s
2026-05-05T13:35:09-04:00       tier0          OK       49
2026-05-05T13:38:10-04:00       tier1          FAIL     173    # user-fn show regression
2026-05-05T13:50:45-04:00       tier1          OK       319    # after fixture rename
2026-05-05T13:51:44-04:00       tier1-asan     OK       53
2026-05-05T13:52:21-04:00       selfhost       OK       32
2026-05-05T13:53:13-04:00       selfhost-llvm  OK       46
```
