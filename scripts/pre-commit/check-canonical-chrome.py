#!/usr/bin/env python3
"""Pre-commit hook: forbid system/GNOME-chromed dialogs — enforce canonical chrome.

PJ4 has one canonical dialog family in ``pj_widgets`` that draws its own frameless,
theme-painted chrome (no window-manager title bar):

    PJ::Dialog       instead of  QDialog          (pj_widgets/Dialog.h)
    PJ::MessageBox   instead of  QMessageBox       (pj_widgets/MessageBox.h)
    PJ::FileDialog   instead of  QFileDialog       (pj_widgets/FileDialog.h)

A raw ``QDialog`` / ``QMessageBox`` / ``QFileDialog`` gets the OS decorations,
which don't match the app. This hook fails the commit if one is introduced
outside the canonical home, so "always the same object" is enforced, not just
hoped for.

Exemptions:
  * ``pj_widgets/`` — the canonical wrappers are IMPLEMENTED here (they wrap the
    raw Qt classes on purpose).
  * ``tests/`` dirs and ``*_test.*`` files, ``demos/`` dirs — not shipped UI.
  * A line carrying ``NOLINT(pj-canonical-chrome)`` — a reviewed exception (e.g.
    hosting a plugin-PROVIDED widget tree that the host canonicalizes at runtime).

Exit code: 0 clean, 1 on any violation.
"""

import re
import sys
from pathlib import Path
from typing import List, Tuple

# (regex, human message). Patterns are deliberately narrow so they match real
# construction / static-call sites — NOT comments, #include lines, base-class
# method calls (``QDialog::accept``), pointer casts (``qobject_cast<QDialog*>``),
# or unrelated types (``QDialogButtonBox``).
_PATTERNS: List[Tuple[re.Pattern, str]] = [
    (re.compile(r":\s*public\s+QDialog\b"),
     "derive from PJ::Dialog (pj_widgets/Dialog.h), not QDialog — QDialog shows OS chrome"),
    (re.compile(r"\bnew\s+QDialog\b"),
     "construct PJ::Dialog (pj_widgets/Dialog.h), not QDialog"),
    (re.compile(r"\bQDialog\s+[A-Za-z_]\w*\s*[({]"),
     "use PJ::Dialog (pj_widgets/Dialog.h) for a stack dialog, not QDialog"),
    (re.compile(r"\bQMessageBox\s*::|(?:\bnew\s+QMessageBox\b)|\bQMessageBox\s+[A-Za-z_]\w*\s*[({]"),
     "use PJ::MessageBox (pj_widgets/MessageBox.h), not QMessageBox"),
    (re.compile(r"\bQFileDialog\s*::|(?:\bnew\s+QFileDialog\b)|\bQFileDialog\s+[A-Za-z_]\w*\s*[({]"),
     "use PJ::FileDialog (pj_widgets/FileDialog.h), not QFileDialog"),
    (re.compile(r"\bQInputDialog\s*::|(?:\bnew\s+QInputDialog\b)"),
     "use a PJ::Dialog-based input instead of QInputDialog (OS chrome)"),
]

_SRC_SUFFIXES = {".cpp", ".h", ".hpp", ".cxx", ".cc", ".c"}
_NOLINT = "NOLINT(pj-canonical-chrome)"


def _exempt(path: Path) -> bool:
    parts = path.as_posix()
    if path.suffix not in _SRC_SUFFIXES:
        return True
    # pj_widgets is where the canonical wrappers wrap the raw Qt classes.
    if "pj_widgets/" in parts:
        return True
    if "/tests/" in parts or parts.startswith("tests/") or "_test." in path.name:
        return True
    if "/demos/" in parts or parts.startswith("demos/"):
        return True
    if "3rdparty/" in parts or "plotjuggler_sdk/" in parts or "/build" in parts:
        return True
    return False


def check_file(filepath: str) -> List[Tuple[int, str, str]]:
    path = Path(filepath)
    if _exempt(path):
        return []
    violations: List[Tuple[int, str, str]] = []
    try:
        with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
            for line_num, line in enumerate(f, start=1):
                if _NOLINT in line:
                    continue
                for pattern, message in _PATTERNS:
                    if pattern.search(line):
                        violations.append((line_num, message, line.rstrip()))
                        break
    except OSError as e:
        print(f"Error reading {filepath}: {e}", file=sys.stderr)
    return violations


def main(argv: List[str] = None) -> int:
    argv = argv if argv is not None else sys.argv[1:]
    found = False
    for filepath in argv:
        for line_num, message, line_content in check_file(filepath):
            print(f"{filepath}:{line_num}: {message}")
            print(f"  {line_content}")
            found = True
    if found:
        print(
            "\nCanonical chrome required. Use PJ::Dialog / PJ::MessageBox / PJ::FileDialog "
            "(pj_widgets), or, for a reviewed exception (e.g. hosting a plugin-PROVIDED "
            "widget), add a trailing '// NOLINT(pj-canonical-chrome)' with a reason.",
            file=sys.stderr,
        )
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
