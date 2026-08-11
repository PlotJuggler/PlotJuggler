# ---------------------------------------------------------------------------
# pj_scene2d_core — media decode/playback core, no Qt dependency
# ---------------------------------------------------------------------------

if(EMSCRIPTEN)
  # PjWasmScene2DDependencies provides exact cross-compiled equivalents of the
  # native targets. Fail closed instead of accidentally resolving host binaries.
  if(NOT TARGET libjpeg-turbo::libjpeg-turbo OR NOT TARGET PNG::PNG)
    message(FATAL_ERROR "WASM Scene2D codec targets were not initialized")
  endif()
else()
  find_package(libjpeg-turbo REQUIRED)
  # Conan's config exposes the umbrella target libjpeg-turbo::libjpeg-turbo;
  # upstream's own config exports per-API targets instead (::jpeg, ::turbojpeg).
  # Bridge to the turbojpeg API — the only one this module uses (<turbojpeg.h>).
  # GLOBAL: pj_scene2D/tools is a sibling add_subdirectory (not a descendant of
  # this one) and links this same target name directly — a non-GLOBAL IMPORTED
  # target would be invisible there.
  if(NOT TARGET libjpeg-turbo::libjpeg-turbo)
    add_library(libjpeg-turbo::libjpeg-turbo INTERFACE IMPORTED GLOBAL)
    target_link_libraries(libjpeg-turbo::libjpeg-turbo INTERFACE libjpeg-turbo::turbojpeg)
  endif()
  find_package(PNG REQUIRED)
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../cmake/PjScene2DFfmpeg.cmake")
pj_scene2d_find_ffmpeg(PJ_SCENE2D_HAS_FFMPEG PJ_SCENE2D_FFMPEG_TARGETS PJ_SCENE2D_FFMPEG_SOURCE)

set(PJ_SCENE2D_CORE_SOURCES
  src/codec_pipeline.cpp
  src/codecs.cpp
  src/image_resolve.cpp
  src/image_pipeline_source.cpp
  src/undistort_remap.cpp
  src/image_rectifier.cpp
  src/parser_object.cpp
  src/depth_pipeline_source.cpp
  src/scene_pipeline_source.cpp
  src/scene_entities_2d_decoder.cpp
  src/composite_media_source.cpp
  src/async_frame_worker.cpp
  src/overlay_geometry.cpp
  src/video_color.cpp
)

if(PJ_SCENE2D_HAS_FFMPEG)
  list(APPEND PJ_SCENE2D_CORE_SOURCES
    src/ffmpeg_decoder.cpp
    src/thumbnail_codec.cpp
    src/h264_utils.cpp
    src/video_codec_utils.cpp
    src/streaming_video_decoder.cpp
    src/streaming_video_source.cpp
    src/video_frame_nal_extractor.cpp
    src/entry_thumbnail_cache.cpp
  )
endif()

add_library(pj_scene2d_core STATIC ${PJ_SCENE2D_CORE_SOURCES})
target_include_directories(pj_scene2d_core PUBLIC include)
target_compile_features(pj_scene2d_core PUBLIC cxx_std_20)
target_compile_options(pj_scene2d_core PRIVATE
  ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS}
)
if(MSVC)
  # libpng uses setjmp/longjmp for error handling, which MSVC warns
  # about via C4611 (non-portable interaction with C++ destruction).
  # The decoder paths that call setjmp here do not construct RAII
  # objects in the longjmp scope, so the warning is safe to silence.
  target_compile_options(pj_scene2d_core PRIVATE /wd4611)
endif()
target_link_libraries(pj_scene2d_core
  PUBLIC pj_base pj_datastore pj_plugin_sdk
  PRIVATE libjpeg-turbo::libjpeg-turbo PNG::PNG pj_internal_fmt
)

if(PJ_SCENE2D_HAS_FFMPEG)
  target_link_libraries(pj_scene2d_core PRIVATE ${PJ_SCENE2D_FFMPEG_TARGETS})
  target_compile_definitions(pj_scene2d_core PUBLIC PJ_HAS_FFMPEG)
  message(STATUS "pj_scene2d_core: FFmpeg found via ${PJ_SCENE2D_FFMPEG_SOURCE} -> building FfmpegDecoder")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

if(PJ_BUILD_TESTS)
  find_package(mcap REQUIRED)
  find_package(nlohmann_json REQUIRED)

  function(pj_scene2d_add_core_test test_name)
    set(options RUN_IN_SOURCE_DIR SUPPRESS_MSVC_SETJMP_WARNING)
    set(multiValueArgs SOURCES LIBS)
    cmake_parse_arguments(PJ_SCENE2D_TEST "${options}" "" "${multiValueArgs}" ${ARGN})

    add_executable(${test_name} ${PJ_SCENE2D_TEST_SOURCES})
    target_link_libraries(${test_name} PRIVATE ${PJ_SCENE2D_TEST_LIBS})
    target_compile_options(${test_name} PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
    if(MSVC AND PJ_SCENE2D_TEST_SUPPRESS_MSVC_SETJMP_WARNING)
      # Some test helpers call libpng's setjmp(png_jmpbuf(...)) with a
      # PngWriteCtx on the stack. Same C4611 rationale as the library
      # target above; suppress here for consistency across the module.
      target_compile_options(${test_name} PRIVATE /wd4611)
    endif()
    if(PJ_SCENE2D_TEST_RUN_IN_SOURCE_DIR)
      add_test(NAME ${test_name} COMMAND ${test_name}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      )
    else()
      add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
  endfunction()

  set(PJ_SCENE2D_CORE_TESTS
    ../tests/decoded_frame_test.cpp
    ../tests/video_color_test.cpp
    ../tests/depth_range_test.cpp
    ../tests/cancel_token_test.cpp
    ../tests/image_codecs_test.cpp
    ../tests/async_frame_worker_test.cpp
    ../tests/codecs_test.cpp
    ../tests/undistort_remap_test.cpp
    ../tests/image_pipeline_source_test.cpp
    tests/depth_pipeline_source_test.cpp
    ../tests/scene_frame_test.cpp
    ../tests/scene_pipeline_source_test.cpp
    tests/scene_entities_2d_decoder_test.cpp
    tests/overlay_geometry_test.cpp
    ../tests/composite_media_source_test.cpp
    tests/borrowed_media_source_test.cpp
    tests/composite_pixel_layers_test.cpp
  )

  foreach(test_src ${PJ_SCENE2D_CORE_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    pj_scene2d_add_core_test(${test_name}
      SOURCES ${test_src}
      LIBS pj_scene2d_core libjpeg-turbo::libjpeg-turbo PNG::PNG GTest::gtest_main
      SUPPRESS_MSVC_SETJMP_WARNING
    )
  endforeach()

  # Loader-side CDR adapter test. Builds the test alongside its adapter/palette
  # helper sources (the test's only consumers — they relocated here from the
  # former pj_scene2D/demos/ playground). nanocdr supplies the CDR reader and
  # pj_internal_fmt supplies fmt for marker_palette.cpp.
  pj_scene2d_add_core_test(cdr_to_image_annotation_test
    SOURCES
      ../tests/cdr_to_image_annotation_test.cpp
      ../tests/cdr_detection2d_to_image_annotation.cpp
      ../tests/cdr_yolo_to_image_annotation.cpp
      ../tests/marker_palette.cpp
    LIBS pj_scene2d_core pj_base GTest::gtest_main nanocdr pj_internal_fmt
  )
  target_compile_features(cdr_to_image_annotation_test PRIVATE cxx_std_20)

  # MCAP integration test (needs mcap + nlohmann_json + ObjectStore)
  pj_scene2d_add_core_test(mcap_integration_test
    SOURCES ../tests/mcap_integration_test.cpp
    LIBS
      pj_scene2d_core pj_datastore mcap::mcap nlohmann_json::nlohmann_json
      libjpeg-turbo::libjpeg-turbo GTest::gtest_main
    RUN_IN_SOURCE_DIR
  )

  # Streaming test: push + retention + concurrent read (M13)
  pj_scene2d_add_core_test(streaming_test
    SOURCES ../tests/streaming_test.cpp
    LIBS pj_scene2d_core pj_datastore GTest::gtest_main
  )

  # FFmpeg decoder test (needs test video file)
  if(PJ_SCENE2D_HAS_FFMPEG)
    pj_scene2d_add_core_test(ffmpeg_decoder_test
      SOURCES ../tests/ffmpeg_decoder_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
      RUN_IN_SOURCE_DIR
    )
    set_tests_properties(ffmpeg_decoder_test PROPERTIES
      ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/../tests/lsan_suppressions.txt"
    )

    # Thumbnail codec test (encode/decode/downscale — fixture-free)
    pj_scene2d_add_core_test(thumbnail_codec_test
      SOURCES ../tests/thumbnail_codec_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        libjpeg-turbo::libjpeg-turbo
        GTest::gtest_main
    )
    # thumbnail_codec.cpp is the only test that uses libswscale (downscaling),
    # and swscale pulls libavutil transitively. The linker default DT_RUNPATH is
    # NOT searched for a dependency's own dependencies, so on a host without a
    # system FFmpeg (CI) the Conan libavutil.so resolves for direct-dep tests but
    # not for libswscale -> libavutil here. Emit the (transitive) DT_RPATH instead
    # so the Conan FFmpeg dir on the rpath resolves libavutil for libswscale too.
    # DT_RUNPATH/DT_RPATH are an ELF concept: Mach-O resolves transitive
    # dependencies through @rpath entries that CMake already emits, and Apple's
    # ld rejects --disable-new-dtags outright, so the flag is ELF-only.
    if(NOT MSVC AND NOT APPLE)
      target_link_options(thumbnail_codec_test PRIVATE "LINKER:--disable-new-dtags")
    endif()

    # H.264 NAL utilities test
    pj_scene2d_add_core_test(h264_utils_test
      SOURCES ../tests/h264_utils_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
      RUN_IN_SOURCE_DIR
    )

    # Codec-generic video utilities test (format->codec_id map + keyframe oracles)
    pj_scene2d_add_core_test(video_codec_utils_test
      SOURCES ../tests/video_codec_utils_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
    )

    # StreamingVideoDecoder test (ObjectStore + FfmpegDecoder)
    pj_scene2d_add_core_test(streaming_video_decoder_test
      SOURCES ../tests/streaming_video_decoder_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
      RUN_IN_SOURCE_DIR
    )

    # Consumer-side zero-copy unwrap: store canonical PJ.VideoFrame wire messages
    # and decode them through a view-extractor that unwraps each to its Annex-B
    # data (the runtime's store-wire-bytes / defer-parse contract). Needs the
    # canonical codec (pj_base) + FFmpeg; uses test_mp4_demux.h to source frames.
    pj_scene2d_add_core_test(streaming_video_consumer_unwrap_test
      SOURCES ../tests/streaming_video_consumer_unwrap_test.cpp
      LIBS
        pj_scene2d_core pj_base pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
      RUN_IN_SOURCE_DIR
    )

    # StreamingVideoSource test (MediaSource adapter for StreamingVideoDecoder)
    pj_scene2d_add_core_test(streaming_video_source_test
      SOURCES ../tests/streaming_video_source_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
      RUN_IN_SOURCE_DIR
    )

    # Playback-fluidity benchmark (skips unless PJ_BENCH_MP4 points at a file)
    pj_scene2d_add_core_test(video_playback_bench_test
      SOURCES ../tests/video_playback_bench_test.cpp
      LIBS
        pj_scene2d_core pj_datastore
        ${PJ_SCENE2D_FFMPEG_TARGETS}
        GTest::gtest_main
    )

    if(WIN32 AND PJ_SCENE2D_FFMPEG_BIN_DIRS)
      # Windows is a non-blocking portability tracker (PJ4 v1 is Linux-only). Any
      # exe that transitively links pj_scene2d_core (the FFmpeg tests AND pj_app)
      # needs FFmpeg's shared DLLs (avcodec/avformat/avutil/swscale) resolvable at
      # launch, else STATUS_DLL_NOT_FOUND (0xc0000135). Conan 2's CMakeDeps declares
      # ffmpeg::* as INTERFACE IMPORTED with no IMPORTED_LOCATION, so
      # $<TARGET_RUNTIME_DLLS:tgt> is empty. Rather than copy DLLs next to every
      # target (a fragile per-target list that silently drops new consumers), export
      # the Conan bin dir(s) so the CI Test step can prepend them to PATH for all
      # test exes at once. Proper bundling (windeployqt + CPack) is deferred until
      # Windows is a supported release target — see .github/workflows/windows-ci.yml
      # and the "Non-goals" section of the root CLAUDE.md.
      string(REPLACE ";" "\n" _pj_ffmpeg_dirs_text "${PJ_SCENE2D_FFMPEG_BIN_DIRS}")
      file(WRITE "${CMAKE_BINARY_DIR}/pj_ffmpeg_bin_dirs.txt" "${_pj_ffmpeg_dirs_text}\n")
    endif()
  endif()

  # Dual-store test: ObjectStore + DataEngine from same MCAP (M11)
  pj_scene2d_add_core_test(dual_store_test
    SOURCES ../tests/dual_store_test.cpp
    LIBS pj_scene2d_core pj_datastore mcap::mcap GTest::gtest_main
    RUN_IN_SOURCE_DIR
  )
endif()
