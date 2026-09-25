if(NOT TARGET benchmark::benchmark)
    find_package(benchmark CONFIG REQUIRED)
endif()

add_executable(adapter_streaming_benchmark adapter_streaming_benchmark.cpp)
target_link_libraries(adapter_streaming_benchmark PRIVATE pj_plotting_core benchmark::benchmark)

# Canvas frame-cost comparison; needs both canvases, i.e. the QRhi build.
if(PJ_SCENE3D_RHI AND NOT EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS OpenGLWidgets Test)
    add_executable(plot_canvas_benchmark plot_canvas_benchmark.cpp)
    target_link_libraries(plot_canvas_benchmark PRIVATE pj_plotting pj_bench_probes Qt6::OpenGLWidgets Qt6::Test)
endif()
