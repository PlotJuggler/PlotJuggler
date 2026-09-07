#!/usr/bin/env python3
"""
Pre-commit hook to detect forbidden patterns in source code.

Checks for patterns that should never be committed (e.g., std::getenv on Windows).
Exit code: 0 if no violations found, 1 if violations found.
"""

import re
import sys
from pathlib import Path
from typing import List, Tuple

# Pattern definitions: (regex, message, file_extensions)
FORBIDDEN_PATTERNS = [
    (
        r'\bstd::getenv\s*\(',
        "std::getenv is not portable (Windows MSVC C4996); use sdk::getEnv instead",
        {'.cpp', '.h', '.hpp', '.cxx', '.cc', '.c'},
    ),
]


def check_file(filepath: str) -> List[Tuple[int, str, str]]:
    """
    Check a file for forbidden patterns.

    Returns: List of (line_number, pattern_message, line_content)
    """
    path = Path(filepath)

    # Check if file extension matches any pattern
    applicable_patterns = [
        (pattern, message) for pattern, message, extensions in FORBIDDEN_PATTERNS
        if path.suffix in extensions
    ]

    if not applicable_patterns:
        return []

    violations = []
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            for line_num, line in enumerate(f, start=1):
                for pattern, message in applicable_patterns:
                    if re.search(pattern, line):
                        violations.append((line_num, message, line.rstrip()))
    except Exception as e:
        print(f"Error reading {filepath}: {e}", file=sys.stderr)

    return violations


def main(argv: List[str] = None) -> int:
    """Check files for forbidden patterns."""
    argv = argv or sys.argv[1:]

    if not argv:
        print("Usage: check-forbidden-patterns.py <file1> [file2] ...", file=sys.stderr)
        return 0

    found_violations = False

    for filepath in argv:
        violations = check_file(filepath)
        for line_num, message, line_content in violations:
            print(f"{filepath}:{line_num}: {message}")
            print(f"  {line_content}")
            found_violations = True

    return 1 if found_violations else 0


if __name__ == '__main__':
    sys.exit(main())
