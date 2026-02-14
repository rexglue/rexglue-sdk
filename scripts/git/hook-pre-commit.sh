#!/bin/bash
# Pre-commit hook: run clang-format on staged C/C++ files

CLANG_FORMAT="clang-format"

# Find staged C/C++ files
STAGED_FILES=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(cpp|hpp|c|h|cc|hh|cxx|hxx)$')

if [ -z "$STAGED_FILES" ]; then
    exit 0
fi

# Format each staged file
for file in $STAGED_FILES; do
    "$CLANG_FORMAT" -i "$file"
    git add "$file"
done

echo "clang-format applied to staged files."
exit 0
