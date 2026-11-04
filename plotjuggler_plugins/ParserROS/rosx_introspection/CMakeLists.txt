cmake_minimum_required(VERSION 3.16)

project(rosx_introspection LANGUAGES CXX VERSION 1.0.2)

# create compile_commands.json
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Enable PIC only for the rosx_introspection target

###############################################
## Declare a C++ library
###############################################
add_library(rosx_introspection STATIC
    src/ros_type.cpp
    src/ros_field.cpp
    src/stringtree_leaf.cpp
    src/ros_message.cpp
    src/ros_parser.cpp
    src/deserializer.cpp
    src/serializer.cpp
    src/idl_parser.cpp
    src/flat_message_writer.cpp
    )

set_target_properties(rosx_introspection PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(rosx_introspection PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

target_compile_features(rosx_introspection PUBLIC cxx_std_17)

# peglib.h (used by idl_parser.cpp) checks __cplusplus >= 201703L, but MSVC
# keeps __cplusplus at 199711L for ABI back-compat unless /Zc:__cplusplus
# is passed explicitly.
if(MSVC)
    target_compile_options(rosx_introspection PRIVATE /Zc:__cplusplus)
endif()

# Tests
if(BUILD_TESTING)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    enable_testing()

    add_executable(test_ros_field tests/test_ros_field.cpp)
    target_link_libraries(test_ros_field PRIVATE rosx_introspection GTest::gtest_main)

    add_executable(test_cdr_alignment tests/test_cdr_alignment.cpp)
    target_link_libraries(test_cdr_alignment PRIVATE rosx_introspection GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(test_ros_field)
    gtest_discover_tests(test_cdr_alignment)
  endif()
endif()
