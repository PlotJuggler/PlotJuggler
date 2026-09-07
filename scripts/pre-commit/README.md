# Pre-commit Scripts

## `check-forbidden-patterns.py`

Detects forbidden patterns in C++ source files that should never be committed.

### Current Patterns

- **`std::getenv()`** — Not portable (Windows MSVC C4996 warning); use `sdk::getEnv()` instead.

### Extending with New Patterns

Edit the `FORBIDDEN_PATTERNS` list in `check-forbidden-patterns.py` to add new checks:

```python
FORBIDDEN_PATTERNS = [
    (
        r'\bstd::getenv\s*\(',  # regex pattern
        "Error message shown to user",  # descriptive message
        {'.cpp', '.h', '.hpp', '.cxx', '.cc', '.c'},  # file extensions to check
    ),
    # Add new patterns here:
    (
        r'malloc\s*\(',  # example: disallow malloc
        "Use std::make_unique or std::make_shared instead of malloc",
        {'.cpp', '.h', '.hpp'},
    ),
]
```

### Testing

Test the script manually before running via pre-commit:

```bash
# Test on a specific file
scripts/pre-commit/check-forbidden-patterns.py path/to/file.cpp

# Exit code 0 = no violations, 1 = violations found
```

### Running via Pre-commit

```bash
# Run only this hook
pre-commit run check-forbidden-patterns --all-files

# Run all hooks on staged files
pre-commit run
```

### Notes

- Patterns are regex-based (`re.search()`)
- Each file is checked against only the extensions listed in its pattern entry
- Line content is printed for context
- The hook is automatically run before commits (configured in `.pre-commit-config.yaml`)
