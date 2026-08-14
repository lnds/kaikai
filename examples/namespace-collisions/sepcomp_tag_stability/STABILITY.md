Tag stability under partial recompilation: adding a constructor to one
module must not renumber the tags already baked into another module's
emitted code. This is the property that forbids a globally dense tag
index, and the reason the fix for homonymous constructors is to change
the hash KEY to (home, ctor) rather than to replace the scheme.

Measured on the current compiler: inserting `AAAFirst` into `sb.kai` —
a name that sorts before `Alpha1` — leaves `Alpha1`'s tag at 1,
unchanged. The property holds today and must survive the fix.
