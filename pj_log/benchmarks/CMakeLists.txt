# GUI-thread CPU of the Log panel under playback and streaming (a dev benchmark, no ctest).
add_executable(log_panel_benchmark log_panel_benchmark.cpp)
target_link_libraries(log_panel_benchmark PRIVATE pj_log pj_resources pj_bench_probes)
target_compile_options(log_panel_benchmark PRIVATE ${PJ_WARNING_FLAGS})
