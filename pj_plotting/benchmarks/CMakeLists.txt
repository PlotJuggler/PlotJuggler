if(NOT TARGET benchmark::benchmark)
    find_package(benchmark CONFIG REQUIRED)
endif()

add_executable(adapter_streaming_benchmark adapter_streaming_benchmark.cpp)
target_link_libraries(adapter_streaming_benchmark PRIVATE pj_plotting_core benchmark::benchmark)

# Canvas frame-cost comparison (QRhi and raster canvases).
if(NOT EMSCRIPTEN)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    add_executable(plot_canvas_benchmark plot_canvas_benchmark.cpp)
    target_link_libraries(plot_canvas_benchmark PRIVATE pj_plotting pj_bench_probes Qt6::Test)
endif()
