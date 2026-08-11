#!/usr/bin/env bash
# Installs the Qt version pinned in versions.env into ./.qt via aqtinstall.
#
# build.sh, run.sh, Docker, and CI all source versions.env; update
# PJ_QT_VERSION there when changing the Qt toolchain pin. See docs/QT_NOTES.md
# for the rationale behind the current pin and what changed since 6.8.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/versions.env"

# aqt names the host, the architecture, and the directory it unpacks into
# differently per platform, and none of the three is derivable from the others.
case "$(uname -s)" in
  Linux)  QT_HOST=linux; QT_ARCH=linux_gcc_64; QT_KIT=gcc_64 ;;
  Darwin) QT_HOST=mac;   QT_ARCH=clang_64;     QT_KIT=macos ;;
  *)      echo "Unsupported host $(uname -s); install Qt ${PJ_QT_VERSION} manually" >&2; exit 1 ;;
esac

QT_DIR="${SCRIPT_DIR}/.qt/${PJ_QT_VERSION}/${QT_KIT}"

if [[ -d "$QT_DIR" ]]; then
  echo "Qt ${PJ_QT_VERSION} already installed at ${QT_DIR}"
  echo "export CMAKE_PREFIX_PATH=${QT_DIR}"
  exit 0
fi

if ! command -v aqt &>/dev/null; then
  echo "Installing aqtinstall..."
  pip install 'aqtinstall>=3.3'
fi

echo "Installing Qt ${PJ_QT_VERSION} via aqtinstall..."
# No add-on Qt modules needed (PJ4 uses only desktop-default modules; charts is
# replaced by the vendored Qwt, websockets is unused).
#
# Retry with backoff: aqt intermittently picks a mirror that is missing the
# metadata checksum ("Failed to download checksum ... Failed to locate XML data
# for Qt version"). It's transient — a retry usually lands on a healthy mirror.
attempt=0
until aqt install-qt "$QT_HOST" desktop "$PJ_QT_VERSION" "$QT_ARCH" --outputdir "${SCRIPT_DIR}/.qt"; do
  attempt=$((attempt + 1))
  if [[ "$attempt" -ge 5 ]]; then
    echo "aqt failed after ${attempt} attempts" >&2
    exit 1
  fi
  echo "aqt attempt ${attempt} failed (transient mirror?); retrying in $((attempt * 15))s..."
  sleep $((attempt * 15))
done

echo ""
echo "Qt ${PJ_QT_VERSION} installed at ${QT_DIR}"
echo ""
echo "To use it, run:"
echo "  export CMAKE_PREFIX_PATH=${QT_DIR}"
