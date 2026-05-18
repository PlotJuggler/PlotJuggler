set(SDK_SOURCES
    flight/metadata.cpp
    flight/connection_pool.cpp
    flight/query.cpp
    flight/mosaico_client.cpp
)

add_library(mosaico_sdk STATIC ${SDK_SOURCES})

target_include_directories(mosaico_sdk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# Plugins always link Arrow Flight statically.
if(NOT TARGET ArrowFlight::arrow_flight_static)
    message(FATAL_ERROR "[MosaicoSDK] ArrowFlight::arrow_flight_static target not found.")
endif()

target_link_libraries(mosaico_sdk PUBLIC
    arrow::arrow
    ArrowFlight::arrow_flight_static
    nlohmann_json::nlohmann_json
)

# Conan's CMakeDeps propagates ARROW_STATIC via Arrow::arrow_static but does
# not propagate ARROW_FLIGHT_STATIC via ArrowFlight::arrow_flight_static.
# Without ARROW_FLIGHT_STATIC, Arrow's flight headers default to
# __declspec(dllimport) on MSVC, producing LNK2019 on inline/implicit ctors.
# Pin the defines directly so they reach this target's .cpp files and
# propagate (PUBLIC) to consumers like ToolboxMosaico.
target_compile_definitions(mosaico_sdk PUBLIC ARROW_STATIC ARROW_FLIGHT_STATIC)

target_compile_features(mosaico_sdk PUBLIC cxx_std_17)
