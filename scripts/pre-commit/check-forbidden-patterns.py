#!/usr/bin/env python3
"""
Pre-commit hook to detect forbidden patterns in source code.

Checks for patterns that should never be committed (e.g., std::getenv, identifiers
named near/far, which windows.h defines as macros, path::string(), which narrows through
the Windows code page, or QMenu::exec(), which aborts the browser build).
Exit code: 0 if no violations found, 1 if violations found.
"""

import re
import sys
from pathlib import Path
from typing import List, Tuple

CPP_EXTENSIONS = {'.cpp', '.h', '.hpp', '.cxx', '.cc', '.c'}

# Pattern definitions: (regex, message, file_extensions, code_only, skip_tests, exempt_paths).
# A code_only pattern ignores comments and string literals, for words that also occur in prose.
# A skip_tests pattern does not apply under a tests/ directory. A file whose path contains one
# of the exempt_paths fragments is not checked against the pattern.
FORBIDDEN_PATTERNS = [
    (
        r'\bstd::getenv\s*\(',
        "std::getenv is not portable (Windows MSVC C4996); use sdk::getEnv instead",
        CPP_EXTENSIONS,
        False,
        False,
        (),
    ),
    (
        r'\b(near|far|NEAR|FAR)\b',
        "near/far are empty macros on Windows (windows.h, pulled in transitively), so an "
        "identifier with that name breaks the MSVC build; use e.g. near_plane / far_image",
        CPP_EXTENSIONS,
        True,
        False,
        (),
    ),
    (
        r'\.string\(\)',
        "path::string() narrows through the ANSI code page on Windows and throws for a name "
        "outside it; use PJ::pathToUtf8 (pj_marketplace/utf8_path.hpp) for text, or "
        "u16string() / QString::fromStdU16String for a QString",
        CPP_EXTENSIONS,
        True,
        True,
        (),
    ),
    (
        r'(?<![.>])\bpath(?:\s+\w+)?\s*[({][^;]*toStdString\s*\(\)',
        "std::filesystem::path(qstring.toStdString()) decodes the UTF-8 bytes through the ANSI "
        "code page on Windows; use std::filesystem::path(qstring.toStdU16String())",
        CPP_EXTENSIONS,
        True,
        True,
        (),
    ),
    (
        r'(?i:\w*menu\w*)(?:\s*\(\s*\))?\s*(?:\.|->)\s*exec\s*\(|\bQMenu\s*::\s*exec\s*\(|\bQMenu\b[^;]*\)\s*\.\s*exec\s*\(',
        "QMenu::exec() runs a nested event loop, which the browser (WebAssembly) build cannot "
        "enter: it aborts the whole application. Heap-allocate the menu, wire each choice "
        "through a signal handler, and show it with PJ::showOwnedMenu (pj_widgets/OwnedMenu.h); "
        "the check matches variables named *menu*",
        CPP_EXTENSIONS,
        True,
        False,
        ('pj_widgets/src/OwnedMenu.cpp', '3rdparty/'),
    ),
]

_RAW_STRING_LITERAL = re.compile(r'R"([^(\s]*)\(.*?\)\1"')
_STRING_LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')


def code_part(line: str) -> str:
    """The line without string literals and comments (block-comment lines are skipped whole)."""
    stripped = line.lstrip()
    if stripped.startswith(('*', '/*')):
        return ''
    line = _STRING_LITERAL.sub('""', _RAW_STRING_LITERAL.sub('""', line))
    return line.split('//', 1)[0]


def check_file(filepath: str) -> List[Tuple[int, str, str]]:
    """
    Check a file for forbidden patterns.

    Returns: List of (line_number, pattern_message, line_content)
    """
    path = Path(filepath)

    # Check if file extension matches any pattern
    in_tests = 'tests' in path.parts
    posix_path = path.as_posix()
    applicable_patterns = [
        (pattern, message, code_only)
        for pattern, message, extensions, code_only, skip_tests, exempt_paths in FORBIDDEN_PATTERNS
        if path.suffix in extensions
        and not (skip_tests and in_tests)
        and not any(fragment in posix_path for fragment in exempt_paths)
    ]

    if not applicable_patterns:
        return []

    violations = []
    try:
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            for line_num, line in enumerate(f, start=1):
                code = code_part(line)
                for pattern, message, code_only in applicable_patterns:
                    if re.search(pattern, code if code_only else line):
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
