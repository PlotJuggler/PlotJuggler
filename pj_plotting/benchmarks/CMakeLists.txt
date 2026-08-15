if(NOT TARGET benchmark::benchmark)
    find_package(benchmark CONFIG REQUIRED)
endif()

add_executable(adapter_streaming_benchmark adapter_streaming_benchmark.cpp)
target_link_libraries(adapter_streaming_benchmark PRIVATE pj_plotting_core benchmark::benchmark)
