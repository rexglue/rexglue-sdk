#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/out/build/linux-amd64}"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
  echo "Error: compile_commands.json not found at $BUILD_DIR"
  echo "Usage: $0 [build-dir]"
  echo "Generate it first: cmake --preset linux-amd64"
  exit 1
fi

echo "=== Running clang-tidy ==="
find "$REPO_ROOT/include" "$REPO_ROOT/src" \
  -type f \( -name '*.cpp' -o -name '*.h' \) \
  | xargs clang-tidy --config-file="$REPO_ROOT/.clang-tidy" -p "$BUILD_DIR"

echo "Done."
