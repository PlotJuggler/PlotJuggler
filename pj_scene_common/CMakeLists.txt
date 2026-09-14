find_package(Qt6 REQUIRED COMPONENTS Core Widgets Xml)

add_library(pj_scene_common STATIC
  src/layer_factory.cpp
  src/layer_params.cpp
  src/scene_dock_widget.cpp
  src/scene_layer.cpp
  include/pj_scene_common/layer_factory.h
  include/pj_scene_common/layer_params.h
  include/pj_scene_common/scene_dock_widget.h
  include/pj_scene_common/scene_layer.h
)

set_target_properties(pj_scene_common PROPERTIES
  AUTOMOC ON
)

target_include_directories(pj_scene_common PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(pj_scene_common PUBLIC cxx_std_20)
target_compile_options(pj_scene_common PRIVATE
  ${PJ_WARNING_FLAGS}
)

target_link_libraries(pj_scene_common PUBLIC
  Qt6::Core
  Qt6::Widgets
  Qt6::Xml
  pj_runtime
  pj_datastore
  plotjuggler_sdk::base
)

if(PJ_BUILD_TESTS)
  add_subdirectory(tests)
endif()
