#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== Formatting C/C++ files ==="
find "$REPO_ROOT/include" "$REPO_ROOT/src" "$REPO_ROOT/tests" \
  -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' -o -name '*.c' \) \
  | xargs clang-format -i

echo "Done."
