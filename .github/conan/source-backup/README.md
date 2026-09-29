# Conan source backup

Repo-hosted mirror for third-party **source tarballs** whose upstream host is
unreachable from GitHub Actions runners. CI copies this directory into Conan's
source **download cache** (`core.sources:download_cache`), so a
`conan install --build=missing` is served locally and never depends on a
third-party download host being reachable from a datacenter IP.

## Safety net: ConanCenter's source mirror

CI also sets `core.sources:download_urls` to `["origin", <ConanCenter's
sha256-addressed source mirror>]`, so a recipe whose upstream host refuses a
datacenter IP is retried against the mirror instead of failing the build. That
is what unblocked `elfutils/0.190`, whose only URL, sourceware.org, answered 403
to a release runner.

**`"origin"` must stay first.** Conan treats the two list positions very
differently (`caching_file_downloader.py`): an *origin* failure of any kind is
caught and falls through to the next entry, whereas a *backup* entry that
answers 401/403 raises immediately — only its 404 falls through. A mirror-first
list therefore makes every cold `--build=missing`, on every platform, depend on
that one server staying up and anonymous. Origin-first costs nothing when
upstream is healthy: the mirror is not contacted at all.

Coverage is partial and not contractual: the mirror holds what ConanCenter's CI
happened to back up, which skews old (spot-checked 2026-09: `elfutils/0.190` and
`kissfft/131.1.0` present; `ffmpeg/9.0.1`, `openssl/3.6.4`, `assimp/6.0.5`
absent — those fetch fine from their own hosts today). So it is a safety net,
not a guarantee; this directory remains the answer whenever a source must be
under our control.

## Why this exists

`minizip/1.2.13` (a hard, unconditional requirement of `assimp`, which pj_scene3D
uses for URDF mesh loading) has no source of its own: its Conan recipe downloads
**zlib's** source tarball (minizip lives in zlib's `contrib/minizip/`) from a
single URL, `https://zlib.net/fossils/zlib-1.2.13.tar.gz` — and zlib.net rejects
GitHub-runner IPs with HTTP 415. Any cold Windows CI build was therefore a
lottery: runs could die mid-`conan install` before the missing binary package
was available in the PlotJuggler Conan repository.

The recipe for zlib itself carries a GitHub mirror URL and never fails; the
minizip recipe simply lacks one. An upstream PR adding the mirror to
conan-center's minizip recipe accompanies this change — **once that merges and
the pinned recipe revision picks it up, this directory can be retired.**

## Contract

- Each backed-up source is stored as **two files**, exactly as Conan's source
  download-cache layout expects:
  - `<sha256>` — the tarball itself, named by the sha256 of its contents. This
    is the same checksum the recipe's `conandata.yml` pins, and Conan re-verifies
    it on every use — a corrupted or tampered file can never be consumed.
  - `<sha256>.json` — Conan's metadata sidecar (`{"references": {...}}`). Without
    it Conan treats the blob as missing. Generate it by letting Conan download
    the source once with `core.sources:download_cache=<dir>` set, then copy the
    pair out of `<dir>/s/`.
- CI wires it up in the "Seed Conan source download cache" step of every
  workflow that runs Conan: copy both files into `<download_cache>/s/` and
  append `core.sources:download_cache=<dir>` (plus the mirror's
  `core.sources:download_urls=`) to `global.conf`. One lane escapes it: the
  MSan sanitizer lane switches to a dedicated `CONAN_HOME` (`build.sh`) after
  that step, and only rebuilds gtest + nanoarrow, both GitHub-hosted. A seeded source is
  served locally; anything not seeded falls through to the recipe's own URLs
  as usual. The `<dir>/s/` layout is Conan-internal but observed stable across
  Conan 2.x; if it ever changes, the seed simply misses and CI degrades to the
  old (network-dependent) behavior rather than breaking.

## Current contents

| File (sha256) | What it is | Consumed by |
|---|---|---|
| `b3a24de97a8fdb…` | `zlib-1.2.13.tar.gz` (byte-identical to the official release; also mirrored at `github.com/madler/zlib/releases/download/v1.2.13/`) | `minizip/1.2.13` ← `assimp` |

## Adding a new file

```bash
# 1. Fetch the tarball from any trustworthy mirror.
curl -LO <mirror-url>/<tarball>
# 2. Its sha256 MUST match the one pinned in the recipe's conandata.yml.
sha256sum <tarball>
# 3. Store it under that sha256 name, generate the .json via Conan's download
#    cache (see Contract above), commit both, and add a row to the table.
```
