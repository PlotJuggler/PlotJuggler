#!/usr/bin/env bash
#
# Custom AppRun for the PlotJuggler 4 AppImage.
#
# Replaces linuxdeploy's generated launcher purely to reproduce the library/Qt
# environment the stock AppRun would set. It does NOT inject --plugin-dir: the
# app finds the bundled plugins at <prefix>/lib/plotjuggler/plugins
# (usr/bin/plotjuggler4 -> ../lib/plotjuggler/plugins) and seeds them into the
# user's extensions dir at startup, so --plugin-dir stays a user-facing
# option — anything the user passes is forwarded verbatim via "$@".
set -e

# $APPDIR is set by the AppImage runtime when mounted; fall back to this
# script's location for `--appimage-extract`ed / manual runs.
HERE="$(dirname "$(readlink -f "${0}")")"
APPDIR="${APPDIR:-${HERE}}"

# Join with ':' only when the variable is already set: an unconditional
# "${new}:${old:-}" leaves a trailing colon when $old is empty, and the loader
# reads an empty search-path entry as the current working directory.
if [[ -n "${LD_LIBRARY_PATH:-}" ]]; then
  export LD_LIBRARY_PATH="${APPDIR}/usr/lib:${LD_LIBRARY_PATH}"
else
  export LD_LIBRARY_PATH="${APPDIR}/usr/lib"
fi
if [[ -n "${QT_PLUGIN_PATH:-}" ]]; then
  export QT_PLUGIN_PATH="${APPDIR}/usr/plugins:${QT_PLUGIN_PATH}"
else
  export QT_PLUGIN_PATH="${APPDIR}/usr/plugins"
fi
export XDG_DATA_DIRS="${APPDIR}/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

# Embedded CPython (Python Data Processors) must find its stdlib at startup. The
# app bakes PYTHONHOME to the build machine's Conan cpython path, which is absent
# here; point it at the bundled stdlib (usr/lib/pythonX.Y under ${APPDIR}/usr).
# python_engine.cpp only sets PYTHONHOME when unset, so this takes precedence;
# a user-provided PYTHONHOME still wins.
export PYTHONHOME="${PYTHONHOME:-${APPDIR}/usr}"

# Mosaico's bundled gRPC (Arrow Flight client) has its default CA-roots path
# compiled to a build-sandbox path that does not exist at runtime, so TLS dials
# fail with "pem_root_certs cannot be nullptr". Point gRPC at the host CA bundle
# (first match wins; honor any pre-set override).
if [[ -z "${GRPC_DEFAULT_SSL_ROOTS_FILE_PATH:-}" ]]; then
  for _ca in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do
    [[ -f "${_ca}" ]] && export GRPC_DEFAULT_SSL_ROOTS_FILE_PATH="${_ca}" && break
  done
fi

# IBus input-context module loaded from a system/older Qt segfaults under the
# bundled Qt 6.11 runtime (same reason run.sh unsets it for dev runs).
unset QT_IM_MODULE

exec "${APPDIR}/usr/bin/plotjuggler4" "$@"
