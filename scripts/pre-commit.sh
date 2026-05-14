#!/bin/bash
# Pre-commit hook to check C++ formatting

# Find staged C++ files
STAGED_FILES=$(git diff --cached --name-only --diff-filter=ACMR | grep -E '\.(cpp|h|hpp)$')

if [ -z "$STAGED_FILES" ]; then
    exit 0
fi

# Run formatting check via Makefile
make check-format

if [ $? -ne 0 ]; then
    echo "----------------------------------------------------------------"
    echo "FAIL: C++ formatting check failed."
    echo "Please run 'make format' to fix formatting and try again."
    echo "----------------------------------------------------------------"
    exit 1
fi

exit 0
