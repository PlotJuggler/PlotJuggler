# SPDX-License-Identifier: MPL-2.0
"""Prune the repository's GitHub Actions caches to the newest entry per family.

GitHub gives each repository a single ~10 GB Actions cache budget shared across
*all* workflows and branches, and LRU-evicts (by last-accessed) above it. Rolling
per-SHA cache keys (``ccache-linux-main-<sha>``, ``conan-...-<hash>``) write a
fresh large blob on every saving run, so the budget fills with stale duplicates
and starts evicting the infrequently-accessed entries.

Scope: this reaches the *GitHub* cache store only — the management endpoint
(``/repos/{owner}/{repo}/actions/caches``) is served by GitHub, while jobs on
``depot-*`` runners have their cache traffic redirected to Depot Cache, whose
entries this API neither lists nor deletes. Depot Cache is bounded by the
retention policy in the Depot organisation settings and by the push-only save
gating in the workflows, not by this script. See ``cache-cleanup.yml``.

Since every consumer restores via ``restore-keys`` prefix (always the most
recent match), only the newest entry per (ref, key-family) is useful; the rest
are dead weight. This script deletes them. It also removes every cache scoped
to a closed PR. The pull-request ``closed`` cleanup can run before that PR's
in-flight jobs have finished their post-job cache saves; without this second
pass, those late entries survive forever because there is no second event.

It groups caches by (git ref, key-family), where the family is the key with its
trailing rolling suffix (``-<7..64 hex>``) stripped, then keeps the newest
``--keep`` per group and deletes the older ones via the REST API. Version- or
content-stable keys (Qt, pre-commit) have no hex suffix, form singleton
families, and are therefore never touched.

Two further classes of dead weight are removed outright:

* caches scoped to ``refs/heads/*`` whose branch no longer exists (a merged
  feature branch's conan/ccache archives otherwise linger until the 7-day TTL);
* caches scoped to ``refs/tags/*`` older than a grace window — tag runs can
  RESTORE from the default branch but nothing can ever restore from another
  tag's ref, so tag-scoped saves are write-only garbage (the grace window keeps
  same-tag re-runs of a failed release warm).

Dry-run by default; pass ``--apply`` to actually delete. Auth and transport go
through the ``gh`` CLI (``GH_TOKEN`` in CI), so no token handling lives here.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys

# Rolling per-SHA / per-conanfile-hash suffix appended to a stable key stem.
_ROLLING_SUFFIX = re.compile(r"-[0-9a-f]{7,64}$")
_PR_CACHE_REF = re.compile(r"^refs/pull/[0-9]+/merge$")
_BRANCH_CACHE_REF = re.compile(r"^refs/heads/(.+)$")
_TAG_CACHE_REF = re.compile(r"^refs/tags/")

# Keep tag-scoped caches this long so a re-run of a just-failed release still
# restores its own saves; beyond it they are unreachable by any other ref.
_TAG_GRACE = datetime.timedelta(hours=48)


def _family(key: str) -> str:
    """Key stem shared by every rolling variant (suffix stripped)."""
    return _ROLLING_SUFFIX.sub("", key)


def _delete_cache(repo: str, cache: dict) -> None:
    """Delete one entry, tolerating races: a PR-close cleanup or GitHub's own
    eviction can remove it between our list call and this delete."""
    try:
        _gh_api(repo, f"actions/caches/{cache['id']}", method="DELETE")
    except subprocess.CalledProcessError as error:
        print(f"  (delete failed for {cache['key']}, continuing: {error.stderr.strip()})")


def _gh_api(repo: str, path: str, method: str | None = None) -> str:
    args = ["gh", "api"]
    if method:
        args += ["--method", method]
    args.append(f"/repos/{repo}/{path}")
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def _list_caches(repo: str) -> list[dict]:
    """All cache entries, following pagination."""
    caches: list[dict] = []
    page = 1
    while True:
        batch = json.loads(_gh_api(repo, f"actions/caches?per_page=100&page={page}"))["actions_caches"]
        caches += batch
        if len(batch) < 100:
            return caches
        page += 1


def _list_open_pr_refs(repo: str) -> set[str]:
    """Return the cache refs belonging to currently-open pull requests."""
    refs: set[str] = set()
    page = 1
    while True:
        batch = json.loads(_gh_api(repo, f"pulls?state=open&per_page=100&page={page}"))
        refs.update(f"refs/pull/{pull['number']}/merge" for pull in batch)
        if len(batch) < 100:
            return refs
        page += 1


def _list_branch_names(repo: str) -> set[str]:
    """Names of every live branch (for detecting deleted-branch cache refs)."""
    names: set[str] = set()
    page = 1
    while True:
        batch = json.loads(_gh_api(repo, f"branches?per_page=100&page={page}"))
        names.update(branch["name"] for branch in batch)
        if len(batch) < 100:
            return names
        page += 1


def _is_dead_ref(cache: dict, open_pr_refs: set[str], branch_names: set[str],
                 now: datetime.datetime) -> bool:
    """True when no future workflow run can ever restore this cache."""
    ref = cache["ref"]
    if _PR_CACHE_REF.fullmatch(ref):
        return ref not in open_pr_refs
    branch = _BRANCH_CACHE_REF.fullmatch(ref)
    if branch:
        return branch.group(1) not in branch_names
    if _TAG_CACHE_REF.match(ref):
        created = datetime.datetime.fromisoformat(cache["created_at"].replace("Z", "+00:00"))
        return now - created > _TAG_GRACE
    return False


def prune(repo: str, keep: int, apply: bool) -> int:
    """Delete all but the newest ``keep`` caches per (ref, family). Returns bytes freed."""
    caches = _list_caches(repo)
    open_pr_refs = _list_open_pr_refs(repo)
    branch_names = _list_branch_names(repo)
    now = datetime.datetime.now(datetime.timezone.utc)
    retained: list[dict] = []
    freed = 0

    # Dead refs first: closed-PR merge refs (a PR-close workflow can finish
    # before that PR's still-running jobs save their caches — this is the
    # backstop), deleted branches, and tag refs past the re-run grace window.
    for cache in caches:
        if _is_dead_ref(cache, open_pr_refs, branch_names, now):
            verb = "deleting" if apply else "would delete"
            print(f"{verb} {cache['size_in_bytes'] / 1e9:6.2f} GB  {cache['ref']}  {cache['key']}")
            if apply:
                _delete_cache(repo, cache)
            freed += cache["size_in_bytes"]
        else:
            retained.append(cache)

    groups: dict[tuple[str, str], list[dict]] = {}
    for cache in retained:
        groups.setdefault((cache["ref"], _family(cache["key"])), []).append(cache)

    for (ref, family), items in sorted(groups.items()):
        items.sort(key=lambda c: c["created_at"], reverse=True)  # newest first
        for stale in items[keep:]:
            verb = "deleting" if apply else "would delete"
            print(f"{verb} {stale['size_in_bytes'] / 1e9:6.2f} GB  {ref}  {stale['key']}")
            if apply:
                _delete_cache(repo, stale)
            freed += stale["size_in_bytes"]

    total = sum(c["size_in_bytes"] for c in caches)
    mode = "freed" if apply else "would free"
    print(f"\n{len(caches)} caches, {total / 1e9:.2f} GB total; "
          f"{mode} {freed / 1e9:.2f} GB -> {(total - freed) / 1e9:.2f} GB remaining")
    return freed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default=os.environ.get("REPO") or os.environ.get("GITHUB_REPOSITORY"),
                        help="owner/name (defaults to $REPO / $GITHUB_REPOSITORY)")
    parser.add_argument("--keep", type=int, default=1, help="newest entries to keep per family (default 1)")
    parser.add_argument("--apply", action="store_true", help="actually delete (default: dry-run)")
    args = parser.parse_args()
    if not args.repo:
        parser.error("no repository: pass --repo or set $REPO / $GITHUB_REPOSITORY")
    prune(args.repo, args.keep, args.apply)
    return 0


if __name__ == "__main__":
    sys.exit(main())
