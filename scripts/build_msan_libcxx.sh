#!/usr/bin/env bash
# SPDX-License-Identifier: MPL-2.0
#
# Build libc++ / libc++abi with MemorySanitizer instrumentation.
#
# WHY THIS EXISTS
#
# MSan reports reads of memory it never saw written. The C++ standard library
# writes memory constantly on behalf of your code, so if it is uninstrumented
# every std::string, std::vector and stream read looks like a use of
# uninitialised memory — reported inside YOUR code, about a value the library
# initialised correctly. And MSan has no runtime suppressions to mute that.
#
# A distro libc++ (apt's libc++-<ver>-dev) is a NORMAL build and does not help.
# libstdc++ cannot help either: it is not supported by MSan and is not shipped
# instrumented by anyone. So the lane needs its own libc++, built here, from the
# same LLVM release as the installed clang++.
#
# Output prefix is passed to CMake as -DPJ_MSAN_LIBCXX_ROOT (see
# cmake/PjSanitizers.cmake). The build is skipped when the prefix already holds
# a libc++ from that release, so callers may invoke this unconditionally.
#
# Usage:
#   scripts/build_msan_libcxx.sh [--prefix <dir>] [--llvm-version <ver>] [--force]
set -euo pipefail

PREFIX="${PJ_MSAN_LIBCXX_ROOT:-/opt/msan-libcxx}"
# Empty means the installed clang++'s own version, resolved below: the libc++ must
# come from the same LLVM release as the compiler instrumenting the code linking it.
LLVM_VERSION="${PJ_MSAN_LLVM_VERSION:-}"
FORCE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix)        PREFIX="${2:?--prefix needs a directory}"; shift 2 ;;
    --llvm-version)  LLVM_VERSION="${2:?--llvm-version needs a value}"; shift 2 ;;
    --force)         FORCE=1; shift ;;
    -h|--help)       sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "build_msan_libcxx: unknown argument: $1" >&2; exit 2 ;;
  esac
done

command -v clang++ >/dev/null 2>&1 || {
  echo "build_msan_libcxx: clang++ not found. MemorySanitizer is Clang-only; GCC rejects" >&2
  echo "                   -fsanitize=memory outright. Install clang first." >&2
  exit 3
}
[[ -n "${LLVM_VERSION}" ]] || LLVM_VERSION="$(clang++ -dumpversion)"

# Skip only when the prefix holds a libc++ from THIS LLVM release. The prefix is
# usually a persistent volume that outlives compiler bumps, and a libc++ from
# another release links and runs while no longer matching the compiler.
STAMP="${PREFIX}/.llvm-version"
if [[ "${FORCE}" -eq 0 && -e "${PREFIX}/lib/libc++.so" && "$(cat "${STAMP}" 2>/dev/null)" == "${LLVM_VERSION}" ]]; then
  echo "build_msan_libcxx: ${PREFIX} already holds an instrumented libc++ ${LLVM_VERSION} — nothing to do"
  exit 0
fi

SRC_DIR="$(mktemp -d)"
trap 'rm -rf "${SRC_DIR}"' EXIT

echo "build_msan_libcxx: fetching llvm-project ${LLVM_VERSION} runtimes"
# The runtimes-only source drop, not the full monorepo: this build needs just
# libcxx, libcxxabi and their shared cmake modules.
curl -fsSL --retry 3 \
  "https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}/llvm-project-${LLVM_VERSION}.src.tar.xz" \
  | tar -xJ -C "${SRC_DIR}" --strip-components=1

echo "build_msan_libcxx: configuring (this instruments the standard library itself)"
cmake -S "${SRC_DIR}/runtimes" -B "${SRC_DIR}/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi" \
  -DLLVM_USE_SANITIZER=MemoryWithOrigins \
  -DLIBCXX_ENABLE_SHARED=ON \
  -DLIBCXX_ENABLE_STATIC=OFF \
  -DLIBCXXABI_ENABLE_SHARED=ON \
  -DLIBCXXABI_ENABLE_STATIC=OFF \
  -DLIBCXX_CXX_ABI=libcxxabi \
  -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
  -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
  -DLIBCXX_INCLUDE_TESTS=OFF \
  -DLIBCXXABI_INCLUDE_TESTS=OFF

cmake --build "${SRC_DIR}/build" -j "$(nproc)"
# Replace, never overlay: headers another LLVM release installed here would
# otherwise survive next to this one's and be picked up by -isystem.
rm -rf "${PREFIX:?}/include" "${PREFIX:?}/lib" "${STAMP}"
cmake --install "${SRC_DIR}/build"

# Prove the result is actually instrumented rather than merely built: an
# uninstrumented libc++ links and runs fine and silently defeats the whole lane,
# which is exactly the failure mode this project keeps hitting.
# grep -c, not grep -q: -q exits at the first match, nm then dies of SIGPIPE, and
# under pipefail that failed pipeline reads as "no symbols" for a large library.
msan_syms="$(nm -D "${PREFIX}/lib/libc++.so.1" 2>/dev/null | grep -c '__msan_' || true)"
if [[ "${msan_syms}" -eq 0 ]]; then
  echo "build_msan_libcxx: ERROR — installed libc++ has no __msan_ symbols; it is NOT instrumented." >&2
  exit 4
fi
echo "${LLVM_VERSION}" > "${STAMP}"

echo "build_msan_libcxx: installed instrumented libc++ to ${PREFIX}"
echo "build_msan_libcxx: pass -DPJ_MSAN_LIBCXX_ROOT=${PREFIX} to CMake (build.sh --sanitize msan does this)"
