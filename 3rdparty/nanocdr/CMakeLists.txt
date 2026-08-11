# ---------------------------------------------------------------------------
# nanocdr — single-header CDR (Common Data Representation) encoder/decoder,
# Apache-2.0. Used by loader-side adapters that translate ROS 2 CDR messages
# into pj_scene_protocol's canonical wire format.
# ---------------------------------------------------------------------------

add_library(nanocdr INTERFACE)
# SYSTEM: this is vendored upstream code we do not style-own, and consumers build
# with -Wall -Wextra -Wconversion. getCurrentEndianness() also reads back the
# inactive member of a union, which clang rejects outright under the
# DefaultError -Winvalid-constexpr; SYSTEM keeps that diagnostic out of consumer
# builds without weakening their own warning set.
target_include_directories(nanocdr SYSTEM INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_compile_features(nanocdr INTERFACE cxx_std_20)
