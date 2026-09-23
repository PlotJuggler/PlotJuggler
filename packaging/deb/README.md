# PlotJuggler 4 — Debian/Ubuntu packaging

Builds `plotjuggler4_<version>_amd64.deb` by repackaging the AppDir that
[`packaging/appimage/build_appimage.sh`](../appimage/README.md) already produced. Nothing
is compiled here.

## Build

```bash
./build.sh                                        # build the app
packaging/appimage/build_appimage.sh --plugins-registry     # populate build/AppDir
packaging/deb/build_deb.sh                                  # -> packaging/deb/plotjuggler4_<ver>_amd64.deb
```

Useful flags:

| Flag | Purpose |
|---|---|
| `--appdir <path>` | Package an AppDir somewhere other than `build/AppDir`. |
| `--binary <path>` | Substitute the app binary (release CI ships one stamped `PJ_INSTALLATION=deb`) and restore the deployed `RUNPATH`. Needs `patchelf`. |
| `--output-dir <dir>` | Where to write the `.deb` (default: this folder). |
| `--commit-hash <hash>` | Mark a non-tag build: version becomes `<ver>~<hash>`. |

## What kind of package this is

A **bundled ("vendor") package**, like Chrome, VS Code and Zoom: the whole Qt 6
and Conan runtime is installed under `/opt/plotjuggler4`, with only a launcher
in `/usr/bin` plus a desktop entry and icon. `lintian` reports bundled
libraries; that is inherent, not a defect to fix.

A distro-native package is not achievable: `versions.env` pins Qt 6.11.1 and no
Ubuntu release ships it, and much of the Conan graph (mcap, cloudini, nanoarrow,
nanocdr, backward-cpp) is not packaged by Debian at all.

Supported floor: **Ubuntu 22.04+ / Debian 12+** (glibc 2.35), matching the
jammy container the release build runs in.

## Layout

```
/opt/plotjuggler4/bin/          plotjuggler4, qt.conf
/opt/plotjuggler4/bin/3rdparty/retro/
                                separately-licensed retro payload, when the
                                AppDir was built with --retro-wad
/opt/plotjuggler4/lib/          Qt 6, Conan closure, CPython stdlib,
                                plotjuggler/plugins/<id>/ (bundled extensions)
/opt/plotjuggler4/plugins/      Qt platform + imageformat plugins
/opt/plotjuggler4/translations/ Qt translations
/opt/plotjuggler4/share/doc/    licenses of the bundled libraries
/usr/bin/plotjuggler4           launcher (packaging/deb/plotjuggler4.wrapper.in)
/usr/share/applications/        desktop entry
/usr/share/icons/hicolor/…      icons (256px PNG + scalable SVG)
```

`bin` and `lib` stay siblings because the app resolves its bundled-plugin seed
as `applicationDirPath()/../lib/plotjuggler/plugins`. Those plugins are seeded
into a per-user writable directory at startup, so marketplace install and
uninstall keep working against a root-owned, read-only `/opt`.

**Only the desktop entry and icon may be installed into `/usr/share`.**
linuxdeploy fills `AppDir/usr/share/doc/<distro-pkg>/` with the copyright file
of every bundled system library; inside an AppImage those paths are private, but
in a `.deb` they would collide with the real `libglib2.0-0t64`,
`libkrb5support0` and ~36 other packages. They stay under `/opt`.

## Bundled plugins (seeding)

The registry plugins under `/opt/plotjuggler4/lib/plotjuggler/plugins/<id>/`
are a **seed source, never a load path** — identical to the AppImage. On every
startup `ExtensionCatalogService::seedBundledPlugins()` syncs them into the
per-user extensions dir (`~/.local/share/PlotJuggler/PlotJuggler4/extensions`):
copy when absent, staged refresh when the bundled version is semver-newer,
never a downgrade (a marketplace update above the bundled version wins). The
app then loads only from the user dir, so a root-owned read-only `/opt` is by
design — marketplace install/uninstall never writes there. Bundled ids count
as *core* extensions: at or below the bundled version they cannot be
uninstalled; one upgraded above it can be, and the next launch re-seeds the
bundled version ("downgrade to bundled").

Consequences specific to the `.deb` lifecycle: a package **upgrade** refreshes
each user's copies on their next launch (per-user, on demand — no maintainer
script touches home directories); package **removal** leaves the seeded
per-user copies behind (`postrm` deliberately spares user data), and being
self-contained they keep loading like any marketplace-installed extension.

The `ros2-topic-subscriber` bundle needs the same special treatment here as in
the release glibc audit: its per-distro inners under `dist/<distro>/` bind to
the user's *sourced* ROS 2 installation by design, so `build_deb.sh` excludes
that subtree from the `Depends` scan — otherwise librclcpp & co. would become
hard package dependencies no desktop machine satisfies.

## Dependencies

`Depends` is derived on every build from the **staged package payload** (after
the optional `--binary` substitution, so what is scanned is exactly what
ships): each ELF's `DT_NEEDED` minus the sonames the package itself provides
(ELF files only — a stray non-ELF named like a library cannot satisfy a real
dependency), mapped through the `SONAME_PKG` table in `build_deb.sh`. **An
unmapped soname is a hard error** — a dependency bump that introduces a new
external library fails the build instead of shipping a package that cannot
start on a clean machine. When that fires, add the soname to the table after
confirming the package name exists on Ubuntu 22.04.

`ca-certificates` is added unconditionally: the wrapper's gRPC CA export and
Qt's TLS (marketplace downloads, update check) need the system bundle, a
non-ELF requirement the `DT_NEEDED` scan structurally cannot see.

## Testing

`smoke_test.sh` installs the package in a bare container and runs it:

```bash
docker run --rm -v "$PWD/deb:/pkg:ro" ubuntu:22.04 sh /pkg/smoke_test.sh
```

It checks what a static dependency list cannot: `apt install` succeeding proves
the declared `Depends` resolve, but a too-new glibc or libstdc++ version symbol
is burned into the ELF and no package name expresses it. The test therefore,
in order: asserts the CA bundle landed; runs an **`ldd` closure gate** over
every shipped ELF *before* Xvfb is installed, so the linkable closure is proven
against the declared `Depends` alone (Xvfb pulls X11 libraries that would
otherwise mask a missing GUI dependency); runs `--selftest-python`
(display-free, and proves the wrapper's `PYTHONHOME` reaches the bundled
stdlib); and finally a real GUI startup under Xvfb — the bundled Qt ships only
the `xcb` platform plugin, so an X server is required, and this phase is what
covers `dlopen`'d libraries that `ldd` cannot see.

Release CI runs it on `ubuntu:22.04` and `ubuntu:24.04`, and the `.deb` is
attached to a GitHub Release only after it passes. The same container then
re-runs the runtime checks against the AppImage (`packaging/appimage/smoke_test.sh`),
using the libraries this package's `Depends` pulled in.

## Supported distros

amd64 with glibc ≥ 2.35 and dpkg: Ubuntu 22.04 LTS and newer, Debian 12
"bookworm" and newer, and their derivatives (Mint 21+, Pop!\_OS 22.04+, KDE
neon, Zorin 17+, elementary 7.1+). Every `Depends` entry has existed unchanged
since Ubuntu 22.04 / Debian 12 (the Ubuntu 24.04 `t64` rename touched none of
them). Older releases fail at run time on the glibc floor; non-dpkg distros
(Fedora, Arch, openSUSE) should use the AppImage.

## Distribution

The same `.deb` reaches users two ways.

**A release asset**, downloaded and installed by hand:

```bash
sudo apt install ./plotjuggler4_<version>_amd64.deb
```

`apt upgrade` does not update a package installed this way — nothing tells apt
where the file came from.

**The apt repository**, which does support `apt upgrade`:

```bash
curl -fsSL https://apt.plotjuggler.io/pj4_install.sh | sudo sh
```

[`pj4_install.sh`](pj4_install.sh) adds the key and source entry, then
installs `plotjuggler4`. It checks the system (root, apt, amd64, glibc ≥ 2.35,
no duplicate source entry) before changing anything, and is safe to re-run.
`publish_apt.sh` uploads it with every publish.

`arch=amd64` is there because that is the only architecture built today; a host
on another architecture would otherwise log a missing-index warning on every
`apt update`. `build_deb.sh` already maps `aarch64` → `arm64`, so adding arm64
later means publishing into the same suite — existing users need no change.

The in-app update check is unchanged either way: it is a compile-time
`PJ_INSTALLATION=deb` stamp on one binary, so an apt-installed copy still
points at the GitHub release page rather than saying `apt upgrade`.

### The suite name is permanent

Packages are filed under the single suite **`stable`**, one component
(`main`). One suite is the honest model: this is a bundled vendor package with
a glibc 2.35 floor, not a per-codename build, so `jammy`/`noble` suites would
promise a distinction that does not exist.

Never rename it. apt refuses a repository whose `Release` reports a different
`Suite` than the one it last saw and makes every user confirm the change by
hand.

## Publishing to the apt repository

`publish_apt.sh` uploads a built `.deb` to the repository on **Cloudflare R2**
and regenerates the signed metadata around it:

```bash
R2_BUCKET=<bucket> R2_ENDPOINT=https://<account-id>.r2.cloudflarestorage.com \
AWS_ACCESS_KEY_ID=<key> AWS_SECRET_ACCESS_KEY=<secret> \
APT_GPG_PRIVATE_KEY="$(cat archive-key.asc)" \
PJ_APT_PUBLIC_URL=https://apt.plotjuggler.io \
  packaging/deb/publish_apt.sh packaging/deb/plotjuggler4_<version>_amd64.deb
```

`--dry-run` builds and signs everything locally, uploads nothing, and leaves
the staged repository tree for inspection. It needs no R2 credentials.

Release CI does this in the `apt-publish` job of
[`linux-appimage-release.yml`](../../.github/workflows/linux-appimage-release.yml),
gated on `deb-release` so the GitHub Release asset lands first. It needs the
secrets `R2_BUCKET`, `R2_ACCOUNT_ID`, `R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`,
`APT_GPG_PRIVATE_KEY` (and `APT_GPG_PASSPHRASE` if the key has one) plus the
repository variable `PJ4_APT_PUBLIC_URL`; **without that variable the job is
skipped**, so a fork releases exactly as before.

Tag builds only — the script refuses a `<ver>~<hash>` dispatch version, which
sorts below the release it precedes and could never be offered as an upgrade.

### Why the repository rebuilds itself every time

R2 is plain object storage, so unlike a package-manager service it maintains no
index: `dists/**` is regenerated and re-signed on every publish. Doing that
needs a `Packages` stanza for **every** version still on offer, not just the new
one — and re-downloading hundreds of MB of old `.deb`s to rescan them would be
absurd. So each stanza is kept beside the pool as repository state:

```
pool/main/p/plotjuggler4/plotjuggler4_<version>_amd64.deb   the packages
.index/<suite>/<component>/<arch>/<pkg>_<ver>_<arch>.stanza one per published version
dists/<suite>/<component>/binary-<arch>/Packages{,.gz}      built from those stanzas
dists/<suite>/{Release,Release.gpg,InRelease}               signed
plotjuggler-archive-keyring.asc                             the public key
```

A few KB per release buys an incremental publish. Nothing under `.index/` is
read by apt.

### Two orderings that matter

**Upload order is the reverse of apt's read order.** apt reads `InRelease` →
`Packages` → the `.deb`, so the publisher writes the package first, then the
indexes, then `Release`. A client fetching mid-publish can then only ever see
an index *older* than the pool, never one promising a file that has not landed.

**Retention is decided before the index is built, applied after it is live.**
`--keep <n>` drops the retired versions from the set `Packages` is generated
from, and only deletes their objects once that index is published. The reverse
order would briefly advertise a version whose file is already gone — a 404 at
install time for anyone who ran `apt update` in the window. Retention is off by
default; `--keep` never touches anything but other versions of the same
package and architecture.

### Cache headers

Pool objects go up `immutable` with a one-year max-age — they never change.
Metadata goes up with a 60-second max-age, because a stale index at the edge is
exactly what makes `apt update` miss a release that is already published.

### No `Valid-Until`

The `Release` file deliberately carries no expiry. It would turn any pause in
releases longer than the window into "repository is no longer signed" errors on
machines that are perfectly fine.

## One-time Cloudflare setup

1. An **R2 bucket** for the repository, exposed at a stable public URL —
   a custom domain (`apt.plotjuggler.io`) is worth it over the `r2.dev`
   subdomain, because that hostname ends up in every user's `sources.list` and
   moving it later means asking all of them to edit a file.
2. **Public read** on the bucket. apt fetches unauthenticated; the publisher
   verifies this at the end of every run by re-fetching through the public URL
   rather than the S3 endpoint.
3. An **R2 API token** scoped to object read/write on that bucket only.
4. A **GPG signing key** — the archive key. Generate it offline, keep the
   private half in GitHub secrets and the revocation certificate somewhere
   else. Give it an expiry and a calendar reminder; a key that outlives its
   owner's attention is the usual way archive keys go wrong.

### Cost

Storage runs to a few cents a month at this package size, and R2 charges
nothing for egress — which is the entire reason this repository lives here
rather than somewhere metered. The bundled Qt and Conan closure make each
release a few hundred MB, and every user pulls it.

### The signing key lives in CI

This is the one real downside against a hosted package service: anyone who can
land a workflow change on this repository can sign packages as PlotJuggler.
Keep the key's use narrow, prefer a signing subkey over the primary key, and
treat `APT_GPG_PRIVATE_KEY` as the most sensitive secret in the repository.
