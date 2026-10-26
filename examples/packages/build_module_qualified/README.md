# examples/packages/build_module_qualified

Exercises `import <pkgname>.<module>` from within the same
package. The manifest names the package
`build_module_qualified`; `main.kai` imports
`build_module_qualified.store`. The package-name qualifier resolves
to the package root, so the import finds `store.kai` there.

Bare `import store` resolves the same module: the manifest dir itself
is a search path.

Reproduce:

```sh
cd examples/packages/build_module_qualified
kai build                       # produces ./build_module_qualified
./build_module_qualified        # matches main.out.expected
```
