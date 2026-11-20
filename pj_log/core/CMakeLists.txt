# Qt-free: the merged row store and the row filter.
add_library(pj_log_core STATIC
    src/LogStore.cpp
    include/pj_log/LogStore.h
)

target_include_directories(pj_log_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(pj_log_core PUBLIC cxx_std_20)
target_compile_options(pj_log_core PRIVATE ${PJ_WARNING_FLAGS})
