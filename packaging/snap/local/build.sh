#!/bin/bash
# SPDX-License-Identifier: MPL-2.0
set -euo pipefail

src="${CRAFT_PART_SRC}"
# Working-tree version file copied by the wrapper.
# shellcheck disable=SC1091
. "${src}/versions.env"
work="${CRAFT_PART_BUILD}/snap-build"
build="${CRAFT_PART_BUILD}/build"
dest="${CRAFT_PART_INSTALL}"
mkdir -p "${work}"
python3 -m venv "${work}/tools"
"${work}/tools/bin/pip" install 'aqtinstall==3.3.0' 'conan==2.34.0' 'cmake==3.31.10'
export PATH="${work}/tools/bin:${PATH}"
export CONAN_HOME="${work}/conan-home"
export LD_LIBRARY_PATH="${dest}/usr/lib/${CRAFT_ARCH_TRIPLET_BUILD_FOR}:${dest}/lib/${CRAFT_ARCH_TRIPLET_BUILD_FOR}:${dest}/usr/lib"

export CC=gcc-13 CXX=g++-13
conan profile detect --exist-ok

bash "${CRAFT_PART_BUILD}/scripts/install_qt6.sh"
case "${CRAFT_ARCH_BUILD_FOR}" in
  amd64) qt_kit=gcc_64 ;;
  arm64) qt_kit=gcc_arm64 ;;
  *) echo "Unsupported architecture: ${CRAFT_ARCH_BUILD_FOR}" >&2; exit 1 ;;
esac
qt="${CRAFT_PART_BUILD}/.qt/${PJ_QT_VERSION}/${qt_kit}"

PJ_INSTALLATION=snap \
PJ_VERSION="${PJ_APP_VERSION}" \
PJ_BUILD_JOBS=4 \
PJ_BUILD_RASTER_HELPER=ON \
CMAKE_GENERATOR=Ninja \
bash "${CRAFT_PART_BUILD}/build.sh" --no-tests --no-demos --debug-info none

# Deploy the already-built Conan runtime and dependency licenses.
# Match the Release/Luau PIC settings used by the upstream build.sh.
conan install "${CRAFT_PART_BUILD}" \
  --output-folder="${build}" --no-remote --build=never \
  --lockfile="${CRAFT_PART_BUILD}/conan.lock" --lockfile-partial \
  -s build_type=Release -s compiler.cppstd=20 \
  -c "luau/*:tools.build:cxxflags=['-fPIC']" \
  -c "luau/*:tools.info.package_id:confs=['tools.build:cxxflags']" \
  --deployer="${src}/snap/local/runtime_deploy.py" \
  --deployer-folder="${dest}"

# Deploy the pj4 app.
install -Dm755 "${build}/pj_app/plotjuggler4" "${dest}/usr/bin/plotjuggler4"
install -Dm755 "${build}/pj_app/pj-plugin-check" "${dest}/usr/bin/pj-plugin-check"
install -Dm644 "${src}/resources/svg/plotjuggler4.png" "${dest}/usr/share/plotjuggler/plotjuggler4.png"
install -Dm644 "${src}/LICENSE" "${dest}/usr/share/doc/plotjuggler/copyright"

retro="${dest}/usr/bin/3rdparty/retro"
install -Dm755 "${build}/raster/helper/pj-raster-helper" "${retro}/pj-raster-helper"
install -m644 /usr/share/games/doom/doom1.wad "${retro}/base.wad"
install -m644 "${src}/3rdparty/retro/COPYING" \
  "${src}/3rdparty/retro/SOURCE-OFFER.txt" \
  "${src}/3rdparty/retro/SHAREWARE-LICENSE.txt" \
  "${src}/3rdparty/retro/README.md" "${retro}/"

# Detect and stage the Python version embedded by Conan.
pyhome=$(head -n1 "${build}/pj_python_home.txt")
mkdir -p "${dest}/usr/lib" "${dest}/usr/plugins"
cp -a "${pyhome}"/lib/python[0-9]*/ "${dest}/usr/lib/"

# Qt is installed separately from Conan.
# We copy its shared runtime and plugins.
# WARNING: aqt doesn't ship licenses files.
cp -a "${qt}"/lib/*.so* "${dest}/usr/lib/"
cp -a "${qt}/plugins/." "${dest}/usr/plugins/"

# APT archives contain MIME XML; their post-install cache generation is not run
# when Snapcraft extracts stage-packages.
update-mime-database "${dest}/usr/share/mime"
