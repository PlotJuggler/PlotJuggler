# SPDX-License-Identifier: MPL-2.0
# Compile the 5.9 MB asset set once: rcc produces a 25.8 MB C++ byte array.
# Sharing it across 49 executables avoids 1.26 GB of generated source and
# 0.25 GB of repeated objects per tree. An OBJECT library includes the
# qInitResources initializer in every consumer's link; a static archive member
# with no referenced symbols would be discarded by the linker.
add_library(pj_resources OBJECT resources.qrc)
set_target_properties(pj_resources PROPERTIES AUTORCC ON)
target_link_libraries(pj_resources PRIVATE Qt6::Core)
# Debug info for a byte array has no value; keep the object small under every
# PJ_DEBUG_INFO level.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(pj_resources PRIVATE -g0)
endif()
