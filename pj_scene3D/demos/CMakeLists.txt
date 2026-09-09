# ---------------------------------------------------------------------------
# pj_scene3D demos — standalone visual tests (no app, no store, no parser)
# ---------------------------------------------------------------------------

set(CMAKE_AUTOMOC ON)

if(TARGET pj_scene3d_widgets)
  add_executable(marker_gallery_demo marker_gallery_demo.cpp)
  target_compile_features(marker_gallery_demo PRIVATE cxx_std_20)
  target_link_libraries(marker_gallery_demo PRIVATE
    pj_scene3d_widgets pj_scene3d_core Qt6::Widgets
  )

  # PosesInFrame triad gizmos (PosesRenderPass / PosesInFrameLayer render path),
  # with a --screenshot mode for offscreen visual checks.
  add_executable(poses_gallery_demo poses_gallery_demo.cpp)
  target_compile_features(poses_gallery_demo PRIVATE cxx_std_20)
  target_link_libraries(poses_gallery_demo PRIVATE
    pj_scene3d_widgets pj_scene3d_core Qt6::Widgets
  )

  # Standalone URDF mesh viewer — fast look-development loop for the 3D mesh
  # pipeline without loading an MCAP. Reaches the private widgets/src headers
  # (urdf_package_resolver.h) the same way the module tests do.
  # resources.qrc gives the demo the same themed SVG icons the app registers
  # (pj_resources embeds + auto-registers them through its object files).
  add_executable(scene3d_mesh_viewer mesh_viewer.cpp)
  target_compile_features(scene3d_mesh_viewer PRIVATE cxx_std_20)
  target_compile_options(scene3d_mesh_viewer PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  target_include_directories(scene3d_mesh_viewer PRIVATE ../widgets/src)
  target_link_libraries(scene3d_mesh_viewer PRIVATE pj_scene3d_widgets pj_resources Qt6::Xml)

  # Self-verifying GPU-vs-CPU AABB-reduction benchmark (offscreen GL, no window).
  # Prints GL_RENDERER + timings and exits non-zero if the GPU result disagrees
  # with the CPU scan. Run with QT_QPA_PLATFORM=offscreen for headless CI.
  add_executable(pointcloud_aabb_benchmark pointcloud_aabb_benchmark.cpp)
  target_compile_features(pointcloud_aabb_benchmark PRIVATE cxx_std_20)
  target_compile_options(pointcloud_aabb_benchmark PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  target_link_libraries(pointcloud_aabb_benchmark PRIVATE pj_scene3d_widgets pj_scene3d_core Qt6::Gui)

  # Render benchmark for the three point-cloud shapes (offscreen GL, no window).
  # Times the production PointcloudRenderPass at the scene's MSAA sample count so
  # any change to the structurally-expensive cube path is backed by a number.
  add_executable(pointcloud_shape_benchmark pointcloud_shape_benchmark.cpp)
  target_compile_features(pointcloud_shape_benchmark PRIVATE cxx_std_20)
  target_compile_options(pointcloud_shape_benchmark PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  target_link_libraries(pointcloud_shape_benchmark PRIVATE pj_scene3d_widgets pj_scene3d_core Qt6::Gui)

  # Qt/GL-free benchmark for the canonical conversion + WASM-style retained
  # vertex assembly used by W14. It is part of the ordinary desktop build so
  # the million-point memory/time evidence stays reproducible.
  add_executable(pointcloud_convert_benchmark pointcloud_convert_benchmark.cpp)
  target_compile_features(pointcloud_convert_benchmark PRIVATE cxx_std_20)
  target_compile_options(pointcloud_convert_benchmark PRIVATE ${PJ_WARNING_FLAGS} ${PJ_SANITIZER_FLAGS})
  target_link_libraries(pointcloud_convert_benchmark PRIVATE pj_scene3d_core)
endif()
