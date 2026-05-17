# examples/packages/build_hello

Issue #658 fixture — exercises `kai build` with no argument from a
package directory. The driver reads `kai.toml` for the package name
and entry-point default (`main.kai`) and writes the binary to the
package name (`./build_hello`).

Reproduce:

```sh
cd examples/packages/build_hello
kai build           # produces ./build_hello
./build_hello       # matches main.out.expected

kai build .         # the dot is an equivalent explicit form
```
