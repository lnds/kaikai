# contracts

Design-by-Contract — `requires` / `ensures` on a function, and
`where` predicates on a type (refinement types).

## Description

A contract states what a function assumes and guarantees. `requires`
is a precondition checked on entry; `ensures` is a postcondition
checked on the result. A refinement type (`Base where P`) carries a
predicate every value of that type must satisfy. Predicates are
effect-pure expressions over the parameters (`requires`/`ensures`) or
over `self` (refinement types); `ensures` may also name `result`.

When the type-checker can prove a predicate statically it omits the
runtime check; otherwise it inserts one that panics with a structured
diagnostic on violation. Code with no contracts pays nothing.

## Function contracts

```kaikai
fn divide(a: Int, b: Int) : Int
  requires b != 0
  ensures result * b == a
{
  a / b
}

fn main() : Unit / Stdout {
  Stdout.print("q=#{int_to_string(divide(10, 2))}")
}
```

`requires` sits between the return type and the body; `ensures` names
the return value `result`. Both are checked; a literal argument that
provably violates a `requires` is a compile error, and a runtime
violation panics:

```
panic: requires violated in `divide`
required: b != 0
declared at line 15, col 14
  = help: narrow `b` to `Int where != 0`
argument b was: 0
```

## Refinement types

A type alias may carry a predicate over `self`:

```kaikai
type NonNeg = Int where self >= 0
type Percent = Real where 0.0 <= self and self <= 1.0

fn clamp_low(n: NonNeg) : NonNeg = n

fn main() : Int = clamp_low(3)
```

A refinement is a subtype of its base: passing a `NonNeg` where an
`Int` is expected is free (upcast). Going the other way (a plain `Int`
into a `NonNeg`) is a downcast — the predicate is checked, statically
when provable, otherwise at runtime.

## Predicates are pure

A predicate may not perform effects. This is rejected:

```kaikai-neg
type Loud = Int where { Stdout.print("x"); self >= 0 }
```

The checker reports the unhandled effect rather than admitting a
predicate with side effects.

## See also

`kai info syntax`, `kai info match`, `docs/refinements-and-contracts.md`
