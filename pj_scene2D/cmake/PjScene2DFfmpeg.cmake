function(pj_scene2d_find_ffmpeg out_found out_targets out_source)
  # The browser only ever links the cross-compiled archives from
  # cmake/PjWasmFfmpeg.cmake (PJ_WASM_WITH_VIDEO); never let a host package or a
  # sysroot installation leak native FFmpeg into the WASM graph.
  if(EMSCRIPTEN)
    if(TARGET ffmpeg::avcodec AND TARGET ffmpeg::avutil AND TARGET ffmpeg::swscale)
      set(${out_found} TRUE PARENT_SCOPE)
      set(${out_targets} ffmpeg::avcodec ffmpeg::swscale ffmpeg::avutil PARENT_SCOPE)
      set(${out_source} "cross-compiled WASM archives" PARENT_SCOPE)
    else()
      set(${out_found} FALSE PARENT_SCOPE)
      set(${out_targets} "" PARENT_SCOPE)
      set(${out_source} "" PARENT_SCOPE)
    endif()
    return()
  endif()

  find_package(ffmpeg CONFIG QUIET)
  if(TARGET ffmpeg::avcodec AND TARGET ffmpeg::avformat AND TARGET ffmpeg::avutil AND TARGET ffmpeg::swscale)
    set(${out_found} TRUE PARENT_SCOPE)
    set(${out_targets} ffmpeg::avcodec ffmpeg::avformat ffmpeg::avutil ffmpeg::swscale PARENT_SCOPE)
    set(${out_source} "Conan" PARENT_SCOPE)
    # Conan 2's CMakeDeps declares ffmpeg::* as INTERFACE IMPORTED targets
    # without IMPORTED_LOCATION, so $<TARGET_RUNTIME_DLLS:tgt> cannot find
    # the DLLs. Forward the per-config bin/ directories Conan wrote to its
    # data files so the caller can locate runtime DLLs (Windows needs them
    # next to test exes; otherwise STATUS_DLL_NOT_FOUND at launch).
    set(_bin_dirs "")
    foreach(_cfg RELWITHDEBINFO RELEASE DEBUG MINSIZEREL)
      if(DEFINED ffmpeg_BIN_DIRS_${_cfg})
        list(APPEND _bin_dirs "${ffmpeg_BIN_DIRS_${_cfg}}")
      endif()
    endforeach()
    list(REMOVE_DUPLICATES _bin_dirs)
    set(PJ_SCENE2D_FFMPEG_BIN_DIRS "${_bin_dirs}" PARENT_SCOPE)
    return()
  endif()

  find_package(PkgConfig QUIET)
  if(PkgConfig_FOUND)
    pkg_check_modules(LIBAVCODEC IMPORTED_TARGET libavcodec)
    pkg_check_modules(LIBAVFORMAT IMPORTED_TARGET libavformat)
    pkg_check_modules(LIBAVUTIL IMPORTED_TARGET libavutil)
    pkg_check_modules(LIBSWSCALE IMPORTED_TARGET libswscale)
  endif()

  if(LIBAVCODEC_FOUND AND LIBAVFORMAT_FOUND AND LIBAVUTIL_FOUND AND LIBSWSCALE_FOUND)
    set(${out_found} TRUE PARENT_SCOPE)
    set(${out_targets}
      PkgConfig::LIBAVCODEC
      PkgConfig::LIBAVFORMAT
      PkgConfig::LIBAVUTIL
      PkgConfig::LIBSWSCALE
      PARENT_SCOPE
    )
    set(${out_source} "pkg-config" PARENT_SCOPE)
    return()
  endif()

  set(${out_found} FALSE PARENT_SCOPE)
  set(${out_targets} "" PARENT_SCOPE)
  set(${out_source} "" PARENT_SCOPE)
endfunction()
