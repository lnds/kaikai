# same_name_shadowing — package fn vs stdlib fn

The package `mypkg` declares `pub fn length(label: String) :
String` in `mypkg/util.kai`. The stdlib also exports `list.length`
(and `string.length`) — both colliding on the bare name `length`.

The qualified call `util.length(...)` must resolve to the
package's own definition. The stdlib's bare `string_length` and
`int_to_string` builtins remain available through the core.

Run:

```sh
bin/kai run examples/packages/same_name_shadowing
```

Expected output:

```text
len:hello
5
```

Regression guard for resolution-precedence rules between
package-qualified names and stdlib qualified surface.
