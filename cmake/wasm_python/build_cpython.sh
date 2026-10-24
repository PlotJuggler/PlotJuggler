#!/usr/bin/env bash
# Copyright 2026 Davide Faconti
# SPDX-License-Identifier: MPL-2.0
#
# Builds the static wasm32-emscripten CPython (pthreads) that cmake/PjWasmPython.cmake links, from the pinned source
# tarball, and writes the stage it consumes. Called by that file at configure time; no network.
#
#   build_cpython.sh <tarball> <work dir> <stage dir> <jobs> <emscripten dir>
#
# Three steps, the same as CPython's own Platforms/emscripten driver minus its downloads:
#   1. a native build Python, which a cross build of CPython needs (same release);
#   2. the host build: cpython-pj4-wasm.patch, Setup.local, and libpython plus the archives of its builtins' native
#      code (HACL* SHA-2, the bundled libmpdec, so no mpdecimal download);
#   3. the stage: headers, archives, the stdlib zip (stdlib_zip.py) and the third-party notices.
set -euo pipefail

# Everything below sees only what emsdk needs. An inherited CC, CFLAGS, LDFLAGS, EMCC_CFLAGS, EM_PKG_CONFIG_PATH or
# PKG_CONFIG_LIBDIR (the Emscripten toolchain's CMake run exports some) would change the archives without changing
# the stage's key.
if [[ -z ${PJ_CPYTHON_CLEAN_ENV:-} ]]; then
  keep=(PJ_CPYTHON_CLEAN_ENV=1 PATH="${PATH}" HOME="${HOME}" TMPDIR="${TMPDIR:-/tmp}")
  for name in EMSDK EMSDK_NODE EMSDK_PYTHON EM_CONFIG EM_CACHE; do
    if [[ -n ${!name:-} ]]; then
      keep+=("${name}=${!name}")
    fi
  done
  exec env -i "${keep[@]}" bash "$0" "$@"
fi
# Fixes __DATE__ and __TIME__ (Modules/getbuildinfo.c); with the prefix map below, a cold build anywhere gives the same
# archives.
export SOURCE_DATE_EPOCH=315532800

tarball=$1
work=$2
stage=$3
jobs=$4
emscripten=$5
recipe=$(cd "$(dirname "$0")" && pwd)

rm -rf "${work}" "${stage}"
mkdir -p "${work}/native" "${work}/host/Modules"
tar -xf "${tarball}" -C "${work}"
src=$(echo "${work}"/Python-*)
patch -d "${src}" -p1 --quiet < "${recipe}/cpython-pj4-wasm.patch"

(cd "${work}/native" && "${src}/configure" --quiet && make --quiet -j"${jobs}" all)

# An out-of-tree build reads Modules/Setup.local from its build directory.
cp "${recipe}/Setup.local" "${work}/host/Modules/Setup.local"
cd "${work}/host"
export PATH="${emscripten}:${PATH}"
# -DPY_CALL_TRAMPOLINE with the patch's JS trampoline: the wasm-gc one is registered in one thread's table only.
# -ffile-prefix-map, and configure by a relative path (getpath.c embeds VPATH): no trace of the work directory.
# n/a: modules whose native dependency the browser build has none of, or never imports.
CONFIG_SITE="${src}/Platforms/emscripten/config.site-wasm32-emscripten" HOSTRUNNER=node \
  emconfigure "../$(basename "${src}")/configure" --quiet \
  "CFLAGS=-DPY_CALL_TRAMPOLINE -pthread -ffile-prefix-map=${work}=/cpython" \
  --host=wasm32-emscripten --build="$("${src}/config.guess")" \
  --with-build-python="${work}/native/python" --prefix=/ \
  --disable-shared --disable-ipv6 --disable-test-modules \
  --enable-wasm-pthreads --disable-wasm-dynamic-linking \
  --without-pymalloc --without-mimalloc --enable-big-digits=30 --without-system-libmpdec \
  py_cv_module__ctypes=n/a py_cv_module__zstd=n/a py_cv_module__bz2=n/a py_cv_module__lzma=n/a \
  py_cv_module__sqlite3=n/a py_cv_module_pyexpat=n/a py_cv_module__elementtree=n/a py_cv_module__ssl=n/a \
  py_cv_module__hashlib=n/a py_cv_module__tkinter=n/a py_cv_module__curses=n/a py_cv_module__curses_panel=n/a \
  py_cv_module__dbm=n/a py_cv_module__gdbm=n/a py_cv_module_readline=n/a py_cv_module__uuid=n/a \
  py_cv_module_xxlimited=n/a py_cv_module_xxlimited_35=n/a
version=$(sed -n 's/^VERSION=[[:space:]]*//p' Makefile)
archives=("libpython${version}.a" Modules/_hacl/libHacl_Hash_SHA2.a Modules/_decimal/libmpdec/libmpdec.a)
make --quiet -j"${jobs}" "${archives[@]}"

mkdir -p "${stage}/include/python${version}" "${stage}/lib"
cp -R "${src}/Include/." "${stage}/include/python${version}/"
cp pyconfig.h "${stage}/include/python${version}/"
cp "${archives[@]}" "${stage}/lib/"
"${work}/native/python" -I "${recipe}/stdlib_zip.py" "${src}/Lib" "${stage}/lib/python${version/./}.zip"
{
  echo "Third-party notices for the CPython ${version} interpreter that PlotJuggler's browser build embeds"
  echo "(plotjuggler4.wasm, on-demand Python scripts). From the CPython source release: LICENSE, the"
  echo "incorporated-software section of Doc/license.rst, and the HACL* licence of Modules/_hacl."
  echo
  cat "${src}/LICENSE"
  echo
  sed -n '/^Licenses and Acknowledgements for Incorporated Software$/,$p' "${src}/Doc/license.rst"
  echo
  echo "HACL* (Modules/_hacl, the SHA-2 implementation)"
  echo "-----------------------------------------------"
  sed -n '1,/\*\//p' "${src}/Modules/_hacl/Hacl_Hash_SHA2.c"
} > "${stage}/THIRD_PARTY_NOTICES.txt"

rm -rf "${work}"
