# Contributing to AgentStateStorage

Thanks for your interest. Issues, design discussion and pull requests are all welcome.

## Before your first pull request: the CLA

Every contributor signs the [Individual Contributor License Agreement](CLA.md) once. The CLA Assistant bot checks each pull request; if you have not signed yet, it posts a link where you sign with your GitHub account. You keep the copyright in your work. The CLA lets the maintainer keep shipping the project under Apache-2.0 and, if needed, under other terms later.

If your employer owns what you write, make sure you have its permission first (CLA §4).

## Build and test

See [BUILD.md](BUILD.md). The short version:

```bash
make all      # C++ (ctest), Go and Python suites
```

CI runs on Ubuntu 22.04 (CMake 3.22, GCC 11, OpenSSL 3.0, RocksDB 6.x) and must stay green. If you change the build, check that it still configures with CMake 3.22.

## Pull requests

- Keep each pull request to one change, with tests.
- Reference the design decision your code implements (the `D-*` / `L*-*` IDs) where one applies.
- Commit messages follow `type(scope): summary`, for example `fix(ci): ...`.
- Do not change the capability matrix in the README unless the code and tests in your pull request back the change.

## Security issues

Do not open a public issue. See [SECURITY.md](SECURITY.md).
