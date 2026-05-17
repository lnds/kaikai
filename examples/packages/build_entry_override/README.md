# examples/packages/build_entry_override

Issue #658 fixture — exercises `kai.toml` `entry = "src/main.kai"`
as an explicit override of the conventional `main.kai`. Mirrors
Cargo's `[[bin]] path =` shape for packages that keep sources
under `src/`.

Reproduce:

```sh
cd examples/packages/build_entry_override
kai build                   # uses src/main.kai per the manifest
./build_entry_override      # matches main.out.expected
```

Errors when the manifest points at a missing file:

```sh
$ rm src/main.kai
$ kai build
kai: error: kai.toml declares entry = 'src/main.kai' but
src/main.kai does not exist
```
