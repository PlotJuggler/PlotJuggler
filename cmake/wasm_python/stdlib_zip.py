# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0
"""stdlib_zip.py <Lib dir> <out.zip>: the pyc-only stdlib zip the browser build embeds.

Run by the native build Python of the same CPython release (`python -I`), so the bytecode matches the interpreter
that reads it. The zip holds the modules that importing ROOTS loads and nothing else (the full stdlib would add about
2.3 MB Brotli). A module imported lazily, inside a function, is missed unless a root names it; python_sandbox.cpp loads
the ones a strict script reaches at boot, and the browser acceptance probe (pj_app/tests/wasm_python_stdlib_probe.cpp)
fails on one it does not, which is the signal to add it there and here.

The zip is deterministic (fixed entry times, unchecked hash-based pycs, no build paths in the code objects) and stored
uncompressed: the package's Brotli compresses the whole wasm module better than deflate would compress each entry.
"""
import sys

ROOTS = [
    # python_sandbox.cpp: the boot's pre-imports, which include the strict profile's modules (kStrictImports).
    "ast", "types", "json", "math", "cmath", "statistics",
    # The boot itself and a script error's report.
    "site", "os", "stat", "posixpath", "genericpath", "codecs", "io", "abc", "zipimport",
    "encodings.utf_8", "encodings.latin_1", "encodings.ascii",
    "traceback", "linecache", "tokenize", "warnings",
    # What python_sandbox.cpp loads at boot for the allowed modules' lazy imports (kLazyDependencies and the warmed
    # decimal fallback); the acceptance probe fails on one a strict script reaches that is not loaded.
    "heapq",  # statistics re-exports collections.Counter, whose most_common(n) imports it
    "_pydecimal",  # decimal's C format falls back to it for a spec libmpdec lacks
]


def main():
    for name in ROOTS:
        __import__(name)
    loaded = list(sys.modules.values())
    # Only now: what this script needs must not join the zip.
    import importlib.util
    import pathlib
    import zipfile

    lib = pathlib.Path(sys.argv[1]).resolve()
    out = pathlib.Path(sys.argv[2])
    files = {}
    for module in loaded:
        path = getattr(module, "__file__", None)
        if path and path.endswith(".py"):
            path = pathlib.Path(path).resolve()
            if lib in path.parents:
                files[path.relative_to(lib).with_suffix(".pyc").as_posix()] = path

    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as archive:
        for arcname, path in sorted(files.items()):
            source = path.read_bytes()
            code = compile(source, f"/lib/{out.name}/{arcname[:-1]}", "exec", dont_inherit=True)
            pyc = importlib._bootstrap_external._code_to_hash_pyc(
                code, importlib.util.source_hash(source), checked=False)
            archive.writestr(zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0)), pyc)
    print(f"{out}: {len(files)} modules")


main()
