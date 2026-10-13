# ---------------------------------------------------------------------------
# pj_tf - the TF frame tree (Transform/StampedTransform/TransformBuffer), a
# Qt-free leaf both pj_scene3d_core and pj_runtime link.
# ---------------------------------------------------------------------------

find_package(glm REQUIRED)
find_package(nlohmann_json REQUIRED)

add_library(pj_tf STATIC src/tf_buffer.cpp)
target_include_directories(pj_tf PUBLIC include)
target_compile_features(pj_tf PUBLIC cxx_std_20)
target_compile_options(pj_tf PRIVATE
  ${PJ_WARNING_FLAGS}
)
target_link_libraries(pj_tf
  PUBLIC glm::glm plotjuggler_sdk::base nlohmann_json::nlohmann_json
)

if(PJ_BUILD_TESTS)
  # plain: pj_tf links no Qt and none of its tests reference it.
  pj_add_test_runner(pj_tf_tests
    MAIN plain WARNINGS
    SOURCES
      tests/tf_buffer_test.cpp
      tests/tf_buffer_hierarchy_test.cpp
    LIBS pj_tf
  )
endif()
