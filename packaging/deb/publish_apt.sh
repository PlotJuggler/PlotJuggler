#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Publish a PlotJuggler 4 .deb to the apt repository on Cloudflare R2, so that
# `apt upgrade` reaches users who added it (a GitHub Release asset never could
# — see packaging/deb/README.md, "Distribution").
#
# Nothing is built or repackaged here: this uploads the exact artifact that
# packaging/deb/build_deb.sh produced and packaging/deb/smoke_test.sh proved
# installable, then regenerates and GPG-signs the apt metadata around it.
#
# R2 is plain object storage, so unlike a package-manager service it maintains
# no index of its own: dists/** is rebuilt and re-signed here on every publish.
# Rebuilding it needs a Packages stanza for EVERY version still on offer, not
# just this one, so each stanza is kept beside the pool as repository state
# under .index/ — a few KB per release, which is what makes a publish
# incremental instead of re-downloading every old .deb to rescan it.
#
# Environment (all required unless noted):
#
#   R2_BUCKET               bucket holding the repository
#   R2_ENDPOINT             https://<account-id>.r2.cloudflarestorage.com
#   AWS_ACCESS_KEY_ID       R2 API token with object read/write on the bucket
#   AWS_SECRET_ACCESS_KEY
#   APT_GPG_PRIVATE_KEY     ASCII-armored signing key (the archive key)
#   APT_GPG_PASSPHRASE      optional; omit for an unprotected key
#   PJ_APT_PUBLIC_URL       public base URL users put in sources.list, e.g.
#                           https://apt.plotjuggler.io — only needed to verify
#                           the publish, which --skip-verify turns off
#
# Usage:
#   packaging/deb/publish_apt.sh packaging/deb/plotjuggler4_4.0.0-1_amd64.deb
#
# Options:
#   --distribution <s>  apt suite (default: stable). Renaming it later forces
#                       every existing user to confirm the change by hand.
#   --component <c>     apt component (default: main).
#   --origin <o>        Release Origin field (default: PlotJuggler).
#   --label <l>         Release Label field (default: PlotJuggler).
#   --keep <n>          After publishing, delete all but the n newest versions
#                       of this package/architecture, pool file and stanza
#                       alike. Default: keep everything. Debian version
#                       ordering decides "newest", not filename order.
#   --skip-verify       Do not re-fetch the published repository to check it.
#   --dry-run           Build and sign everything locally, upload nothing, and
#                       leave the staged tree in place for inspection.
set -euo pipefail

DISTRIBUTION="stable"
COMPONENT="main"
ORIGIN="PlotJuggler"
LABEL="PlotJuggler"
KEEP=0
SKIP_VERIFY=0
DRY_RUN=0
DEB=""

# Prints the header comment block, stopping at the first line of real code, so
# it cannot drift out of sync with a fixed line range.
usage() { awk 'NR>1 { if (!/^#/) exit; if (/SPDX-License-Identifier/) next; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --distribution) DISTRIBUTION="${2:?--distribution needs a suite}"; shift 2 ;;
    --component)    COMPONENT="${2:?--component needs a name}"; shift 2 ;;
    --origin)       ORIGIN="${2:?--origin needs a value}"; shift 2 ;;
    --label)        LABEL="${2:?--label needs a value}"; shift 2 ;;
    --keep)         KEEP="${2:?--keep needs a count}"; shift 2 ;;
    --skip-verify)  SKIP_VERIFY=1; shift ;;
    --dry-run)      DRY_RUN=1; shift ;;
    -h | --help)    usage; exit 0 ;;
    -*) echo "publish_apt: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      [[ -z "${DEB}" ]] || { echo "publish_apt: only one .deb may be published per run" >&2; exit 2; }
      DEB="$(realpath -m -- "$1")"; shift ;;
  esac
done

[[ "${KEEP}" =~ ^[0-9]+$ ]] || { echo "ERROR: --keep takes a non-negative integer, got '${KEEP}'" >&2; exit 2; }

# ---------------------------------------------------------------------------
# 1. Sanity checks
# ---------------------------------------------------------------------------
[[ -n "${DEB}" ]] || { echo "ERROR: no .deb given. $(basename "${BASH_SOURCE[0]}") <package.deb>" >&2; exit 2; }
[[ -f "${DEB}" ]] || { echo "ERROR: ${DEB} not found" >&2; exit 1; }
for tool in dpkg-deb apt-ftparchive gpg curl gzip; do
  command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} required (apt-ftparchive is in apt-utils)" >&2; exit 1; }
done
(( DRY_RUN )) || command -v aws >/dev/null || { echo "ERROR: the AWS CLI is required to reach R2's S3 API" >&2; exit 1; }

# The control fields are authoritative — re-deriving name/version/arch from the
# filename would let a renamed file be indexed under the wrong architecture,
# which apt only discovers as a 404 at install time.
PACKAGE="$(dpkg-deb -f "${DEB}" Package)"
VERSION="$(dpkg-deb -f "${DEB}" Version)"
ARCH="$(dpkg-deb -f "${DEB}" Architecture)"
[[ -n "${PACKAGE}" && -n "${VERSION}" && -n "${ARCH}" ]] \
  || { echo "ERROR: ${DEB} has no Package/Version/Architecture control fields" >&2; exit 1; }

if [[ "${VERSION}" == *"~"* ]]; then
  echo "ERROR: '${VERSION}' is a pre-release version (build_deb.sh --commit-hash)." >&2
  echo "It sorts below the release it precedes, so apt would never offer it as an" >&2
  echo "upgrade — publishing it would only add a version users cannot reach." >&2
  exit 1
fi

# Debian's own pool layout: pool/<component>/<first letter>/<source>/<file>.
POOL_PATH="pool/${COMPONENT}/${PACKAGE:0:1}/${PACKAGE}/$(basename "${DEB}")"
INDEX_PREFIX=".index/${DISTRIBUTION}/${COMPONENT}/${ARCH}"
STANZA_NAME="${PACKAGE}_${VERSION}_${ARCH}.stanza"

if (( ! DRY_RUN )); then
  : "${R2_BUCKET:?R2_BUCKET is not set}"
  : "${R2_ENDPOINT:?R2_ENDPOINT is not set}"
  : "${AWS_ACCESS_KEY_ID:?AWS_ACCESS_KEY_ID is not set}"
  : "${AWS_SECRET_ACCESS_KEY:?AWS_SECRET_ACCESS_KEY is not set}"
fi
: "${APT_GPG_PRIVATE_KEY:?APT_GPG_PRIVATE_KEY is not set}"

# R2 is single-region; its S3 API wants a region and ignores which one.
export AWS_DEFAULT_REGION="${AWS_DEFAULT_REGION:-auto}"
# Newer AWS CLI releases attach a CRC32 checksum trailer to every upload by
# default, which R2 rejects. Ask for checksums only where the S3 API requires
# them; without this, `aws s3 cp` fails against R2 with an opaque 400.
export AWS_REQUEST_CHECKSUM_CALCULATION="${AWS_REQUEST_CHECKSUM_CALCULATION:-when_required}"
export AWS_RESPONSE_CHECKSUM_VALIDATION="${AWS_RESPONSE_CHECKSUM_VALIDATION:-when_required}"

s3() { aws s3 "$@" --endpoint-url "${R2_ENDPOINT}"; }

echo "Package:      ${PACKAGE} ${VERSION} (${ARCH})"
echo "Suite:        ${DISTRIBUTION}/${COMPONENT}"
echo "Pool path:    ${POOL_PATH}"
(( DRY_RUN )) || echo "Bucket:       ${R2_BUCKET}"

# ---------------------------------------------------------------------------
# 2. Staging tree — a local mirror of the repository paths being written
# ---------------------------------------------------------------------------
STAGE="$(mktemp -d)"
GNUPGHOME_DIR="$(mktemp -d)"
cleanup() {
  gpgconf --homedir "${GNUPGHOME_DIR}" --kill all >/dev/null 2>&1 || true
  rm -rf "${GNUPGHOME_DIR}"
  if (( DRY_RUN )); then
    echo ""
    echo "--dry-run: staged repository left at ${STAGE}"
  else
    rm -rf "${STAGE}"
  fi
}
trap cleanup EXIT

DIST_DIR="${STAGE}/dists/${DISTRIBUTION}"
BIN_DIR="${DIST_DIR}/${COMPONENT}/binary-${ARCH}"
mkdir -p "${STAGE}/$(dirname "${POOL_PATH}")" "${BIN_DIR}" "${STAGE}/${INDEX_PREFIX}"
cp "${DEB}" "${STAGE}/${POOL_PATH}"

# ---------------------------------------------------------------------------
# 3. Every OTHER version still on offer
#     Fetched from the stanza store rather than rescanned from the .debs: the
#     packages are hundreds of MB each and their stanzas are already final.
# ---------------------------------------------------------------------------
if (( ! DRY_RUN )); then
  echo ""
  echo "Fetching the stanzas of previously published versions..."
  s3 sync "s3://${R2_BUCKET}/${INDEX_PREFIX}/" "${STAGE}/${INDEX_PREFIX}/" \
    --exclude "*" --include "*.stanza" --exact-timestamps --no-progress
fi

# A version is published once. Pool files go up immutable, so an edge that
# already served this version keeps serving those bytes for a year: replacing
# them (a re-run release rebuilds with different bytes) would pair the new
# Packages checksum with the old .deb and fail `apt install` with Hash Sum
# mismatch. Re-publishing identical bytes, e.g. a retried job, is harmless.
PREVIOUS_STANZA="${STAGE}/${INDEX_PREFIX}/${STANZA_NAME}"
if [[ -f "${PREVIOUS_STANZA}" ]]; then
  published_sha="$(awk '/^SHA256:/ { print $2; exit }' "${PREVIOUS_STANZA}")"
  if [[ "${published_sha}" != "$(sha256sum "${DEB}" | cut -d' ' -f1)" ]]; then
    echo "ERROR: ${PACKAGE} ${VERSION} is already published with different content." >&2
    echo "Pool files are immutable at the edge; publish a new version instead." >&2
    exit 1
  fi
fi

# ---------------------------------------------------------------------------
# 4. This package's Packages stanza
#     Written after the sync, so the stanza comes from the .deb actually being
#     uploaded. apt-ftparchive scans a directory, so it is pointed at the staged
#     pool with only this .deb in it.
#     Running from the repository root is what makes the emitted Filename field
#     repository-relative, which is how apt turns it back into a download URL.
# ---------------------------------------------------------------------------
echo ""
echo "Generating the Packages stanza..."
( cd "${STAGE}" && apt-ftparchive packages pool > "${INDEX_PREFIX}/${STANZA_NAME}" )
[[ -s "${STAGE}/${INDEX_PREFIX}/${STANZA_NAME}" ]] \
  || { echo "ERROR: apt-ftparchive produced an empty stanza for ${DEB}" >&2; exit 1; }
grep -qxF "Filename: ${POOL_PATH}" "${STAGE}/${INDEX_PREFIX}/${STANZA_NAME}" \
  || { echo "ERROR: stanza Filename does not match ${POOL_PATH}" >&2; exit 1; }

published=()
while IFS= read -r stanza; do
  published+=("${stanza}")
done < <(find "${STAGE}/${INDEX_PREFIX}" -name '*.stanza' -type f | LC_ALL=C sort)

# ---------------------------------------------------------------------------
# 4b. Retention, decided BEFORE the index is built
#     The index and the pool must never disagree: an index listing a version
#     whose object has been deleted gives every user a 404 at install time.
#     So the versions being retired are dropped from the set the index is built
#     from here, and their objects are deleted only once that index is live
#     (step 7). Deletes only other versions of THIS package and architecture.
# ---------------------------------------------------------------------------
doomed_pool=()
doomed_stanza=()
if (( KEEP > 0 && ${#published[@]} > KEEP )); then
  # Debian version ordering is not lexicographic (1.10 > 1.9), so dpkg is the
  # only correct comparator. Insertion sort, newest first — the list is a
  # handful of releases.
  sorted_stanzas=()
  for stanza in "${published[@]}"; do
    v="$(awk -F': ' '/^Version: /{ print $2; exit }' "${stanza}")"
    inserted=0
    for (( i = 0; i < ${#sorted_stanzas[@]}; i++ )); do
      sv="$(awk -F': ' '/^Version: /{ print $2; exit }' "${sorted_stanzas[i]}")"
      if dpkg --compare-versions "${v}" gt "${sv}"; then
        sorted_stanzas=("${sorted_stanzas[@]:0:i}" "${stanza}" "${sorted_stanzas[@]:i}")
        inserted=1
        break
      fi
    done
    (( inserted )) || sorted_stanzas+=("${stanza}")
  done

  echo "Retention: keeping the ${KEEP} newest version(s) of ${PACKAGE}/${ARCH}."
  for (( i = KEEP; i < ${#sorted_stanzas[@]}; i++ )); do
    stanza="${sorted_stanzas[i]}"
    v="$(awk -F': ' '/^Version: /{ print $2; exit }' "${stanza}")"
    if [[ "${v}" == "${VERSION}" ]]; then
      echo "ERROR: retention would retire ${VERSION}, the version being published." >&2
      echo "Raise --keep, or publish a version newer than what is already there." >&2
      exit 1
    fi
    echo "  retiring ${v}"
    doomed_pool+=("$(awk -F': ' '/^Filename: /{ print $2; exit }' "${stanza}")")
    doomed_stanza+=("${INDEX_PREFIX}/$(basename "${stanza}")")
    rm -f "${stanza}"
  done

  published=()
  while IFS= read -r stanza; do
    published+=("${stanza}")
  done < <(find "${STAGE}/${INDEX_PREFIX}" -name '*.stanza' -type f | LC_ALL=C sort)
fi

echo "Versions in the index: ${#published[@]}"

# Concatenated with a blank line between stanzas — that separator is the record
# delimiter of the Packages format, and apt silently reads two merged stanzas
# as one truncated record without it.
: > "${BIN_DIR}/Packages"
for stanza in "${published[@]}"; do
  cat "${stanza}" >> "${BIN_DIR}/Packages"
  printf '\n' >> "${BIN_DIR}/Packages"
done
# -n drops the timestamp, so identical inputs give identical bytes.
gzip -9nkf "${BIN_DIR}/Packages"

# ---------------------------------------------------------------------------
# 5. Release, signed
#     No Valid-Until: it would expire the repository for every user the moment
#     releases paused for longer than the window, turning a quiet period into
#     "repository is no longer signed" errors on machines that are fine.
# ---------------------------------------------------------------------------
echo "Generating Release..."
apt-ftparchive \
  -o "APT::FTPArchive::Release::Origin=${ORIGIN}" \
  -o "APT::FTPArchive::Release::Label=${LABEL}" \
  -o "APT::FTPArchive::Release::Suite=${DISTRIBUTION}" \
  -o "APT::FTPArchive::Release::Codename=${DISTRIBUTION}" \
  -o "APT::FTPArchive::Release::Architectures=${ARCH}" \
  -o "APT::FTPArchive::Release::Components=${COMPONENT}" \
  -o "APT::FTPArchive::Release::Description=${ORIGIN} packages" \
  release "${DIST_DIR}" > "${STAGE}/Release.tmp"
mv "${STAGE}/Release.tmp" "${DIST_DIR}/Release"

echo "Signing..."
export GNUPGHOME="${GNUPGHOME_DIR}"
chmod 0700 "${GNUPGHOME}"
printf '%s\n' "${APT_GPG_PRIVATE_KEY}" | gpg --batch --quiet --import
KEY_ID="$(gpg --batch --list-secret-keys --with-colons | awk -F: '/^fpr:/ { print $10; exit }')"
[[ -n "${KEY_ID}" ]] || { echo "ERROR: APT_GPG_PRIVATE_KEY contains no secret key" >&2; exit 1; }
echo "Key:          ${KEY_ID}"

gpg_sign() {
  gpg --batch --yes --quiet --local-user "${KEY_ID}" \
      --pinentry-mode loopback --passphrase "${APT_GPG_PASSPHRASE:-}" "$@"
}
# InRelease (inline signature) is what apt prefers; Release + Release.gpg is
# the detached pair older clients fall back to. Publishing both costs nothing.
gpg_sign --clearsign --output "${DIST_DIR}/InRelease" "${DIST_DIR}/Release"
gpg_sign --armor --detach-sign --output "${DIST_DIR}/Release.gpg" "${DIST_DIR}/Release"

# The signature is verified here, against the key just imported, so a signing
# misconfiguration fails the publish instead of shipping an InRelease that
# every apt client rejects.
gpg --batch --quiet --verify "${DIST_DIR}/InRelease" >/dev/null 2>&1 \
  || { echo "ERROR: the InRelease just written does not verify" >&2; exit 1; }

# The public half belongs in the repository: it is what the install
# instructions tell users to drop into /etc/apt/keyrings.
gpg --batch --armor --export "${KEY_ID}" > "${STAGE}/plotjuggler-archive-keyring.asc"

if (( DRY_RUN )); then
  echo ""
  echo "--dry-run: nothing uploaded."
  exit 0
fi

# ---------------------------------------------------------------------------
# 6. Upload, in the reverse of apt's read order
#     apt reads InRelease -> Packages -> the .deb. Writing in the opposite
#     order means a client fetching mid-publish can only ever see an index
#     older than what is in the pool, never an index promising a file that has
#     not landed yet.
#
#     Cache-Control matters as much as the order: pool files are immutable and
#     should be cached at the edge indefinitely, while a stale index is exactly
#     what makes `apt update` miss a release that is already published.
# ---------------------------------------------------------------------------
IMMUTABLE="public, max-age=31536000, immutable"
NOCACHE="public, max-age=60, must-revalidate"

# put <local file> <key relative to the repository root> <content type> <cache-control>
put() { s3 cp "$1" "s3://${R2_BUCKET}/$2" --content-type "$3" --cache-control "$4" --no-progress; }
BIN_KEY="dists/${DISTRIBUTION}/${COMPONENT}/binary-${ARCH}"

echo ""
echo "Uploading the package ($(du -h "${DEB}" | cut -f1))..."
put "${STAGE}/${POOL_PATH}" "${POOL_PATH}" "application/vnd.debian.binary-package" "${IMMUTABLE}"

echo "Uploading the indexes..."
put "${STAGE}/${INDEX_PREFIX}/${STANZA_NAME}" "${INDEX_PREFIX}/${STANZA_NAME}" "text/plain" "${NOCACHE}"
put "${BIN_DIR}/Packages"    "${BIN_KEY}/Packages"    "text/plain"       "${NOCACHE}"
put "${BIN_DIR}/Packages.gz" "${BIN_KEY}/Packages.gz" "application/gzip" "${NOCACHE}"

echo "Uploading Release..."
for f in Release Release.gpg InRelease; do
  put "${DIST_DIR}/${f}" "dists/${DISTRIBUTION}/${f}" "text/plain" "${NOCACHE}"
done
put "${STAGE}/plotjuggler-archive-keyring.asc" "plotjuggler-archive-keyring.asc" "text/plain" "${NOCACHE}"
# The one-line installer the README points at; it rides along with every
# publish so it always matches the repository layout it writes.
put "$(dirname "${BASH_SOURCE[0]}")/pj4_install.sh" "pj4_install.sh" "text/x-shellscript" "${NOCACHE}"

echo "Published: ${POOL_PATH}"

# ---------------------------------------------------------------------------
# 7. Retire the superseded objects
#     Safe now and not before: the index uploaded above no longer references
#     them, so nothing a client can read points at what is about to disappear.
# ---------------------------------------------------------------------------
if (( ${#doomed_pool[@]} > 0 )); then
  echo ""
  echo "Removing ${#doomed_pool[@]} superseded version(s)..."
  for key in "${doomed_pool[@]}" "${doomed_stanza[@]}"; do
    echo "  ${key}"
    s3 rm "s3://${R2_BUCKET}/${key}" --no-progress >/dev/null
  done
fi

# ---------------------------------------------------------------------------
# 8. Verify what a user would actually fetch
#     Unauthenticated and through the public URL, not the S3 endpoint: a bucket
#     that only answers to the CI credentials, or a custom domain that is not
#     wired up, is broken for every user while every upload above succeeded.
# ---------------------------------------------------------------------------
if (( SKIP_VERIFY )); then
  echo ""
  echo "--skip-verify: not re-fetching the published repository."
  exit 0
fi

: "${PJ_APT_PUBLIC_URL:?PJ_APT_PUBLIC_URL is not set (or pass --skip-verify)}"
PUBLIC_URL="${PJ_APT_PUBLIC_URL%/}"
echo ""
echo "Verifying ${PUBLIC_URL} as a user would fetch it..."

VERIFY_DIR="${STAGE}/verify"
mkdir -p "${VERIFY_DIR}"
for attempt in $(seq 1 10); do
  if curl --silent --fail --location --output "${VERIFY_DIR}/InRelease" \
       "${PUBLIC_URL}/dists/${DISTRIBUTION}/InRelease" 2>/dev/null \
     && gpg --batch --quiet --verify "${VERIFY_DIR}/InRelease" >/dev/null 2>&1; then
    break
  fi
  [[ "${attempt}" -lt 10 ]] || {
    {
      echo "ERROR: could not fetch and verify ${PUBLIC_URL}/dists/${DISTRIBUTION}/InRelease"
      echo "The uploads succeeded, so this is a serving problem. Check:"
      echo "  * the bucket is exposed at that URL (R2 custom domain or r2.dev)."
      echo "  * public read is enabled — apt fetches without credentials."
    } >&2
    exit 1
  }
  sleep 6
done

curl --silent --fail --location --output "${VERIFY_DIR}/Packages" \
  "${PUBLIC_URL}/dists/${DISTRIBUTION}/${COMPONENT}/binary-${ARCH}/Packages" \
  || { echo "ERROR: Packages is not fetchable from ${PUBLIC_URL}" >&2; exit 1; }
grep -qxF "Version: ${VERSION}" "${VERIFY_DIR}/Packages" \
  || { echo "ERROR: ${VERSION} is missing from the published Packages index" >&2; exit 1; }

# The index is only as good as the file it points at: a HEAD proves the pool
# object is actually readable at the URL apt will derive from Filename.
curl --silent --fail --location --head --output /dev/null "${PUBLIC_URL}/${POOL_PATH}" \
  || { echo "ERROR: ${PUBLIC_URL}/${POOL_PATH} is not fetchable" >&2; exit 1; }

echo ""
echo "Done. ${PACKAGE} ${VERSION} is live; users will be offered it after 'apt update'."
