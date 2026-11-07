# SPDX-License-Identifier: MPL-2.0
#
# Dialect policy for the renderers' committed .qsb packs (Scene2D, the plot
# canvas, Scene3D). Each shader directory's bake.sh is the single source of its
# targets; every pack carries SPIR-V (Vulkan),
# HLSL 50 (D3D11/12), MSL 12 (Metal), GLSL ES 300 (WebGL2), and a desktop GLSL
# no higher than 330: the OpenGL 3.3 floor, which macOS's 4.1 core context also
# accepts. 440 may ride along. New packs use `300 es,330,440`; older packs keep
# lower dialects they already bake (2D overlays and the plot canvas: 120 and 150).
# The browser-only Scene3D capability probe (pj_scene3D/wasm_probe) bakes GLSL ES
# 300 alone.
#
# pj_find_host_qsb(<out-var> <who>): path of the host qsb that re-bakes committed
# .qsb shader packs for the configure-time hash checks. Fails the configure,
# naming <who>, when Qt ShaderTools' qsb is missing.
function(pj_find_host_qsb out_var who)
  if(NOT TARGET Qt6::qsb)
    message(FATAL_ERROR "${who} shader verification requires the host Qt6::qsb tool")
  endif()
  get_target_property(_qsb Qt6::qsb IMPORTED_LOCATION)
  if(NOT _qsb OR _qsb MATCHES "-NOTFOUND$")
    get_target_property(_qsb Qt6::qsb IMPORTED_LOCATION_RELWITHDEBINFO)
  endif()
  if(NOT EXISTS "${_qsb}")
    message(FATAL_ERROR "${who} cannot locate the host qsb executable")
  endif()
  set(${out_var} "${_qsb}" PARENT_SCOPE)
endfunction()
