#!/usr/bin/env bash
# In-container (or in-CI) AppImage build with bundled Python stdlib.
# Mirrors the steps used by the GitHub `ubuntu` workflow tag job.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

ARCH="${ARCH:-$(uname -m)}"
BUILD_DIR="${BUILD_DIR:-${REPO_ROOT}/build-appimage}"
APPDIR="${APPDIR:-${REPO_ROOT}/AppDir}"

# In CI, the previous step (`Build Plotjuggler`) already produced a build tree
# under ./build with ccache warm. Set SKIP_BUILD=1 to reuse it instead of
# reconfiguring/rebuilding from scratch.
if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
    echo "==> Configuring (arch=$ARCH, build dir=$BUILD_DIR)"
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DPJ_INSTALLATION="appimage" \
        -DPJ_PLUGINS_DIRECTORY="bin" \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache \
        -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

    echo "==> Building"
    cmake --build "$BUILD_DIR" --parallel "$(nproc)"
else
    echo "==> Reusing existing build tree at $BUILD_DIR"
fi

echo "==> Staging AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin"
cp -v "$BUILD_DIR"/bin/* "$APPDIR/usr/bin/"

# Discover the Python ABI the binary was linked against.
PY_LINK="$(ldd "$APPDIR/usr/bin/plotjuggler" | awk '/libpython3/ {print $1; exit}')"
if [[ -z "$PY_LINK" ]]; then
    echo "WARNING: plotjuggler is not linked against libpython — skipping Python bundling"
    PY_VER=""
else
    # libpython3.10.so.1.0 -> 3.10
    PY_VER="$(echo "$PY_LINK" | sed -nE 's/.*libpython([0-9]+\.[0-9]+).*/\1/p')"
    echo "==> Bundling Python $PY_VER stdlib"
    PY_STDLIB="/usr/lib/python${PY_VER}"
    if [[ ! -d "$PY_STDLIB" ]]; then
        echo "ERROR: $PY_STDLIB not found in builder; cannot bundle Python stdlib" >&2
        exit 1
    fi
    mkdir -p "$APPDIR/usr/lib/python${PY_VER}"
    # Copy stdlib via tar-pipe (rsync isn't in every base image) and skip caches
    # / tests / GUI bits to keep the AppImage small.
    tar -C "$PY_STDLIB" \
        --exclude='__pycache__' \
        --exclude='test' \
        --exclude='tests' \
        --exclude='idlelib' \
        --exclude='turtledemo' \
        --exclude='tkinter' \
        --exclude='*.pyc' \
        -cf - . \
      | tar -C "$APPDIR/usr/lib/python${PY_VER}" -xf -
fi

echo "==> Fetching linuxdeploy"
LDPLOY="${REPO_ROOT}/linuxdeploy-${ARCH}.AppImage"
LDPLOY_QT="${REPO_ROOT}/linuxdeploy-plugin-qt-${ARCH}.AppImage"
[[ -f "$LDPLOY" ]] || wget -q "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage" -O "$LDPLOY"
[[ -f "$LDPLOY_QT" ]] || wget -q "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${ARCH}.AppImage" -O "$LDPLOY_QT"
chmod +x "$LDPLOY" "$LDPLOY_QT"

echo "==> Running linuxdeploy (Qt + base)"
"$LDPLOY" --appimage-extract-and-run \
    --appdir "$APPDIR" \
    -d "$REPO_ROOT/io.plotjuggler.PlotJuggler.desktop" \
    -i "$REPO_ROOT/plotjuggler.png" \
    --plugin qt

# Drop the gtk2 platformtheme injection (we don't bundle it; Fusion is forced from main.cpp).
HOOK="$APPDIR/apprun-hooks/linuxdeploy-plugin-qt-hook.sh"
if [[ -f "$HOOK" ]]; then
    sed -i '/QT_QPA_PLATFORMTHEME/d' "$HOOK"
fi

# Add a hook so embedded Python finds the bundled stdlib instead of /usr/lib/python3.X.
if [[ -n "$PY_VER" ]]; then
    mkdir -p "$APPDIR/apprun-hooks"
    cat > "$APPDIR/apprun-hooks/python-home-hook.sh" <<EOF
# Force the embedded interpreter to use the stdlib bundled with the AppImage.
export PYTHONHOME="\$APPDIR/usr"
unset PYTHONPATH
EOF
fi

echo "==> Packaging AppImage"
"$LDPLOY" --appimage-extract-and-run \
    --appdir "$APPDIR" \
    --output appimage

echo "==> Done"
ls -la "$REPO_ROOT"/PlotJuggler*.AppImage
