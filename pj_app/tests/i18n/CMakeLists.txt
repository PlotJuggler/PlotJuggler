# SPDX-License-Identifier: MPL-2.0
cmake_minimum_required(VERSION 3.22)
project(plotjuggler4_i18n_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Exercise the production language loader and embedded catalog with only the
# pinned Qt kit; useful before bootstrapping the full application's SDK graph.
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/PjVersions.cmake")
pj_read_versions("${CMAKE_CURRENT_LIST_DIR}/../../../versions.env")
find_package(Qt6 ${PJ_QT_VERSION} REQUIRED COMPONENTS Core Test LinguistTools)
if(MSVC)
    set(PJ_WARNING_FLAGS /W4 /WX /permissive-)
else()
    set(PJ_WARNING_FLAGS -Wall -Wextra -Wshadow -Wconversion -Wpedantic -Werror)
endif()
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/PjAppLanguage.cmake")
pj_add_app_language()
pj_add_app_language_test()
