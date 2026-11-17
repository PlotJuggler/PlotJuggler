# Pre-commit scripts

The local C/C++ hooks of `.pre-commit-config.yaml`. Each prints the offending line and exits 1 on a violation, 0
when clean.

## `check-forbidden-patterns.py`

Regex checks for constructs that break a shipped platform:

- `std::getenv()`: not portable (MSVC C4996); use `sdk::getEnv()`.
- Identifiers named `near` / `far`: empty macros in `windows.h`.
- `path::string()`: narrows through the ANSI code page on Windows; use `PJ::pathToUtf8`.
- `std::filesystem::path(qstring.toStdString())`: decodes UTF-8 through the ANSI code page; use `toStdU16String()`.

To add a check, append a tuple to `FORBIDDEN_PATTERNS` in the script: the regex, the message, the file extensions it
applies to, `code_only` (ignore comments and string literals, for words that also occur in prose) and `skip_tests`
(do not apply under a `tests/` directory).

## `check-canonical-chrome.py`

Fails on a raw `QDialog`, `QMessageBox`, `QFileDialog` or `QInputDialog` (OS window decorations); use `PJ::Dialog`,
`PJ::MessageBox` or `PJ::FileDialog` from `pj_widgets`. Exempt: `pj_widgets/` (the wrappers), `tests/` and `demos/`
directories, `*_test.*` files, and a line carrying `NOLINT(pj-canonical-chrome)` (a reviewed exception).

## Running

```bash
scripts/pre-commit/check-forbidden-patterns.py path/to/file.cpp      # one hook on specific files
pre-commit run check-canonical-chrome --all-files                    # one hook through pre-commit
```
