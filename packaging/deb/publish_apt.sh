#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Publish a PlotJuggler 4 .deb to the Artifactory apt repository, so that
# `apt upgrade` reaches users who added it (the GitHub Release asset never
# could — see packaging/deb/README.md, "Distribution").
#
# Nothing is built or repackaged here: this uploads the exact artifact that
# packaging/deb/build_deb.sh produced and packaging/deb/smoke_test.sh proved
# installable. Artifactory's Debian repository type generates and GPG-signs
# dists/<suite>/... itself, so there is no apt-ftparchive step to run.
#
# Credentials come from the environment, never the command line (argv is world
# readable in /proc):
#
#   ARTIFACTORY_TOKEN   identity token or API key      (required)
#   ARTIFACTORY_USER    account the token belongs to   (required unless the
#                       token is a bearer identity token; see --auth)
#
# Usage:
#   ARTIFACTORY_USER=ci ARTIFACTORY_TOKEN=... \
#     packaging/deb/publish_apt.sh packaging/deb/plotjuggler4_4.0.0-1_amd64.deb
#
# Options:
#   --repo-url <url>    Artifactory Debian repository base URL. Default:
#                       $PJ_APT_REPO_URL, else the public PlotJuggler repo.
#   --distribution <s>  apt suite to file the package under (default: stable).
#                       Repeatable: one upload can appear in several suites.
#   --component <c>     apt component (default: main).
#   --auth basic|bearer How to present ARTIFACTORY_TOKEN. Default: basic when
#                       ARTIFACTORY_USER is set, bearer otherwise.
#   --allow-prerelease  Permit a ~<hash> dispatch-build version. Off by
#                       default: such a version sorts BELOW the release it
#                       precedes, so apt would never offer it as an upgrade —
#                       it would only consume quota and confuse `apt policy`.
#   --skip-verify       Do not wait for the package to appear in the index.
#   --dry-run           Print what would be uploaded and exit.
set -euo pipefail

DEFAULT_REPO_URL="https://plotjuggler.jfrog.io/artifactory/plotjuggler-deb"
REPO_URL="${PJ_APT_REPO_URL:-${DEFAULT_REPO_URL}}"
COMPONENT="main"
DISTRIBUTIONS=()
AUTH_MODE=""
ALLOW_PRERELEASE=0
SKIP_VERIFY=0
DRY_RUN=0
DEB=""

# Prints the header comment block, stopping at the first line of real code, so
# it cannot drift out of sync with a fixed line range.
usage() { awk 'NR>1 { if (!/^#/) exit; if (/SPDX-License-Identifier/) next; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --repo-url)         REPO_URL="${2:?--repo-url needs a URL}"; shift 2 ;;
    --distribution)     DISTRIBUTIONS+=("${2:?--distribution needs a suite}"); shift 2 ;;
    --component)        COMPONENT="${2:?--component needs a name}"; shift 2 ;;
    --auth)             AUTH_MODE="${2:?--auth needs basic or bearer}"; shift 2 ;;
    --allow-prerelease) ALLOW_PRERELEASE=1; shift ;;
    --skip-verify)      SKIP_VERIFY=1; shift ;;
    --dry-run)          DRY_RUN=1; shift ;;
    -h | --help)        usage; exit 0 ;;
    -*) echo "publish_apt: unknown option: $1" >&2; usage >&2; exit 2 ;;
    *)
      [[ -z "${DEB}" ]] || { echo "publish_apt: only one .deb may be published per run" >&2; exit 2; }
      DEB="$(realpath -m -- "$1")"; shift ;;
  esac
done

(( ${#DISTRIBUTIONS[@]} )) || DISTRIBUTIONS=("stable")
REPO_URL="${REPO_URL%/}"

# ---------------------------------------------------------------------------
# 1. Sanity checks
# ---------------------------------------------------------------------------
[[ -n "${DEB}" ]]     || { echo "ERROR: no .deb given. $(basename "${BASH_SOURCE[0]}") <package.deb>" >&2; exit 2; }
[[ -f "${DEB}" ]]     || { echo "ERROR: ${DEB} not found" >&2; exit 1; }
command -v dpkg-deb >/dev/null || { echo "ERROR: dpkg-deb required (apt install dpkg)" >&2; exit 1; }
command -v curl     >/dev/null || { echo "ERROR: curl required" >&2; exit 1; }
command -v sha256sum >/dev/null || { echo "ERROR: sha256sum required (coreutils)" >&2; exit 1; }

# The control fields are authoritative — re-deriving name/version/arch from the
# filename would let a renamed file be filed under the wrong architecture, which
# apt only discovers as a 404 at install time.
PACKAGE="$(dpkg-deb -f "${DEB}" Package)"
VERSION="$(dpkg-deb -f "${DEB}" Version)"
ARCH="$(dpkg-deb -f "${DEB}" Architecture)"
[[ -n "${PACKAGE}" && -n "${VERSION}" && -n "${ARCH}" ]] \
  || { echo "ERROR: ${DEB} has no Package/Version/Architecture control fields" >&2; exit 1; }

if [[ "${VERSION}" == *"~"* && "${ALLOW_PRERELEASE}" -eq 0 ]]; then
  echo "ERROR: '${VERSION}' is a pre-release version (build_deb.sh --commit-hash)." >&2
  echo "It sorts below the release it precedes, so apt would never offer it as an" >&2
  echo "upgrade. Publish tag builds only, or pass --allow-prerelease." >&2
  exit 1
fi

# Debian's own pool layout: pool/<component>/<first letter>/<source>/<file>.
# Artifactory does not require it, but `apt` users and mirroring tools read the
# path, and a flat pool becomes unnavigable after a dozen releases.
POOL_PATH="pool/${COMPONENT}/${PACKAGE:0:1}/${PACKAGE}/$(basename "${DEB}")"

# Matrix parameters are how Artifactory's Debian repositories learn where a
# package belongs; without them the file uploads but never enters an index.
MATRIX=""
for dist in "${DISTRIBUTIONS[@]}"; do
  MATRIX="${MATRIX};deb.distribution=${dist}"
done
MATRIX="${MATRIX};deb.component=${COMPONENT};deb.architecture=${ARCH}"

TARGET="${REPO_URL}/${POOL_PATH}"

echo "Package:      ${PACKAGE} ${VERSION} (${ARCH})"
echo "Repository:   ${REPO_URL}"
echo "Suites:       ${DISTRIBUTIONS[*]}"
echo "Pool path:    ${POOL_PATH}"

if (( DRY_RUN )); then
  echo ""
  echo "--dry-run: would PUT ${TARGET}${MATRIX}"
  exit 0
fi

# ---------------------------------------------------------------------------
# 2. Credentials
#     Written to a 0600 curl config rather than passed as arguments: anything
#     in argv is readable by every user on the machine via /proc/<pid>/cmdline.
# ---------------------------------------------------------------------------
: "${ARTIFACTORY_TOKEN:?ARTIFACTORY_TOKEN is not set}"
if [[ -z "${AUTH_MODE}" ]]; then
  AUTH_MODE="basic"
  [[ -n "${ARTIFACTORY_USER:-}" ]] || AUTH_MODE="bearer"
fi

CURL_CONFIG="$(mktemp)"
trap 'rm -f "${CURL_CONFIG}"' EXIT
chmod 0600 "${CURL_CONFIG}"
case "${AUTH_MODE}" in
  basic)
    : "${ARTIFACTORY_USER:?--auth basic needs ARTIFACTORY_USER}"
    printf 'user = "%s:%s"\n' "${ARTIFACTORY_USER}" "${ARTIFACTORY_TOKEN}" > "${CURL_CONFIG}" ;;
  bearer)
    printf 'header = "Authorization: Bearer %s"\n' "${ARTIFACTORY_TOKEN}" > "${CURL_CONFIG}" ;;
  *) echo "ERROR: --auth must be 'basic' or 'bearer', got '${AUTH_MODE}'" >&2; exit 2 ;;
esac

# ---------------------------------------------------------------------------
# 3. Upload
#     The checksum headers make Artifactory verify the body it received and
#     reject a truncated transfer, instead of publishing a corrupt package that
#     only fails on the user's machine.
# ---------------------------------------------------------------------------
echo ""
echo "Uploading $(du -h "${DEB}" | cut -f1)..."
# The response body is kept rather than streamed: on success it is noise, on
# failure it is the only place Artifactory explains itself.
RESPONSE="$(mktemp)"
trap 'rm -f "${CURL_CONFIG}" "${RESPONSE}"' EXIT
http_status="$(curl --config "${CURL_CONFIG}" \
  --silent --show-error --retry 3 --retry-connrefused --retry-delay 5 \
  --upload-file "${DEB}" \
  --header "X-Checksum-Sha256: $(sha256sum "${DEB}" | cut -d' ' -f1)" \
  --header "X-Checksum-Sha1: $(sha1sum "${DEB}" | cut -d' ' -f1)" \
  --header "X-Checksum-Md5: $(md5sum "${DEB}" | cut -d' ' -f1)" \
  --output "${RESPONSE}" \
  --write-out '%{http_code}' \
  "${TARGET}${MATRIX}")"

case "${http_status}" in
  2*) echo "Uploaded: ${TARGET}" ;;
  *)
    {
      echo "ERROR: upload failed with HTTP ${http_status}"
      # Artifactory's error JSON carries no trailing newline.
      sed -e '$a\' "${RESPONSE}"
    } >&2
    exit 1 ;;
esac

# ---------------------------------------------------------------------------
# 4. Verify the package actually entered the index
#     Artifactory regenerates Debian metadata asynchronously. A successful PUT
#     therefore proves only that the FILE landed — if indexing fails (wrong
#     matrix parameters, missing signing key, a repo that is not of type Debian)
#     the upload still reports 200 and `apt install` 404s. Poll the real index
#     the way apt would read it.
# ---------------------------------------------------------------------------
if (( SKIP_VERIFY )); then
  echo "--skip-verify: not waiting for the index."
  exit 0
fi

INDEX_URL="${REPO_URL}/dists/${DISTRIBUTIONS[0]}/${COMPONENT}/binary-${ARCH}/Packages"
echo ""
echo "Waiting for ${PACKAGE} ${VERSION} to appear in ${INDEX_URL}..."
for attempt in $(seq 1 30); do
  # Anonymous read is how apt will fetch this, so verify unauthenticated: a repo
  # that only answers to the CI credentials is broken for every user.
  if curl --silent --fail --location "${INDEX_URL}" 2>/dev/null \
       | grep -qxF "Version: ${VERSION}"; then
    echo "Indexed after ~$(( (attempt - 1) * 10 ))s."
    echo ""
    echo "Done. Users on the repository will be offered ${PACKAGE} ${VERSION} after 'apt update'."
    exit 0
  fi
  sleep 10
done

{
  echo "ERROR: ${PACKAGE} ${VERSION} did not appear in the index within 5 minutes."
  echo "The file uploaded, so this is an indexing problem, not a transfer one. Check:"
  echo "  * ${REPO_URL} is a repository of type Debian (a Generic repo accepts the"
  echo "    upload and indexes nothing)."
  echo "  * anonymous read covers it — ${INDEX_URL} must be fetchable without credentials."
  echo "  * a GPG signing key is configured; Artifactory cannot publish an unsigned suite."
} >&2
exit 1
