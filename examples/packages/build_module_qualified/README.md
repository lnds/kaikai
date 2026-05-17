# examples/packages/build_module_qualified

Issue #658 fixture — exercises `import <pkgname>.<module>` from
within the same package. The manifest names the package
`build_module_qualified`; `main.kai` imports
`build_module_qualified.store`. The driver makes the manifest's
parent directory a search path so kaic2's existing `module_to_path`
(`a.b -> a/b.kai`) finds `build_module_qualified/store.kai` on the
first try.

Bare `import store` keeps working — the manifest dir itself is the
primary search path. This fixture asserts both forms coexist.

Reproduce:

```sh
cd examples/packages/build_module_qualified
kai build                       # produces ./build_module_qualified
./build_module_qualified        # matches main.out.expected
```
