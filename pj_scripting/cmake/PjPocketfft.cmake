# SPDX-License-Identifier: MPL-2.0
#
# pocketfft::pocketfft — BSD-3, header-only FFT (bandPower, and the browser build's
# FFT toolbox). Taken from the environment when it has one, else fetched:
#  - Conan: CMakeDeps' pocketfft config.
#  - conda-forge / distro packages (Debian libpocketfft-dev) / nix: the bare header.
#  - otherwise (browser build, a bare checkout): upstream's cpp branch, pinned (pocketfft
#    has no releases; ConanCenter's 2024-08 snapshot predates the <cstdint> include fix).
# Consumers define POCKETFFT_NO_MULTITHREADING: one FFT per call, no worker pool.
include_guard(GLOBAL)

if(NOT TARGET pocketfft::pocketfft)
  find_package(pocketfft CONFIG QUIET)
endif()
if(NOT TARGET pocketfft::pocketfft)
  find_path(PJ_POCKETFFT_INCLUDE_DIR pocketfft_hdronly.h)
  set(_pj_pocketfft_dir "${PJ_POCKETFFT_INCLUDE_DIR}")
  if(NOT PJ_POCKETFFT_INCLUDE_DIR)
    include(FetchContent)
    FetchContent_Declare(pocketfft
      URL https://github.com/mreineck/pocketfft/archive/4d865cba24359b05b37da4e75aee24498c3b78e7.tar.gz
      URL_HASH SHA256=db236e44b1020dd0b415e5040ce10d8be604fefb7ce83ec0b32bc0f78975aa2f
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_SUBDIR _pj_header_only_no_cmakelists)
    FetchContent_MakeAvailable(pocketfft)
    set(_pj_pocketfft_dir "${pocketfft_SOURCE_DIR}")
  endif()
  add_library(pocketfft::pocketfft INTERFACE IMPORTED GLOBAL)
  target_include_directories(pocketfft::pocketfft SYSTEM INTERFACE "${_pj_pocketfft_dir}")
  unset(_pj_pocketfft_dir)
endif()
