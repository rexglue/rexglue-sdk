# Contributing

## Building

Install the tools and build the SDK as the [Getting Started](getting-started.md) guide shows. The SDK needs CMake 3.25 or newer, Clang 20 or newer (Apple Clang 17 or newer on macOS), lld 20 on Linux x86_64, and Ninja. On Linux the SDK packages are built against the GCC 13 libstdc++, so also install `g++-13` and `libstdc++-13-dev` and configure with `-DREXGLUE_GCC_INSTALL_DIR=/usr/lib/gcc/<triple>/13`. A project must build against the same major version.

On Windows, the `scripts/PSReX` PowerShell module wraps the CMake steps. Run `Import-Module ./scripts/PSReX`, then `rex-configure`, `rex-build`, `rex-test` and `rex-format`.

## Code style

The code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html), with the formatting in `.clang-format` and the naming checks in `.clang-tidy`. Format with clang-format before you commit.

| Element | Convention | Example |
| --- | --- | --- |
| Classes and structs | PascalCase | `CommandProcessor` |
| Functions and methods | PascalCase | `Initialize()` |
| Local variables and parameters | snake_case | `vertex_count` |
| Member variables | snake_case with a trailing `_` | `chunk_size_` |
| Constants and enumerators | `k` and PascalCase | `kMaxBufferSize` |
| Namespaces | lowercase | `rex::graphics` |
| Macros | UPPER_CASE | `REX_RESTRICT_VAR` |

Headers use `#pragma once`. SDK includes use angle brackets (`<rex/...>`) and come last, after the C, C++, platform and third party headers. Prefer `using` over `typedef`.

## Git workflow

Files use LF line endings only. On Windows, run `git config core.autocrlf input` and set your editor to LF.

Pull requests target the `development` branch of rexglue/rexglue-sdk and merge into it with a merge commit. `main` only takes release branches, as [RELEASING.md](https://github.com/rexglue/rexglue-sdk/blob/development/docs/RELEASING.md) explains. To submit a change:

1. Open an issue for the change if none exists.
2. Create a branch from `development` in your fork.
3. Bring your branch up to date with the latest upstream `development` before you push, and resolve any conflicts.
4. Open a pull request against `development` and link the issue in the description.

## Pull request checks

A maintainer starts the pull request checks by hand, in one of two ways:

- Add the label `run-checks` to the pull request.
- Open the Actions tab, choose the PR checks workflow, press Run workflow and enter the pull request number. The result then appears as the commit statuses `pr-checks/format` and `pr-checks/lint` on the pull request.

The checks build the merge of the pull request with its base and run two jobs:

- `format` checks the C and C++ formatting with clang-format, runs the CMake format and lint check, and runs the script tests.
- `lint` runs clang-tidy 22 through `scripts/lint.py` on the source files the pull request changes, and on the public headers it changes through their header check units. Any warning or error fails the job. To run the same check locally on a configured build directory, use `rex-lint` on Windows or run `python scripts/lint.py <build dir> --since development`. Add `--all` instead of `--since` to check the whole tree. The build directory needs `-DREXGLUE_VERIFY_HEADERS=ON` for the public headers to be checked. The job also builds the SDK with its unit tests and runs them with ctest. A failing test fails the job, the same as a clang-tidy warning.

A pull request from a fork runs with the default read token and no secrets.
