# Standalone executables (no app, no store, no parser).

set(CMAKE_AUTOMOC ON)

# Qt-free benchmark for the canonical point-cloud conversion and retained vertex assembly (million-point time and
# memory).
if(TARGET pj_scene3d_core)
  add_executable(pointcloud_convert_benchmark pointcloud_convert_benchmark.cpp)
  target_compile_features(pointcloud_convert_benchmark PRIVATE cxx_std_20)
  target_compile_options(pointcloud_convert_benchmark PRIVATE ${PJ_WARNING_FLAGS})
  target_link_libraries(pointcloud_convert_benchmark PRIVATE pj_scene3d_core)
endif()

# GPU/CPU benchmark of the QRhi point pass (instanced quads vs the point-list fast path, static vs re-uploaded clouds)
# on an offscreen QRhi. Its numbers are in pj_scene3D/docs/TECHNICAL_NOTES.md "Measured design choices".
if(TARGET pj_scene3d_widgets)
  add_executable(pointcloud_rhi_benchmark pointcloud_rhi_benchmark.cpp)
  target_compile_features(pointcloud_rhi_benchmark PRIVATE cxx_std_20)
  target_compile_options(pointcloud_rhi_benchmark PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(pointcloud_rhi_benchmark PRIVATE ../widgets)
  target_link_libraries(pointcloud_rhi_benchmark PRIVATE pj_scene3d_widgets pj_bench_probes)
endif()

# GPU cost of the line clip behind the eye: a line buffer drawn as a vertex list vs one instance per segment. Its
# numbers are in pj_scene3D/docs/TECHNICAL_NOTES.md "SwiftShader drops lines that cross behind the eye".
if(TARGET pj_scene3d_widgets)
  add_executable(line_clip_rhi_benchmark line_clip_rhi_benchmark.cpp)
  target_compile_features(line_clip_rhi_benchmark PRIVATE cxx_std_20)
  target_compile_options(line_clip_rhi_benchmark PRIVATE ${PJ_WARNING_FLAGS})
  target_include_directories(line_clip_rhi_benchmark PRIVATE ../widgets)
  target_link_libraries(line_clip_rhi_benchmark PRIVATE pj_scene3d_widgets pj_bench_probes)
endif()
