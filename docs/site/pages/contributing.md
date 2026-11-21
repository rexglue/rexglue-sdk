# Contributing

## Building

Install the tools and build the SDK as the [Getting Started](getting-started.md) guide shows. The SDK needs CMake 3.25 or newer, Clang 20 or newer (Apple Clang 17 or newer on macOS), lld 20 on Linux x86_64, and Ninja. On Linux the SDK packages are built against the GCC 13 libstdc++, so also install `g++-13` and `libstdc++-13-dev` and configure with `-DREXGLUE_GCC_INSTALL_DIR=/usr/lib/gcc/<triple>/13`, where `<triple>` is `x86_64-linux-gnu` or `aarch64-linux-gnu`. A project that uses these packages must build against libstdc++ 13 too.

On Windows, the `scripts/PSReX` PowerShell module wraps the CMake steps. Run `Import-Module ./scripts/PSReX`, then `rex-configure`, `rex-build`, `rex-test` and `rex-format`.

## Running the tests

The test programs are not part of the default build. The `rextest_check_all` target builds every test program and then runs ctest for the configuration that you build. With a configured build folder, run

```
cmake --build out/build/<preset> --config Release --target rextest_check_all
```

Replace `<preset>` with the preset you configured, such as `win-amd64` or `linux-amd64`. Use `--config Debug` to test a Debug build. A failing test prints its output and fails the target. To run one test, build `rexglue_tests` and call `ctest -C <config> -R <name>` in the build folder.

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

## Changing an interface

An interface is an abstract class declared with `REX_DECLARE_INTERFACE(Name, version)` that an [SDK module](reference/glossary.md#sdk-module) serves to other modules (see [Declaring an Interface](app/modules.md#declaring-an-interface)). If you change a function of an interface, the order of its functions, or a type passed through it, raise the version number in the same commit. Never lower or reuse a number. The SDK provides only the newest version, so code built against the old one stops loading (see [Versions and Builds](app/modules.md#versions-and-builds)). A reviewer checks the interface headers of every pull request for such a change.

## Benchmarks

A test case tagged `[benchmark]` is a benchmark. ctest runs each of them once, with `--benchmark-samples 1 --benchmark-warmup-time 0`, set in `tests/unit/CMakeLists.txt`, so that a check shows that the case runs. Those runs give no figures. Figures come from the `rextest_benchmark_all` target, which builds `unit_tests`, runs every benchmark with 100 samples and writes the results to a CSV file.

```
cmake --build out/build/<preset> --config Release --target rextest_benchmark_all
```

Use a Release build, since Debug figures do not describe the speed of the SDK. Catch2 prints its table to the terminal during the run. The CSV file is `rextest/rextest-benchmark-<commit>-<config>-<time>.csv` in the build folder, and every run writes a new file. [Running the Benchmarks](running-benchmarks.md) lists the columns and the steps for a person who only wants to produce the figures.

Two cache variables change the run. `REXTEST_BENCHMARK_FILTER` is the Catch2 test spec and defaults to `[benchmark]`. `REXTEST_BENCHMARK_SAMPLES` is the number of samples and defaults to 100. Set them at configure time, for example `-DREXTEST_BENCHMARK_FILTER="[benchmark]" -DREXTEST_BENCHMARK_SAMPLES=50`. A filter that matches no case fails the target.

A benchmark times one operation. Lookups, allocation and thread start stay outside the timed region, and each group has a baseline, an empty call through the same path, whose cost is read beside the result. A case that needs threads to overlap starts them once and releases them together through `ThreadPool` in `tests/unit/benchmark_pool.h`, which also reports how long they ran together. A report names the host, whether the run was pinned, the number of runs, and the median with the spread.

To compare a change with its base, run `rextest_benchmark_all` in two worktrees, one for each, and compare the two CSV files. Runs of the same code taken at different times on a shared host have differed by up to 2x. Alternate the two builds several times, and read each figure beside the baseline case of its own run.

## Pull request checks

A maintainer starts the pull request checks by hand, in one of two ways:

- Add the label `run-checks` to the pull request.
- Open the Actions tab, choose the PR checks workflow, press Run workflow and enter the pull request number. The result then appears as the commit statuses `pr-checks/format` and `pr-checks/lint` on the pull request.

The checks build the merge of the pull request with its base and run two jobs:

- `format` checks the C and C++ formatting with clang-format, runs the CMake format and lint check, and runs the script tests.
- `lint` runs clang-tidy 22 through `scripts/lint.py` on the source files the pull request changes, and on the public headers it changes, each compiled alone through the targets that `REXGLUE_VERIFY_HEADERS` creates. Any warning or error fails the job. To run the same check locally on a configured build directory, use `rex-lint` on Windows or run `python scripts/lint.py <build dir> --since development`. Add `--all` instead of `--since` to check the whole tree. The build directory needs `-DREXGLUE_VERIFY_HEADERS=ON` for the public headers to be checked. The job also builds the SDK with its unit tests and runs them with ctest. A failing test fails the job, the same as a clang-tidy warning.

The checks need no secrets, so they also run for pull requests from forks.
