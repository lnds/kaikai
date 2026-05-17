# examples/packages/build_sub_package

Issue #658 fixture — exercises `kai build ./sub` against a
directory whose `kai.toml` declares it as its own package. Two
independent binaries (`build_sub_package_parent` and `sub_pkg`)
sit in the same tree.

Reproduce:

```sh
cd examples/packages/build_sub_package
kai build                                # parent package
./build_sub_package_parent               # matches main.out.expected

kai build ./sub                          # sub-package
./sub/sub_pkg                            # matches sub/main.out.expected
```

A directory without its own `kai.toml` is not a sub-package; the
driver rejects it:

```sh
$ kai build ./not_a_package
kai: error: ./not_a_package has no kai.toml — sub-packages must
declare their own manifest
```
