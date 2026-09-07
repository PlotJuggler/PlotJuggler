# pj-raster-helper: GPLv2 (links vendored doomgeneric). Separate executable;
# PlotJuggler never links it. Only built when the vendored source is present.
enable_language(C)
find_package(Qt6 REQUIRED COMPONENTS Core Network)

set(DG_DIR ${CMAKE_SOURCE_DIR}/3rdparty/doomgeneric/doomgeneric)
file(GLOB DG_SRCS CONFIGURE_DEPENDS ${DG_DIR}/*.c)
# Exclude upstream platform front-ends (they provide their own main/DG_* impls)
# and non-generic utility/backend sources.
list(FILTER DG_SRCS EXCLUDE REGEX "/(doomgeneric_(allegro|emscripten|linuxvt|sdl|soso|sosox|win|xlib)|i_main|mus2mid|i_(allegro|sdl)(music|sound))\\.c$")

add_executable(pj-raster-helper
    raster_helper_main.cpp
    ${DG_SRCS}
)
target_include_directories(pj-raster-helper PRIVATE ${DG_DIR} ${CMAKE_SOURCE_DIR}/raster/ipc)
target_link_libraries(pj-raster-helper PRIVATE Qt6::Core Qt6::Network)
# doomgeneric is old C; silence its warnings and don't apply PJ's -Werror.
set_source_files_properties(${DG_SRCS} PROPERTIES COMPILE_FLAGS "-w")
