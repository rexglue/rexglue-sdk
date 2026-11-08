# Contributing

See the [Contributing Guide](https://github.com/rexglue/rexglue-sdk/wiki/Development/Contributing) in the wiki for:

- Build prerequisites and setup
- Code style conventions
- Formatting and linting instructions
- Git setup (line endings, rebasing)
- PR submission workflow

## Pull request checks

A maintainer starts the pull request checks by hand, in one of two ways:

- Add the label `run-checks` to the pull request.
- Open the Actions tab, choose the PR checks workflow, press Run workflow and enter the pull request number. The result then appears as the commit statuses `pr-checks/format` and `pr-checks/lint` on the pull request.

The checks build the merge of the pull request with its base and run two jobs:

- `format` checks the C and C++ formatting with clang-format, runs the CMake format and lint check, and runs the script tests. These checks no longer run on every push.
- `lint` runs clang-tidy 20 on the source files the pull request changes, and on the public headers it changes through their header check units. Any warning or error fails the job. To run the same check locally on a configured build directory, run `python scripts/lint_changed.py <build dir> <base sha>`.

A pull request from a fork runs with the default read token and no secrets.
