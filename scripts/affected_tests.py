#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""Pick the ctest cases a change can affect, from the build tree's own graph.

Changed files (git diff against the merge base with origin/main, untracked files included, or the paths given) map
to the CMake targets that own them; every target depending on those, transitively, is affected; a ctest case is
selected when its command names an affected target's artifact or a changed file. Prints the ctest -R regex on stdout
(^$, which no case matches, when none is affected) and the reasoning on stderr:

    ./test.sh -R "$(scripts/affected_tests.py)"

Ownership: a target owns the files it lists as sources, the files its .qrc lists, and, failing those, every file under
its CMakeLists directory except Markdown (the deepest one wins; the top-level directory owns nothing this way). A
module's CMakeLists.txt is owned by every target at or below its directory. The top-level CMakeLists.txt, any .cmake
file and the dependency pins select every case.

Not seen: code reached only at run time (plugins a test loads without depending on them, files read by path from
outside the owning directory), and other repositories (plotjuggler_sdk, plugins).

The graph comes from the CMake File API reply in the build tree. The first run writes the query and reconfigures;
every later configure (each ./build.sh) refreshes the reply.
"""
import argparse
import json
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path, PurePosixPath

CONFIGURE_FILES = {"conanfile.txt", "conan.lock", "versions.env"}
# An empty -R would select every case.
NOTHING = "^$"


def git(repo, *args):
    return subprocess.run(["git", "-C", str(repo), *args], check=True, capture_output=True, text=True).stdout


def changed_files(repo, base):
    if base is None:
        base = git(repo, "merge-base", "origin/main", "HEAD").strip()
    names = git(repo, "diff", "--name-only", base).splitlines()
    names += git(repo, "ls-files", "--others", "--exclude-standard").splitlines()
    return sorted(set(names))


def load_codemodel(build):
    api = build / ".cmake" / "api" / "v1"
    query = api / "query" / "codemodel-v2"
    if not query.exists():
        query.parent.mkdir(parents=True, exist_ok=True)
        query.touch()
        print(f"affected_tests: enabling the CMake File API in {build} (one reconfigure)", file=sys.stderr)
        subprocess.run(["cmake", str(build)], check=True, stdout=subprocess.DEVNULL)
    indexes = sorted((api / "reply").glob("index-*.json"))
    if not indexes:
        sys.exit(f"affected_tests: no File API reply in {build}; run ./build.sh first")
    index = json.loads(indexes[-1].read_text())
    entry = next(r for k, r in index["reply"].items() if k.startswith("codemodel-v2"))
    codemodel = json.loads((api / "reply" / entry["jsonFile"]).read_text())
    config = codemodel["configurations"][0]
    targets = {}
    for t in config["targets"]:
        targets[t["id"]] = json.loads((api / "reply" / t["jsonFile"]).read_text())
    return Path(codemodel["paths"]["source"]), targets


def qrc_files(source_root, qrc_rel):
    qrc = source_root / qrc_rel
    try:
        root = ET.parse(qrc).getroot()
    except (OSError, ET.ParseError):
        return []
    base = PurePosixPath(qrc_rel).parent
    return [str(base / f.text.strip()) for f in root.iter("file") if f.text]


def owners_of(path, explicit, dir_owners):
    """
    >>> explicit = {"m/a.cpp": {"lib"}}
    >>> dirs = {"m": {"lib"}, "m/tests": {"t"}}
    >>> [sorted(owners_of(p, explicit, dirs)) for p in ("m/a.cpp", "m/tests/data.json", "m/x.h", "m/README.md")]
    [['lib'], ['t'], ['lib'], []]
    >>> sorted(owners_of("m/CMakeLists.txt", explicit, dirs)), sorted(owners_of("docs/x.txt", explicit, dirs))
    (['lib', 't'], [])
    """
    if path in explicit:
        return explicit[path]
    p = PurePosixPath(path)
    if p.suffix == ".md":
        return set()
    if p.name == "CMakeLists.txt":
        # Its variables and targets reach only its own directory and the ones below it.
        d = str(p.parent)
        return {t for td, ts in dir_owners.items() if td == d or td.startswith(d + "/") for t in ts}
    for parent in p.parents:
        if str(parent) == ".":
            break
        if str(parent) in dir_owners:
            return dir_owners[str(parent)]
    # A directory path (a submodule bump): every target whose directory lies under it.
    return {t for d, ts in dir_owners.items() if d.startswith(path + "/") for t in ts}


def needs_everything(path):
    return path == "CMakeLists.txt" or path.endswith(".cmake") or path in CONFIGURE_FILES


def ctest_regex(selected, all_names):
    """Shortest safe regex: ^prefix\\. for a runner whose every case is selected, exact names otherwise.

    >>> ctest_regex({"a.X.1", "a.X.2", "b.Y.1", "solo"}, {"a.X.1", "a.X.2", "b.Y.1", "b.Y.2", "solo", "other"})
    '^(a\\\\.|b\\\\.Y\\\\.1$|solo$)'
    """
    by_prefix = defaultdict(set)
    for name in all_names:
        by_prefix[name.split(".", 1)[0]].add(name)
    parts = []
    for prefix, names in sorted(by_prefix.items()):
        chosen = names & selected
        if not chosen:
            continue
        if chosen == names and any("." in n for n in names):
            parts.append(re.escape(prefix) + r"\.")
        else:
            parts += [re.escape(n) + "$" for n in sorted(chosen)]
    return "^(" + "|".join(parts) + ")"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("files", nargs="*", help="repo-relative paths (default: the git diff)")
    parser.add_argument("--base", help="git ref to diff against (default: merge base with origin/main)")
    parser.add_argument("--build-dir", type=Path, help="build tree (default: <repo>/build)")
    args = parser.parse_args()

    repo = Path(git(Path.cwd(), "rev-parse", "--show-toplevel").strip())
    build = (args.build_dir or repo / "build").resolve()
    files = args.files or changed_files(repo, args.base)
    if not files:
        print("affected_tests: no changed files", file=sys.stderr)
        print(NOTHING)
        return

    source_root, targets = load_codemodel(build)
    explicit, dir_owners, rdeps, artifacts = defaultdict(set), defaultdict(set), defaultdict(set), defaultdict(set)
    for tid, t in targets.items():
        directory = t["paths"]["source"]
        if directory != ".":
            dir_owners[directory].add(tid)
        for s in t.get("sources", []):
            explicit[s["path"]].add(tid)
            if s["path"].endswith(".qrc"):
                for f in qrc_files(source_root, s["path"]):
                    explicit[f].add(tid)
        for d in t.get("dependencies", []):
            rdeps[d["id"]].add(tid)
        for a in t.get("artifacts", []):
            artifacts[tid].add((build / a["path"]).as_posix())

    tests = json.loads(
        subprocess.run(["ctest", "--show-only=json-v1"], cwd=build, check=True, capture_output=True, text=True).stdout
    )["tests"]
    all_names = {t["name"] for t in tests}

    configure = [f for f in files if needs_everything(f)]
    if configure:
        print(f"affected_tests: configure-level change ({', '.join(configure)}): every case", file=sys.stderr)
        print(".")
        return

    affected, unmapped = set(), []
    for f in files:
        owners = owners_of(f, explicit, dir_owners)
        if not owners:
            unmapped.append(f)
        affected |= owners
        names = sorted(targets[o]["name"] for o in owners)
        print(f"  {f} -> {', '.join(names) or '(no target)'}", file=sys.stderr)
    stack = list(affected)
    while stack:
        for dependent in rdeps[stack.pop()] - affected:
            affected.add(dependent)
            stack.append(dependent)

    needles = {a for tid in affected for a in artifacts[tid]} | {(source_root / f).as_posix() for f in files}
    selected, display = set(), set()
    for t in tests:
        if any(n in arg for arg in t.get("command", []) for n in needles):
            selected.add(t["name"])
            labels = next((p["value"] for p in t.get("properties", []) if p["name"] == "LABELS"), [])
            if any(label.startswith("qrhi") for label in labels):
                display.add(t["name"].split(".", 1)[0])

    if unmapped:
        print(f"affected_tests: no target owns {len(unmapped)} file(s); judge those by hand", file=sys.stderr)
    runners = sorted({n.split(".", 1)[0] for n in selected})
    print(f"affected_tests: {len(selected)} of {len(all_names)} cases in {len(runners)} runner(s)", file=sys.stderr)
    if display:
        print(f"affected_tests: QRhi runners need ./test.sh --display: {', '.join(sorted(display))}", file=sys.stderr)
    print(ctest_regex(selected, all_names) if selected else NOTHING)


if __name__ == "__main__":
    main()
