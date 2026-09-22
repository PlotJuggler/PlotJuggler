# Update channels — apt repository and Windows maintenance tool

Status: **delivered** (`claude/debian-repository-setup-xeu8ne`). Records the
decisions behind how a released PJ4 reaches an already-installed machine, and
the alternatives that were weighed and dropped. The operational detail lives
with each format — [`packaging/deb/README.md`](../packaging/deb/README.md) and
[`packaging/installer/README.md`](../packaging/installer/README.md); this file
is the *why*.

## The problem

Every PJ4 artifact was a GitHub Release asset. That is a fine way to hand
someone a file and a terrible way to ship them a second one:

- a `.deb` installed with `apt install ./plotjuggler4_x.y.z_amd64.deb` is
  invisible to `apt upgrade` — nothing records where the file came from;
- the Windows installer's maintenance tool had no `<RemoteRepositories>`
  entry, so it could only uninstall.

In both cases the in-app `UpdateChecker` could *tell* a user a release existed,
but every upgrade was a manual download.

## Where to host it

The `.deb` is a **bundled vendor package** — it carries all of Qt 6 and the
Conan closure, a few hundred MB, and every user pulls the whole thing on every
upgrade. That single fact decided the hosting question: the binding cost is
**egress**, not storage, not features.

| Option | Verdict |
|---|---|
| **Cloudflare R2** | **Chosen.** Egress is not metered. Storage for a realistic number of retained releases sits inside the free tier. |
| JFrog Artifactory | Implemented first (commit `1401613`), then replaced. PlotJuggler already runs `plotjuggler.jfrog.io` for the `plotjuggler-conan` remote, so the account, DNS, anonymous-read policy and CI credential story all existed — but every user download meters against the plan quota. The Conan remote survives that because its traffic is mostly cached CI; an apt repository's is not. |
| GitHub Pages | Rejected outright. The ~1 GB site limit cannot hold even a handful of releases at this package size. |
| Launchpad PPA | Not applicable. PPAs build from source on Launchpad; PJ4 pins Qt 6.11.1 and much of the Conan graph is not packaged by Debian at all — which is *why* this is a vendor `.deb`. |
| Cloudsmith / packagecloud | Viable (they handle signing and CDN) but quota'd, and they solve a problem R2 does not have. |

### Rejected: metadata-only hosting with redirects to GitHub Releases

Since the `.deb` is already a Release asset and GitHub's release CDN costs the
project nothing, an attractive variant was to host only `dists/**` (kilobytes)
and serve `pool/**` as HTTP 302 redirects to the GitHub asset URL. apt follows
redirects, and integrity is unaffected because apt verifies the SHA256 from
`Packages` against whatever bytes arrive.

Dropped. It saves a few cents of storage a month and costs a Cloudflare Worker
(or a Redirect Rule whose regex support is plan-dependent), plus a filename →
release-tag mapping that breaks quietly whenever the naming convention moves.
Putting the package in the bucket is the boring answer and the right one.

## Accepted trade-offs

**The signing key lives in CI.** This is the one place a hosted package service
was genuinely better: Artifactory holds the GPG key server-side, where no
workflow can read it. On R2 the archive key is a GitHub secret, so anyone who
can land a workflow change can sign packages as PlotJuggler. Mitigations, not
fixes: prefer a signing subkey over the primary key, give it an expiry, keep
the revocation certificate offline.

**The index is rebuilt on every publish.** R2 is plain object storage and
maintains no package index, so `dists/**` is regenerated and re-signed each
time. That needs a `Packages` stanza for every version still on offer — and
re-downloading hundreds of MB of old `.deb`s to rescan them would be absurd.
Each stanza is therefore kept beside the pool under `.index/` as repository
state: a few KB per release that makes a publish incremental. Nothing under
`.index/` is read by apt.

**No delta updates, either platform.** The app is one indivisible payload, so
every upgrade re-downloads all of it. Acceptable because it matches what users
already do by hand; worth revisiting only if release cadence rises sharply.

## Two orderings carry the correctness

Both publishers write in the **reverse of the client's read order**, so a
client polling mid-publish can only ever see an index *older* than the payload
— never one promising a file that has not landed.

- apt reads `InRelease` → `Packages` → `.deb`, so `publish_apt.sh` uploads the
  package, then the indexes, then `Release`.
- The IFW maintenance tool reads `Updates.xml` → component archives, so
  `publish_update_repo.ps1` uploads archives, then `Updates.xml`.

**Retention is decided before the index is built and applied after it is
live.** An earlier draft deleted retired packages straight after upload, which
left `Packages` briefly advertising a version whose object was already gone —
a 404 at install time for anyone who ran `apt update` in that window. This was
caught in testing, not review.

## Other decisions worth not re-litigating

- **`Release` carries no `Valid-Until`.** An expiry would turn any pause in
  releases longer than the window into "repository is no longer signed" errors
  on machines that are perfectly fine.
- **One suite, `stable`, permanently.** A per-codename split (`jammy`,
  `noble`) would promise a distinction that does not exist: this is one
  bundled build with a glibc 2.35 floor. The name can never change — apt
  refuses a repository whose `Release` reports a different `Suite` than the one
  it last saw, and makes every user confirm by hand.
- **`arch=amd64` in the published `sources.list` line**, because that is the
  only architecture built today and a host on another one would otherwise warn
  on every `apt update`. Adding arm64 later means publishing into the same
  suite; existing users need no change.
- **Cache headers split by mutability.** Pool objects and IFW component
  archives are immutable (version in the filename) and cached for a year;
  indexes get 60 seconds, because a stale index at the edge is precisely what
  makes a client miss a release that is already published.
- **Windows: `<RemoteRepositories>` is injected at render time**, not
  templated into the checked-in `config.xml`. A build without `-UpdateUrl`
  stays byte-identical to one from before this existed; an empty `<Url/>`
  would leave every maintenance tool polling nothing on each launch.
- **Windows: installer and update repository come from one staging run.**
  `repogen` reads the same staged packages tree `binarycreator` consumed, so
  the offline installer and the update repository cannot describe different
  payloads for one version.

## Two properties users and maintainers must know

**The Windows update URL is compiled into the maintenance tool at install
time.** Only machines that installed a build already carrying it can ever check
for updates. Enabling this reaches nobody who installed an earlier release —
an argument for turning it on sooner rather than later.

**Windows integrity rests on TLS.** IFW has no signing mechanism comparable to
the apt repository's detached GPG signature, so the trust anchor is the HTTPS
certificate of the update host. Authenticode-signing the installer and
maintenance tool is the usual complement, and PJ4 does not sign yet.

## Verification status

Asymmetric, deliberately stated:

- **Linux — exercised end to end against real `apt`.** `apt-get update` accepts
  the signed repository; `apt-cache policy` resolves Debian version ordering
  (4.10.0-1 > 4.9.0-1 > 4.0.0-1, which lexicographic sorting gets wrong);
  `apt-get download` validates package checksums; a tampered `Packages.gz` is
  rejected against the signed `Release`. Sequential publishes confirm the
  stanza store keeps older versions in the index.
- **Windows — not executed at all.** `repogen` and the maintenance tool are
  Windows-only and no PowerShell was available where this was written, so the
  scripts are reviewed but unverified. The likeliest first-run failure is the
  `Updates.xml` version assertion, if IFW formats `<Version>` differently than
  expected; both call sites print what they actually found so that is a
  one-look diagnosis. **Run a `workflow_dispatch` build with
  `PJ4_WINDOWS_UPDATE_URL` temporarily set before relying on a tag.**

One methodology note, because it nearly produced a false negative: the first
tamper test edited the plain `Packages` file and apt accepted it. apt fetches
`Packages.gz`, so the edited file was never downloaded. Integrity was never
bypassed; the test was wrong. Any future test of this chain must target the
compressed index.

## Configuration this needs

Secrets shared by both channels: `R2_BUCKET`, `R2_ACCOUNT_ID`,
`R2_ACCESS_KEY_ID`, `R2_SECRET_ACCESS_KEY`. Linux additionally needs
`APT_GPG_PRIVATE_KEY` (and `APT_GPG_PASSPHRASE` if the key has one).

Each channel is gated on a repository **variable**, not a secret, because the
`secrets` context is unavailable in a job-level `if`: `PJ4_APT_PUBLIC_URL` and
`PJ4_WINDOWS_UPDATE_URL`. Unset either and that channel's job is skipped, so a
fork releases exactly as it did before.

## Known follow-ups

- **The in-app update check ignores the install channel.** `UpdateChecker`
  points every `PJ_INSTALLATION=deb` build at the GitHub release page, so an
  apt-installed copy is told to download a file rather than run
  `sudo apt upgrade plotjuggler4`. The two installs share one binary, so
  distinguishing them needs a runtime probe or a separate build stamp.
- **arm64.** `build_deb.sh` already maps `aarch64` → `arm64`, but CI builds
  only x86_64.
- **Retention is off by default.** `--keep <n>` exists and is tested; nobody
  has decided how many releases to keep.
- **`build_windows_installer.ps1` hardcodes the Qt version** in its `.qt`
  auto-detect path, against the "never hardcode a version" rule in the root
  `CLAUDE.md`. Only a local-dev convenience path — CI passes `-QtDir`
  explicitly — but it will stop auto-detecting at the next Qt bump.
- **`docs/plans/2026-08-03-debian-packaging.md` does not exist**, though
  `packaging/deb/build_deb.sh` and `linux-appimage-release.yml` both cite it.
  Pre-existing dangling references; `packaging/deb/README.md` now covers what
  that document would have said.
