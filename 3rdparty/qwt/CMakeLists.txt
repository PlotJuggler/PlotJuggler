# Qwt is NOT vendored (see README.PJ4.md). This file provides the
# `plotjuggler_qwt` target both build paths link, from one of two sources:
#
#  - PJ_SYSTEM_QWT=OFF (default; the Conan/build.sh path): download the
#    official release tarball at configure time, pinned by SHA-256, and build
#    the plot-widget subset as a static library (no polar/dial/knob/thermo
#    families), mirroring what PJ3 shipped. Never patch the downloaded
#    sources; PlotJuggler customizations live in pj_plotting (PlotScaleDraw,
#    curve-style mapping in PlotWidgetBase).
#
#  - PJ_SYSTEM_QWT=ON (the pixi/conda path, Linux only): link the
#    environment's prebuilt shared Qwt (conda-forge `qwt`, built against the
#    same qt6-main the env provides). The pixi `configure` task turns this on;
#    Windows always uses the FetchContent build (see the guard below).

option(PJ_SYSTEM_QWT
    "Link the environment's prebuilt Qwt (conda/pixi) instead of downloading and building the pinned release" OFF)

# The browser build excludes the QOpenGLWidget-based canvas: QOpenGLWidget
# cannot provide its context contract on WebGL, so the wasm plot canvas is the
# QRhi one in pj_plotting and Qwt is built without the OpenGL components.
if(EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS
        Core Gui Widgets Concurrent Svg PrintSupport SvgWidgets)
    set(PJ_QWT_QT_LIBS
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::Svg
        Qt6::PrintSupport
        Qt6::SvgWidgets
    )
else()
    find_package(Qt6 REQUIRED COMPONENTS
        Core Gui Widgets Concurrent Svg OpenGL PrintSupport OpenGLWidgets SvgWidgets)
    set(PJ_QWT_QT_LIBS
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Qt6::Concurrent
        Qt6::Svg
        Qt6::OpenGL
        Qt6::PrintSupport
        Qt6::OpenGLWidgets
        Qt6::SvgWidgets
    )
endif()

if(PJ_SYSTEM_QWT)
    if(WIN32)
        # conda-forge ships qwt as a DLL on win-64, and MSVC emits the members
        # of QwtSeriesData<QPointF> both as qwt.dll exports and as local
        # instantiations in our QwtSeriesData subclasses -> LNK2005 at link.
        # Windows must use the default FetchContent static build.
        message(FATAL_ERROR "PJ_SYSTEM_QWT is not supported on Windows (shared-Qwt template-export collision); use the default FetchContent build")
    endif()
    find_path(PJ_QWT_INCLUDE_DIR NAMES qwt_plot.h PATH_SUFFIXES qwt qwt6 REQUIRED)
    find_library(PJ_QWT_LIBRARY NAMES qwt REQUIRED)
    # find_path/find_library search independently; make sure the headers are the
    # Qwt generation this codebase targets before trusting the pair.
    file(STRINGS "${PJ_QWT_INCLUDE_DIR}/qwt_global.h" pj_qwt_version_line REGEX "#define QWT_VERSION_STR")
    if(NOT pj_qwt_version_line MATCHES "\"6\\.3\\.")
        message(FATAL_ERROR "PJ_SYSTEM_QWT: expected Qwt 6.3.x headers at ${PJ_QWT_INCLUDE_DIR}, found: ${pj_qwt_version_line}")
    endif()
    message(STATUS "PJ_SYSTEM_QWT: using ${PJ_QWT_LIBRARY} (headers: ${PJ_QWT_INCLUDE_DIR})")

    add_library(plotjuggler_qwt INTERFACE)
    # SYSTEM: same third-party-header treatment as the FetchContent path below.
    target_include_directories(plotjuggler_qwt SYSTEM INTERFACE ${PJ_QWT_INCLUDE_DIR})
    target_link_libraries(plotjuggler_qwt INTERFACE ${PJ_QWT_LIBRARY} ${PJ_QWT_QT_LIBS})
    return()
endif()

include(FetchContent)
# DOWNLOAD_EXTRACT_TIMESTAMP only exists on CMake >= 3.24 (where CMP0135 warns
# when it is unset); older CMake would mis-parse the keyword as URL_HASH data.
set(pj_qwt_fetch_extra "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.24")
    list(APPEND pj_qwt_fetch_extra DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()
# URLs are tried in order. The downloads.sourceforge.net redirector is behind
# Cloudflare and intermittently answers 522 to CI runners; master.dl is
# SourceForge's master file host, reached without that redirect hop. URL_HASH
# verifies whichever one answers.
FetchContent_Declare(qwt_upstream
    URL https://downloads.sourceforge.net/project/qwt/qwt/6.3.0/qwt-6.3.0.tar.bz2
        https://master.dl.sourceforge.net/project/qwt/qwt/6.3.0/qwt-6.3.0.tar.bz2
    URL_HASH SHA256=dcb085896c28aaec5518cbc08c0ee2b4e60ada7ac929d82639f6189851a6129a
    ${pj_qwt_fetch_extra}
)
# The tarball has no top-level CMakeLists.txt, so this only downloads/extracts;
# the static library below compiles the sources directly.
FetchContent_MakeAvailable(qwt_upstream)

set(QWT_SRC
    qwt.h
    qwt_abstract_scale_draw.h
    qwt_bezier.h
    qwt_clipper.h
    qwt_color_map.h
    qwt_column_symbol.h
    qwt_date.h
    qwt_date_scale_draw.h
    qwt_date_scale_engine.h
    qwt_dyngrid_layout.h
    qwt_global.h
    qwt_graphic.h
    qwt_interval.h
    qwt_interval_symbol.h
    qwt_math.h
    qwt_magnifier.h
    qwt_null_paintdevice.h
    qwt_painter.h
    qwt_painter_command.h
    qwt_panner.h
    qwt_picker.h
    qwt_picker_machine.h
    qwt_pixel_matrix.h
    qwt_point_3d.h
    qwt_point_polar.h
    qwt_round_scale_draw.h
    qwt_scale_div.h
    qwt_scale_draw.h
    qwt_scale_engine.h
    qwt_scale_map.h
    qwt_spline_basis.h
    qwt_spline.h
    qwt_spline_parametrization.h
    qwt_spline_local.h
    qwt_spline_cubic.h
    qwt_spline_pleasing.h
    qwt_spline_polynomial.h
    qwt_symbol.h
    qwt_system_clock.h
    qwt_text_engine.h
    qwt_text_label.h
    qwt_text.h
    qwt_transform.h
    qwt_widget_overlay.h

    qwt_curve_fitter.h
    qwt_spline_curve_fitter.h
    qwt_weeding_curve_fitter.h
    qwt_event_pattern.h
    qwt_abstract_legend.h
    qwt_legend.h
    qwt_legend_data.h
    qwt_legend_label.h
    qwt_plot.h
    qwt_plot_renderer.h
    qwt_plot_curve.h
    qwt_plot_dict.h
    qwt_plot_directpainter.h
    qwt_plot_grid.h
    qwt_plot_histogram.h
    qwt_plot_item.h
    qwt_plot_abstract_barchart.h
    qwt_plot_barchart.h
    qwt_plot_multi_barchart.h
    qwt_plot_intervalcurve.h
    qwt_plot_tradingcurve.h
    qwt_plot_layout.h
    qwt_plot_marker.h
    qwt_plot_zoneitem.h
    qwt_plot_textlabel.h
    qwt_plot_rasteritem.h
    qwt_plot_spectrogram.h
    qwt_plot_spectrocurve.h
    qwt_plot_scaleitem.h
    qwt_plot_legenditem.h
    qwt_plot_seriesitem.h
    qwt_plot_shapeitem.h
    qwt_plot_canvas.h
    qwt_plot_panner.h
    qwt_plot_picker.h
    qwt_plot_zoomer.h
    qwt_plot_magnifier.h
    qwt_plot_rescaler.h
    qwt_point_mapper.h
    qwt_raster_data.h
    qwt_matrix_raster_data.h
    qwt_sampling_thread.h
    qwt_samples.h
    qwt_series_data.h
    qwt_series_store.h
    qwt_point_data.h
    qwt_scale_widget.h

    qwt.cpp
    qwt_curve_fitter.cpp
    qwt_bezier.cpp
    qwt_spline_curve_fitter.cpp
    qwt_weeding_curve_fitter.cpp
    qwt_abstract_legend.cpp
    qwt_legend.cpp
    qwt_legend_data.cpp
    qwt_legend_label.cpp
    qwt_plot.cpp
    qwt_plot_renderer.cpp
    qwt_plot_axis.cpp
    qwt_plot_curve.cpp
    qwt_plot_dict.cpp
    qwt_plot_directpainter.cpp
    qwt_plot_grid.cpp
    qwt_plot_histogram.cpp
    qwt_plot_item.cpp
    qwt_plot_abstract_barchart.cpp
    qwt_plot_barchart.cpp
    qwt_plot_multi_barchart.cpp
    qwt_plot_intervalcurve.cpp
    qwt_plot_zoneitem.cpp
    qwt_plot_tradingcurve.cpp
    qwt_plot_spectrogram.cpp
    qwt_plot_spectrocurve.cpp
    qwt_plot_scaleitem.cpp
    qwt_plot_legenditem.cpp
    qwt_plot_seriesitem.cpp
    qwt_plot_shapeitem.cpp
    qwt_plot_marker.cpp
    qwt_plot_textlabel.cpp
    qwt_plot_layout.cpp
    qwt_plot_canvas.cpp
    qwt_plot_panner.cpp
    qwt_plot_rasteritem.cpp
    qwt_plot_picker.cpp
    qwt_plot_zoomer.cpp
    qwt_plot_magnifier.cpp
    qwt_plot_rescaler.cpp
    qwt_point_mapper.cpp
    qwt_raster_data.cpp
    qwt_matrix_raster_data.cpp
    qwt_sampling_thread.cpp
    qwt_series_data.cpp
    qwt_point_data.cpp
    qwt_scale_widget.cpp
    qwt_abstract_scale_draw.cpp
    qwt_clipper.cpp
    qwt_color_map.cpp
    qwt_column_symbol.cpp
    qwt_date.cpp
    qwt_date_scale_draw.cpp
    qwt_date_scale_engine.cpp
    qwt_dyngrid_layout.cpp
    qwt_event_pattern.cpp
    qwt_graphic.cpp
    qwt_interval.cpp
    qwt_interval_symbol.cpp
    qwt_math.cpp
    qwt_magnifier.cpp
    qwt_null_paintdevice.cpp
    qwt_painter.cpp
    qwt_painter_command.cpp
    qwt_panner.cpp
    qwt_picker.cpp
    qwt_picker_machine.cpp
    qwt_pixel_matrix.cpp
    qwt_point_3d.cpp
    qwt_point_polar.cpp
    qwt_round_scale_draw.cpp
    qwt_scale_div.cpp
    qwt_scale_draw.cpp
    qwt_scale_map.cpp
    qwt_scale_engine.cpp
    qwt_spline_basis.cpp
    qwt_spline.cpp
    qwt_spline_parametrization.cpp
    qwt_spline_local.cpp
    qwt_spline_cubic.cpp
    qwt_spline_pleasing.cpp
    qwt_symbol.cpp
    qwt_system_clock.cpp
    qwt_text_engine.cpp
    qwt_text_label.cpp
    qwt_text.cpp
    qwt_transform.cpp
    qwt_widget_overlay.cpp

    qwt_plot_abstract_canvas.h
    qwt_plot_abstract_canvas.cpp
    )
if(NOT EMSCRIPTEN)
    list(APPEND QWT_SRC
        qwt_plot_opengl_canvas.h
        qwt_plot_opengl_canvas.cpp)
endif()
list(TRANSFORM QWT_SRC PREPEND "${qwt_upstream_SOURCE_DIR}/src/")

add_library(plotjuggler_qwt STATIC ${QWT_SRC})

set_target_properties(plotjuggler_qwt PROPERTIES
    AUTOMOC ON
)

# SYSTEM for consumers: external Qwt headers must not trip the strict
# PJ_WARNING_FLAGS (-Werror=conversion on GCC, /W4 /WX on MSVC) of the modules
# that include them — same treatment Qt and the Conan deps already get.
target_include_directories(plotjuggler_qwt PRIVATE ${qwt_upstream_SOURCE_DIR}/src)
target_include_directories(plotjuggler_qwt SYSTEM INTERFACE
    $<BUILD_INTERFACE:${qwt_upstream_SOURCE_DIR}/src>
)

target_link_libraries(plotjuggler_qwt PUBLIC ${PJ_QWT_QT_LIBS})

# Upstream 6.3.0 still calls Qt APIs deprecated by Qt 6.8 (QDateTime time-spec
# setters); silence those third-party warnings without touching the sources.
if(MSVC)
    target_compile_options(plotjuggler_qwt PRIVATE /wd4996)
else()
    target_compile_options(plotjuggler_qwt PRIVATE -Wno-deprecated-declarations)
endif()
