# Developer tools, built only with PJ_BUILD_TOOLS; not part of the app build or CI.

find_package(mcap REQUIRED)
find_package(libjpeg-turbo REQUIRED)

# Extracts a frame from an MCAP to PPM; no Qt.
add_executable(extract_frame extract_frame.cpp)
target_compile_features(extract_frame PRIVATE cxx_std_20)
target_compile_options(extract_frame PRIVATE ${PJ_WARNING_FLAGS})
target_link_libraries(extract_frame PRIVATE
  pj_scene2d_core pj_datastore mcap::mcap libjpeg-turbo::libjpeg-turbo
)

# mp4_to_mcap/ is a standalone Python script (MP4 -> MCAP converter); it has no
# build target.
